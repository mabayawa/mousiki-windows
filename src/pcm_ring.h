#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace muisc {

// Every decode path produces this many interleaved channels, and the output
// device is opened with the same count. Upstream was mono end-to-end -- the
// ffmpeg path passed -ac 1 and miniaudio was configured for one channel -- so
// every stereo source, local FLAC included, was flattened before it was ever
// heard. One constant so the decoders, the device and the buffer can never
// disagree about the layout.
inline constexpr int kAudioChannels = 2;

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------
// This replaced a whole-track std::vector sized from ffprobe's duration, which
// cost roughly 105 MB of resident memory for a five-minute stereo track and
// could not represent a stream of unknown length at all -- the reason Spotify
// audio had nowhere to go.
//
// The split matters as much as the total. The producer is held to a bounded
// LEAD over the play cursor, which leaves the rest of the ring as HISTORY
// behind it. That retained history is what lets a backward seek be a cursor
// move instead of restarting the decoder, so the -5s key stays instant.
inline constexpr size_t kRingFrames       = size_t{1} << 20;               // 1,048,576 frames = 23.78 s @44.1k
inline constexpr size_t kRingSamples      = kRingFrames * kAudioChannels;  // 8.0 MiB of f32
inline constexpr size_t kRingMask         = kRingSamples - 1;              // power of two -> & instead of %
inline constexpr size_t kHistoryFrames    = size_t{1} << 19;               //   524,288 frames = 11.89 s retained
inline constexpr size_t kLookaheadSamples = (kRingFrames - kHistoryFrames) * kAudioChannels;

// A bounded single-producer/single-consumer PCM ring.
//
// ---------------------------------------------------------------------------
// The memory-ordering contract -- the load-bearing part
// ---------------------------------------------------------------------------
// Positions are ABSOLUTE interleaved-sample indices (uint64_t, monotonic
// within an epoch); the slot for one is `abs & kRingMask`. Absolute counters
// rather than wrapped ones keep the arithmetic total-order and mean nothing
// but an epoch change ever moves a position backwards.
//
// Producer, in append():
//   1. w = valid_hi_(relaxed)          -- sole writer, so relaxed is correct
//   2. write the samples into the slots
//   3. valid_lo_.store(release)        -- the reclaim floor, BEFORE hi
//   4. valid_hi_.store(release)        -- the single publication point
//
// Consumer, in read():
//   1. hi = valid_hi_(acquire)   <-- FIRST. Pairs with the producer's step 4:
//                                    every sample below `hi` was fully written
//                                    before this load could observe it.
//   2. lo = valid_lo_(acquire)   <-- SECOND, and the order is not incidental.
//                                    Loaded later, `lo` is at least as fresh
//                                    as `hi`, so [lo, hi) is always a SUBSET
//                                    of a genuinely valid window. Load them
//                                    the other way round and you can be handed
//                                    a window whose bottom has already been
//                                    recycled underneath you.
//   3. read [max(cursor, lo), hi), zero-fill the rest
//   4. retain_floor_.store(release)    -- tells the producer how far it may go
//
// Producer, in wait_for_room():
//   5. r = retain_floor_(acquire); proceed only while (hi - r) + want <= kLookaheadSamples
//
// Step 5 IS the history guarantee, on its own. Resident data is always
// [hi - kRingSamples, hi); the gate forces hi - r <= lookahead, and therefore
// r - kHistoryFrames*ch >= hi - kRingSamples. Everything from kHistoryFrames
// behind the cursor up to the write head is resident by construction, with no
// separate bookkeeping to get wrong.
//
// There is no mutex and no condition variable anywhere near the consumer: the
// audio callback would have to call notify_one(), which is not wait-free and
// has no business in a WASAPI/OpenSL callback. wait_for_room() polls instead.
class PcmRing {
public:
    PcmRing() : data_(kRingSamples, 0.0f) {}

    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    // ---- consumer (audio callback) -- never blocks, never allocates -------

    // First line of the callback, above every early return: begin_epoch()
    // waits on these ticks to know a reader has been through, and a paused
    // callback that never ticked would stall a seek.
    void tick() { reader_ticks_.fetch_add(1, std::memory_order_relaxed); }

    // Copies `frames` frames starting at absolute frame `first_frame` into
    // `dst`, zero-filling anything not currently resident. Returns the number
    // of frames actually backed by real data.
    size_t read(int64_t first_frame, float* dst, size_t frames) const;

    // How far the consumer has played. Publishing this is what releases ring
    // space back to the producer.
    void publish_cursor(int64_t frame) {
        const uint64_t s = frame > 0 ? static_cast<uint64_t>(frame) * channels_u() : 0;
        retain_floor_.store(s, std::memory_order_release);
    }

    // Highest absolute frame the producer has published. Used by the seek
    // router to decide "cursor move" vs "restart the producer".
    int64_t decoded_hi_frames() const {
        return static_cast<int64_t>(valid_hi_.load(std::memory_order_acquire) / channels_u());
    }

    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    // ---- producer (decode thread) ----------------------------------------

    // Blocks until there is room for `want_samples`, or `abort` returns true.
    // Returns false if it gave up so the caller can re-test cancel/seek.
    bool wait_for_room(size_t want_samples, const std::function<bool()>& abort);

    void append(const float* samples, size_t count);

    // Restarts the stream at a new origin after a seek. See the comment on the
    // definition: the ORDER of the stores is what keeps an in-flight callback
    // from reading recycled slots.
    void begin_epoch(int64_t first_frame);

    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};

    int sample_rate = 44100;
    int channels    = kAudioChannels;

private:
    uint64_t channels_u() const {
        return static_cast<uint64_t>(channels > 0 ? channels : 1);
    }

    std::vector<float> data_;                      // kRingSamples, allocated once, never resized
    std::atomic<uint64_t> valid_lo_{0};            // oldest readable absolute sample
    std::atomic<uint64_t> valid_hi_{0};            // one past newest; 0 == window closed
    std::atomic<uint64_t> retain_floor_{0};        // where the consumer is
    std::atomic<uint64_t> reader_ticks_{0};        // callback liveness, for the epoch handshake
    mutable std::atomic<uint64_t> underruns_{0};   // diagnostic: frames we had to zero-fill
};

} // namespace muisc
