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
    plan_epoch_.fetch_add(1, std::memory_order_release);
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
    // Realignment padding owed to the stream: bytes to discard from the next
    // read so that frame boundaries land where they actually are. Set by the
    // resync drain below, which is the only thing that can consume a partial
    // frame's worth of bytes without emitting it.
    size_t skip_bytes = 0;
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
        int epoch = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            epoch = plan_epoch_.load(std::memory_order_acquire);
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
            long long dropped = 0;
            bool went_quiet = false;
            while (now_ms() < deadline) {
                if (now_ms() - last_data >= 250) { went_quiet = true; break; }
                const long long avail = proc->bytes_available();
                if (avail < 0) break;
                if (avail == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const long long n = proc->read_stdout(
                    buf.data(), std::min<size_t>(buf.size(), static_cast<size_t>(avail)));
                if (n <= 0) break;
                dropped += n;
                last_data = now_ms();
            }
            // Dropped, not kept: half a frame from before the jump must not be
            // glued onto the first frame after it.
            //
            // But dropping a partial frame's worth of BYTES is not the same as
            // dropping a whole number of frames, and only the second is safe.
            // Frame boundaries here are positions in the byte stream, nothing
            // more -- there are no markers (see the class comment) -- so the
            // reader's idea of where a frame starts survives only if every byte
            // it consumes without emitting is a multiple of the frame size.
            // Neither half of what just got consumed is: bytes_available()
            // reports whatever the OS pipe happens to hold (PeekNamedPipe /
            // FIONREAD, no alignment guarantee), read_stdout may short-read on
            // top of that, and carry_len is by definition a partial frame. So
            // seven times in eight the stream resumed mid-frame and stayed
            // shifted for the rest of the session -- audible as loud static.
            // That is the same corruption the class comment describes for the
            // 806-byte OAuth banner, reintroduced by the code that cleans up
            // after a track switch or a seek, which is exactly when a user
            // hears it: interrupt a playing track, get static.
            //
            // So account for both and owe the difference to the next read.
            // Deliberately not a second read loop here: the caller has paused
            // Spotify before asking for this, so a blocking read could park
            // indefinitely -- the reason this drain is availability-polled at
            // all. Carrying the remainder forward cannot block.
            constexpr size_t kFrameBytes = kAudioChannels * sizeof(float);
            const size_t orphaned = carry_len + static_cast<size_t>(dropped);
            carry_len = 0;
            skip_bytes = (kFrameBytes - (orphaned % kFrameBytes)) % kFrameBytes;
            // Which way this ended matters to the caller. Quiet means the stream
            // really did stop, so whatever arrives next belongs to what was asked
            // for after the drain. The 2 s cap means it never stopped -- the pause
            // did not take effect -- and the next bytes may still be the outgoing
            // track's. Published BEFORE resync_ is cleared, since resync_ going
            // down is the signal the caller waits on.
            resync_quiet_.store(went_quiet, std::memory_order_release);
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

        // A bounded availability poll BEFORE the blocking read, so that quit,
        // reader_paused_ and resync_ are observed within ~10 ms rather than
        // "whenever the next byte happens to arrive".
        //
        // This used to be harmless: nothing waited on the drain, so a reader
        // parked inside read_stdout simply woke late. App now stages a track
        // switch as pause -> WAIT for the drain -> install the new plan, and the
        // whole point of the wait is that Spotify has been paused -- so there may
        // be no next byte at all. Usually the flags are set while bytes are still
        // flowing (the pause has only been queued at that point, so the read
        // returns within milliseconds) and this changes nothing; the case it
        // fixes is a stream that had already gone quiet on its own, for instance
        // because the user paused from their phone.
        //
        // Bounded, and then the blocking read happens anyway, because
        // bytes_available() cannot portably tell a quiet pipe from a closed one:
        // PeekNamedPipe reports a broken pipe as an error, but ioctl(FIONREAD)
        // on POSIX just reports 0 bytes. Only read_stdout() returning 0
        // distinguishes them on every platform, and losing that would mean a
        // librespot that died mid-track was never noticed on the Linux build.
        // The residual case -- a stream that goes quiet later than this window
        // and a switch after that -- is covered by App's own drain timeout.
        //
        // None of this weakens the rate limiter: wait_for_room() above is still
        // what applies backpressure, and the sleep below only runs when the pipe
        // is empty, which is precisely when there is nothing to pace.
        {
            const long long poll_until = now_ms() + 2000;
            long long avail = proc->bytes_available();
            while (avail == 0 && !abort() && now_ms() < poll_until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                avail = proc->bytes_available();
            }
            if (avail < 0) { state_.store(State::Dead, std::memory_order_release); return; }
            // A pause/resync/quit landed while we waited: go round and act on it
            // rather than reading bytes that belong to a plan on its way out.
            if (avail == 0 && abort()) continue;
        }

        const long long n = proc->read_stdout(buf.data(), buf.size());
        if (n < 0) { state_.store(State::Dead, std::memory_order_release); return; }
        if (n == 0) {
            // Broken pipe: the process is gone. This is STILL the ONLY
            // end-of-stream condition -- a paused Spotify just blocks the read
            // above, exactly as before.
            state_.store(State::Dead, std::memory_order_release);
            std::lock_guard<std::mutex> lk(mu_);
            if (error_.empty()) error_ = "librespot: process exited unexpectedly";
            return;
        }
        last_byte_ms_.store(now_ms(), std::memory_order_release);
        if (state_.load(std::memory_order_acquire) == State::WaitingForDevice) {
            state_.store(State::Ready, std::memory_order_release);
        }

        // Realignment padding owed from a resync drain, discarded before any of
        // it can be mistaken for the start of a frame.
        size_t off = 0;
        if (skip_bytes > 0) {
            off = std::min(skip_bytes, static_cast<size_t>(n));
            skip_bytes -= off;
            if (off == static_cast<size_t>(n)) continue;   // the whole read was padding
        }

        // Was this read overtaken by a reset_plan()? The main thread fires
        // set_reader_paused / request_resync / reset_plan back to back with no
        // wait, and this thread can already be parked in read_stdout past both
        // flag checks -- so these bytes can belong to a plan that no longer
        // exists. Writing them would store written_cur_ over the origin
        // reset_plan() just published, leaving the incoming track's
        // (expected - written) room short by that much, so it stops being fed
        // early and ends truncated.
        //
        // This suppresses only the WRITE. The frame accounting below still
        // runs, so carry_len stays a partial frame and the stream stays
        // aligned; dropping the bytes outright would leave them unaccounted and
        // shift every frame after them, which is the bug the drain above just
        // got fixed for.
        const bool stale_plan = (plan_epoch_.load(std::memory_order_acquire) != epoch);

        // Reassemble whole frames across reads: at most (channels*4 - 1) bytes
        // are ever carried, and leftovers simply stay in the byte carry rather
        // than needing a second float-level buffer.
        const size_t got = static_cast<size_t>(n) - off;
        std::memcpy(acc.data() + carry_len, buf.data() + off, got);
        const size_t total = carry_len + got;
        const size_t floats = total / sizeof(float);
        const size_t usable = (floats / kAudioChannels) * kAudioChannels;
        const size_t used = usable * sizeof(float);
        carry_len = total - used;

        if (usable > 0 && !stale_plan) {
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
