#include "queue_ops.h"
#include "tiny_test.h"

using namespace muisc;
using namespace muisc::test;

namespace {

OnlineResult spotify_row(const char* title, const std::string& uri, double dur) {
    OnlineResult r;
    r.title = title;
    r.uploader = "Someone";
    r.spotify_uri = uri;
    r.duration_sec = dur;
    return r;
}

} // namespace

TEST(mapping_carries_the_uri_and_duration) {
    // The regression that QueueItem's own comment records: dropping these made a
    // queued Spotify track replay through the YouTube search path as a
    // different recording, silently.
    QueueItem q = queue_item_from(spotify_row("Midnight City", "spotify:track:t1", 243.0));
    CHECK(!q.is_local);
    CHECK_EQ(q.title, std::string("Midnight City"));
    CHECK_EQ(q.artist, std::string("Someone"));
    CHECK_EQ(q.spotify_uri, std::string("spotify:track:t1"));
    CHECK_EQ(q.duration_sec, 243.0);
    CHECK(q.local_path.empty());
    CHECK_EQ(q.video_id, std::string(""));
}

TEST(mapping_keeps_a_youtube_row_a_youtube_row) {
    OnlineResult r;
    r.title = "a video";
    r.uploader = "chan";
    r.video_id = "abc123";
    QueueItem q = queue_item_from(r);
    CHECK_EQ(q.video_id, std::string("abc123"));
    CHECK_EQ(q.spotify_uri, std::string(""));
}

TEST(append_preserves_order_and_reports_the_count) {
    std::vector<QueueItem> queue;
    queue.push_back(queue_item_from(spotify_row("already here", "spotify:track:t0", 10.0)));
    std::vector<OnlineResult> items;
    for (int i = 1; i <= 82; ++i) {
        items.push_back(spotify_row("t", "spotify:track:x" + std::to_string(i), 1.0));
    }
    const int added = append_online(queue, items);
    CHECK_EQ(added, 82);
    CHECK_EQ(static_cast<int>(queue.size()), 83);
    CHECK_EQ(queue[1].spotify_uri, std::string("spotify:track:x1"));
    CHECK_EQ(queue[82].spotify_uri, std::string("spotify:track:x82"));
}

TEST(appending_nothing_changes_nothing) {
    std::vector<QueueItem> queue;
    CHECK_EQ(append_online(queue, {}), 0);
    CHECK(queue.empty());
}

TEST(uri_lookup_finds_a_queued_track) {
    std::vector<QueueItem> queue;
    append_online(queue, {spotify_row("a", "spotify:track:t1", 1.0),
                          spotify_row("b", "spotify:track:t2", 1.0)});
    CHECK(queue_contains_uri(queue, "spotify:track:t2"));
    CHECK(!queue_contains_uri(queue, "spotify:track:t9"));
}

TEST(an_empty_uri_never_matches_anything) {
    // Local rows and YouTube rows both carry an empty spotify_uri, so a bare
    // equality test would mark every one of them as already queued.
    std::vector<QueueItem> queue;
    queue.push_back(QueueItem{true, "local song", "artist", "C:/music/a.mp3", "", "", 100.0});
    OnlineResult yt;
    yt.title = "vid";
    yt.video_id = "abc";
    queue.push_back(queue_item_from(yt));
    CHECK(!queue_contains_uri(queue, ""));
}

TEST(uri_lookup_on_an_empty_queue_is_false) {
    CHECK(!queue_contains_uri({}, "spotify:track:t1"));
}
