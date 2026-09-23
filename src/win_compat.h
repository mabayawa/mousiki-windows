#pragma once

// Win32 platform layer for mousiki.
//
// Upstream targets POSIX: termios for raw mode, ioctl(TIOCGWINSZ) for the
// terminal size, read(STDIN_FILENO) for keys, posix_spawnp for subprocesses,
// wcwidth for column widths, $HOME for every config/cache path. None of that
// exists on Windows, in either the MSVC CRT or MinGW-w64.
//
// Everything Win32 lives behind this header so the rest of the codebase keeps
// calling neutral names and stays readable on all three platforms. Nothing
// here is compiled off Windows -- the whole file is guarded -- so the Linux,
// macOS and Android builds are untouched.

#ifdef _WIN32

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace muisc {

// ---------------------------------------------------------------------------
// Console lifecycle
// ---------------------------------------------------------------------------

// Switches the console to UTF-8, enables VT escape processing, and installs a
// streambuf on std::cout that renders through WriteConsoleW. Returns false if
// the host cannot do VT sequences at all, in which case the caller should
// print a clear message and exit rather than spray escape codes at the user.
bool win_console_init();

// Leaves the alternate screen and restores the modes, code pages and stream
// buffer captured by win_console_init(). Safe to call more than once.
void win_console_restore();

// Raw mode: clears ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT. The POSIX side also
// clears ISIG; we deliberately keep ENABLE_PROCESSED_INPUT so Ctrl+C still
// arrives, because our console-control handler is what restores the terminal.
void win_raw_mode_enter();
void win_raw_mode_exit();

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Non-blocking. Returns 0 when nothing is waiting.
//
// Deliberately built on ReadConsoleInputW + INPUT_RECORD rather than
// ENABLE_VIRTUAL_TERMINAL_INPUT: VT input on Windows is known to drop modifier
// information, and INPUT_RECORD carries strictly more about each keypress.
// Arrow keys collapse to 'A'/'B'/'C'/'D' to match the escape-sequence mapping
// terminal_ui.cpp already hands to app.cpp, Escape returns 27, Backspace 127.
// Non-ASCII keys are converted to UTF-8 and returned one byte per call.
int win_poll_key();

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// Measured from srWindow (the visible window), not dwSize, which on Windows
// also counts the scrollback buffer and would report a wildly too-tall screen.
int win_term_rows();
int win_term_cols();

// ---------------------------------------------------------------------------
// Environment and paths
// ---------------------------------------------------------------------------

// Points HOME at %USERPROFILE% when it is not already set, so the ten or so
// existing getenv("HOME") call sites keep working unchanged.
void win_bootstrap_env();

std::string win_home_dir();            // %USERPROFILE%
std::string win_music_dir();           // the real Known Folder, not ~/Music
std::string win_executable_path();     // GetModuleFileNameW, UTF-8

// Resolves a bare name through PATH honouring PATHEXT, or validates an
// explicit path. Returns "" when not found.
std::string win_find_executable(const std::string& name);

// Cached argv for invoking Python. Tries the "py -3" launcher first, then
// python3, then python, rejecting the %LOCALAPPDATA%\Microsoft\WindowsApps
// stub that opens the Microsoft Store instead of running anything. Returns an
// empty vector when no interpreter is available. A vector rather than a string
// because the launcher needs its "-3" as a separate argv entry.
const std::vector<std::string>& win_python_argv();

std::string win_os_version();

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

std::wstring win_utf8_to_wide(const std::string& utf8);
std::string  win_wide_to_utf8(const std::wstring& wide);

// wcwidth replacement. Takes a full uint32_t codepoint rather than a wchar_t:
// wchar_t is 16 bits on Windows, so casting to it would silently truncate
// every codepoint above U+FFFF (emoji in track titles, musical symbols).
int win_codepoint_width(uint32_t cp);

// ---------------------------------------------------------------------------
// Misc shims
// ---------------------------------------------------------------------------

// localtime_r equivalent; MSVC's localtime_s takes its arguments the other
// way round, which is an easy silent mistake to make.
std::tm win_localtime(std::time_t t);

} // namespace muisc

#endif // _WIN32
