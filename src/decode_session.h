#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "envelope_accumulator.h"
#include "pcm_ring.h"

namespace muisc {

namespace fs = std::filesystem;

class ChildProcess;

// One child process slot that another thread is allowed to kill.
//
// The owning thread is the only one that ever creates or reaps the child; a
// canceller only ever calls terminate(), and holds a shared_ptr while doing so
// so the object cannot be destroyed mid-call.
struct ChildSlot {
    std::mutex mu;
    std::shared_ptr<ChildProcess> child;
    void set(std::shared_ptr<ChildProcess> c);
    void kill_current();
};

// Stop token shared by a session's threads.
struct DecodeControl {
    std::atomic<bool>     cancel{false};
    std::atomic<uint64_t> seek_request{0};       // generation counter
    std::atomic<int64_t>  seek_target_frames{0};
    ChildSlot produce_child;
    ChildSlot scan_child;

    void request_cancel();
    // Publishes the target BEFORE bumping the generation, so a producer that
    // observes the new generation is guaranteed to read the matching target.
    void request_seek(int64_t frames);
};

// Owns everything needed to turn one track into audio: a bounded ring, the
// producer that fills it, and a separate pass that builds the waveform.
//
// Replaces the previous arrangement of a detached, uncancellable thread
// writing into a whole-track buffer. Two things forced the change. A producer
// held back by ring backpressure spends most of a track blocked, and pausing
// makes that its normal state -- so an uncancellable one is not merely wasteful
// (the old code let a skipped track's ffmpeg run to completion, as app.cpp's
// own comment conceded) but a thread and an 8 MiB ring leaked for good. And a
// backward seek past the retained history has to restart decoding at an
// offset, which needs a live handle on the producer.
class DecodeSession {
public:
    DecodeSession();
    ~DecodeSession();

    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;

    // `total_seconds <= 0` means the duration is unknown; the envelope then
    // adapts its bucket width instead of assuming a fixed length.
    // `want_scanner` builds the waveform on a second, unthrottled pass -- see
    // the note on scanner_main().
    void start(const fs::path& path, double start_sec, double total_seconds, bool want_scanner);

    // Same ring and envelope, but NO producer of our own: the samples arrive
    // from somewhere this class does not control -- librespot's stdout, whose
    // stream is decrypted Spotify audio we cannot decode a second time.
    //
    // Two consequences worth stating. There is no scanner, so the waveform can
    // only fill at playback speed as audio streams past; and the total is taken
    // from Spotify's reported duration rather than a container header.
    void start_external(double total_seconds);
    bool is_external() const { return external_; }

    // Called by the external feeder with the ABSOLUTE interleaved-sample index
    // of samples[0], exactly as the internal producer does.
    void feed_envelope(uint64_t first_sample, const float* samples, size_t count);

    // Cancels both threads, kills their children and joins. Idempotent.
    void shutdown();

    // Restart the producer at an absolute frame. Cheap: it reuses the running
    // producer thread rather than spawning one per seek.
    void request_seek(int64_t frame);

    const std::shared_ptr<PcmRing>& ring() const { return ring_; }

    // The best envelope available right now: the full-speed scan once it has
    // finished, otherwise whatever the playback producer has streamed past so
    // far (which fills in real time, and is better than an empty panel).
    std::vector<EnvelopeBin> best_bins() const;
    bool scan_complete() const { return scan_done_.load(std::memory_order_acquire); }

private:
    void producer_main(fs::path path, double start_sec);
    void scanner_main(fs::path path);

    std::shared_ptr<PcmRing> ring_;
    DecodeControl ctl_;
    std::thread producer_, scanner_;
    EnvelopeAccumulator live_env_, scan_env_;
    std::atomic<bool> scan_done_{false};
    std::atomic<bool> started_{false};
    bool external_ = false;
    uint64_t total_samples_ = 0;
};

} // namespace muisc
