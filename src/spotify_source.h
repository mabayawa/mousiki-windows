#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "online_source.h"

namespace muisc {

namespace fs = std::filesystem;

struct SpotifyPlaylist {
    std::string id;
    std::string name;
    std::string owner;
    int tracks = 0;
};

// Thin C++ face over scripts/spotify.py.
//
// The network work deliberately lives in Python: mousiki does no HTTP in C++
// anywhere, and adding it would mean WinHTTP on Windows plus libcurl on every
// other platform. These calls are user-initiated -- opening a playlist, typing
// a search -- not per-frame, so a subprocess per call costs nothing that
// matters, and it is the same pattern lyrics_fetcher.cpp already uses.
//
// Results come back as OnlineResult so they drop straight into the existing
// ListSource::Online view with no new UI. A Spotify result has an empty
// video_id and a populated spotify_uri; how that becomes audio is the caller's
// choice -- resolve it through YouTube, or hand the URI to librespot.
class SpotifySource {
public:
    SpotifySource() = default;

    // `client_id` empty disables the whole feature, which is the default:
    // config.txt ships SpotifyClientId blank because the ID belongs to
    // whoever runs mousiki, not to the repository.
    void configure(std::string client_id, fs::path script_path);

    bool enabled() const { return !client_id_.empty() && !script_.empty(); }

    // True once an OAuth token is cached and still refreshable. `detail`, if
    // given, receives a human-readable reason when it is not.
    bool logged_in(std::string* detail = nullptr) const;

    // Runs the interactive PKCE flow: opens a browser and blocks until the
    // user approves or it times out. Call from a background thread.
    bool login(std::string* detail = nullptr) const;

    std::vector<SpotifyPlaylist> playlists(std::string* error_out = nullptr) const;
    std::vector<OnlineResult> playlist_tracks(const std::string& playlist_id,
                                              std::string* error_out = nullptr) const;
    std::vector<OnlineResult> saved_tracks(std::string* error_out = nullptr) const;
    std::vector<OnlineResult> search(const std::string& query,
                                     std::string* error_out = nullptr) const;

private:
    // Builds `python spotify.py --client-id <id> <args...>` and parses the one
    // JSON object the script prints.
    bool call(const std::vector<std::string>& args, std::string& json_out,
              std::string* error_out) const;

    std::string client_id_;
    fs::path script_;
};

} // namespace muisc
