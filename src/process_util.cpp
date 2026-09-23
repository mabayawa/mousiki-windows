#include "process_util.h"
#include "console_log.h"

#include <array>
#include <cctype>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include "win_compat.h"
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace muisc {

std::string describe_argv(const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out += ' ';
        // Only for the log pane: show which arguments were one token.
        if (argv[i].find_first_of(" \t\"") != std::string::npos) {
            out += '"';
            out += argv[i];
            out += '"';
        } else {
            out += argv[i];
        }
    }
    return out;
}

// ===========================================================================
// Windows
// ===========================================================================
#ifdef _WIN32

namespace {

// Serialises one argument the way CreateProcessW's own parser
// (CommandLineToArgvW) will read it back. These are NOT the shell's rules:
// a backslash is only special immediately before a quote, where the run of
// backslashes must be doubled and the quote escaped.
void append_quoted(std::string& cmd, const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        cmd += arg;
        return;
    }
    cmd += '"';
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            cmd.append(backslashes * 2 + 1, '\\');
            cmd += '"';
        } else {
            cmd.append(backslashes, '\\');
            cmd += *it;
        }
    }
    cmd += '"';
}

bool is_batch_file(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return ext == ".cmd" || ext == ".bat";
}

// Every child is assigned to this job. JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
// means that when mousiki exits -- cleanly, by Ctrl+C, or by crashing -- the
// kernel closes the last handle to the job and terminates whatever is still
// inside it. Upstream acknowledges leaking ffmpeg when you quit mid-decode
// (app.cpp: "that ffmpeg subprocess can be orphaned rather than cleanly
// killed"); this is the fix, and it needs no cleanup code anywhere.
HANDLE job_handle() {
    static HANDLE job = [] {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (!h) return HANDLE(nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        return h;
    }();
    return job;
}

// Returns nullptr rather than INVALID_HANDLE_VALUE on failure, so callers can
// test it the same way they test every other handle here. INVALID_HANDLE_VALUE
// is (HANDLE)-1, which is truthy -- passing it on to an inherit list or a
// std handle is exactly the kind of mistake this avoids.
HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE h = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);
    return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
}

} // namespace

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE read_end = nullptr;

    ~Impl() {
        if (read_end) CloseHandle(read_end);
        if (process) CloseHandle(process);
    }
};

ChildProcess::ChildProcess() : impl_(new Impl) {}
ChildProcess::~ChildProcess() = default;

std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                                  bool merge_stderr) {
    if (argv.empty()) return nullptr;

    std::string exe = win_find_executable(argv[0]);
    if (exe.empty()) return nullptr;

    std::string cmdline;
    if (is_batch_file(exe)) {
        // CreateProcessW cannot execute a .cmd/.bat directly -- it is script
        // for the command interpreter, not an image. yt-dlp installed through
        // pip lands as exactly such a shim.
        std::string comspec = win_find_executable("cmd.exe");
        if (comspec.empty()) comspec = "cmd.exe";
        append_quoted(cmdline, comspec);
        cmdline += " /c ";
        append_quoted(cmdline, exe);
        for (size_t i = 1; i < argv.size(); ++i) { cmdline += ' '; append_quoted(cmdline, argv[i]); }
        exe = comspec;
    } else {
        append_quoted(cmdline, exe);
        for (size_t i = 1; i < argv.size(); ++i) { cmdline += ' '; append_quoted(cmdline, argv[i]); }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return nullptr;
    // Our end of the pipe must not travel into the child, or the child holds a
    // writer open and our read never sees EOF.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE nul_err = merge_stderr ? nullptr : open_nul(GENERIC_WRITE);

    // Handle inheritance is a process-wide property, so a plain
    // bInheritHandles=TRUE lets a child started on one thread inherit the
    // pipes of a spawn happening concurrently on another. mousiki runs the
    // background ffprobe sweep, a track probe, yt-dlp resolution and a lyrics
    // fetch at the same time, so that is not hypothetical: the stray writer
    // keeps the wrong pipe open and a reader blocks forever waiting for an
    // EOF that never comes. An explicit handle list pins down exactly which
    // handles this one call may inherit.
    std::vector<HANDLE> inherit;
    inherit.push_back(write_end);
    if (nul_in)  inherit.push_back(nul_in);
    if (nul_err) inherit.push_back(nul_err);

    // The sizing call is expected to fail with ERROR_INSUFFICIENT_BUFFER; its
    // job is only to fill in attr_size.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    // Tracked separately from have_attrs: if the list initialises but the
    // update fails, it still has to be deleted.
    bool attrs_inited = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) != FALSE;
    bool have_attrs = attrs_inited &&
                      UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                inherit.data(),
                                                inherit.size() * sizeof(HANDLE),
                                                nullptr, nullptr) != FALSE;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = have_attrs ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = nul_in;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError  = merge_stderr ? write_end : nul_err;
    if (have_attrs) si.lpAttributeList = attrs;

    // CREATE_NO_WINDOW is the counterpart of the POSIX /dev/null stdin
    // redirect: the child gets no console of its own and no access to ours, so
    // ffmpeg cannot read our keystrokes or leave our console mode altered --
    // and yt-dlp does not flash a black window on every metadata probe, of
    // which the background library sweep issues one per track.
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (have_attrs) flags |= EXTENDED_STARTUPINFO_PRESENT;

    std::wstring wexe = win_utf8_to_wide(exe);
    std::wstring wcmd = win_utf8_to_wide(cmdline);
    wcmd.push_back(L'\0');   // CreateProcessW may write into this buffer

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(wexe.c_str(), &wcmd[0], nullptr, nullptr,
                             TRUE, flags, nullptr, nullptr,
                             &si.StartupInfo, &pi);

    if (attrs_inited) DeleteProcThreadAttributeList(attrs);
    CloseHandle(write_end);
    if (nul_in)  CloseHandle(nul_in);
    if (nul_err) CloseHandle(nul_err);

    if (!ok) {
        CloseHandle(read_end);
        return nullptr;
    }

    // Assign to the job while the child is still suspended, so there is no
    // window in which it could spawn grandchildren outside the job.
    if (HANDLE job = job_handle()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->process  = pi.hProcess;
    child->impl_->read_end = read_end;
    return child;
}

long long ChildProcess::read_stdout(void* buf, size_t count) {
    if (!impl_->read_end) return -1;
    DWORD got = 0;
    if (!ReadFile(impl_->read_end, buf, static_cast<DWORD>(count), &got, nullptr)) {
        // The child closed its end: a normal end of stream, not a failure.
        return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
    }
    return static_cast<long long>(got);
}

int ChildProcess::wait() {
    if (impl_->read_end) { CloseHandle(impl_->read_end); impl_->read_end = nullptr; }
    if (!impl_->process) return -1;
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) return -1;
    return static_cast<int>(code);
}

// ===========================================================================
// POSIX
// ===========================================================================
#else

struct ChildProcess::Impl {
    pid_t pid = -1;
    int read_fd = -1;

    ~Impl() {
        if (read_fd >= 0) close(read_fd);
    }
};

ChildProcess::ChildProcess() : impl_(new Impl) {}
ChildProcess::~ChildProcess() = default;

// Deliberately NOT popen()/fork()+exec() — popen() forks, and fork()ing a
// multithreaded process is unsafe: if another thread holds a libc lock
// (malloc's arena lock, etc.) at the instant of fork(), the child
// inherits that lock permanently held with no thread left alive to
// release it, and can hang forever the next time it needs that lock —
// intermittently, depending on timing. That's invisible in a
// single-threaded program but became a real, hard-to-reproduce hang the
// moment loading/search/lyrics started running on background threads
// concurrently. posix_spawn() is specified to be safe to call from a
// multithreaded process (no full fork() semantics), which is why this
// uses it.
//
// Note there is no longer an `sh -c` in the middle: posix_spawnp takes the
// argv directly, which is both one fewer process and the reason no argument
// needs shell-quoting any more.
std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv,
                                                  bool merge_stderr) {
    if (argv.empty()) return nullptr;

    int out_pipe[2];
    if (pipe(out_pipe) != 0) return nullptr;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Every subprocess we spawn was inheriting our own stdin -- i.e. the
    // terminal, in raw mode, which is exactly what poll_key() reads
    // from. That's harmless for something like ffprobe that never
    // touches stdin, but ffmpeg specifically reads keystrokes for
    // interactive control (pause/quit/etc) whenever stdin is a real TTY,
    // and manipulates termios to do it -- when it exits it can leave the
    // terminal back in canonical/line-buffered mode instead of raw,
    // which turns our normally non-blocking key read into a blocking
    // one and stalls the whole render loop until a keypress+Enter
    // happens to satisfy it. Redirecting every child's stdin to
    // /dev/null here means no subprocess we spawn can ever see or touch
    // our terminal, regardless of what that program's stdin behavior is.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);           // child doesn't read
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    if (merge_stderr) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);           // close the now-duped fd

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);

    // posix_spawnp (not posix_spawn) resolves argv[0] through PATH instead of
    // requiring an absolute path -- Termux's filesystem lives under its own
    // prefix rather than the standard FHS layout.
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, raw[0], &actions, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]); // parent never writes

    if (rc != 0) {
        close(out_pipe[0]);
        return nullptr;
    }

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->pid = pid;
    child->impl_->read_fd = out_pipe[0];
    return child;
}

long long ChildProcess::read_stdout(void* buf, size_t count) {
    if (impl_->read_fd < 0) return -1;
    ssize_t n = ::read(impl_->read_fd, buf, count);
    return static_cast<long long>(n);
}

int ChildProcess::wait() {
    if (impl_->read_fd >= 0) { close(impl_->read_fd); impl_->read_fd = -1; }
    if (impl_->pid < 0) return -1;
    int status = 0;
    if (waitpid(impl_->pid, &status, 0) == impl_->pid && WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

#endif

// ===========================================================================
// Shared
// ===========================================================================

ProcResult run_capture(const std::vector<std::string>& argv, bool merge_stderr) {
    ProcResult result;
    std::string described = describe_argv(argv);

    auto child = ChildProcess::spawn(argv, merge_stderr);
    if (!child) {
        result.exit_code = -1;
        // Every subprocess mousiki runs goes through here, so this is the
        // single point that can log "==> cmd" plus the fact it never started.
        ConsoleLog::instance().log_command(described, "spawn failed", result.exit_code);
        return result;
    }

    std::array<char, 4096> buf{};
    std::ostringstream oss;
    for (;;) {
        long long n = child->read_stdout(buf.data(), buf.size());
        if (n <= 0) break;
        oss.write(buf.data(), static_cast<std::streamsize>(n));
    }
    result.out = oss.str();
    result.exit_code = child->wait();

    ConsoleLog::instance().log_command(described, result.out, result.exit_code);
    return result;
}

} // namespace muisc
