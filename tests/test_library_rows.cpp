#include "spotify_library.h"
#include "spotify_parse.h"
#include "tiny_test.h"

using namespace muisc;
using namespace muisc::test;

namespace {

SpotifyLibraryItem pl(const char* id, const char* owner, const char* owner_id) {
    SpotifyLibraryItem it;
    it.id = id;
    it.name = id;
    it.owner = owner;
    it.owner_id = owner_id;
    it.kind = SpotifyLibraryItem::Kind::Playlist;
    return it;
}
SpotifyLibraryItem alb(const char* id, const char* artist) {
    SpotifyLibraryItem it;
    it.id = id;
    it.name = id;
    it.owner = artist;
    it.kind = SpotifyLibraryItem::Kind::Album;
    return it;
}
std::string ids(const std::vector<SpotifyLibraryItem>& v) {
    std::string s;
    for (const auto& i : v) { if (!s.empty()) s += ","; s += i.id; }
    return s;
}

} // namespace

TEST(rows_put_mine_first_then_followed_then_albums) {
    auto rows = build_library_rows("bono",
                                   {pl("a", "mabayawa", "bono"),
                                    pl("b", "Spotify", "spotify"),
                                    pl("c", "mabayawa", "bono")},
                                   {alb("z", "Radiohead")});
    CHECK_EQ(ids(rows), std::string("a,c,b,z"));
    CHECK(rows[0].mine);
    CHECK(rows[1].mine);
    CHECK(!rows[2].mine);
    CHECK(!rows[3].mine);
}

TEST(rows_preserve_spotify_order_within_each_group) {
    // The order Spotify returns playlists in is the order the user arranged
    // them in, so grouping must not re-sort inside a group.
    auto rows = build_library_rows("bono",
                                   {pl("m1", "me", "bono"), pl("f1", "x", "ux"),
                                    pl("m2", "me", "bono"), pl("f2", "y", "uy")},
                                   {});
    CHECK_EQ(ids(rows), std::string("m1,m2,f1,f2"));
}

TEST(a_matching_display_name_with_a_different_id_is_not_mine) {
    // The whole reason owner_id exists. display_name is neither unique nor
    // non-null, so comparing names would claim someone else's playlist.
    auto rows = build_library_rows("bono", {pl("theirs", "mabayawa", "u_twin")}, {});
    CHECK_EQ(static_cast<int>(rows.size()), 1);
    CHECK(!rows[0].mine);
}

TEST(an_empty_me_id_claims_nothing) {
    // A token minted before user-read-private cannot tell us who we are. Without
    // the guard, "" would match every unreadable owner_id and every album.
    auto rows = build_library_rows("",
                                   {pl("p", "someone", ""), pl("q", "other", "uq")},
                                   {alb("z", "Artist")});
    for (const auto& r : rows) CHECK(!r.mine);
    CHECK_EQ(ids(rows), std::string("p,q,z"));  // playlists then albums, order kept
}

TEST(albums_are_never_mine_even_when_the_artist_name_matches) {
    auto rows = build_library_rows("bono", {}, {alb("z", "bono")});
    CHECK(!rows[0].mine);
    CHECK(rows[0].kind == SpotifyLibraryItem::Kind::Album);
}

TEST(an_empty_library_yields_no_rows) {
    CHECK(build_library_rows("bono", {}, {}).empty());
}

TEST(rows_built_from_the_real_fixture_shapes) {
    // End to end over the recorded payloads, so the parser and the classifier
    // are checked against the same bytes the helper actually emits.
    std::string err;
    SpotifyProfile me;
    CHECK(spotify_parse::profile(fixture("me.json"), me, &err));
    auto rows = build_library_rows(me.id,
                                  spotify_parse::playlists(fixture("playlists.json"), &err),
                                  spotify_parse::albums(fixture("albums.json"), &err));
    CHECK_EQ(static_cast<int>(rows.size()), 7);   // 5 playlists kept + 2 albums
    CHECK_EQ(ids(rows), std::string("p1,p3,p2,p4,p5,a1,a2"));
    CHECK(rows[0].mine);
    CHECK(rows[1].mine);
    CHECK(!rows[4].mine);  // p5, the display-name impostor
}
