#pragma once

// UTF-8 <-> std::filesystem::path conversion.
//
// This exists because of one specific MSVC behaviour: std::filesystem::path
// stores wchar_t on Windows, and path::string() converts it through the
// process ANSI code page. For any character that code page cannot represent --
// Cyrillic, CJK, and plenty of accented Latin depending on the locale -- that
// conversion does not produce mojibake, it *throws* std::system_error. A music
// library containing a single such filename therefore crashes the scan rather
// than merely mis-rendering it.
//
// Every fs::path -> std::string conversion in the codebase must go through
// path_utf8() instead of .string(), and every std::string -> fs::path through
// path_from_utf8(). On POSIX both are the identity, so the Linux, macOS and
// Android builds keep exactly the behaviour they had.

#include <filesystem>
#include <string>

#ifdef _WIN32
#include "win_compat.h"
#endif

namespace muisc {

inline std::string path_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
    return win_wide_to_utf8(p.wstring());
#else
    return p.string();
#endif
}

inline std::filesystem::path path_from_utf8(const std::string& s) {
#ifdef _WIN32
    return std::filesystem::path(win_utf8_to_wide(s));
#else
    return std::filesystem::path(s);
#endif
}

} // namespace muisc
