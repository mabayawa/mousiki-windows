#include "pcm_ring.h"

#include <chrono>
#include <thread>

namespace muisc {

size_t PcmRing::read(int64_t first_frame, float* dst, size_t frames) const {
    const uint64_t ch = channels_u();
    const size_t want_samples = frames * static_cast<size_t>(ch);
    if (!dst || frames == 0) return 0;

    // Order matters -- see the contract in the header. `hi` first, so the
    // `lo` we pair it with is never staler than the window it bounds.
    const uint64_t hi = valid_hi_.load(std::memory_order_acquire);
    const uint64_t lo = valid_lo_.load(std::memory_order_acquire);

    if (first_frame < 0 || hi == 0 || lo >= hi) {
        std::memset(dst, 0, want_samples * sizeof(float));
        underruns_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    const uint64_t req_lo = static_cast<uint64_t>(first_frame) * ch;
    const uint64_t req_hi = req_lo + want_samples;

    const uint64_t src_lo = std::max(req_lo, lo);
    const uint64_t src_hi = std::min(req_hi, hi);
    if (src_lo >= src_hi) {
        std::memset(dst, 0, want_samples * sizeof(float));
        underruns_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    // Zero the parts that fall outside what is resident: a prefix already
    // recycled, and/or a tail not yet produced.
    if (src_lo > req_lo) {
        std::memset(dst, 0, static_cast<size_t>(src_lo - req_lo) * sizeof(float));
    }
    if (src_hi < req_hi) {
        std::memset(dst + (src_hi - req_lo), 0,
                    static_cast<size_t>(req_hi - src_hi) * sizeof(float));
    }

    // At most two contiguous runs -- the wrap point is computed once rather
    // than masking per sample.
    const size_t n     = static_cast<size_t>(src_hi - src_lo);
    const size_t start = static_cast<size_t>(src_lo & kRingMask);
    const size_t first = std::min(n, kRingSamples - start);
    std::memcpy(dst + (src_lo - req_lo), data_.data() + start, first * sizeof(float));
    if (n > first) {
        std::memcpy(dst + (src_lo - req_lo) + first, data_.data(), (n - first) * sizeof(float));
    }

    const size_t got_frames = n / static_cast<size_t>(ch);
    if (got_frames < frames) {
        underruns_.fetch_add(frames - got_frames, std::memory_order_relaxed);
    }
    return got_frames;
}

bool PcmRing::wait_for_room(size_t want_samples, const std::function<bool()>& abort) {
    for (;;) {
        if (abort && abort()) return false;
        const uint64_t w = valid_hi_.load(std::memory_order_relaxed);
        const uint64_t r = retain_floor_.load(std::memory_order_acquire);
        const uint64_t lead = (w > r) ? (w - r) : 0;
        if (lead + want_samples <= kLookaheadSamples) return true;
        // Polling, not a condvar: waking the producer would mean notify_one()
        // from the audio callback. The gate only reopens once the consumer has
        // drained a meaningful slice of an ~11.9 s lookahead, so a 10 ms
        // granularity is far finer than it needs to be.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void PcmRing::append(const float* samples, size_t count) {
    if (!samples || count == 0) return;
    if (count > kRingSamples) { samples += (count - kRingSamples); count = kRingSamples; }

    const uint64_t w = valid_hi_.load(std::memory_order_relaxed); // sole writer
    const size_t start = static_cast<size_t>(w & kRingMask);
    const size_t first = std::min(count, kRingSamples - start);
    std::memcpy(data_.data() + start, samples, first * sizeof(float));
    if (count > first) {
        std::memcpy(data_.data(), samples + first, (count - first) * sizeof(float));
    }

    const uint64_t new_hi = w + count;
    const uint64_t new_lo = (new_hi > kRingSamples) ? (new_hi - kRingSamples) : 0;
    // lo BEFORE hi: a consumer that observes the new hi must also observe a lo
    // that is at least as new, or it could read a slot this call just recycled.
    uint64_t cur_lo = valid_lo_.load(std::memory_order_relaxed);
    if (new_lo > cur_lo) valid_lo_.store(new_lo, std::memory_order_release);
    valid_hi_.store(new_hi, std::memory_order_release);
}

// A seek restart drags valid_hi_ backwards while a callback may be partway
// through a read, and the producer then immediately overwrites slots that read
// is looking at. Closing the window first, and waiting for the reader to come
// round, is what makes that safe.
//
// Walk the states a consumer can observe (it loads hi, then lo):
//   (old_hi, old_lo) valid   (old_hi, MAX) empty   (0, old_lo) empty
//   (0, MAX)         empty   (0, T)        empty   (T, T)      empty
// No interleaving yields a non-empty window over recycled slots. The tick wait
// then guarantees any callback that already loaded old_hi has returned before
// the first sample of the new epoch is written.
void PcmRing::begin_epoch(int64_t first_frame) {
    // decode_done first: Player declares "finished" on
    // decode_done && cursor >= available, and an empty window makes that
    // trivially true. Clearing it before the window closes is what stops a
    // seek from being seen as end-of-track and skipping to the next one.
    decode_done.store(false, std::memory_order_release);
    decode_failed.store(false, std::memory_order_release);

    valid_lo_.store(UINT64_MAX, std::memory_order_release);
    valid_hi_.store(0, std::memory_order_release);

    const uint64_t t0 = reader_ticks_.load(std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (reader_ticks_.load(std::memory_order_relaxed) < t0 + 2) {
        // Capped, because there may be no callback running at all -- play()
        // can have failed, or the device may not be started yet, in which case
        // nothing can be mid-read and there is nothing to wait for.
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const uint64_t origin = first_frame > 0
                          ? static_cast<uint64_t>(first_frame) * channels_u() : 0;
    retain_floor_.store(origin, std::memory_order_release);
    valid_lo_.store(origin, std::memory_order_release);
    valid_hi_.store(origin, std::memory_order_release);
}

} // namespace muisc
