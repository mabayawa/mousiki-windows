#pragma once
#include <algorithm>
#include <atomic>
#include <vector>

namespace muisc {

// A growing, single-producer/single-consumer PCM buffer: one decode
// thread appends to it while the audio callback (and, once decode
// finishes, the waveform pass) read whatever's been decoded so far.
//
// Capacity is reserved up front from ffprobe's duration estimate, and
// append() DELIBERATELY REFUSES to grow past that reserved capacity
// (it silently clamps instead of reallocating). That's not laziness —
// it's what makes the read side safe without a lock: once `available`
// is published (release-store), every element up to that count is
// guaranteed already written AND the vector's backing pointer is
// guaranteed to never have moved, because it never reallocates. A
// reader that does `size_t n = available.load(acquire); read data[0..n)`
// is race-free by construction. The tradeoff is a duration estimate
// that's wildly wrong (rare — ffprobe reads the container header, it's
// usually right) truncates the last bit of a track rather than risking
// a use-after-free on a mid-playback reallocation touched by another
// thread. That trade is worth it here.
// Every decode path produces this many interleaved channels, and the output
// device is opened with the same count. Upstream was mono end-to-end -- the
// ffmpeg path passed -ac 1 and miniaudio was configured for one channel -- so
// every stereo source, local FLAC included, was flattened before it was ever
// heard. One constant so the decoders, the device and the buffer can never
// disagree about the layout.
inline constexpr int kAudioChannels = 2;

struct StreamingPcm {
    std::vector<float> data;            // interleaved: L R L R ...
    std::atomic<size_t> available{0};   // SAMPLES safe to read (frames * channels)
    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};
    std::atomic<bool> capacity_exceeded{false}; // diagnostic only
    int sample_rate = 44100;
    int channels = kAudioChannels;

    // `available` and `data.size()` count samples, not frames; divide by
    // channels to get a frame index. Kept as samples because that is the unit
    // append() works in and the unit the lock-free publish protects.
    size_t frames_available() const {
        int ch = channels > 0 ? channels : 1;
        return available.load(std::memory_order_acquire) / static_cast<size_t>(ch);
    }

    void reserve_for_seconds(double seconds, int sr, int ch = kAudioChannels) {
        sample_rate = sr;
        channels = ch > 0 ? ch : 1;
        // Stereo doubles this: roughly 105 MB for a five-minute track before
        // the headroom, 132 MB with it. That is the cost of not downmixing,
        // and it is paid per track, not cumulatively.
        size_t est = static_cast<size_t>(std::max(1.0, seconds) * sr * channels * 1.25); // 25% headroom
        data.reserve(std::max<size_t>(est, static_cast<size_t>(sr) * channels * 5)); // at least 5s worth
    }

    // Decode thread only.
    void append(const float* samples, size_t count) {
        size_t room = data.capacity() - data.size();
        size_t n = std::min(count, room);
        if (n > 0) {
            data.insert(data.end(), samples, samples + n);
            available.store(data.size(), std::memory_order_release);
        }
        if (n < count) capacity_exceeded.store(true, std::memory_order_relaxed);
    }
};

} // namespace muisc
