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

    if (!self || !self->pcm_ || self->paused_.load()) {
        std::memset(out, 0, frame_count * sizeof(float));
        return;
    }

    StreamingPcm& pcm = *self->pcm_;
    long long cur = self->cursor_frames_.load();
    float gain = self->gain_.load();
    // Acquire-load: pairs with the release-store in StreamingPcm::append(),
    // guaranteeing every index below `avail` was fully written by the
    // decode thread before we read it here.
    size_t avail = pcm.available.load(std::memory_order_acquire);

    for (ma_uint32 i = 0; i < frame_count; ++i) {
        long long idx = cur + static_cast<long long>(i);
        out[i] = (idx >= 0 && static_cast<size_t>(idx) < avail) ? pcm.data[static_cast<size_t>(idx)] * gain : 0.0f;
    }

    if (self->fft_sink_) self->fft_sink_->push_samples(out, frame_count, self->sample_rate_.load());

    long long new_cur = cur + static_cast<long long>(frame_count);
    // Only truly "finished" once decode is done AND playback has caught
    // all the way up to everything it ever produced — not just the
    // current available count, which may still be growing while we play.
    if (pcm.decode_done.load() &&
        new_cur >= 0 && static_cast<size_t>(new_cur) >= pcm.available.load(std::memory_order_acquire)) {
        self->finished_.store(true);
    }
    self->cursor_frames_.store(new_cur);
}

bool Player::play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
                   FftVisualizer* fft_sink) {
    stop();
    if (!pcm) return false;

    if (!context_ready_) {
        context_ready_ = init_platform_audio_context(context_);
        // Not fatal if this fails — ma_device_init(nullptr, ...) below
        // falls back to miniaudio's own default backend selection.
    }

    pcm_ = std::move(pcm);
    fft_sink_ = fft_sink;
    sample_rate_.store(pcm_->sample_rate > 0 ? pcm_->sample_rate : 44100);
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
    capacity_frames_.store(static_cast<long long>(pcm_->data.capacity()));
    finished_.store(false);
    paused_.store(false);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * sample_rate_.load()));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate = static_cast<ma_uint32>(sample_rate_.load());
    cfg.dataCallback = data_callback;
    cfg.pUserData = this;

    ma_context* ctx = context_ready_ ? &context_ : nullptr;
    ma_result init_res = ma_device_init(ctx, &cfg, &device_);
    if (init_res != MA_SUCCESS) {
        pcm_.reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_init failed: ") + ma_result_description(init_res));
        return false;
    }
    ma_result start_res = ma_device_start(&device_);
    if (start_res != MA_SUCCESS) {
        ma_device_uninit(&device_);
        pcm_.reset();
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_start failed: ") + ma_result_description(start_res));
        return false;
    }
    ConsoleLog::instance().log_verbose(
        std::string("audio: device started, backend=") + ma_get_backend_name(device_.pContext->backend) +
        ", rate=" + std::to_string(sample_rate_.load()) + "Hz");

    device_ready_ = true;
    return true;
}

void Player::pause() { paused_.store(true); }
void Player::resume() { paused_.store(false); }

void Player::seek_relative(double delta_sec) {
    // Clamp against reserved capacity (the eventual max), not the
    // currently-decoded amount — seeking a bit ahead of what's decoded
    // so far is fine, it just plays silence until decode catches up.
    //
    // Read from the cached atomic rather than pcm_->data.capacity(): this runs
    // on the main thread, and the device worker may be reassigning pcm_ at the
    // same moment.
    long long cap = capacity_frames_.load();
    if (cap <= 0) return;
    long long delta_frames = static_cast<long long>(delta_sec * sample_rate_.load());
    long long cur = cursor_frames_.load();
    long long next = std::clamp<long long>(cur + delta_frames, 0, cap);
    cursor_frames_.store(next);
    if (next < cap) finished_.store(false);
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
    // the time pcm_ is released nothing can still be reading through it.
    if (device_ready_) {
        ma_device_uninit(&device_);
        device_ready_ = false;
    }
    capacity_frames_.store(0);
    pcm_.reset();
    fft_sink_ = nullptr;
}

} // namespace muisc
