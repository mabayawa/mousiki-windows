#pragma once
#include <string>
#include <vector>

#include "spotify_source.h"

// Turning two helper responses into the one flat list the library browser shows.
//
// The rule worth isolating is "is this mine". It compares owner IDS, never
// display names: Spotify's display_name is nullable and not unique, so a
// playlist someone else made under the same display name would be claimed as
// the user's own. Comparing ids cannot make that mistake. This lives in its own
// translation unit rather than in a lambda inside a worker thread because it is
// the part most likely to be subtly wrong, and being a free function is what
// makes it testable at all.
namespace muisc {

// Merges playlists and saved albums into one ordered list and stamps `mine`.
//
// Order: the user's own playlists, then the ones they follow, then their saved
// albums -- Spotify's own order preserved WITHIN each group, because the order
// it returns playlists in is the order the user arranged them in and throwing
// that away to sort alphabetically would lose information they created.
//
// `me_id` empty (a token minted before user-read-private, which is a real state
// -- Spotify returns a null product for such tokens) claims nothing as the
// user's own rather than guessing: everything simply reads as followed. An album
// is never `mine`, because its owner_id is always empty and an empty me_id must
// not match an empty owner_id.
std::vector<SpotifyLibraryItem> build_library_rows(const std::string& me_id,
                                                   const std::vector<SpotifyLibraryItem>& playlists,
                                                   const std::vector<SpotifyLibraryItem>& albums);

} // namespace muisc
