#include "cache_manager.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace muisc {

CacheManager::CacheManager() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path(".");
    cache_dir_ = base / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(cache_dir_, ec); // ignore failure, we surface it on first write instead
}

std::string CacheManager::sanitize(const std::string& raw) {
    // Upstream kept only [A-Za-z0-9] and dropped everything else. Applied
    // byte-by-byte to UTF-8 that is not merely lossy, it is destructive: every
    // byte of a CJK, Cyrillic, Greek or Hebrew title is non-alnum, so the name
    // collapses to the empty string and becomes "untitled" -- meaning every
    // non-Latin track shares a single cache filename and silently plays back
    // whichever one happened to be downloaded first.
    //
    // A filesystem forbids a short, specific list of ASCII characters; the
    // rest of Unicode is perfectly legal in a filename on both NTFS and ext4.
    // So strip exactly what is illegal and keep everything else.
    static const std::string kIllegal = "<>:\"/\\|?*";

    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) continue;                     // control characters
        if (c >= 0x80) { out += static_cast<char>(c); continue; } // UTF-8 lead/continuation
        if (kIllegal.find(static_cast<char>(c)) != std::string::npos) continue;
        if (c == ' ' || c == '-' || c == '_') { out += '_'; continue; }
        // ASCII-only fold; std::tolower is locale-dependent and must never see
        // a byte above 0x7F, which is why the non-ASCII case returned above.
        if (c >= 'A' && c <= 'Z') { out += static_cast<char>(c - 'A' + 'a'); continue; }
        out += static_cast<char>(c);
    }
    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }

    // Windows silently rejects a name ending in a dot or a space.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

    // Keep well clear of the 255-character component limit, and cut on a UTF-8
    // boundary so the name never ends mid-sequence.
    const size_t kMaxBytes = 180;
    if (out.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }

    if (out.empty()) out = "untitled";

    // CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are reserved device names on
    // Windows -- and reserved with any extension, so "con.opus" fails too.
    static const char* kReserved[] = {"con", "prn", "aux", "nul",
                                      "com1", "com2", "com3", "com4", "com5",
                                      "com6", "com7", "com8", "com9",
                                      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
                                      "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* reserved : kReserved) {
        if (out == reserved) { out.insert(out.begin(), '_'); break; }
    }
    return out;
}

fs::path CacheManager::path_for(const std::string& title, const std::string& ext) const {
    return cache_dir_ / (sanitize(title) + "." + ext);
}

bool CacheManager::is_cached(const std::string& title, const std::string& ext) const {
    std::error_code ec;
    auto p = path_for(title, ext);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}

} // namespace muisc
