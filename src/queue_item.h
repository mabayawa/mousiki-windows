#pragma once
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

// Moved out of app.h so the queue mapping helpers (queue_ops.h) can be a leaf
// translation unit the unit tests link directly. app.h pulls in player.h, which
// pulls in miniaudio and a platform audio backend, so nothing that lived there
// could be tested without linking the whole application.
//
// The field ORDER is load-bearing and this move preserved it exactly. It is
// positionally aggregate-initialised at app.cpp:1482 (local rows),
// app.cpp:1623 (snapshot restore) and inside queue_item_from() (every online
// row), and SnapshotTrack in snapshot.h mirrors the same information by name.
// Reordering or inserting a field silently changes what those braces mean.
struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
    // Carried so a queued Spotify track stays a Spotify track. Without these,
    // enqueuing one dropped its URI and its duration, and replaying it fell
    // through to the YouTube search path with an empty artist -- a different
    // recording, silently.
    std::string spotify_uri;
    double duration_sec = -1.0;
};

} // namespace muisc
