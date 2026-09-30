#include "queue_ops.h"

namespace muisc {

QueueItem queue_item_from(const OnlineResult& r) {
    return QueueItem{false, r.title, r.uploader, {}, r.video_id, r.spotify_uri, r.duration_sec};
}

int append_online(std::vector<QueueItem>& queue, const std::vector<OnlineResult>& items) {
    int added = 0;
    queue.reserve(queue.size() + items.size());
    for (const auto& r : items) {
        queue.push_back(queue_item_from(r));
        ++added;
    }
    return added;
}

bool queue_contains_uri(const std::vector<QueueItem>& queue, const std::string& uri) {
    if (uri.empty()) return false;
    for (const auto& q : queue) {
        if (q.spotify_uri == uri) return true;
    }
    return false;
}

} // namespace muisc
