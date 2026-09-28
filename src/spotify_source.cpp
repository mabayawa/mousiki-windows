#include "spotify_source.h"

#include "console_log.h"
#include "path_utf8.h"
#include "process_util.h"
#include "spotify_parse.h"
#include "tiny_json.h"

namespace muisc {

using namespace tinyjson;

void SpotifySource::configure(std::string client_id, fs::path script_path) {
    client_id_ = std::move(client_id);
    script_ = std::move(script_path);
}

bool SpotifySource::call(const std::vector<std::string>& args, std::string& json_out,
                         std::string* error_out) const {
    auto set_err = [&](const std::string& m) { if (error_out) *error_out = m; };

    if (client_id_.empty()) {
        set_err("spotify: no SpotifyClientId set in config.txt");
        return false;
    }
    if (script_.empty()) {
        set_err("spotify: scripts/spotify.py not found");
        return false;
    }

    std::vector<std::string> argv = python_argv();
    if (argv.empty()) {
        set_err("spotify: no Python interpreter found");
        return false;
    }
    argv.push_back(path_utf8(script_));
    argv.push_back("--client-id");
    argv.push_back(client_id_);
    argv.insert(argv.end(), args.begin(), args.end());

    ProcResult r = run_capture(argv);
    if (r.out.empty()) {
        set_err(r.exit_code < 0 ? "spotify: helper failed to start"
                                : "spotify: helper produced no output");
        return false;
    }
    json_out = std::move(r.out);
    return true;
}

// Kept as a thin forwarder rather than replaced at every call site: the
// transport accessors below (devices/state/ensure_librespot) want the parsed
// root itself, not a mapped struct, and spotify_parse::envelope is exactly what
// this always was. The ensure_ascii=False note that used to live here moved with
// it -- see spotify_parse.h.
static bool parse_ok(const std::string& text, Value& root, std::string* error_out) {
    return spotify_parse::envelope(text, root, error_out);
}

bool SpotifySource::logged_in(std::string* detail) const {
    std::string text;
    if (!call({"status"}, text, detail)) return false;
    Value root;
    return parse_ok(text, root, detail);
}

bool SpotifySource::login(std::string* detail) const {
    std::string text;
    // The helper blocks here while a browser tab is open, so this must not run
    // on the render thread.
    if (!call({"login"}, text, detail)) return false;
    Value root;
    return parse_ok(text, root, detail);
}

bool SpotifySource::profile(SpotifyProfile& out, std::string* error_out) const {
    std::string text;
    if (!call({"status"}, text, error_out)) return false;
    return spotify_parse::profile(text, out, error_out);
}

std::vector<SpotifyLibraryItem> SpotifySource::playlists(std::string* error_out) const {
    std::string text;
    if (!call({"playlists"}, text, error_out)) return {};
    return spotify_parse::playlists(text, error_out);
}

std::vector<SpotifyLibraryItem> SpotifySource::saved_albums(std::string* error_out) const {
    std::string text;
    if (!call({"albums"}, text, error_out)) return {};
    return spotify_parse::albums(text, error_out);
}

std::vector<OnlineResult> SpotifySource::album_tracks(const std::string& album_id,
                                                     std::string* error_out) const {
    std::string text;
    if (!call({"album-tracks", album_id}, text, error_out)) return {};
    return spotify_parse::tracks(text, error_out);
}

std::vector<OnlineResult> SpotifySource::playlist_tracks(const std::string& playlist_id,
                                                         std::string* error_out) const {
    std::string text;
    if (!call({"tracks", playlist_id}, text, error_out)) return {};
    return spotify_parse::tracks(text, error_out);
}

std::vector<OnlineResult> SpotifySource::saved_tracks(std::string* error_out) const {
    std::string text;
    if (!call({"saved"}, text, error_out)) return {};
    return spotify_parse::tracks(text, error_out);
}

std::vector<OnlineResult> SpotifySource::search(const std::string& query,
                                                std::string* error_out) const {
    std::string text;
    if (!call({"search", query}, text, error_out)) return {};
    return spotify_parse::tracks(text, error_out);
}

std::string SpotifySource::product(std::string* error_out) const {
    std::string text;
    if (!call({"status"}, text, error_out)) return std::string();
    Value root;
    if (!parse_ok(text, root, error_out)) return std::string();
    if (auto* p = root.find("product")) return p->as_string();
    return std::string();
}

fs::path SpotifySource::ensure_librespot(std::string* error_out) const {
    // Deliberately bypasses the client-id check in call(): provisioning a binary
    // needs no account at all, and refusing it when SpotifyClientId is blank
    // would be a confusing way to fail.
    if (script_.empty()) {
        if (error_out) *error_out = "spotify: scripts/spotify.py not found";
        return {};
    }
    std::vector<std::string> argv = python_argv();
    if (argv.empty()) {
        if (error_out) *error_out = "spotify: no Python interpreter found";
        return {};
    }
    argv.push_back(path_utf8(script_));
    argv.push_back("ensure-librespot");

    ProcResult r = run_capture(argv);
    if (r.out.empty()) {
        if (error_out) *error_out = "spotify: librespot helper produced no output";
        return {};
    }
    Value root;
    if (!parse_ok(r.out, root, error_out)) return {};
    if (auto* p = root.find("path")) {
        const std::string path = p->as_string();
        if (!path.empty()) return path_from_utf8(path);
    }
    if (error_out) *error_out = "spotify: librespot helper returned no path";
    return {};
}

std::vector<SpotifyDevice> SpotifySource::devices(std::string* error_out) const {
    std::vector<SpotifyDevice> out;
    std::string text;
    if (!call({"devices"}, text, error_out)) return out;
    Value root;
    if (!parse_ok(text, root, error_out)) return out;
    if (auto* arr = root.find("devices")) {
        for (const auto& d : arr->arr) {
            SpotifyDevice dev;
            if (auto* p = d.find("id")) dev.id = p->as_string();
            if (auto* p = d.find("name")) dev.name = p->as_string();
            if (auto* p = d.find("type")) dev.type = p->as_string();
            if (auto* p = d.find("active")) dev.active = p->as_bool(false);
            if (!dev.id.empty()) out.push_back(std::move(dev));
        }
    }
    return out;
}

bool SpotifySource::state(SpotifyPlaybackState& out, std::string* error_out) const {
    std::string text;
    if (!call({"state"}, text, error_out)) return false;
    Value root;
    if (!parse_ok(text, root, error_out)) return false;
    if (auto* p = root.find("playing")) out.playing = p->as_bool(false);
    if (auto* p = root.find("uri")) out.uri = p->as_string();
    if (auto* p = root.find("track_id")) out.track_id = p->as_string();
    if (auto* p = root.find("device_id")) out.device_id = p->as_string();
    if (auto* p = root.find("device_name")) out.device_name = p->as_string();
    if (auto* p = root.find("device_type")) out.device_type = p->as_string();
    if (auto* p = root.find("progress_ms")) out.progress_ms = static_cast<long long>(p->as_number(0));
    if (auto* p = root.find("duration_ms")) out.duration_ms = static_cast<long long>(p->as_number(0));
    if (auto* p = root.find("volume_percent")) out.volume_percent = static_cast<int>(p->as_number(-1));
    return true;
}

// Every transport call lands here: run the subcommand, and treat a well-formed
// {"ok":true} as success. The helper already maps HTTP status onto the error
// codes, so there is nothing status-specific to repeat on this side.
bool SpotifySource::call_ok(const std::vector<std::string>& args, std::string* error_out) const {
    std::string text;
    if (!call(args, text, error_out)) return false;
    Value root;
    return parse_ok(text, root, error_out);
}

bool SpotifySource::play(const std::string& device_id, const std::vector<std::string>& uris,
                         long long position_ms, std::string* error_out) const {
    if (uris.empty()) {
        if (error_out) *error_out = "spotify: nothing to play";
        return false;
    }
    std::vector<std::string> args = {"play", device_id};
    args.insert(args.end(), uris.begin(), uris.end());
    if (position_ms >= 0) {
        args.push_back("--position-ms");
        args.push_back(std::to_string(position_ms));
    }
    return call_ok(args, error_out);
}

bool SpotifySource::enqueue(const std::string& device_id, const std::string& uri,
                            std::string* error_out) const {
    return call_ok({"queue", device_id, uri}, error_out);
}

bool SpotifySource::pause(const std::string& device_id, std::string* error_out) const {
    return call_ok({"pause", device_id}, error_out);
}

bool SpotifySource::resume(const std::string& device_id, std::string* error_out) const {
    return call_ok({"resume", device_id}, error_out);
}

bool SpotifySource::seek(const std::string& device_id, long long position_ms,
                         std::string* error_out) const {
    return call_ok({"seek", std::to_string(position_ms), device_id}, error_out);
}

bool SpotifySource::next(const std::string& device_id, std::string* error_out) const {
    return call_ok({"next", device_id}, error_out);
}

bool SpotifySource::transfer(const std::string& device_id, bool start_playing,
                             std::string* error_out) const {
    std::vector<std::string> args = {"transfer", device_id};
    if (start_playing) args.push_back("--play");
    return call_ok(args, error_out);
}

} // namespace muisc
