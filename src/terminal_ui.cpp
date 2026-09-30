#include "terminal_ui.h"
#include "indic_handler.h"
#include "utf8_util.h"
#include <algorithm>
#include <cstdint>
#include <iostream>

#ifdef _WIN32
#include "win_compat.h"
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <cwchar>
#endif

namespace muisc {

#ifndef _WIN32
static struct termios g_orig_termios;
#endif

TerminalIO::TerminalIO() {
#ifdef _WIN32
    // win_console_init() has already run from main() -- it has to, because the
    // VT capability check decides whether the program can start at all. All
    // that is left here is entering raw mode.
    win_raw_mode_enter();
#else
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
    raw_mode_active_ = true;
    // Alternate screen buffer: the terminal keeps a second, separate
    // grid (same dimensions as the visible one) while this is active.
    // "\x1b[H" homing the cursor and any accidental scroll it causes
    // both happen *within* that private grid, never touching the user's
    // actual shell scrollback -- and switching back with "\x1b[?1049l"
    // on exit restores exactly whatever was on screen before mousiki
    // started, cleanly, rather than leaving a trail of overwritten
    // frames behind in their scrollback history.
    //
    // To be clear about what this does and doesn't fix: it does NOT by
    // itself prevent the frame-height-exceeds-terminal-rows scrolling
    // bug (the alt-screen grid is still exactly term.rows() tall, and
    // still scrolls internally if you print past its bottom with no
    // clear) -- that's what term_rows_ and clamp_output_rows() in
    // app.cpp actually fix. This is a separate, complementary
    // improvement: even if some future change reintroduces a height
    // miscalculation, the damage is contained to mousiki's own private
    // buffer instead of polluting the terminal the person is actually
    // going to keep using afterwards.
    //
    // Windows Terminal, and conhost since Windows 10 1511, implement
    // ?1049 and ?25 natively once ENABLE_VIRTUAL_TERMINAL_PROCESSING is
    // set, so this line is identical on every platform.
    std::cout << "\x1b[?1049h" << "\x1b[?25l" << std::flush; // enter alt-screen, hide cursor
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
#ifdef _WIN32
        win_raw_mode_exit();
        std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush;
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush; // show cursor, leave alt-screen
#endif
        raw_mode_active_ = false;
    }
}

// Subprocesses we spawn are supposed to never touch our stdin at all
// (see process_util.cpp's run_capture() and waveform.cpp's ffmpeg
// fallback — both redirect the child's stdin to /dev/null specifically
// because of this). But that fix lives in the spawn call sites, and
// this is the one place that actually NEEDS raw+non-blocking mode to
// keep working no matter what: re-applying our own termios settings on
// every poll is cheap (one syscall, ~25x/sec) and means that even if
// something unexpected resets the terminal to canonical/line-buffered
// mode, we're never more than one frame away from correcting it,
// instead of the read() call silently becoming blocking and stalling
// the entire render loop until a keypress+Enter happens to satisfy it.
//
// The same reasoning holds on Windows: a child that is handed our
// console can call SetConsoleMode on it. CREATE_NO_WINDOW plus a NUL
// stdin in process_util.cpp is the real fix, and this is the cheap
// belt-and-braces backstop.
void TerminalIO::reassert_raw_mode() {
    if (!raw_mode_active_) return;
#ifdef _WIN32
    win_raw_mode_enter();
#else
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
}

int TerminalIO::poll_key() {
    reassert_raw_mode();
#ifdef _WIN32
    // Win32 has no escape sequences to decode: the console reports keys as
    // INPUT_RECORDs, and win_poll_key() maps the arrows onto the same
    // 'A'/'B'/'C'/'D' this function returns on POSIX.
    return win_poll_key();
#else
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;

    if (c == '\x1b') {
        unsigned char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'A';
                case 'B': return 'B';
                case 'C': return 'C';
                case 'D': return 'D';
            }
        }
        return 27;
    }
    return c;
#endif
}

int TerminalIO::rows() const {
#ifdef _WIN32
    return win_term_rows();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 40;
#endif
}

int TerminalIO::cols() const {
#ifdef _WIN32
    return win_term_cols();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 155;
#endif
}

static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (is_indic_codepoint(cp)) return indic_codepoint_width(cp);
#ifdef _WIN32
    // Neither the MSVC CRT nor MinGW-w64 provides wcwidth, so win_compat
    // carries its own table. It takes a uint32_t rather than a wchar_t on
    // purpose: wchar_t is 16 bits here, and the POSIX line below silently
    // truncates every codepoint above U+FFFF -- emoji in track titles,
    // musical symbols -- to whatever the low half happens to be.
    int w = win_codepoint_width(cp);
#else
    int w = wcwidth(static_cast<wchar_t>(cp));
#endif
    return w < 0 ? 0 : w;
}
// w
// int display_width(const std::string& s) {
//     std::string clean = sanitize_lyric_text(s);
//
//     int cols = 0;
//     size_t i = 0;
//     uint32_t prev_cp = 0;
//
//     while (i < clean.size()) {
//         uint32_t cp = utf8_decode(clean, i);
//         int w = codepoint_width(cp);

int display_width(const std::string& s) {
    int cols = 0;
    size_t i = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        // Virama conjunct subtraction: if previous char was a Virama and current is a consonant (width 1),
        // they form a conjunct ligature that fits in the same cell. Subtract 1 to compensate.
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D ||
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD ||
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    cols -= 1; // Merge with previous cell
                }
            }
        }

        cols += w;
        prev_cp = cp;
    }
    return cols;
}

std::string utf8_take(const std::string& s, int width) {
    std::string out;
    size_t i = 0;
    int used = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);

        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D ||
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD ||
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    used -= 1; // Merge with previous cell
                }
            }
        }

        if (used + w > width) {
            if (w == 0) {
                out += s.substr(start, i - start);
                used += w;
                prev_cp = cp;
                continue;
            }
            break;
        }
        out += s.substr(start, i - start);
        used += w;
        prev_cp = cp;
    }
    return out;
}

std::string pad_right(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return s + std::string(width - w, ' ');
}

std::string pad_left(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return std::string(width - w, ' ') + s;
}

std::string truncate_str(const std::string& s, int width) {
    if (width <= 0) return "";
    int w = display_width(s);
    if (w <= width) return s;
    if (width <= 3) return utf8_take(s, width);
    return utf8_take(s, width - 3) + "...";
}

} // namespace muisc
