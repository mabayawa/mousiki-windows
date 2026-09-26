#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spotify_source.h"

namespace muisc {

// Serialises Spotify Web API transport commands onto one background thread.
//
// Two reasons this is not the codebase's usual "one detached thread per job"
// pattern. Every call is a Python subprocess plus an HTTPS round trip, i.e.
// 200-600 ms, which cannot happen on a 25 fps render loop. And unlike a lyrics
// fetch these are ORDERED: a queue-for-gapless must land after the play that
// established the context, so firing them off concurrently would sometimes
// reverse them.
//
// Results come back through the same mutex + "ready" flag handoff the rest of
// App already drains once per frame.
class SpotifyControl {
public:
    ~SpotifyControl();

    void configure(const SpotifySource* src);
    void stop();

    // --- fire and forget, executed in submission order -------------------
    void play(const std::string& device_id, std::vector<std::string> uris,
              long long position_ms = -1);
    void enqueue(const std::string& device_id, const std::string& uri);
    void pause(const std::string& device_id);
    void resume(const std::string& device_id);
    void seek(const std::string& device_id, long long position_ms);
    void next(const std::string& device_id);

    // --- polls, coalesced so a slow round trip cannot pile up ------------
    void request_devices();
    void request_state();

    // --- drained once per frame by App -----------------------------------
    bool take_devices(std::vector<SpotifyDevice>& out);
    bool take_state(SpotifyPlaybackState& out);
    // Last error from any command, or empty. Cleared by reading it.
    std::string take_error();

    bool busy() const { return pending_.load(std::memory_order_relaxed) > 0; }

private:
    struct Cmd {
        enum class Kind { Play, Queue, Pause, Resume, Seek, Next, Devices, State } kind;
        std::string device_id;
        std::vector<std::string> uris;
        long long position_ms = -1;
    };

    void ensure_worker();
    void worker_loop();
    void submit(Cmd c);

    const SpotifySource* src_ = nullptr;

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Cmd> queue_;
    bool quit_ = false;
    std::atomic<int> pending_{0};

    // Coalescing flags: one outstanding poll of each kind at most, so a slow
    // network cannot build a backlog of stale requests.
    bool devices_inflight_ = false;
    bool state_inflight_ = false;

    std::mutex out_mu_;
    std::vector<SpotifyDevice> devices_out_;
    bool devices_ready_ = false;
    SpotifyPlaybackState state_out_;
    bool state_ready_ = false;
    std::string error_out_;
};

} // namespace muisc
