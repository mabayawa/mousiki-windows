#include "spotify_parse.h"

namespace muisc::spotify_parse {

using namespace tinyjson;

namespace {

// One row of /me/playlists or /me/albums. The two differ only in the array they
// arrive in and the kind they get, so the reader is shared.
//
// mine is deliberately NOT set here. Whether a row belongs to the signed-in
// user is a comparison against the profile, which the parser has no business
// knowing about -- build_library_rows() in spotify_library.cpp does it.
SpotifyLibraryItem item_from(const Value& p, SpotifyLibraryItem::Kind kind) {
    SpotifyLibraryItem it;
    it.kind = kind;
    if (auto* v = p.find("id")) it.id = v->as_string();
    if (auto* v = p.find("name")) it.name = v->as_string();
    // as_string() returns its default for a non-string, so a JSON null
    // display_name -- which real accounts do have -- lands as "" rather than
    // throwing or reading as the word "null".
    if (auto* v = p.find("owner")) it.owner = v->as_string();
    if (auto* v = p.find("owner_id")) it.owner_id = v->as_string();
    if (auto* v = p.find("uri")) it.uri = v->as_string();
    if (auto* v = p.find("tracks")) it.tracks = static_cast<int>(v->as_number(0));
    return it;
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

std::vector<SpotifyLibraryItem> collection(const std::string& text, const char* array_key,
                                           SpotifyLibraryItem::Kind kind,
                                           std::string* error_out) {
    std::vector<SpotifyLibraryItem> out;
    Value root;
    if (!envelope(text, root, error_out)) return out;
    if (auto* arr = root.find(array_key)) {
        for (const auto& p : arr->arr) {
            SpotifyLibraryItem it = item_from(p, kind);
            // No id means nothing can be fetched for this row, so it would be
            // an entry the cursor could land on and never open.
            if (!it.id.empty()) out.push_back(std::move(it));
        }
    }
    return out;
}

} // namespace

std::string error_line(const Value& root) {
    std::string err, detail;
    if (auto* p = root.find("error")) err = p->as_string();
    if (auto* p = root.find("detail")) detail = p->as_string();
    if (detail.empty()) return err.empty() ? "spotify: unknown error" : ("spotify: " + err);
    return "spotify: " + detail;
}

bool envelope(const std::string& text, Value& root, std::string* error_out) {
    if (!parse(text, root)) {
        if (error_out) *error_out = "spotify: helper returned unparseable JSON";
        return false;
    }
    auto* ok = root.find("ok");
    if (!ok || !ok->as_bool(false)) {
        if (error_out) *error_out = error_line(root);
        return false;
    }
    return true;
}

bool profile(const std::string& text, SpotifyProfile& out, std::string* error_out) {
    Value root;
    if (!envelope(text, root, error_out)) return false;
    if (auto* v = root.find("id")) out.id = v->as_string();
    if (auto* v = root.find("user")) out.display_name = v->as_string();
    // Stays empty when the helper sent null -- which it does for a token minted
    // before user-read-private was requested, and which real accounts return
    // too. Empty means UNKNOWN. Never render it as "free" and never gate
    // playback on it; let a 403 at play time decide.
    if (auto* v = root.find("product")) out.product = v->as_string();
    if (auto* v = root.find("country")) out.country = v->as_string();
    return true;
}

std::vector<SpotifyLibraryItem> playlists(const std::string& text, std::string* error_out) {
    return collection(text, "playlists", SpotifyLibraryItem::Kind::Playlist, error_out);
}

std::vector<SpotifyLibraryItem> albums(const std::string& text, std::string* error_out) {
    return collection(text, "albums", SpotifyLibraryItem::Kind::Album, error_out);
}

std::vector<OnlineResult> tracks(const std::string& text, std::string* error_out) {
    std::vector<OnlineResult> out;
    Value root;
    if (!envelope(text, root, error_out)) return out;
    if (auto* arr = root.find("tracks")) {
        for (const auto& t : arr->arr) {
            OnlineResult r = track_from(t);
            if (!r.spotify_uri.empty()) out.push_back(std::move(r));
        }
    }
    return out;
}

} // namespace muisc::spotify_parse
