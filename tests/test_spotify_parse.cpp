#include "spotify_parse.h"
#include "tiny_test.h"

using namespace muisc;
using namespace muisc::test;

// --- the ok/error envelope -------------------------------------------------

TEST(envelope_accepts_ok) {
    std::string err = "untouched";
    tinyjson::Value root;
    CHECK(spotify_parse::envelope(fixture("me.json"), root, &err));
    CHECK_EQ(err, std::string("untouched"));  // success must not write an error
}

TEST(envelope_prefers_detail_over_error_code) {
    std::string err;
    tinyjson::Value root;
    CHECK(!spotify_parse::envelope(fixture("error_no_auth.json"), root, &err));
    // detail, when present, is the human-readable half -- the bare code is not
    // what belongs in a status bar.
    CHECK_EQ(err, std::string("spotify: no cached token; run login first"));
}

TEST(envelope_falls_back_when_no_detail) {
    std::string err;
    tinyjson::Value root;
    CHECK(!spotify_parse::envelope(fixture("error_bare.json"), root, &err));
    CHECK_EQ(err, std::string("spotify: unknown error"));
}

TEST(envelope_rejects_non_json) {
    std::string err;
    tinyjson::Value root;
    CHECK(!spotify_parse::envelope(fixture("error_garbage.json"), root, &err));
    CHECK_EQ(err, std::string("spotify: helper returned unparseable JSON"));
}

// --- profile ---------------------------------------------------------------

TEST(profile_reads_id_separately_from_display_name) {
    SpotifyProfile p;
    std::string err;
    CHECK(spotify_parse::profile(fixture("me.json"), p, &err));
    CHECK_EQ(p.id, std::string("bono"));
    CHECK_EQ(p.display_name, std::string("mabayawa"));
    CHECK_EQ(p.product, std::string("premium"));
    CHECK_EQ(p.country, std::string("PH"));
}

TEST(profile_null_product_reads_as_unknown_not_free) {
    SpotifyProfile p;
    std::string err;
    CHECK(spotify_parse::profile(fixture("me_no_product.json"), p, &err));
    // The shape a real account actually returns. Empty means UNKNOWN; rendering
    // it as "free" would tell the user they cannot stream when they can.
    CHECK_EQ(p.product, std::string(""));
    CHECK_EQ(p.id, std::string("bono"));
}

TEST(profile_market_is_read_and_may_be_empty) {
    // An empty market is the reason unavailable tracks cannot be filtered:
    // Spotify reports is_playable only when a market is supplied, and a token
    // minted before user-read-private resolves no country at all. The app shows
    // a hint when this is empty, so reading it correctly matters.
    SpotifyProfile p;
    std::string err;
    CHECK(spotify_parse::profile(fixture("me.json"), p, &err));
    CHECK_EQ(p.market, std::string("PH"));

    SpotifyProfile q;
    CHECK(spotify_parse::profile(fixture("me_no_product.json"), q, &err));
    CHECK_EQ(q.market, std::string(""));
    CHECK_EQ(q.product, std::string(""));   // the same cause, both empty
}

TEST(profile_fails_on_error_envelope) {
    SpotifyProfile p;
    std::string err;
    CHECK(!spotify_parse::profile(fixture("error_no_auth.json"), p, &err));
}

// --- playlists and albums --------------------------------------------------

TEST(playlists_reads_every_field_and_drops_rows_with_no_id) {
    std::string err;
    auto v = spotify_parse::playlists(fixture("playlists.json"), &err);
    CHECK_EQ(static_cast<int>(v.size()), 5);  // six in the fixture, one has no id
    CHECK_EQ(v[0].id, std::string("p1"));
    CHECK_EQ(v[0].name, std::string("Chill Vibes"));
    CHECK_EQ(v[0].owner, std::string("mabayawa"));
    CHECK_EQ(v[0].owner_id, std::string("bono"));
    CHECK_EQ(v[0].uri, std::string("spotify:playlist:p1"));
    CHECK_EQ(v[0].tracks, 82);
    CHECK(v[0].kind == SpotifyLibraryItem::Kind::Playlist);
    // mine is NOT the parser's job -- it is a comparison against the profile.
    CHECK(!v[0].mine);
}

TEST(playlists_empty_owner_display_name_is_not_an_error) {
    std::string err;
    auto v = spotify_parse::playlists(fixture("playlists.json"), &err);
    CHECK_EQ(v[3].owner, std::string(""));
    CHECK_EQ(v[3].owner_id, std::string("u_other"));  // still classifiable
}

TEST(albums_are_tagged_album_and_never_carry_an_owner_id) {
    std::string err;
    auto v = spotify_parse::albums(fixture("albums.json"), &err);
    CHECK_EQ(static_cast<int>(v.size()), 2);
    CHECK(v[0].kind == SpotifyLibraryItem::Kind::Album);
    CHECK_EQ(v[0].name, std::string("In Rainbows"));
    CHECK_EQ(v[0].owner, std::string("Radiohead"));  // artist, in the owner column
    // Empty owner_id is what stops an album ever being classified as "mine".
    CHECK_EQ(v[0].owner_id, std::string(""));
    CHECK_EQ(v[1].tracks, 17);
}

TEST(albums_on_a_playlists_payload_finds_nothing) {
    // Each reader looks under its own array key, so a mixed-up call yields an
    // empty list rather than mis-tagging playlists as albums.
    std::string err;
    CHECK(spotify_parse::albums(fixture("playlists.json"), &err).empty());
}

// --- tracks ---------------------------------------------------------------

TEST(tracks_maps_fields_and_leaves_video_id_empty) {
    std::string err;
    auto v = spotify_parse::tracks(fixture("tracks.json"), &err);
    CHECK_EQ(static_cast<int>(v.size()), 3);  // four in the fixture, one has no uri
    CHECK_EQ(v[0].title, std::string("Midnight City"));
    CHECK_EQ(v[0].uploader, std::string("M83"));
    CHECK_EQ(v[0].spotify_uri, std::string("spotify:track:t1"));
    CHECK_EQ(v[0].duration_sec, 243.0);
    // The identity rule: a Spotify row has a uri and NO video_id. Setting one
    // here would make every Spotify row match every other on video_id.
    CHECK_EQ(v[0].video_id, std::string(""));
}

TEST(tracks_preserves_raw_utf8_titles) {
    std::string err;
    auto v = spotify_parse::tracks(fixture("tracks.json"), &err);
    CHECK_EQ(v[1].title, std::string("\u96fb\u5149\u77f3\u706b"));
}

TEST(tracks_cannot_decode_u_escapes_which_is_why_ensure_ascii_is_false) {
    // A CANARY. tiny_json.h does not decode \u sequences, so if anyone drops
    // ensure_ascii=False from scripts/spotify.py's emit(), titles arrive looking
    // like this instead of as text. Asserting the mangling makes the cause
    // obvious the day it changes, rather than leaving a silent corruption.
    std::string err;
    auto v = spotify_parse::tracks(fixture("tracks_uescaped.json"), &err);
    CHECK_EQ(static_cast<int>(v.size()), 1);
    CHECK(v[0].title != std::string("\u96fb\u5149\u77f3\u706b"));
}

TEST(tracks_keeps_unknown_duration_negative) {
    std::string err;
    auto v = spotify_parse::tracks(fixture("tracks.json"), &err);
    CHECK_EQ(v[2].duration_sec, -1.0);  // not silently turned into 0
}

TEST(empty_track_list_is_an_answer_not_an_error) {
    // This is what a playlist holding only local files looks like by the time it
    // reaches C++, and the UI renders "no playable tracks" rather than a failure
    // -- so error_out must stay clean.
    std::string err = "untouched";
    auto v = spotify_parse::tracks(fixture("tracks_empty.json"), &err);
    CHECK(v.empty());
    CHECK_EQ(err, std::string("untouched"));
}

TEST(tracks_on_error_envelope_returns_empty_and_sets_error) {
    std::string err;
    auto v = spotify_parse::tracks(fixture("error_no_auth.json"), &err);
    CHECK(v.empty());
    CHECK(!err.empty());
}
