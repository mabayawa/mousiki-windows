#pragma once
#include <string>
#include <vector>

#include "online_source.h"
#include "spotify_source.h"
#include "tiny_json.h"

// The JSON half of the Spotify helper, separated from the subprocess half.
//
// spotify_source.cpp owns call() -- spawning `python spotify.py ...` and
// capturing its stdout. Everything here turns that captured string into structs
// and touches nothing else: no process_util.h, no path_utf8.h, no platform
// headers. That is what lets the unit tests link these functions against
// recorded response fixtures instead of shelling out to Spotify, and it is why
// the split exists at all.
//
// It also removes a duplication that was already here: the "tracks":[...]
// reader was copy-pasted three times in spotify_source.cpp (playlist items,
// saved tracks, search hits) and album tracks would have made it four. All four
// are the same array of the same objects, so there is now one tracks() for all
// of them.
//
// Every function takes the helper's whole response and reports failure the same
// way: false / an empty vector, with `error_out` set to one line fit for the
// status bar. A successful call that legitimately found nothing returns an
// empty vector and leaves error_out alone -- "this playlist is empty" is an
// answer, not an error, and the UI renders the two very differently.
namespace muisc::spotify_parse {

// {"ok":false,"error":...,"detail":...} flattened into one status-bar line.
std::string error_line(const tinyjson::Value& root);

// Parses `text` and checks the {"ok":true} envelope, handing back the root.
//
// The helper prints with ensure_ascii=False, i.e. raw UTF-8 rather than \uXXXX
// escapes. That is load-bearing, not incidental: tiny_json.h does not decode \u
// sequences, so an escaped response would silently mangle every non-ASCII
// title. tests/test_spotify_parse.cpp pins this from the reading side and
// tests/test_spotify_py.py from the writing side.
bool envelope(const std::string& text, tinyjson::Value& root, std::string* error_out);

bool profile(const std::string& text, SpotifyProfile& out, std::string* error_out);

// Both read the same row shape -- a named collection with an owner and a count
// -- and differ only in which array they look under and the Kind they stamp.
std::vector<SpotifyLibraryItem> playlists(const std::string& text, std::string* error_out);
std::vector<SpotifyLibraryItem> albums(const std::string& text, std::string* error_out);

// Rows with no uri are dropped: the uri is how a Spotify track is played and
// how it is recognised again in the queue, so a row without one is unusable.
std::vector<OnlineResult> tracks(const std::string& text, std::string* error_out);

} // namespace muisc::spotify_parse
