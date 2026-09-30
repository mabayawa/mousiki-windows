#include "local_source.h"
#include "path_utf8.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#ifdef _WIN32
#include "win_compat.h"
#endif

namespace muisc {

namespace {

// Lowercase ASCII and nothing else.
//
// ::tolower applied byte-by-byte over a UTF-8 string is not case folding, it
// is corruption: under a non-C locale it will happily map individual bytes in
// the 0x80-0xFF range, rewriting one continuation byte of a multi-byte
// sequence and turning "Björk" into mojibake. Restricting the fold to A-Z
// leaves every non-ASCII byte exactly as it was, which is all this codebase
// needs for extension matching and substring search.
std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

bool LocalSource::is_audio_file(const fs::path& p) {
    static const std::set<std::string> exts = {".wav", ".mp3", ".opus", ".flac", ".ogg", ".m4a", ".aac", ".webm"};
    // Compared case-insensitively. ".MP3" and ".FLAC" are entirely ordinary on
    // Windows, whose filesystem does not distinguish case at all -- upstream's
    // exact match silently hid every such file from the library.
    return exts.count(ascii_lower(path_utf8(p.extension()))) > 0;
}

std::vector<LocalTrack> LocalSource::scan(const std::vector<std::string>& custom_paths) const {
    std::vector<LocalTrack> tracks;
    std::vector<fs::path> roots;

    if (!custom_paths.empty()) {
        for (const auto& cp : custom_paths) {
            roots.push_back(fs::path(cp));
        }
    } else {
#ifdef _WIN32
        // Ask the OS where Music actually is rather than assuming
        // %USERPROFILE%\Music: it is a Known Folder, and users routinely
        // relocate it to another drive or into OneDrive, in which case the
        // path under the profile does not exist at all. Any overlap with the
        // HOME-relative roots below is harmless -- the canonical-path dedupe
        // further down collapses it.
        roots.push_back(path_from_utf8(win_music_dir()));
#endif
        const char* home = std::getenv("HOME");
        if (home) {
            roots.push_back(fs::path(home) / "Music");
            roots.push_back(fs::path(home) / "disk" / "Music");
        }
    }

    if (roots.empty()) return tracks;

    // Dedupe by resolved (symlink-following) path — if one root is a
    // symlink that overlaps with the other (common on Android, e.g.
    // "disk" pointing into shared storage that also contains "Music"),
    // recursive_directory_iterator would otherwise walk and list the
    // exact same file twice, once per root.
    std::set<std::string> seen_canonical;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_regular_file() && is_audio_file(entry.path())) {
                std::error_code cec;
                fs::path canon = fs::canonical(entry.path(), cec);
                std::string key = cec ? path_utf8(entry.path()) : path_utf8(canon);
                if (!seen_canonical.insert(key).second) continue; // already listed via another root

                std::string folder = path_utf8(entry.path().parent_path().filename());
                if (folder.empty() || fs::path(folder) == root.filename()) folder = "-";
                tracks.push_back({path_utf8(entry.path().stem()), entry.path(), folder});
            }
        }
    }
    return tracks;
}

std::optional<LocalTrack> LocalSource::find(const std::string& query, const std::vector<std::string>& custom_paths) const {
    std::string needle = ascii_lower(query);

    for (const auto& track : scan(custom_paths)) {
        std::string hay = ascii_lower(track.title);
        if (hay.find(needle) != std::string::npos) return track;
    }
    return std::nullopt;
}

} // namespace muisc
