#include "spotify_control.h"

#include "console_log.h"

namespace muisc {

SpotifyControl::~SpotifyControl() { stop(); }

void SpotifyControl::configure(const SpotifySource* src) { src_ = src; }

void SpotifyControl::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
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

void SpotifyControl::submit(Cmd c) {
    if (!src_) return;
    ensure_worker();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return;
        queue_.push_back(std::move(c));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

void SpotifyControl::play(const std::string& device_id, std::vector<std::string> uris,
                          long long position_ms) {
    Cmd c{Cmd::Kind::Play, device_id, std::move(uris), position_ms};
    submit(std::move(c));
}

void SpotifyControl::enqueue(const std::string& device_id, const std::string& uri) {
    submit(Cmd{Cmd::Kind::Queue, device_id, {uri}, -1});
}

void SpotifyControl::pause(const std::string& device_id) {
    submit(Cmd{Cmd::Kind::Pause, device_id, {}, -1});
}

void SpotifyControl::resume(const std::string& device_id) {
    submit(Cmd{Cmd::Kind::Resume, device_id, {}, -1});
}

void SpotifyControl::seek(const std::string& device_id, long long position_ms) {
    submit(Cmd{Cmd::Kind::Seek, device_id, {}, position_ms});
}

void SpotifyControl::next(const std::string& device_id) {
    submit(Cmd{Cmd::Kind::Next, device_id, {}, -1});
}

void SpotifyControl::request_devices() {
    if (!src_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (devices_inflight_) return;   // coalesce
        devices_inflight_ = true;
    }
    submit(Cmd{Cmd::Kind::Devices, {}, {}, -1});
}

void SpotifyControl::request_state() {
    if (!src_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_inflight_) return;     // coalesce
        state_inflight_ = true;
    }
    submit(Cmd{Cmd::Kind::State, {}, {}, -1});
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
