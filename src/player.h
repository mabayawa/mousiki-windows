#pragma once
#include <atomic>
#include <memory>
#include "miniaudio.h"
#include "pcm_ring.h"
#include "fft_visualizer.h"

namespace muisc {

// Plays back a PcmRing through a real audio device via
// miniaudio, using the backend pinned in audio_backend.h
// (PulseAudio/ALSA -> PipeWire on Linux, WASAPI on Windows, OpenSL ES on
// Android).
//
// This reads from a buffer that may STILL BE FILLING IN — play() can be
// called the moment decode starts (as soon as duration is known from
// ffprobe and the buffer's capacity is reserved), and the callback below
// just plays silence for any frame past what's been decoded so far,
// self-correcting once the decode thread catches up. That's what lets
// playback start almost immediately instead of waiting for the whole
// track to decode first.
//
// This replaced an earlier ffplay-subprocess design. ffplay's audio
// output goes through SDL, and SDL's Android backend expects to be
// running inside a proper Activity with its Java glue — a plain Termux
// CLI process has neither, so SDL_OpenAudioDevice effectively never
// succeeds there and nothing plays, silently. Talking to OpenSL ES
// directly through miniaudio sidesteps that entirely.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // `pcm` must stay alive for as long as playback is active — App holds
    // it via shared_ptr and only replaces it once stop() has fully torn
    // the device down. `fft_sink`, if given, gets push_samples() called
    // from the audio callback with each chunk actually played (nullptr
    // to disable — e.g. not needed for a plain smoke test).
    bool play(std::shared_ptr<PcmRing> pcm, double start_sec, int volume_pct,
              FftVisualizer* fft_sink = nullptr);

    // Switches to another buffer WITHOUT tearing the audio device down.
    //
    // play() cannot be used at a gapless boundary: its first act is stop(),
    // i.e. ma_device_uninit followed by a fresh init, which is tens of
    // milliseconds of silence -- audible as a gap between album tracks, which
    // is the exact thing gapless playback exists to avoid.
    //
    // Safe against the live callback by double-buffering: the callback reads
    // whichever slot `active_slot_` names, and this fills the OTHER slot before
    // flipping it. Nothing the callback might be mid-read on is reassigned, and
    // the outgoing buffer stays alive in its slot rather than being freed
    // underneath it.
    void adopt_ring(std::shared_ptr<PcmRing> ring, double start_sec);

    void pause();
    void resume();
    bool is_paused() const { return paused_; }

    // Seek within what the ring still holds. Returns false when the target is
    // outside it, which means the caller must restart the producer at that
    // offset instead (App::seek_to does exactly that).
    bool try_seek_in_window(double target_sec);
    // Move the cursor for a seek that IS being served by a producer restart.
    // Must run BEFORE the restart is requested: it clears finished_, without
    // which the empty window the restart briefly creates is read as
    // end-of-track and the run loop skips to the next song.
    void rebase_for_restart(double target_sec);
    void set_volume(int volume_pct);
    int volume() const { return volume_pct_.load(); }

    double poll_elapsed() const;
    double position_seconds() const { return poll_elapsed(); }
    bool finished() const { return finished_.load(); }
    // Synchronously clears a stale finished flag left over from the
    // previous track. play() itself resets this too, but play() now
    // runs on a detached background thread (device init can genuinely
    // stall) — without this, there's a window where has_track_ is
    // already true for the NEW track but finished_ is still true from
    // the OLD one, and the main loop's "if (has_track_ && finished())
    // advance_track()" check fires again immediately, skipping straight
    // past the track that was just supposed to start.
    void clear_finished() { finished_.store(false); }

    void stop();

private:
    ma_context context_{};
    bool context_ready_ = false;
    ma_device device_{};
    bool device_ready_ = false;

    // Two slots rather than one pointer, so adopt_ring() can hand the callback
    // a new buffer without a lock and without freeing the old one.
    std::shared_ptr<PcmRing> pcm_slots_[2];
    std::atomic<int> active_slot_{0};
    FftVisualizer* fft_sink_ = nullptr;
    // These are read from the main/render thread every frame while the
    // device worker thread may be inside play(). They are atomics rather than
    // mutex-protected state on purpose: a mutex held across ma_device_init()
    // would block the render loop for however long device initialisation
    // stalls, which is the exact thing running play() off-thread exists to
    // avoid.
    std::atomic<int> sample_rate_{44100};
    std::atomic<long long> cursor_frames_{0};
    std::atomic<bool> finished_{false};
    std::atomic<float> gain_{0.7f};
    std::atomic<bool> paused_{false};
    std::atomic<int> volume_pct_{70};
    // Highest frame the producer has published, mirrored out of the ring by
    // the audio callback. The main thread needs this to decide whether a
    // forward seek is already decoded, and mirroring it through an atomic
    // preserves the rule above: the main thread never dereferences pcm_, which
    // the device worker may be reassigning at the same moment.
    std::atomic<long long> decoded_hi_frames_{0};

    static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
};

} // namespace muisc
