#include <clocale>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#include "app.h"

#ifdef _WIN32
#include "win_compat.h"
#endif

int main() {
#ifdef _WIN32
    // Must happen before anything reads getenv("HOME") -- Settings, the cache
    // manager and the console log all resolve their directories at first use.
    muisc::win_bootstrap_env();

    // The entire renderer is ANSI escape sequences. Without VT processing the
    // user would get screenfuls of literal "[38;5;250m" garbage and no way to
    // tell why, so refuse to start and say what to do instead.
    if (!muisc::win_console_init()) {
        std::fprintf(stderr,
                     "mousiki: this console does not support virtual terminal "
                     "sequences.\n"
                     "Run mousiki from Windows Terminal (wt.exe), or from any "
                     "host that\nsupports ENABLE_VIRTUAL_TERMINAL_PROCESSING.\n");
        muisc::win_console_restore();
        return 1;
    }
#endif

    if (!std::setlocale(LC_ALL, "")) {
        // "C.UTF-8" is a glibc spelling and is not a valid locale name for the
        // Windows CRT. The UCRT accepts a bare ".UTF-8" (VS2015+ / Win10 1803+)
        // to mean "current language, UTF-8 code page".
#ifdef _WIN32
        std::setlocale(LC_ALL, ".UTF-8");
#else
        std::setlocale(LC_ALL, "C.UTF-8");
#endif
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
#ifdef _WIN32
            std::setlocale(LC_ALL, ".UTF-8");
#else
            std::setlocale(LC_ALL, "C.UTF-8");
#endif
        }
    }

    // setlocale(LC_ALL, "") also adopts the OS numeric conventions. On any
    // comma-decimal locale (German, French, Spanish, Portuguese, ...) that
    // makes std::stod stop at the first '.', so ffprobe's "213.456" silently
    // becomes 213 and every lyric timestamp collapses -- with no exception and
    // no error to notice. Durations and timings are machine-readable data, not
    // user-facing text, so pin the numeric category back to C.
    std::setlocale(LC_NUMERIC, "C");

    int rc = 0;
    try {
        muisc::App app;
        rc = app.run();
    } catch (const std::exception& e) {
        // Without this, an exception escaping main on Windows terminates the
        // process through the CRT with no message at all -- just a silent
        // disappearance and an unrestored console.
        rc = 1;
#ifdef _WIN32
        muisc::win_console_restore();
#endif
        std::fprintf(stderr, "mousiki: fatal error: %s\n", e.what());
    } catch (...) {
        rc = 1;
#ifdef _WIN32
        muisc::win_console_restore();
#endif
        std::fprintf(stderr, "mousiki: fatal error: unknown exception\n");
    }

#ifdef _WIN32
    muisc::win_console_restore();
#endif
    return rc;
}
