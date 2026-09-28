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

    // A handle on one submitted command, so a caller can tell when that
    // specific round trip has actually landed. busy() below cannot answer
    // this: it counts every queued command including the coalesced
    // /me/player polls, so on a live Spotify transport it is never cleanly
    // zero.
    using Ticket = unsigned long long;

    void configure(const SpotifySource* src);
    void stop();

    // --- fire and forget, executed in submission order -------------------
    // Each returns the ticket for the command it queued; callers that do not
    // need to know when it landed can ignore it.
    Ticket play(const std::string& device_id, std::vector<std::string> uris,
                long long position_ms = -1);
    Ticket enqueue(const std::string& device_id, const std::string& uri);
    Ticket pause(const std::string& device_id);
    Ticket resume(const std::string& device_id);
    Ticket seek(const std::string& device_id, long long position_ms);
    Ticket next(const std::string& device_id);

    // --- the one command that may overtake the queue ----------------------
    //
    // Every other command here is deliberately FIFO -- a gapless enqueue must
    // land after the play that established the context (see the class
    // comment). A pause is the exception, and it earns it: it is the only
    // command whose whole value is in how fast it lands, and its meaning does
    // not depend on what was queued before it. So it goes to the FRONT, and
    // every queued Play/Queue/Resume/Seek/Next is dropped on the way -- those
    // all describe a track the user has just moved away from, so running them
    // would be a round trip spent making the outgoing track louder. That
    // queueing, not the round trip itself, is what turned "the old song stops
    // in 300 ms" into "the old song plays for a few seconds".
    //
    // What this cannot do: cancel the command the worker is already inside.
    // One in-flight HTTPS round trip is the irreducible floor on how fast a
    // Connect device can be told to stop.
    Ticket pause_now(const std::string& device_id);

    // True once `t` has finished, or was dropped as superseded (it will never
    // run, so nothing may wait on it forever). Ticket 0 means "nothing was
    // issued" and is always satisfied.
    //
    // HONEST LIMITATION: completed_ is a high-water mark, not a set, because a
    // priority pause completes out of ticket order. It therefore answers
    // correctly only for a ticket that cannot be overtaken -- which is
    // guaranteed for pause_now()'s ticket, since that is the newest ticket at
    // the moment it is issued AND it sits at the head of the queue, so nothing
    // submitted after it can run first. Do not use this to wait on an ordinary
    // queued command.
    bool completed(Ticket t) const {
        return t == 0 || completed_.load(std::memory_order_acquire) >= t;
    }

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
        Ticket ticket = 0;
    };

    // What a priority pause supersedes. Pause itself is deliberately NOT in
    // this set: dropping an older pause and marking it complete would tell a
    // waiter "the audio has stopped" while the replacement pause was still
    // queued. Devices/State are not either -- they are coalesced polls whose
    // *_inflight_ flags are only ever cleared by the worker, so dropping one
    // would wedge that kind of poll for the rest of the session.
    static bool is_superseded_by_pause(Cmd::Kind k) {
        return k == Cmd::Kind::Play || k == Cmd::Kind::Queue || k == Cmd::Kind::Resume ||
               k == Cmd::Kind::Seek || k == Cmd::Kind::Next;
    }

    void ensure_worker();
    void worker_loop();
    Ticket submit(Cmd c);
    // Caller must hold mu_. Keeping every write to completed_ under the mutex
    // is what makes the max() race-free between the worker finishing a command
    // and a pause_now() dropping several.
    void note_completed(Ticket t);

    const SpotifySource* src_ = nullptr;

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Cmd> queue_;
    bool quit_ = false;
    std::atomic<int> pending_{0};
    Ticket next_ticket_ = 0;             // guarded by mu_
    // Written only under mu_, read with a bare acquire load. That asymmetry is
    // the point: the 25 fps render loop polls completed() every frame and must
    // never take this mutex.
    std::atomic<Ticket> completed_{0};

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
