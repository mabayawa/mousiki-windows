#include "spotify_source.h"

#include "console_log.h"
#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"

namespace muisc {

using namespace tinyjson;

namespace {

// Pulls {"ok":false,"error":...,"detail":...} out of a helper response and
// turns it into one line the status bar can show.
std::string error_line(const Value& root) {
    std::string err, detail;
    if (auto* p = root.find("error")) err = p->as_string();
    if (auto* p = root.find("detail")) detail = p->as_string();
    if (detail.empty()) return err.empty() ? "spotify: unknown error" : ("spotify: " + err);
    return "spotify: " + detail;
}

OnlineResult track_from(const Value& t) {
    OnlineResult r;
    if (auto* p = t.find("title")) r.title = p->as_string();
    if (auto* p = t.find("artist")) r.uploader = p->as_string();
    if (auto* p = t.find("duration_sec")) r.duration_sec = p->as_number(-1.0);
    if (auto* p = t.find("uri")) r.spotify_uri = p->as_string();
    // video_id stays empty on purpose: a Spotify track has no YouTube id
    // until something resolves one.
    return r;
}

} // namespace

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

// Parses the helper's response and hands back the root on success. Shared by
// every accessor below so the ok/error shape is only handled once.
static bool parse_ok(const std::string& text, Value& root, std::string* error_out) {
    if (!parse(text, root)) {
        if (error_out) *error_out = "spotify: helper returned unparseable JSON";
        return false;
    }
    // Note the helper prints with ensure_ascii=False, i.e. raw UTF-8 rather
    // than \uXXXX escapes. That is load-bearing: tiny_json.h does not decode
    // \u sequences, so an escaped response would silently mangle every
    // non-ASCII title.
    auto* ok = root.find("ok");
    if (!ok || !ok->as_bool(false)) {
        if (error_out) *error_out = error_line(root);
        return false;
    }
    return true;
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

std::vector<SpotifyPlaylist> SpotifySource::playlists(std::string* error_out) const {
    std::vector<SpotifyPlaylist> out;
    std::string text;
    if (!call({"playlists"}, text, error_out)) return out;
    Value root;
    if (!parse_ok(text, root, error_out)) return out;

    if (auto* arr = root.find("playlists")) {
        for (const auto& p : arr->arr) {
            SpotifyPlaylist pl;
            if (auto* v = p.find("id")) pl.id = v->as_string();
            if (auto* v = p.find("name")) pl.name = v->as_string();
            if (auto* v = p.find("owner")) pl.owner = v->as_string();
            if (auto* v = p.find("tracks")) pl.tracks = static_cast<int>(v->as_number(0));
            if (!pl.id.empty()) out.push_back(std::move(pl));
        }
    }
    return out;
}

std::vector<OnlineResult> SpotifySource::playlist_tracks(const std::string& playlist_id,
                                                         std::string* error_out) const {
    std::vector<OnlineResult> out;
    std::string text;
    if (!call({"tracks", playlist_id}, text, error_out)) return out;
    Value root;
    if (!parse_ok(text, root, error_out)) return out;
    if (auto* arr = root.find("tracks")) {
        for (const auto& t : arr->arr) {
            OnlineResult r = track_from(t);
            if (!r.spotify_uri.empty()) out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<OnlineResult> SpotifySource::saved_tracks(std::string* error_out) const {
    std::vector<OnlineResult> out;
    std::string text;
    if (!call({"saved"}, text, error_out)) return out;
    Value root;
    if (!parse_ok(text, root, error_out)) return out;
    if (auto* arr = root.find("tracks")) {
        for (const auto& t : arr->arr) {
            OnlineResult r = track_from(t);
            if (!r.spotify_uri.empty()) out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<OnlineResult> SpotifySource::search(const std::string& query,
                                                std::string* error_out) const {
    std::vector<OnlineResult> out;
    std::string text;
    if (!call({"search", query}, text, error_out)) return out;
    Value root;
    if (!parse_ok(text, root, error_out)) return out;
    if (auto* arr = root.find("tracks")) {
        for (const auto& t : arr->arr) {
            OnlineResult r = track_from(t);
            if (!r.spotify_uri.empty()) out.push_back(std::move(r));
        }
    }
    return out;
}

} // namespace muisc
