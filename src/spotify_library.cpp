#include "spotify_library.h"

namespace muisc {

std::vector<SpotifyLibraryItem> build_library_rows(const std::string& me_id,
                                                   const std::vector<SpotifyLibraryItem>& playlists,
                                                   const std::vector<SpotifyLibraryItem>& albums) {
    std::vector<SpotifyLibraryItem> out;
    out.reserve(playlists.size() + albums.size());

    // Two passes over `playlists` rather than one pass plus a sort: a sort would
    // need a comparator that keeps the original order within each group, and
    // "append the matches, then append the rest" is that, without the
    // stable_sort subtlety.
    //
    // An empty me_id matches nothing. Without this guard it would match every
    // playlist whose owner_id the helper could not read, and every album, since
    // both are "".
    const bool can_classify = !me_id.empty();
    for (const auto& p : playlists) {
        if (can_classify && p.owner_id == me_id) {
            SpotifyLibraryItem it = p;
            it.mine = true;
            out.push_back(std::move(it));
        }
    }
    for (const auto& p : playlists) {
        if (!(can_classify && p.owner_id == me_id)) {
            SpotifyLibraryItem it = p;
            it.mine = false;
            out.push_back(std::move(it));
        }
    }
    for (const auto& a : albums) {
        SpotifyLibraryItem it = a;
        // Never mine, whatever me_id is: an album has no owning user, so its
        // owner column holds artist names and its owner_id is deliberately
        // empty (see cmd_albums in scripts/spotify.py).
        it.mine = false;
        it.kind = SpotifyLibraryItem::Kind::Album;
        out.push_back(std::move(it));
    }
    return out;
}

} // namespace muisc
