#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace muisc {

struct ProcResult {
    std::string out;   // combined stdout (stderr redirected away unless merge_stderr=true)
    int exit_code = -1;
    bool ok() const { return exit_code == 0; }
};

// Arguments are passed as a vector, never as a shell command line.
//
// Upstream built a single string and ran it through `sh -c`, leaving every
// call site responsible for shell-quoting -- and two of them (the yt-dlp
// search paths in online_source.cpp and youtube_source.cpp) interpolated the
// user's raw search query, so typing a backtick or $( ) into the search box
// executed it. Windows forced this interface to change anyway, because there
// is no `sh`, and because CreateProcessW's quoting rules are not the shell's.
// Taking an argv removes the shell from the picture on every platform and
// makes that entire class of bug unrepresentable.
ProcResult run_capture(const std::vector<std::string>& argv, bool merge_stderr = false);

// A child whose stdout is read incrementally rather than to EOF -- the ffmpeg
// f32le PCM pipe in waveform.cpp, which must start feeding the player before
// the track has finished decoding. run_capture() is implemented on top of
// this, so there is one spawn implementation per platform rather than the two
// near-identical copies upstream carried.
class ChildProcess {
public:
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Returns nullptr if the process could not be started.
    //
    // `stderr_path`, when given, appends the child's stderr to that file. That
    // matters for a long-lived child whose diagnostics you actually need:
    // librespot logs to stderr, and merge_stderr would splice those log lines
    // straight into the raw PCM on stdout -- audible as clicks, and fatal to
    // counting frames. The alternative was stderr to NUL, i.e. no way at all to
    // see why an authentication attempt failed.
    static std::unique_ptr<ChildProcess> spawn(const std::vector<std::string>& argv,
                                               bool merge_stderr = false,
                                               const std::string& stderr_path = std::string());

    // Blocking read from the child's stdout. Returns the byte count, 0 at
    // end of stream, or -1 on error.
    long long read_stdout(void* buf, size_t count);

    // Bytes readable from stdout right now, without blocking. -1 on error.
    //
    // read_stdout() blocks, which is correct for a decoder that will always
    // produce more, but wrong when discarding a backlog: a paused Spotify writes
    // nothing at all, so a blocking drain would simply never return. This lets a
    // drain loop stop as soon as the stream goes quiet.
    long long bytes_available() const;

    // Closes the pipe and reaps the child. Returns its exit code, or -1.
    int wait();

    // Kills the child immediately. Safe to call from another thread while
    // this thread is blocked inside read_stdout().
    //
    // That cross-thread safety is the entire point: without it there is no
    // way to abandon a decode. Killing the child closes ITS end of the pipe,
    // which makes a blocked ReadFile/read() return end-of-stream, so the
    // reader unblocks on its own and the normal `read_stdout() == 0 -> wait()`
    // teardown runs unchanged. Nothing here closes a handle -- only the owning
    // thread does that, in wait() or the destructor -- so a reader can never
    // find the handle yanked out from under it mid-call.
    //
    // A terminated child always reports a non-zero exit status, so callers
    // that kill on purpose must check their own cancel flag before treating
    // that as a decode failure.
    void terminate();

private:
    ChildProcess();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// argv for invoking Python, or empty if no usable interpreter exists.
//
// Upstream hardcoded "python3", which is not how Python is named on Windows:
// the usual entry point is the `py` launcher, and a bare `python` may be the
// Microsoft Store alias stub that opens the Store instead of running anything.
// Shared because both the lyrics helper and the Spotify helper need it.
std::vector<std::string> python_argv();

// Renders an argv as a human-readable line for the console log. This is for
// display only -- it is never parsed or executed.
std::string describe_argv(const std::vector<std::string>& argv);

} // namespace muisc
