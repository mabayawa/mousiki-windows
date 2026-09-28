#pragma once
#include <string>
#include <vector>

#include "online_source.h"
#include "queue_item.h"

// The queue mapping, in one place instead of copy-pasted at each call site.
//
// mousiki's queue is a plain std::vector<QueueItem> owned by the main thread --
// Spotify's own queue endpoint is used only for a one-track gapless lookahead,
// never as the queue itself. So adding a 300-track playlist is 300 push_backs:
// instant, no HTTP, no rate limit, nothing to pace or cap.
namespace muisc {

// The OnlineResult -> QueueItem mapping. Carries spotify_uri and duration_sec,
// whose loss is exactly the regression QueueItem's own comment records.
QueueItem queue_item_from(const OnlineResult& r);

// Appends every item, in order. Returns how many were added, so the caller can
// name a real number in the status line rather than claim a count it guessed.
int append_online(std::vector<QueueItem>& queue, const std::vector<OnlineResult>& items);

// Is a track with this Spotify uri already queued?
//
// An EMPTY uri never matches, which is the point: local rows and YouTube rows
// both carry an empty spotify_uri, so a bare equality test would make every one
// of them match every other. The library browser's in-queue marker needs this
// uri-keyed form because list_row_in_queue() indexes into local_view_/
// online_view_ and cannot see the browser's own track list.
bool queue_contains_uri(const std::vector<QueueItem>& queue, const std::string& uri);

} // namespace muisc
