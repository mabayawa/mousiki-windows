#include "spotify_control.h"

#include <algorithm>

#include "console_log.h"

namespace muisc {

SpotifyControl::~SpotifyControl() { stop(); }

void SpotifyControl::configure(const SpotifySource* src) { src_ = src; }

void SpotifyControl::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
        // Everything queued is about to be thrown away, so mark it complete
        // first: a staged track start waits on a ticket, and a shutdown that
        // dropped the command without completing the ticket would leave that
        // wait unsatisfiable forever.
        for (const Cmd& c : queue_) note_completed(c.ticket);
        queue_.clear();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SpotifyControl::ensure_worker() {
    if (worker_.joinable()) return;
    worker_ = std::thread([this] {
        // An exception escaping a std::thread calls std::terminate, which on
        // Windows kills the process with no message at all. Same guard the
        // device worker uses.
        try {
            worker_loop();
        } catch (const std::exception& e) {
            ConsoleLog::instance().log_verbose(std::string("spotify: control worker aborted: ") +
                                               e.what());
        } catch (...) {
            ConsoleLog::instance().log_verbose("spotify: control worker aborted: unknown exception");
        }
    });
}

void SpotifyControl::note_completed(Ticket t) {   // mu_ held by the caller
    if (t > completed_.load(std::memory_order_relaxed)) {
        completed_.store(t, std::memory_order_release);
    }
}

SpotifyControl::Ticket SpotifyControl::submit(Cmd c) {
    if (!src_) return 0;
    ensure_worker();
    Ticket t = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return 0;
        // Assigned under mu_, so ticket order is queue order -- which is what
        // lets completed() be a single high-water mark rather than a set.
        t = c.ticket = ++next_ticket_;
        queue_.push_back(std::move(c));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return t;
}

SpotifyControl::Ticket SpotifyControl::volume(const std::string& device_id, int percent) {
    if (!src_) return 0;
    ensure_worker();
    Ticket t = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return 0;
        // Replace any volume still waiting. Holding a volume key otherwise
        // queues one round trip per press and the device audibly climbs through
        // every step on the way, seconds behind the bar on screen.
        Ticket dropped_hi = 0;
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->kind == Cmd::Kind::Volume) {
                dropped_hi = (std::max)(dropped_hi, it->ticket);
                pending_.fetch_sub(1, std::memory_order_relaxed);
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        // Dropped means it will never run, so nothing may wait on it forever.
        // Every dropped ticket is older than the one issued just below.
        if (dropped_hi) note_completed(dropped_hi);

        t = ++next_ticket_;
        Cmd c{Cmd::Kind::Volume, device_id, {}, -1, t};
        c.percent = percent;
        queue_.push_back(std::move(c));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return t;
}

SpotifyControl::Ticket SpotifyControl::pause_now(const std::string& device_id) {
    if (!src_) return 0;
    ensure_worker();
    Ticket t = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return 0;
        Ticket dropped_hi = 0;
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (is_superseded_by_pause(it->kind)) {
                dropped_hi = (std::max)(dropped_hi, it->ticket);
                pending_.fetch_sub(1, std::memory_order_relaxed);
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        // Dropping counts as completing: the command will never run, so
        // anything waiting on it must not wait forever. Every dropped ticket is
        // older than the one issued just below, so this cannot pre-complete the
        // pause itself.
        if (dropped_hi) note_completed(dropped_hi);

        t = ++next_ticket_;
        queue_.push_front(Cmd{Cmd::Kind::Pause, device_id, {}, -1, t});
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
    return t;
}

SpotifyControl::Ticket SpotifyControl::play(const std::string& device_id,
                                           std::vector<std::string> uris,
                                           long long position_ms) {
    Cmd c{Cmd::Kind::Play, device_id, std::move(uris), position_ms, 0};
    return submit(std::move(c));
}

SpotifyControl::Ticket SpotifyControl::enqueue(const std::string& device_id,
                                               const std::string& uri) {
    return submit(Cmd{Cmd::Kind::Queue, device_id, {uri}, -1, 0});
}

SpotifyControl::Ticket SpotifyControl::pause(const std::string& device_id) {
    return submit(Cmd{Cmd::Kind::Pause, device_id, {}, -1, 0});
}

SpotifyControl::Ticket SpotifyControl::resume(const std::string& device_id) {
    return submit(Cmd{Cmd::Kind::Resume, device_id, {}, -1, 0});
}

SpotifyControl::Ticket SpotifyControl::seek(const std::string& device_id,
                                            long long position_ms) {
    return submit(Cmd{Cmd::Kind::Seek, device_id, {}, position_ms, 0});
}

SpotifyControl::Ticket SpotifyControl::next(const std::string& device_id) {
    return submit(Cmd{Cmd::Kind::Next, device_id, {}, -1, 0});
}

void SpotifyControl::request_devices() {
    if (!src_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (devices_inflight_) return;   // coalesce
        devices_inflight_ = true;
    }
    submit(Cmd{Cmd::Kind::Devices, {}, {}, -1, 0});
}

void SpotifyControl::request_state() {
    if (!src_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_inflight_) return;     // coalesce
        state_inflight_ = true;
    }
    submit(Cmd{Cmd::Kind::State, {}, {}, -1, 0});
}

void SpotifyControl::worker_loop() {
    for (;;) {
        Cmd c;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            c = std::move(queue_.front());
            queue_.pop_front();
        }

        std::string err;
        bool ok = true;
        switch (c.kind) {
            case Cmd::Kind::Play:
                ok = src_->play(c.device_id, c.uris, c.position_ms, &err);
                break;
            case Cmd::Kind::Queue:
                ok = src_->enqueue(c.device_id, c.uris.empty() ? std::string() : c.uris[0], &err);
                break;
            case Cmd::Kind::Pause:  ok = src_->pause(c.device_id, &err);  break;
            case Cmd::Kind::Resume: ok = src_->resume(c.device_id, &err); break;
            case Cmd::Kind::Seek:   ok = src_->seek(c.device_id, c.position_ms, &err); break;
            case Cmd::Kind::Next:   ok = src_->next(c.device_id, &err);   break;
            case Cmd::Kind::Volume: ok = src_->set_volume(c.device_id, c.percent, &err); break;
            case Cmd::Kind::Devices: {
                auto devs = src_->devices(&err);
                ok = err.empty();
                {
                    std::lock_guard<std::mutex> lk(out_mu_);
                    devices_out_ = std::move(devs);
                    devices_ready_ = true;
                }
                std::lock_guard<std::mutex> lk(mu_);
                devices_inflight_ = false;
                break;
            }
            case Cmd::Kind::State: {
                SpotifyPlaybackState st;
                ok = src_->state(st, &err);
                if (ok) {
                    std::lock_guard<std::mutex> lk(out_mu_);
                    state_out_ = st;
                    state_ready_ = true;
                }
                std::lock_guard<std::mutex> lk(mu_);
                state_inflight_ = false;
                break;
            }
        }

        if (!ok && !err.empty()) {
            std::lock_guard<std::mutex> lk(out_mu_);
            error_out_ = err;
        }
        {
            // AFTER the round trip returned, so completed(t) means "Spotify has
            // answered", not "we sent it". A caller waiting on a pause ticket is
            // waiting for the audio to have actually stopped.
            std::lock_guard<std::mutex> lk(mu_);
            note_completed(c.ticket);
        }
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool SpotifyControl::take_devices(std::vector<SpotifyDevice>& out) {
    std::lock_guard<std::mutex> lk(out_mu_);
    if (!devices_ready_) return false;
    out = std::move(devices_out_);
    devices_out_.clear();
    devices_ready_ = false;
    return true;
}

bool SpotifyControl::take_state(SpotifyPlaybackState& out) {
    std::lock_guard<std::mutex> lk(out_mu_);
    if (!state_ready_) return false;
    out = state_out_;
    state_ready_ = false;
    return true;
}

std::string SpotifyControl::take_error() {
    std::lock_guard<std::mutex> lk(out_mu_);
    std::string e = std::move(error_out_);
    error_out_.clear();
    return e;
}

} // namespace muisc
