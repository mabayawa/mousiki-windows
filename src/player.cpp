#include "player.h"
#include "audio_backend.h"
#include "console_log.h"
#include <algorithm>
#include <cstring>

namespace muisc {

Player::Player() = default;
Player::~Player() { stop(); }

void Player::data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    Player* self = static_cast<Player*>(device->pUserData);
    float* out = static_cast<float*>(output);

    const int ch = static_cast<int>(device->playback.channels);
    const size_t total_samples = static_cast<size_t>(frame_count) * static_cast<size_t>(ch);

    // Read the slot index once: a swap partway through this call simply means we
    // finish with the buffer we started on, which is correct.
    PcmRing* ring_ptr =
        self ? self->pcm_slots_[self->active_slot_.load(std::memory_order_acquire)].get() : nullptr;
    if (!ring_ptr) {
        std::memset(out, 0, total_samples * sizeof(float));
        return;
    }

    PcmRing& ring = *ring_ptr;
    // Ticked before the paused early-return, not after: begin_epoch() waits on
    // these ticks to know no reader is still inside the window it is about to
    // move, and a paused callback that stopped ticking would stall every seek
    // until the wait timed out.
    ring.tick();

    // The outgoing track ended the moment the user asked for a new one --
    // see begin_track_switch(). Emit silence, and critically do NOT advance
    // cursor_frames_ or raise finished_ on the way out: the cursor is the
    // clock the entire UI reads (progress bar, timestamp, lyric highlighting),
    // and a finished_ raised here would make the main loop's
    // "has_track_ && finished()" check skip the track that is about to start.
    if (self->switching_.load(std::memory_order_acquire)) {
        std::memset(out, 0, total_samples * sizeof(float));
        return;
    }

    if (self->paused_.load()) {
        std::memset(out, 0, total_samples * sizeof(float));
        return;
    }

    const long long cur = self->cursor_frames_.load();
    const float gain = self->gain_.load();

    // One bulk copy with the gaps zero-filled, instead of the old per-sample
    // bounds test. Strictly cheaper than what it replaces, and it keeps the
    // acquire/release pairing inside the ring where the contract is documented.
    ring.read(cur, out, frame_count);
    for (size_t i = 0; i < total_samples; ++i) out[i] *= gain;

    // The spectrum analyser wants one signal, not one per channel -- a stereo
    // FFT would mean drawing two spectra. Sum to mono here, which is what any
    // analyser does, while the device above still receives full stereo.
    if (self->fft_sink_) {
        constexpr size_t kChunk = 1024;
        float mono[kChunk];
        const float inv = 1.0f / static_cast<float>(ch);
        size_t done = 0;
        while (done < frame_count) {
            size_t n = (std::min)(kChunk, static_cast<size_t>(frame_count) - done);
            for (size_t i = 0; i < n; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < ch; ++c) sum += out[(done + i) * ch + c];
                mono[i] = sum * inv;
            }
            self->fft_sink_->push_samples(mono, n, self->sample_rate_.load());
            done += n;
        }
    }

    const long long new_cur = cur + static_cast<long long>(frame_count);
    const long long hi = ring.decoded_hi_frames();
    // Mirrored for the main thread's forward-seek test -- see player.h.
    self->decoded_hi_frames_.store(hi, std::memory_order_relaxed);

    // Only truly "finished" once the producer is done AND playback has caught
    // up to everything it ever produced. During a seek restart the window is
    // briefly empty, which would satisfy the second half on its own -- but
    // begin_epoch() clears decode_done before closing the window, so this
    // cannot misfire and skip the track the user just seeked within.
    if (ring.decode_done.load(std::memory_order_acquire) && new_cur >= hi) {
        self->finished_.store(true);
    }
    self->cursor_frames_.store(new_cur);
    // Publishing the cursor is what releases ring space back to the producer.
    ring.publish_cursor(new_cur);
}

bool Player::play(std::shared_ptr<PcmRing> pcm, double start_sec, int volume_pct,
                   FftVisualizer* fft_sink) {
    stop();
    if (!pcm) return false;

    if (!context_ready_) {
        context_ready_ = init_platform_audio_context(context_);
        // Not fatal if this fails — ma_device_init(nullptr, ...) below
        // falls back to miniaudio's own default backend selection.
    }

    active_slot_.store(0, std::memory_order_release);
    pcm_slots_[0] = std::move(pcm);
    pcm_slots_[1].reset();
    PcmRing* ring = pcm_slots_[0].get();
    fft_sink_ = fft_sink;
    sample_rate_.store(ring->sample_rate > 0 ? ring->sample_rate : 44100);
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
    int ch = ring->channels > 0 ? ring->channels : 1;
    decoded_hi_frames_.store(ring->decoded_hi_frames());
    finished_.store(false);
    paused_.store(false);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * sample_rate_.load()));
    // The incoming ring is in its slot and the clock is rebased, so the switch
    // is over. Cleared before ma_device_init so the very first callback of the
    // new device already plays audio rather than one period of silence.
    switching_.store(false, std::memory_order_release);

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(ch);
    cfg.sampleRate = static_cast<ma_uint32>(sample_rate_.load());
    cfg.dataCallback = data_callback;
    cfg.pUserData = this;

    ma_context* ctx = context_ready_ ? &context_ : nullptr;
    ma_result init_res = ma_device_init(ctx, &cfg, &device_);
    if (init_res != MA_SUCCESS) {
        pcm_slots_[0].reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_init failed: ") + ma_result_description(init_res));
        return false;
    }
    ma_result start_res = ma_device_start(&device_);
    if (start_res != MA_SUCCESS) {
        ma_device_uninit(&device_);
        pcm_slots_[0].reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_start failed: ") + ma_result_description(start_res));
        return false;
    }
    ConsoleLog::instance().log_verbose(
        std::string("audio: device started, backend=") + ma_get_backend_name(device_.pContext->backend) +
        ", rate=" + std::to_string(sample_rate_.load()) + "Hz" +
        ", ch=" + std::to_string(device_.playback.channels) +
        " (requested " + std::to_string(ch) + ")");

    device_ready_ = true;
    return true;
}

void Player::adopt_ring(std::shared_ptr<PcmRing> ring, double start_sec) {
    if (!ring) return;
    const int cur = active_slot_.load(std::memory_order_acquire);
    const int next = 1 - cur;
    // Filled before the flip, so the callback never observes a half-assigned
    // shared_ptr. The outgoing buffer stays in its slot and is only released on
    // the NEXT adopt, by which point no callback can still be inside it.
    pcm_slots_[next] = std::move(ring);
    const int rate = pcm_slots_[next]->sample_rate > 0 ? pcm_slots_[next]->sample_rate : 44100;
    sample_rate_.store(rate);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * rate));
    decoded_hi_frames_.store(pcm_slots_[next]->decoded_hi_frames());
    finished_.store(false);
    switching_.store(false, std::memory_order_release);
    active_slot_.store(next, std::memory_order_release);
}

void Player::begin_track_switch() {
    // Order matters. The flag goes up FIRST: once the callback observes it, it
    // stops touching cursor_frames_ and finished_, so the zeroing below cannot
    // be overwritten by a callback that was already in flight. Zeroing first
    // would leave a window where the callback stores cur + frame_count over
    // the top of it and the clock resumes counting from the old position --
    // the exact carry-over this function exists to remove.
    switching_.store(true, std::memory_order_release);
    cursor_frames_.store(0);
    decoded_hi_frames_.store(0);
    finished_.store(false);
}

void Player::pause() { paused_.store(true); }
void Player::resume() { paused_.store(false); }

bool Player::try_seek_in_window(double target_sec) {
    const int rate = sample_rate_.load();
    if (rate <= 0) return false;
    const long long tgt = static_cast<long long>(target_sec * rate);
    if (tgt < 0) return false;
    const long long cur = cursor_frames_.load();

    if (tgt <= cur) {
        // Backward. The ring guarantees kHistoryFrames behind the cursor, less
        // a margin: between this test and the store below, the callback can
        // advance retain_floor_ by up to a device period, dragging the resident
        // base forward by the same amount. Half a second is ~25 periods of
        // slack, and still admits an 11.4 s jump -- so a -5 s tap, or two in a
        // row, never costs a decoder restart.
        const long long margin = rate / 2;
        if (cur - tgt > static_cast<long long>(kHistoryFrames) - margin) return false;
    } else {
        // Forward. Exact test against what has actually been produced; no
        // margin needed, since a false negative only costs a restart that was
        // not strictly necessary.
        if (tgt >= decoded_hi_frames_.load()) return false;
    }

    cursor_frames_.store(tgt);
    finished_.store(false);
    return true;
}

void Player::rebase_for_restart(double target_sec) {
    const int rate = sample_rate_.load();
    if (rate <= 0) return;
    cursor_frames_.store(static_cast<long long>(std::max(0.0, target_sec) * rate));
    finished_.store(false);
}

void Player::set_volume(int volume_pct) {
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
}

double Player::poll_elapsed() const {
    int rate = sample_rate_.load();
    if (rate <= 0) return 0.0;
    return static_cast<double>(cursor_frames_.load()) / rate;
}

void Player::stop() {
    // ma_device_uninit() must come first: it stops the audio callback, so by
    // the time the buffers are released nothing can still be reading them.
    if (device_ready_) {
        ma_device_uninit(&device_);
        device_ready_ = false;
    }
    decoded_hi_frames_.store(0);
    pcm_slots_[0].reset();
    pcm_slots_[1].reset();
    active_slot_.store(0, std::memory_order_release);
    fft_sink_ = nullptr;
}

} // namespace muisc
