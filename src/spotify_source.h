#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "online_source.h"

namespace muisc {

namespace fs = std::filesystem;

// One Spotify Connect endpoint as the Web API reports it.
struct SpotifyDevice {
    std::string id;
    std::string name;
    std::string type;    // "Computer", "Smartphone", "Speaker", ...
    bool active = false;
};

// Flattened /me/player. `playing == false` with an empty device_id is the
// normal "nothing is playing anywhere" answer (HTTP 204), not an error.
struct SpotifyPlaybackState {
    bool playing = false;
    std::string uri;
    std::string track_id;
    std::string device_id;
    std::string device_name;
    std::string device_type;
    long long progress_ms = 0;
    long long duration_ms = 0;
    int volume_percent = -1;
};

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

    // "premium", "free", or EMPTY when unknown. Empty is the normal answer for
    // a token minted before user-read-private was requested -- treat it as
    // "unknown" and let a 403 at play time decide, never as "not premium", or
    // an existing login is locked out of playback it is entitled to.
    std::string product(std::string* error_out = nullptr) const;

    // Returns the path to a usable librespot, fetching a pinned build if the
    // helper has one. Empty when there is none -- which is the normal answer
    // today, since no checksum is pinned yet and the helper refuses to install
    // an executable it cannot verify.
    fs::path ensure_librespot(std::string* error_out = nullptr) const;

    // --- transport. Each is one subprocess and one HTTPS round trip
    // (~200-600 ms), so none of these belong on the render thread. ---

    std::vector<SpotifyDevice> devices(std::string* error_out = nullptr) const;
    bool state(SpotifyPlaybackState& out, std::string* error_out = nullptr) const;

    // Naming more than one uri lets Spotify roll straight into the next track,
    // which is what makes the handover gapless. Restarts playback, so it is
    // only usable at the start of a track -- mid-track, use enqueue().
    bool play(const std::string& device_id, const std::vector<std::string>& uris,
              long long position_ms = -1, std::string* error_out = nullptr) const;
    // Appends WITHOUT disturbing what is playing: the gapless primitive.
    bool enqueue(const std::string& device_id, const std::string& uri,
                 std::string* error_out = nullptr) const;
    bool pause(const std::string& device_id, std::string* error_out = nullptr) const;
    bool resume(const std::string& device_id, std::string* error_out = nullptr) const;
    bool seek(const std::string& device_id, long long position_ms,
              std::string* error_out = nullptr) const;
    bool next(const std::string& device_id, std::string* error_out = nullptr) const;
    bool transfer(const std::string& device_id, bool start_playing,
                  std::string* error_out = nullptr) const;

private:
    // Builds `python spotify.py --client-id <id> <args...>` and parses the one
    // JSON object the script prints.
    bool call(const std::vector<std::string>& args, std::string& json_out,
              std::string* error_out) const;
    // call() plus the {"ok":true} check, for commands with no payload.
    bool call_ok(const std::vector<std::string>& args, std::string* error_out) const;

    std::string client_id_;
    fs::path script_;
};

} // namespace muisc
