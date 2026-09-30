#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace muisc {

// One bin of the waveform envelope, kept in RAW, PRE-NORMALIZATION form.
//
// Storing the raw accumulators rather than the finished envelope is what
// makes re-rendering lossless. generate_high_res_envelope() ends with
// smooth -> normalize-by-global-max -> pow(x, 2.5); re-running that chain on
// an envelope that has already been through it would smooth an
// already-smoothed signal, renormalize an already-normalized one and apply
// the contrast curve twice, crushing every quiet passage. From (sum_sq,
// count) the exact same output falls out every time, for any `smooth` flag
// and any terminal width.
//
// 4096 bins x 12 bytes = 48 KiB per track, versus the ~105 MB of raw PCM the
// old whole-track buffer had to keep alive purely so the waveform could be
// recomputed on a resize.
struct EnvelopeBin {
    float    sum_sq = 0.0f;  // sum of s*s over the interleaved samples in this bin
    float    peak   = 0.0f;  // max |s| -- not used by the RMS envelope, kept for future use
    uint32_t count  = 0;     // interleaved samples accumulated
};

// Builds the waveform envelope incrementally as audio streams past, instead
// of requiring the whole decoded track to be resident at once.
//
// Bins are addressed by ABSOLUTE interleaved-sample index, which is what
// makes it correct across a seek: a producer restarted at 2:30 reports
// absolute positions, so its samples land in the bins they belong to rather
// than at the front. Fills may arrive out of order or with gaps; a gap simply
// leaves count == 0, which renders as silence -- honest for "not decoded
// yet", and not something interpolation should invent transients into.
//
// Matches generate_high_res_envelope()'s bucketing exactly: chunk size floors
// and any sample past resolution*chunk_size is dropped, the same trailing
// `size % resolution` samples the original discards.
class EnvelopeAccumulator {
public:
    // total_samples == 0 means "duration unknown", which switches on adaptive
    // mode: the accumulator starts from an assumption and merges bin pairs as
    // the track turns out longer, then stretches the filled portion back
    // across the full width in finalize().
    //
    // With a known total it does NOT adapt. It buckets exactly the way
    // generate_high_res_envelope() does -- floor the chunk size, and discard
    // anything past resolution*chunk_size -- because a floored chunk size
    // means the last few thousand samples fall outside the final bin, and
    // "growing to fit" them would silently halve the resolution of every
    // normal track. Dropping them is what the original does, and matching it
    // is what makes the two paths produce identical output.
    void reset(int resolution, uint64_t total_samples);

    // Feeder thread only. `first_sample` is the absolute interleaved-sample
    // index of samples[0].
    void add(uint64_t first_sample, const float* samples, size_t count);

    // Called once the true end is known. If the estimate was wrong, re-buckets
    // so the waveform spans exactly [0, observed_total_samples) instead of
    // trailing off into empty bins at 80% across.
    void finalize(uint64_t observed_total_samples);

    // Any thread. A 48 KiB copy under the lock.
    std::vector<EnvelopeBin> snapshot() const;

    // Fraction of bins with any data, 0..1. Lets the UI tell a complete
    // envelope from one that is still filling in.
    double coverage() const;

    bool empty() const;

private:
    void rebucket_locked(uint64_t new_chunk_size);
    void stretch_locked(uint64_t observed_total_samples);

    mutable std::mutex mu_;
    std::vector<EnvelopeBin> bins_;
    int      resolution_ = 0;
    uint64_t chunk_size_ = 1;
    uint64_t max_sample_seen_ = 0;
    bool     adaptive_ = false;   // true only when the duration was unknown
};

} // namespace muisc
