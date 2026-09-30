#include "envelope_accumulator.h"

#include <algorithm>
#include <cmath>

namespace muisc {

namespace {
// Used when the duration is genuinely unknown. Deliberately the same 300 s
// the old reserve_for_seconds() fallback assumed -- except that where that
// number silently TRUNCATED anything longer, here it only sets the starting
// bucket width, and rebucket_locked() grows it as the track turns out longer.
constexpr uint64_t kAssumedSamples = 300ull * 44100ull * 2ull;
} // namespace

void EnvelopeAccumulator::reset(int resolution, uint64_t total_samples) {
    std::lock_guard<std::mutex> lk(mu_);
    resolution_ = std::max(1, resolution);
    bins_.assign(static_cast<size_t>(resolution_), EnvelopeBin{});
    adaptive_ = (total_samples == 0);
    uint64_t total = adaptive_ ? kAssumedSamples : total_samples;
    chunk_size_ = std::max<uint64_t>(1, total / static_cast<uint64_t>(resolution_));
    max_sample_seen_ = 0;
}

void EnvelopeAccumulator::add(uint64_t first_sample, const float* samples, size_t count) {
    if (!samples || count == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (bins_.empty()) return;

    const uint64_t last = first_sample + count - 1;
    if (last > max_sample_seen_) max_sample_seen_ = last;

    // Only when the duration was unknown: the track is longer than assumed, so
    // merge bin pairs until it fits rather than dropping the tail. O(resolution)
    // per doubling, at most log2 times over a track.
    //
    // Deliberately NOT done when the total is known. A floored chunk size
    // leaves the last few thousand samples past the final bin on almost every
    // track (e.g. 5292000 samples -> chunk 1291 -> last sample lands in bin
    // 4099), and growing to absorb them would halve the resolution of every
    // file. The loop below drops them instead, exactly as the resident-PCM
    // path does.
    if (adaptive_) {
        while (last / chunk_size_ >= static_cast<uint64_t>(resolution_)) {
            rebucket_locked(chunk_size_ * 2);
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const uint64_t bin = (first_sample + i) / chunk_size_;
        if (bin >= static_cast<uint64_t>(resolution_)) break;
        EnvelopeBin& b = bins_[static_cast<size_t>(bin)];
        const float s = samples[i];
        // float, not double, on purpose -- see the PERF note in
        // generate_high_res_envelope(): promoting to double here blocked ARM
        // NEON auto-vectorization and roughly halved throughput, and the
        // output is quantized to six visual levels regardless.
        b.sum_sq += s * s;
        const float a = std::fabs(s);
        if (a > b.peak) b.peak = a;
        b.count += 1;
    }
}

void EnvelopeAccumulator::rebucket_locked(uint64_t new_chunk_size) {
    if (new_chunk_size <= chunk_size_) return;
    const uint64_t factor = new_chunk_size / chunk_size_;
    std::vector<EnvelopeBin> merged(bins_.size(), EnvelopeBin{});
    for (size_t i = 0; i < bins_.size(); ++i) {
        if (bins_[i].count == 0) continue;
        const size_t dst = static_cast<size_t>(i / factor);
        if (dst >= merged.size()) break;
        merged[dst].sum_sq += bins_[i].sum_sq;
        merged[dst].count  += bins_[i].count;
        merged[dst].peak    = std::max(merged[dst].peak, bins_[i].peak);
    }
    bins_.swap(merged);
    chunk_size_ = new_chunk_size;
}

void EnvelopeAccumulator::finalize(uint64_t observed_total_samples) {
    std::lock_guard<std::mutex> lk(mu_);
    if (bins_.empty() || observed_total_samples == 0) return;
    // With a known duration the bucketing was already right; touching it here
    // would only break the exact match with the resident-PCM path.
    if (!adaptive_) return;
    stretch_locked(observed_total_samples);
}

// Adaptive mode only. The assumed duration was too long, so the real audio
// occupies just the first slice of the bins and the waveform would render as
// ending partway across. Rescale that filled slice back over the full width.
//
// Narrowing chunk_size_ directly is impossible -- that would mean
// redistributing individual samples that are no longer held -- so this
// resamples the per-bin RMS instead and stores it back as (rms^2, 1). That
// round-trips exactly through envelope_from_bins(), which computes
// sqrt(sum_sq / count).
void EnvelopeAccumulator::stretch_locked(uint64_t observed_total_samples) {
    const size_t res = bins_.size();
    size_t used = static_cast<size_t>(observed_total_samples / chunk_size_) + 1;
    if (used > res) used = res;
    if (used == 0 || used == res) return;

    std::vector<float> rms(used, 0.0f);
    for (size_t i = 0; i < used; ++i) {
        if (bins_[i].count > 0) {
            rms[i] = std::sqrt(bins_[i].sum_sq / static_cast<float>(bins_[i].count));
        }
    }

    std::vector<EnvelopeBin> out(res, EnvelopeBin{});
    for (size_t i = 0; i < res; ++i) {
        const double src = (static_cast<double>(i) * static_cast<double>(used - 1)) /
                           static_cast<double>(res - 1);
        const size_t  i0 = static_cast<size_t>(src);
        const size_t  i1 = std::min(i0 + 1, used - 1);
        const float   t  = static_cast<float>(src - static_cast<double>(i0));
        const float   v  = rms[i0] * (1.0f - t) + rms[i1] * t;
        out[i].sum_sq = v * v;
        out[i].count  = 1;
        out[i].peak   = std::max(bins_[i0].peak, bins_[i1].peak);
    }
    bins_.swap(out);
    chunk_size_ = std::max<uint64_t>(1, observed_total_samples / static_cast<uint64_t>(res));
}

std::vector<EnvelopeBin> EnvelopeAccumulator::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bins_;
}

double EnvelopeAccumulator::coverage() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (bins_.empty()) return 0.0;
    size_t filled = 0;
    for (const auto& b : bins_) if (b.count > 0) ++filled;
    return static_cast<double>(filled) / static_cast<double>(bins_.size());
}

bool EnvelopeAccumulator::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& b : bins_) if (b.count > 0) return false;
    return true;
}

} // namespace muisc
