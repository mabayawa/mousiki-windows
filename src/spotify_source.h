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

// One row of the user's library: a playlist they created, a playlist they
// follow, or a saved album.
//
// One struct with a kind tag rather than two near-identical types, because a
// playlist and an album ARE the same row as far as browsing goes -- a named
// collection of tracks with an owner and a count -- and the only thing that
// differs is which helper subcommand fetches the contents.
struct SpotifyLibraryItem {
    enum class Kind { Playlist, Album };
    std::string id;
    std::string name;
    // display_name for a playlist, the joined artist names for an album. CAN be
    // empty: a real Spotify account can have a null display_name.
    std::string owner;
    // The owning user's id, and the ONLY reliable answer to "is this mine".
    // owner alone cannot do it -- display_name is nullable and not unique, so
    // comparing names would misclassify a followed playlist made by someone
    // sharing the user's display name. Always empty for an album, which is what
    // keeps an album from ever being classified as the user's own.
    std::string owner_id;
    std::string uri;
    int tracks = 0;
    Kind kind = Kind::Playlist;
    // Filled in by build_library_rows() (spotify_library.h), not by the parser:
    // it is a comparison against the signed-in profile, which the JSON layer
    // knows nothing about.
    bool mine = false;
};

// Flattened /me. `id` is what SpotifyLibraryItem::owner_id is compared against.
//
// `product` empty means UNKNOWN, exactly as product() below documents -- not
// "free". A token minted before user-read-private was requested omits it, and
// Spotify returns null for it on accounts that do have the scope.
struct SpotifyProfile {
    std::string id;
    std::string display_name;
    std::string product;
    std::string country;
    // The market the helper resolved for this token, or empty when it could not.
    // Empty is why tracks unavailable in the user region cannot be filtered out:
    // Spotify only reports is_playable when a market is supplied. It is empty for
    // exactly the same reason product is -- a token minted before
    // user-read-private was requested reports no country.
    std::string market;
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

    // One `status` round trip, flattened. The library browser uses this rather
    // than product() so opening it costs one /me call, not two.
    bool profile(SpotifyProfile& out, std::string* error_out = nullptr) const;

    std::vector<SpotifyLibraryItem> playlists(std::string* error_out = nullptr) const;
    std::vector<SpotifyLibraryItem> saved_albums(std::string* error_out = nullptr) const;
    std::vector<OnlineResult> playlist_tracks(const std::string& playlist_id,
                                              std::string* error_out = nullptr) const;
    // Reads /albums/{id}, which carries the album name AND its first page of
    // tracks, so this is one round trip rather than the two that
    // /albums/{id}/tracks would need (its simplified track objects have no
    // album field and no way to learn the name).
    std::vector<OnlineResult> album_tracks(const std::string& album_id,
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
    // Connect-device volume. mousiki's own gain does nothing in remote mode:
    // the audio never passes through Player at all, so the only way to change
    // what the user hears is to ask the device.
    bool set_volume(const std::string& device_id, int percent,
                    std::string* error_out = nullptr) const;
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
