#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "decode_session.h"
#include "pcm_ring.h"

namespace muisc {

namespace fs = std::filesystem;

class ChildProcess;

// One track librespot is expected to produce, in order.
struct LibrespotTrack {
    std::string uri;
    long long frames_expected = 0;   // from Spotify's duration_ms
    // Absolute frame this stream begins at. Non-zero only after a seek, where
    // librespot resumes mid-track: the ring is indexed by absolute track frame,
    // so writing post-seek audio at 0 would put it in the wrong place and shift
    // every waveform bin with it.
    long long origin_frames = 0;
    std::shared_ptr<DecodeSession> session;   // owns the ring this track fills
};

// Owns a long-lived librespot process and the thread that turns its stdout into
// PCM in mousiki's rings.
//
// ---------------------------------------------------------------------------
// The one thing that makes this non-obvious: the pipe has no clock
// ---------------------------------------------------------------------------
// librespot's pipe backend is a bare write_all to stdout -- there is no rate
// limiting anywhere in that path. With a real audio device the sink blocking at
// 1x IS librespot's clock; with `--backend pipe` into a consumer that drains
// greedily, nothing holds it back, and it will emit a whole track as fast as it
// can download it.
//
// That is not merely wasteful. librespot reports its Spotify-side progress from
// samples written to the sink, so Spotify would see the track finish in seconds,
// fire end_of_track, roll into whatever is queued next, and blow through the
// queue at ~10x while the user is still hearing the first bars -- with scrobbles
// and "recently played" to match.
//
// So MOUSIKI has to be the clock, and the ring is what makes it one: the reader
// calls PcmRing::wait_for_room() before every read, which blocks once it is a
// lookahead ahead of the play cursor. The ~64 KiB OS pipe then fills, write_all
// blocks, and librespot is held to playback speed. The backpressure that exists
// to bound memory turns out to be the rate limiter too.
//
// Track boundaries are counted, not delimited: the stream carries no markers, so
// each track ends after exactly frames_expected frames and the reader moves on
// to the next ring. A STALL IS NOT EOF -- when nothing is playing librespot
// simply writes nothing and the read blocks; only a broken pipe (read returns 0)
// means the process is gone.
//
// ---------------------------------------------------------------------------
// Why authentication happens in a separate process first
// ---------------------------------------------------------------------------
// librespot's OAuth flow prints to STDOUT:
//
//     Browse to: https://accounts.spotify.com/authorize?response_type=code&...
//
// 806 bytes of ASCII, measured on 0.8.0. Under `--backend pipe` that is the
// same stream the audio comes out of, so on a user's very first play it lands
// in the PCM. The audible result is a loud click -- those bytes read as floats
// with peak ~3.4e38 -- but the lasting damage is worse: 806 is not a multiple
// of the 8-byte stereo frame, so EVERY following frame is shifted by 6 bytes
// and the channels stay corrupted for the whole session.
//
// So when no cached credentials exist, a throwaway librespot is run first with
// its stdout pointed at a file, purely to complete the browser flow and write
// credentials.json. Only then is the real pipe session started, by which point
// `--enable-oauth` is a no-op and stdout carries nothing but audio.
class LibrespotSession {
public:
    enum class State { Idle, Starting, Authenticating, WaitingForDevice, Ready, Failed, Dead };

    ~LibrespotSession();

    struct Config {
        fs::path exe;
        fs::path cache_dir;      // credentials only; the audio cache stays off
        fs::path log_path;       // librespot's stderr
        std::string device_name = "mousiki";
        int bitrate = 320;
    };

    bool start(const Config& cfg, std::string* error_out);
    void shutdown();

    State state() const { return state_.load(std::memory_order_acquire); }
    bool alive() const;
    std::string last_error() const;
    const std::string& device_name() const { return cfg_.device_name; }

    void mark_ready() { state_.store(State::Ready, std::memory_order_release); }
    void mark_waiting_for_device() { state_.store(State::WaitingForDevice, std::memory_order_release); }

    // Replaces the plan: used when starting a track, and after a seek resync.
    void reset_plan(LibrespotTrack first);
    // Appends the track Spotify has been asked to play next, so the reader can
    // cross the boundary without a gap.
    void append_plan(LibrespotTrack next);
    bool has_next() const;
    std::string current_uri() const;

    long long frames_written_current() const {
        return written_cur_.load(std::memory_order_acquire);
    }
    // Bumps every time the reader crosses into the next planned track.
    int boundary_seq() const { return boundary_seq_.load(std::memory_order_acquire); }

    // Stops consuming without killing the process. The pipe then fills and
    // librespot blocks -- which is what "paused" means on this transport.
    void set_reader_paused(bool paused) {
        reader_paused_.store(paused, std::memory_order_release);
    }

    // Asks the READER THREAD to read and discard until the stream goes quiet.
    //
    // Deliberately a request rather than a synchronous call: after a Web API
    // seek the bytes already in flight predate the jump and must be thrown away,
    // but draining them from the caller's thread would mean two threads reading
    // one pipe at once -- and the reader can already be blocked inside
    // read_stdout at that moment, so the race is not even narrow. The reader
    // owns the pipe; only it ever reads.
    void request_resync();

    // The URL librespot printed for the browser flow, if one was needed. Worth
    // surfacing: the automatic browser open can silently fail, and then this is
    // the only way for the user to get to the prompt.
    std::string auth_url() const;
    bool resync_pending() const { return resync_.load(std::memory_order_acquire); }

    double seconds_since_last_byte() const;

private:
    void reader_main();

    // Spawns a throwaway librespot whose only job is to complete the browser
    // flow and leave credentials.json behind. Returns false if it gave up.
    bool ensure_credentials();
    bool spawn_audio_child();
    std::shared_ptr<ChildProcess> get_proc() const;
    void set_proc(std::shared_ptr<ChildProcess> p);

    Config cfg_;
    // Assigned by the reader thread and read by shutdown(), hence the mutex.
    mutable std::mutex proc_mu_;
    std::shared_ptr<ChildProcess> proc_;
    std::thread reader_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> reader_paused_{false};
    std::atomic<bool> resync_{false};
    std::atomic<State> state_{State::Idle};
    std::atomic<long long> written_cur_{0};
    std::atomic<int> boundary_seq_{0};
    std::atomic<long long> last_byte_ms_{0};

    mutable std::mutex mu_;
    std::deque<LibrespotTrack> plan_;
    std::string error_;
    std::string auth_url_;
};

} // namespace muisc
