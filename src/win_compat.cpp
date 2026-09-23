#include "win_compat.h"

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

// Older MinGW-w64 headers predate this flag even at a modern API level.
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <iostream>
#include <iterator>
#include <streambuf>

namespace muisc {
namespace {

// ---------------------------------------------------------------------------
// Console output
// ---------------------------------------------------------------------------

// SetConsoleOutputCP(CP_UTF8) alone is not enough. Whether raw UTF-8 bytes
// written through the CRT survive depends on the console host, the active font
// and how the CRT decides to translate them, and it degrades silently to
// mojibake for non-Latin track titles. Converting to UTF-16 ourselves and
// going straight to WriteConsoleW takes both the CRT and the code page out of
// the path, so what lands on screen is exactly what we composed.
class Utf8ConsoleStreambuf : public std::streambuf {
public:
    explicit Utf8ConsoleStreambuf(HANDLE h) : handle_(h) {
        DWORD mode = 0;
        is_console_ = (GetConsoleMode(h, &mode) != 0);
    }

protected:
    int_type overflow(int_type c) override {
        if (c != traits_type::eof()) pending_.push_back(static_cast<char>(c));
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (n > 0) pending_.append(s, static_cast<size_t>(n));
        return n;
    }

    int sync() override { return flush_pending(); }

private:
    // Length of the longest prefix of `s` ending on a UTF-8 character
    // boundary. A frame can in principle be flushed mid-sequence; holding the
    // tail back until the rest arrives avoids emitting a lone continuation
    // byte, which would render as U+FFFD.
    static size_t complete_prefix(const std::string& s) {
        if (s.empty()) return 0;
        size_t i = s.size();
        size_t scanned = 0;
        while (i > 0 && scanned < 4) {
            unsigned char c = static_cast<unsigned char>(s[i - 1]);
            if ((c & 0xC0) != 0x80) {          // lead byte, or plain ASCII
                size_t need = 1;
                if ((c & 0x80) == 0x00)      need = 1;
                else if ((c & 0xE0) == 0xC0) need = 2;
                else if ((c & 0xF0) == 0xE0) need = 3;
                else if ((c & 0xF8) == 0xF0) need = 4;
                size_t have = s.size() - (i - 1);
                return (have >= need) ? s.size() : (i - 1);
            }
            --i;
            ++scanned;
        }
        return s.size();
    }

    int flush_pending() {
        if (pending_.empty()) return 0;

        size_t take = complete_prefix(pending_);
        if (take == 0) return 0;
        std::string chunk = pending_.substr(0, take);
        pending_.erase(0, take);

        if (!is_console_) {
            // stdout is a file or a pipe: emit the UTF-8 bytes untouched.
            DWORD written = 0;
            WriteFile(handle_, chunk.data(), static_cast<DWORD>(chunk.size()),
                      &written, nullptr);
            return 0;
        }

        int wlen = MultiByteToWideChar(CP_UTF8, 0, chunk.data(),
                                       static_cast<int>(chunk.size()), nullptr, 0);
        if (wlen <= 0) return 0;
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, chunk.data(),
                            static_cast<int>(chunk.size()), &wide[0], wlen);

        // WriteConsoleW can fail on very large single writes; chunk it.
        const size_t kMaxChunk = 8192;
        size_t off = 0;
        while (off < wide.size()) {
            DWORD n = static_cast<DWORD>((std::min)(kMaxChunk, wide.size() - off));
            DWORD written = 0;
            if (!WriteConsoleW(handle_, wide.data() + off, n, &written, nullptr)) break;
            if (written == 0) break;
            off += written;
        }
        return 0;
    }

    HANDLE handle_;
    bool is_console_ = false;
    std::string pending_;
};

struct ConsoleState {
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE in  = INVALID_HANDLE_VALUE;
    DWORD orig_out_mode = 0;
    DWORD orig_in_mode  = 0;
    UINT  orig_out_cp   = 0;
    UINT  orig_in_cp    = 0;
    bool have_out_mode = false;
    bool have_in_mode  = false;
    bool vt_enabled    = false;
    bool initialised   = false;
    bool restored      = false;
};

ConsoleState   g_con;
Utf8ConsoleStreambuf* g_cout_buf  = nullptr;
std::streambuf*       g_cout_orig = nullptr;

// Upstream installs no signal handling at all, so Ctrl+C kills the process
// with the alternate screen still active and the cursor still hidden, leaving
// the user in a shell that looks broken. Windows gives a clean hook for this,
// so the port fixes it rather than faithfully reproducing the bug.
BOOL WINAPI ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            win_console_restore();
            return FALSE;   // let the default handler terminate us
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Pending input bytes
// ---------------------------------------------------------------------------

// poll_key() returns one byte per call because that is the shape of the POSIX
// read() it replaces; a non-ASCII keypress becomes several UTF-8 bytes, so the
// tail waits here for subsequent calls.
std::deque<unsigned char> g_pending_bytes;
wchar_t g_pending_high_surrogate = 0;

// ---------------------------------------------------------------------------
// Character width tables
// ---------------------------------------------------------------------------

struct Range { uint32_t lo, hi; };

// Combining marks, format characters and variation selectors: zero columns.
// Indic scripts are resolved by indic_handler.cpp before this table is
// consulted, so they are deliberately absent here.
const Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x08FF},
    {0x135D, 0x135F}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x206A, 0x206F}, {0x20D0, 0x20F0},
    {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F}, {0x2DE0, 0x2DFF}, {0x302A, 0x302D},
    {0x3099, 0x309A}, {0xA66F, 0xA672}, {0xA674, 0xA67D}, {0xA69E, 0xA69F},
    {0xA6F0, 0xA6F1}, {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B},
    {0xA825, 0xA826}, {0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x101FD, 0x101FD},
    {0x1D167, 0x1D169}, {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B},
    {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244},
    {0xE0001, 0xE0001}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
};

// East Asian Wide / Fullwidth, plus the emoji blocks terminals render at two
// columns.
const Range kWide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
    {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF},
    {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6},
    {0x16FE0, 0x16FE4}, {0x17000, 0x187F7}, {0x18800, 0x18CD5},
    {0x1B000, 0x1B152}, {0x1B164, 0x1B167}, {0x1B170, 0x1B2FB},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F2FF}, {0x1F300, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F900, 0x1F9FF},
    {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

template <size_t N>
bool in_ranges(uint32_t cp, const Range (&table)[N]) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].lo)      hi = mid;
        else if (cp > table[mid].hi) lo = mid + 1;
        else                         return true;
    }
    return false;
}

std::vector<std::string> g_python_argv;
bool g_python_resolved = false;

} // namespace

// ---------------------------------------------------------------------------
// Text conversion
// ---------------------------------------------------------------------------

std::wstring win_utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        &out[0], len);
    return out;
}

std::string win_wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                  static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        &out[0], len, nullptr, nullptr);
    return out;
}

int win_codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return -1;   // control
    if (in_ranges(cp, kZeroWidth)) return 0;
    if (in_ranges(cp, kWide)) return 2;
    return 1;
}

// ---------------------------------------------------------------------------
// Console lifecycle
// ---------------------------------------------------------------------------

bool win_console_init() {
    if (g_con.initialised) return g_con.vt_enabled;

    g_con.out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_con.in  = GetStdHandle(STD_INPUT_HANDLE);

    g_con.orig_out_cp = GetConsoleOutputCP();
    g_con.orig_in_cp  = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (GetConsoleMode(g_con.out, &g_con.orig_out_mode)) {
        g_con.have_out_mode = true;
        // ENABLE_PROCESSED_OUTPUT keeps \n, \r and \b behaving normally.
        // DISABLE_NEWLINE_AUTO_RETURN is deliberately NOT set: the renderer is
        // POSIX-shaped and emits a bare \n expecting the implicit carriage
        // return a Unix terminal provides.
        DWORD mode = g_con.orig_out_mode | ENABLE_PROCESSED_OUTPUT |
                     ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        g_con.vt_enabled = (SetConsoleMode(g_con.out, mode) != 0);
    } else {
        // Not a console at all (redirected to a file or pipe).
        g_con.vt_enabled = false;
    }

    if (GetConsoleMode(g_con.in, &g_con.orig_in_mode)) g_con.have_in_mode = true;

    g_cout_buf  = new Utf8ConsoleStreambuf(g_con.out);
    g_cout_orig = std::cout.rdbuf(g_cout_buf);

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    g_con.initialised = true;
    g_con.restored    = false;
    return g_con.vt_enabled;
}

void win_console_restore() {
    if (!g_con.initialised || g_con.restored) return;
    g_con.restored = true;

    // Show the cursor and leave the alternate screen while VT is still on.
    // Doing it after disabling VT processing would print the escape sequences
    // as literal text into the user's shell.
    if (g_con.vt_enabled) {
        const wchar_t* tail = L"\x1b[?25h\x1b[?1049l";
        DWORD written = 0;
        WriteConsoleW(g_con.out, tail, static_cast<DWORD>(wcslen(tail)),
                      &written, nullptr);
    }

    if (g_cout_orig) {
        std::cout.rdbuf(g_cout_orig);
        g_cout_orig = nullptr;
    }
    delete g_cout_buf;
    g_cout_buf = nullptr;

    if (g_con.have_in_mode)  SetConsoleMode(g_con.in,  g_con.orig_in_mode);
    if (g_con.have_out_mode) SetConsoleMode(g_con.out, g_con.orig_out_mode);
    if (g_con.orig_out_cp)   SetConsoleOutputCP(g_con.orig_out_cp);
    if (g_con.orig_in_cp)    SetConsoleCP(g_con.orig_in_cp);
}

void win_raw_mode_enter() {
    if (!g_con.have_in_mode) return;
    DWORD mode = g_con.orig_in_mode;
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    // Keep ENABLE_PROCESSED_INPUT so Ctrl+C still reaches ctrl_handler(),
    // which is what restores the terminal before we die.
    mode |= ENABLE_PROCESSED_INPUT;
    // Resize is polled every frame like upstream does, so window records would
    // only be noise in the input queue.
    mode &= ~ENABLE_WINDOW_INPUT;
    SetConsoleMode(g_con.in, mode);
}

void win_raw_mode_exit() {
    if (!g_con.have_in_mode) return;
    SetConsoleMode(g_con.in, g_con.orig_in_mode);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

int win_poll_key() {
    if (!g_pending_bytes.empty()) {
        int b = g_pending_bytes.front();
        g_pending_bytes.pop_front();
        return b;
    }
    if (g_con.in == INVALID_HANDLE_VALUE) return 0;

    for (;;) {
        DWORD available = 0;
        if (!GetNumberOfConsoleInputEvents(g_con.in, &available) || available == 0)
            return 0;

        INPUT_RECORD rec;
        DWORD got = 0;
        if (!ReadConsoleInputW(g_con.in, &rec, 1, &got) || got == 0) return 0;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        const KEY_EVENT_RECORD& k = rec.Event.KeyEvent;

        // Arrow keys collapse onto 'A'..'D' because that is exactly what the
        // POSIX escape-sequence reader hands to app.cpp's handle_key().
        switch (k.wVirtualKeyCode) {
            case VK_UP:     return 'A';
            case VK_DOWN:   return 'B';
            case VK_RIGHT:  return 'C';
            case VK_LEFT:   return 'D';
            case VK_ESCAPE: return 27;
            case VK_RETURN: return '\r';
            case VK_TAB:    return '\t';
            case VK_BACK:   return 127;   // POSIX sends DEL for backspace
            default: break;
        }

        wchar_t wc = k.uChar.UnicodeChar;
        if (wc == 0) continue;            // a modifier or an unmapped key

        // Anything outside the BMP arrives as a surrogate pair across two
        // records; hold the high half until its partner shows up.
        if (wc >= 0xD800 && wc <= 0xDBFF) {
            g_pending_high_surrogate = wc;
            continue;
        }
        std::wstring ws;
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            if (g_pending_high_surrogate == 0) continue;
            ws.push_back(g_pending_high_surrogate);
            ws.push_back(wc);
            g_pending_high_surrogate = 0;
        } else {
            g_pending_high_surrogate = 0;
            ws.push_back(wc);
        }

        std::string utf8 = win_wide_to_utf8(ws);
        if (utf8.empty()) continue;
        for (size_t i = 1; i < utf8.size(); ++i)
            g_pending_bytes.push_back(static_cast<unsigned char>(utf8[i]));
        return static_cast<unsigned char>(utf8[0]);
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

int win_term_rows() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_con.out, &info)) {
        int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (rows > 0) return rows;
    }
    return 40;    // same fallback as the POSIX path
}

int win_term_cols() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(g_con.out, &info)) {
        int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (cols > 0) return cols;
    }
    return 155;
}

// ---------------------------------------------------------------------------
// Environment and paths
// ---------------------------------------------------------------------------

static std::string known_folder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw)
        out = win_wide_to_utf8(raw);
    if (raw) CoTaskMemFree(raw);
    return out;
}

std::string win_home_dir() {
    std::string h = known_folder(FOLDERID_Profile);
    if (!h.empty()) return h;
    if (const char* e = std::getenv("USERPROFILE")) return e;
    if (const char* e = std::getenv("HOME")) return e;
    return ".";
}

std::string win_music_dir() {
    std::string m = known_folder(FOLDERID_Music);
    if (!m.empty()) return m;
    return win_home_dir() + "\\Music";
}

void win_bootstrap_env() {
    // Ten or so call sites across settings.cpp, cache_manager.cpp,
    // console_log.cpp, snapshot.cpp, local_source.cpp and app.cpp read
    // getenv("HOME") directly. Rather than touch each one, give Windows a HOME
    // that means the same thing.
    const char* existing = std::getenv("HOME");
    if (!existing || !*existing) {
        std::string home = win_home_dir();
        if (!home.empty()) _putenv_s("HOME", home.c_str());
    }
}

std::string win_executable_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);   // truncated: grow and retry
    }
    return win_wide_to_utf8(buf);
}

std::string win_find_executable(const std::string& name) {
    if (name.empty()) return std::string();
    std::wstring wname = win_utf8_to_wide(name);

    bool explicit_path = name.find('\\') != std::string::npos ||
                         name.find('/')  != std::string::npos ||
                         (name.size() > 1 && name[1] == ':');

    auto has_extension = [&]() {
        size_t dot   = name.find_last_of('.');
        size_t slash = name.find_last_of("\\/");
        return dot != std::string::npos && (slash == std::string::npos || dot > slash);
    };

    if (explicit_path) {
        DWORD attrs = GetFileAttributesW(wname.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return name;
        if (has_extension()) return std::string();
    }

    auto search = [](const std::wstring& candidate) -> std::string {
        wchar_t found[MAX_PATH * 2];
        DWORD n = SearchPathW(nullptr, candidate.c_str(), nullptr,
                              static_cast<DWORD>(std::size(found)), found, nullptr);
        if (n > 0 && n < std::size(found))
            return win_wide_to_utf8(std::wstring(found, n));
        return std::string();
    };

    if (has_extension()) return search(wname);

    // No extension given: walk PATHEXT the way the shell would, so "yt-dlp"
    // finds yt-dlp.exe and, if that is how it was installed, yt-dlp.cmd.
    std::wstring pathext(1024, L'\0');
    DWORD len = GetEnvironmentVariableW(L"PATHEXT", &pathext[0],
                                        static_cast<DWORD>(pathext.size()));
    if (len == 0 || len >= pathext.size()) pathext = L".COM;.EXE;.BAT;.CMD";
    else pathext.resize(len);

    size_t start = 0;
    while (start <= pathext.size()) {
        size_t sep = pathext.find(L';', start);
        std::wstring ext = pathext.substr(
            start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (!ext.empty()) {
            std::string hit = search(wname + ext);
            if (!hit.empty()) return hit;
        }
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return std::string();
}

const std::vector<std::string>& win_python_argv() {
    if (g_python_resolved) return g_python_argv;
    g_python_resolved = true;

    // The py launcher is the most reliable entry point on Windows: it exists
    // independently of which Python is first on PATH, and "-3" pins the major
    // version.
    std::string py = win_find_executable("py");
    if (!py.empty()) {
        g_python_argv = {py, "-3"};
        return g_python_argv;
    }

    for (const char* cand : {"python3", "python"}) {
        std::string p = win_find_executable(cand);
        if (p.empty()) continue;
        // %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe is a zero-byte app
        // execution alias that opens the Microsoft Store instead of running
        // Python. Treating it as an interpreter produces a baffling failure.
        if (p.find("\\WindowsApps\\") != std::string::npos ||
            p.find("/WindowsApps/")  != std::string::npos) continue;
        g_python_argv = {p};
        return g_python_argv;
    }
    return g_python_argv;   // empty: no interpreter available
}

std::string win_os_version() {
    // Replaces uname(). GetVersionEx is deprecated and lies without a
    // manifest, so ask ntdll for the real build number.
    OSVERSIONINFOEXW osv{};
    osv.dwOSVersionInfoSize = sizeof(osv);
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOEXW);
    std::string ver = "Windows";
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (fn && fn(reinterpret_cast<PRTL_OSVERSIONINFOEXW>(&osv)) == 0) {
            ver += " " + std::to_string(osv.dwMajorVersion) + "." +
                   std::to_string(osv.dwMinorVersion) + "." +
                   std::to_string(osv.dwBuildNumber);
        }
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: ver += " x86_64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: ver += " arm64";  break;
        case PROCESSOR_ARCHITECTURE_INTEL: ver += " x86";    break;
        default: break;
    }
    return ver;
}

// ---------------------------------------------------------------------------
// Misc shims
// ---------------------------------------------------------------------------

std::tm win_localtime(std::time_t t) {
    std::tm out{};
#if defined(_MSC_VER)
    localtime_s(&out, &t);   // note: arguments are the reverse of localtime_r
#else
    // MinGW-w64 exposes the POSIX spelling rather than the MSVC one.
    localtime_r(&t, &out);
#endif
    return out;
}

} // namespace muisc

#endif // _WIN32
