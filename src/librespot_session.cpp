#include "librespot_session.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "console_log.h"
#include "path_utf8.h"
#include "process_util.h"

namespace muisc {

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr size_t kReadBytes = 32768;

} // namespace

LibrespotSession::~LibrespotSession() { shutdown(); }

bool LibrespotSession::start(const Config& cfg, std::string* error_out) {
    if (reader_.joinable()) return true;   // already running
    cfg_ = cfg;
    quit_.store(false);
    written_cur_.store(0);
    boundary_seq_.store(0);
    { std::lock_guard<std::mutex> lk(mu_); error_.clear(); auth_url_.clear(); }
    state_.store(State::Starting, std::memory_order_release);

    std::error_code ec;
    fs::create_directories(cfg_.cache_dir, ec);
    if (!cfg_.log_path.empty()) fs::create_directories(cfg_.log_path.parent_path(), ec);
    if (cfg_.exe.empty()) {
        state_.store(State::Failed, std::memory_order_release);
        std::lock_guard<std::mutex> lk(mu_);
        error_ = "librespot: no binary configured";
        if (error_out) *error_out = error_;
        return false;
    }

    last_byte_ms_.store(now_ms());
    // The process is spawned on the reader thread, not here: signing in has to
    // happen in a SEPARATE process first (see the header), and that can block on
    // a human for minutes. Doing it here would freeze the render loop.
    reader_ = std::thread([this] {
        try {
            if (!ensure_credentials()) {
                state_.store(State::Failed, std::memory_order_release);
                return;
            }
            if (!spawn_audio_child()) {
                state_.store(State::Failed, std::memory_order_release);
                return;
            }
            state_.store(State::WaitingForDevice, std::memory_order_release);
            reader_main();
        } catch (const std::exception& e) {
            state_.store(State::Failed, std::memory_order_release);
            ConsoleLog::instance().log_verbose(std::string("librespot: reader aborted: ") + e.what());
        } catch (...) {
            state_.store(State::Failed, std::memory_order_release);
            ConsoleLog::instance().log_verbose("librespot: reader aborted: unknown exception");
        }
    });
    return true;
}

std::shared_ptr<ChildProcess> LibrespotSession::get_proc() const {
    std::lock_guard<std::mutex> lk(proc_mu_);
    return proc_;
}

void LibrespotSession::set_proc(std::shared_ptr<ChildProcess> p) {
    std::lock_guard<std::mutex> lk(proc_mu_);
    proc_ = std::move(p);
}

std::string LibrespotSession::auth_url() const {
    std::lock_guard<std::mutex> lk(mu_);
    return auth_url_;
}

// The flags shared by both the auth pass and the real session.
static std::vector<std::string> librespot_base_argv(const LibrespotSession::Config& cfg) {
    return {
        path_utf8(cfg.exe),
        // A no-op once credentials.json exists, which is why the auth pass only
        // ever runs the first time.
        "--enable-oauth",
        "--cache", path_utf8(cfg.cache_dir),
        // Credentials are worth keeping; an unbounded disk cache of encrypted
        // audio is not.
        "--disable-audio-cache",
        // No mDNS listener, so no Windows Firewall prompt. We reach this device
        // through the Web API, and a discovery-only instance never authenticates
        // to the account, so it would not appear in /me/player/devices anyway.
        "--disable-discovery",
        "--name", cfg.device_name,
        "--device-type", "computer",
        // Otherwise Spotify streams its own recommendations into our buffer once
        // our context ends.
        "--autoplay", "off",
        "--backend", "pipe",
        // Matches the player's f32 pipeline exactly, so there is no conversion
        // pass. librespot fixes the rate at 44100 and the channels at 2, which
        // is what kAudioChannels already assumes. NOTE the default is S16 -- if
        // this flag were ever dropped, the stream would silently become
        // int16 read as float, i.e. noise.
        "--format", "F32",
        "--bitrate", std::to_string(cfg.bitrate),
        // Bit-exact output, and it stops another device's volume slider from
        // silently attenuating us. mousiki's own gain is the only volume.
        "--volume-ctrl", "fixed",
        "--initial-volume", "100",
    };
}

bool LibrespotSession::ensure_credentials() {
    const fs::path cred = cfg_.cache_dir / "credentials.json";
    std::error_code ec;
    if (fs::exists(cred, ec)) return true;   // nothing to do; the common case

    state_.store(State::Authenticating, std::memory_order_release);

    // The audio goes to a throwaway FILE via --device, so this child's stdout
    // carries nothing but librespot's own text -- which is exactly where the
    // "Browse to: https://..." line appears. Reading it here is therefore both
    // safe and the way to get the URL.
    std::vector<std::string> argv = librespot_base_argv(cfg_);
    argv.push_back("--device");
    argv.push_back(path_utf8(cfg_.cache_dir / "auth-discard.pcm"));

    std::shared_ptr<ChildProcess> child =
        ChildProcess::spawn(argv, /*merge_stderr=*/false, path_utf8(cfg_.log_path));
    if (!child) {
        std::lock_guard<std::mutex> lk(mu_);
        error_ = "librespot: could not start the sign-in step";
        return false;
    }
    // Held in proc_ so shutdown() can still reach it while a human deliberates.
    set_proc(child);

    // Generous, because a person has to approve a browser prompt.
    const long long deadline = now_ms() + 180000;
    bool got = false;
    std::string text;
    std::array<char, 4096> sbuf{};
    while (now_ms() < deadline) {
        if (quit_.load(std::memory_order_acquire)) break;
        std::error_code ec2;
        if (fs::exists(cred, ec2)) { got = true; break; }

        // Availability-checked: a blocking read here would sit forever once
        // librespot has finished printing and is just waiting on the browser.
        const long long avail = child->bytes_available();
        if (avail > 0) {
            const long long n = child->read_stdout(
                sbuf.data(), std::min<size_t>(sbuf.size(), static_cast<size_t>(avail)));
            if (n > 0) text.append(sbuf.data(), static_cast<size_t>(n));
            const size_t at = text.find("https://");
            if (at != std::string::npos) {
                size_t end = text.find_first_of(" \r\n", at);
                if (end == std::string::npos) end = text.size();
                std::lock_guard<std::mutex> lk(mu_);
                if (auth_url_.empty()) auth_url_ = text.substr(at, end - at);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    child->terminate();
    child->wait();
    set_proc(nullptr);
    fs::remove(cfg_.cache_dir / "auth-discard.pcm", ec);

    if (!got) {
        std::lock_guard<std::mutex> lk(mu_);
        if (error_.empty()) {
            error_ = "librespot: sign-in was not completed -- approve the browser prompt and retry";
        }
        return false;
    }
    return true;
}

bool LibrespotSession::spawn_audio_child() {
    std::vector<std::string> argv = librespot_base_argv(cfg_);
    // No --device: the pipe backend then writes to stdout, which is what we read.
    std::shared_ptr<ChildProcess> child =
        ChildProcess::spawn(argv, /*merge_stderr=*/false, path_utf8(cfg_.log_path));
    if (!child) {
        std::lock_guard<std::mutex> lk(mu_);
        error_ = "librespot: failed to start " + path_utf8(cfg_.exe);
        return false;
    }
    ConsoleLog::instance().log_command(describe_argv(argv), "", 0);
    set_proc(child);
    return true;
}

void LibrespotSession::shutdown() {
    quit_.store(true, std::memory_order_release);
    // Killing it is also the only way to unblock a reader parked in read_stdout
    // because Spotify is paused: the dead child closes the pipe, the read
    // returns 0, and the thread falls out of its loop.
    if (auto p = get_proc()) p->terminate();
    if (reader_.joinable()) reader_.join();
    if (auto p = get_proc()) { p->wait(); set_proc(nullptr); }
    state_.store(State::Idle, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mu_);
    plan_.clear();
}

bool LibrespotSession::alive() const {
    const State s = state_.load(std::memory_order_acquire);
    return s == State::Ready || s == State::WaitingForDevice || s == State::Starting ||
           s == State::Authenticating;
}

std::string LibrespotSession::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void LibrespotSession::reset_plan(LibrespotTrack first) {
    std::lock_guard<std::mutex> lk(mu_);
    const long long origin = first.origin_frames;
    plan_.clear();
    plan_.push_back(std::move(first));
    // Counts absolute track frames, so a post-seek stream starts where it
    // actually belongs rather than at zero.
    written_cur_.store(origin, std::memory_order_release);
}

void LibrespotSession::append_plan(LibrespotTrack next) {
    std::lock_guard<std::mutex> lk(mu_);
    plan_.push_back(std::move(next));
}

bool LibrespotSession::has_next() const {
    std::lock_guard<std::mutex> lk(mu_);
    return plan_.size() > 1;
}

std::string LibrespotSession::current_uri() const {
    std::lock_guard<std::mutex> lk(mu_);
    return plan_.empty() ? std::string() : plan_.front().uri;
}

double LibrespotSession::seconds_since_last_byte() const {
    return static_cast<double>(now_ms() - last_byte_ms_.load(std::memory_order_acquire)) / 1000.0;
}

void LibrespotSession::request_resync() {
    resync_.store(true, std::memory_order_release);
}

void LibrespotSession::reader_main() {
    auto proc = get_proc();
    if (!proc) return;
    std::array<char, kReadBytes> buf{};
    size_t carry_len = 0;
    // Aligned because the float view below is a reinterpret_cast over it; a
    // bare char array carries no such guarantee.
    alignas(alignof(float)) std::array<char, kReadBytes + 8> acc{};

    for (;;) {
        if (quit_.load(std::memory_order_acquire)) return;

        // Nothing planned, paused, or a seek is draining elsewhere: do not read.
        // Not reading is exactly what applies backpressure -- the pipe fills and
        // librespot stops. It is also why a stall must never be read as EOF.
        std::shared_ptr<DecodeSession> sess;
        long long expected = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!plan_.empty()) {
                sess = plan_.front().session;
                expected = plan_.front().frames_expected;
            }
        }
        // A seek landed: everything still in flight predates it. Drain here, on
        // the thread that owns the pipe, then drop the carry -- a partial frame
        // from before the jump must not be glued to the first frame after it.
        if (resync_.load(std::memory_order_acquire)) {
            // Availability-checked rather than a blocking read: the caller pauses
            // Spotify before asking for this, so librespot is writing nothing and
            // a blocking read would never return. Quiet for 250 ms means the
            // pre-seek backlog is gone; the 2 s cap stops a pathological case
            // from hanging the reader.
            const long long deadline = now_ms() + 2000;
            long long last_data = now_ms();
            while (now_ms() < deadline && now_ms() - last_data < 250) {
                const long long avail = proc->bytes_available();
                if (avail < 0) break;
                if (avail == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const long long n = proc->read_stdout(
                    buf.data(), std::min<size_t>(buf.size(), static_cast<size_t>(avail)));
                if (n <= 0) break;
                last_data = now_ms();
            }
            // Dropped, not kept: half a frame from before the jump must not be
            // glued onto the first frame after it.
            carry_len = 0;
            resync_.store(false, std::memory_order_release);
            continue;
        }

        if (!sess || reader_paused_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        auto ring = sess->ring();
        const auto abort = [this] {
            return quit_.load(std::memory_order_acquire) ||
                   reader_paused_.load(std::memory_order_acquire) ||
                   resync_.load(std::memory_order_acquire);
        };
        // THE rate limiter. See the class comment: without this the pipe is
        // drained faster than it fills and librespot never blocks.
        if (!ring->wait_for_room(kReadBytes / sizeof(float), abort)) continue;

        const long long n = proc->read_stdout(buf.data(), buf.size());
        if (n < 0) { state_.store(State::Dead, std::memory_order_release); return; }
        if (n == 0) {
            // Broken pipe: the process is gone. This is the ONLY end-of-stream
            // condition -- a paused Spotify just blocks the read above.
            state_.store(State::Dead, std::memory_order_release);
            std::lock_guard<std::mutex> lk(mu_);
            if (error_.empty()) error_ = "librespot: process exited unexpectedly";
            return;
        }
        last_byte_ms_.store(now_ms(), std::memory_order_release);
        if (state_.load(std::memory_order_acquire) == State::WaitingForDevice) {
            state_.store(State::Ready, std::memory_order_release);
        }

        // Reassemble whole frames across reads: at most (channels*4 - 1) bytes
        // are ever carried, and leftovers simply stay in the byte carry rather
        // than needing a second float-level buffer.
        std::memcpy(acc.data() + carry_len, buf.data(), static_cast<size_t>(n));
        const size_t total = carry_len + static_cast<size_t>(n);
        const size_t floats = total / sizeof(float);
        const size_t usable = (floats / kAudioChannels) * kAudioChannels;
        const size_t used = usable * sizeof(float);
        carry_len = total - used;

        if (usable > 0) {
            const float* samples = reinterpret_cast<const float*>(acc.data());
            size_t offset = 0;
            while (offset < usable) {
                long long written = written_cur_.load(std::memory_order_acquire);
                // Only as far as this track's own end -- the stream itself has
                // no boundary marker, so the frame count IS the boundary.
                size_t take = usable - offset;
                if (expected > 0) {
                    const long long room =
                        (expected - written) * static_cast<long long>(kAudioChannels);
                    if (room <= 0) take = 0;
                    else take = std::min<size_t>(take, static_cast<size_t>(room));
                }
                if (take > 0) {
                    ring->append(samples + offset, take);
                    sess->feed_envelope(static_cast<uint64_t>(written) * kAudioChannels,
                                        samples + offset, take);
                    written_cur_.store(written + static_cast<long long>(take / kAudioChannels),
                                       std::memory_order_release);
                    offset += take;
                }
                if (expected > 0 &&
                    written_cur_.load(std::memory_order_acquire) >= expected) {
                    // This track is complete. decode_done is what makes
                    // Player::finished() fire once playback catches up, which
                    // App turns into an advance.
                    ring->decode_done.store(true, std::memory_order_release);
                    std::lock_guard<std::mutex> lk(mu_);
                    if (plan_.size() > 1) {
                        plan_.pop_front();
                        sess = plan_.front().session;
                        expected = plan_.front().frames_expected;
                        ring = sess->ring();
                        written_cur_.store(plan_.front().origin_frames,
                                           std::memory_order_release);
                        boundary_seq_.fetch_add(1, std::memory_order_release);
                    } else {
                        // Nothing queued behind it: stop consuming rather than
                        // spill the next thing Spotify decides to play into this
                        // track's buffer.
                        plan_.pop_front();
                        sess.reset();
                        break;
                    }
                }
                if (take == 0) break;
            }
        }
        if (carry_len > 0) std::memmove(acc.data(), acc.data() + used, carry_len);
    }
}

} // namespace muisc
