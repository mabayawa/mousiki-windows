#include "decode_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "console_log.h"
#include "miniaudio.h"
#include "path_utf8.h"
#include "process_util.h"

namespace muisc {

// ---------------------------------------------------------------------------
// ChildSlot / DecodeControl
// ---------------------------------------------------------------------------

void ChildSlot::set(std::shared_ptr<ChildProcess> c) {
    std::lock_guard<std::mutex> lk(mu);
    child = std::move(c);
}

void ChildSlot::kill_current() {
    std::shared_ptr<ChildProcess> c;
    {
        std::lock_guard<std::mutex> lk(mu);
        c = child;
    }
    // Held alive by the local shared_ptr so the owning thread cannot destroy
    // it while terminate() runs, and killed OUTSIDE the lock so this never
    // contends with the owner swapping in a replacement.
    if (c) c->terminate();
}

void DecodeControl::request_cancel() {
    cancel.store(true, std::memory_order_release);
    produce_child.kill_current();
    scan_child.kill_current();
}

void DecodeControl::request_seek(int64_t frames) {
    seek_target_frames.store(frames, std::memory_order_relaxed);
    seek_request.fetch_add(1, std::memory_order_release);
    // Unblocks a producer parked in read_stdout(): killing ffmpeg closes the
    // pipe, that read returns 0, and the loop comes back round, notices the
    // new generation, and respawns at the new offset.
    produce_child.kill_current();
}

// ---------------------------------------------------------------------------
// Decode sources
// ---------------------------------------------------------------------------

namespace {

// Reads interleaved f32 at 44100 Hz from a requested offset. read() always
// returns a whole number of frames' worth of samples.
class DecodeSource {
public:
    virtual ~DecodeSource() = default;
    // Interleaved samples written to dst; 0 means end of stream.
    virtual size_t read(float* dst, size_t max_samples) = 0;
};

class MiniaudioSource : public DecodeSource {
public:
    static std::unique_ptr<MiniaudioSource> open(const fs::path& path, int64_t start_frame) {
        std::unique_ptr<MiniaudioSource> src(new MiniaudioSource());
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, kAudioChannels, 44100);
#ifdef _WIN32
        // The narrow ma_decoder_init_file converts through the process ANSI
        // code page, so a track called "Пример.flac" simply fails to open.
        // The _w variant takes UTF-16 and opens it correctly.
        if (ma_decoder_init_file_w(path.wstring().c_str(), &config, &src->dec_) != MA_SUCCESS)
#else
        if (ma_decoder_init_file(path.string().c_str(), &config, &src->dec_) != MA_SUCCESS)
#endif
            return nullptr;
        src->open_ = true;
        if (start_frame > 0) {
            if (ma_decoder_seek_to_pcm_frame(&src->dec_,
                                             static_cast<ma_uint64>(start_frame)) != MA_SUCCESS) {
                // VBR MP3 without a seek table cannot seek directly. Decoding
                // forward and discarding lands in the same place, just slower.
                src->discard_to(start_frame);
            }
        }
        return src;
    }

    ~MiniaudioSource() override {
        if (open_) ma_decoder_uninit(&dec_);
    }

    size_t read(float* dst, size_t max_samples) override {
        const size_t max_frames = max_samples / kAudioChannels;
        if (max_frames == 0) return 0;
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec_, dst, max_frames, &got);
        return static_cast<size_t>(got) * kAudioChannels;
    }

private:
    MiniaudioSource() = default;

    void discard_to(int64_t target_frame) {
        std::array<float, 4096> tmp{};
        int64_t done = 0;
        while (done < target_frame) {
            const ma_uint64 want = static_cast<ma_uint64>(
                std::min<int64_t>(target_frame - done,
                                  static_cast<int64_t>(tmp.size() / kAudioChannels)));
            ma_uint64 got = 0;
            if (ma_decoder_read_pcm_frames(&dec_, tmp.data(), want, &got) != MA_SUCCESS || got == 0)
                break;
            done += static_cast<int64_t>(got);
        }
    }

    ma_decoder dec_{};
    bool open_ = false;
};

// Fallback for what miniaudio's built-in decoders do not cover -- Opus
// (yt-dlp's cache format) being the one this project actually needs, which
// makes this the hot path for every YouTube-sourced track.
class FfmpegSource : public DecodeSource {
public:
    static std::unique_ptr<FfmpegSource> open(const fs::path& path, int64_t start_frame,
                                              ChildSlot* slot) {
        std::vector<std::string> argv = {"ffmpeg", "-nostdin", "-v", "error"};
        if (start_frame > 0) {
            // BEFORE -i on purpose. Input-side seek jumps within the container
            // and, since -accurate_seek has been the default, still lands on
            // the exact frame. Output-side -ss would decode the entire prefix
            // and throw it away, which is the whole cost being avoided here.
            char ts[64];
            std::snprintf(ts, sizeof ts, "%.6f", static_cast<double>(start_frame) / 44100.0);
            argv.push_back("-ss");
            argv.push_back(ts);
        }
        argv.push_back("-i");
        argv.push_back(path_utf8(path));
        const std::vector<std::string> tail = {"-f", "f32le", "-ac",
                                               std::to_string(kAudioChannels), "-ar", "44100", "-"};
        argv.insert(argv.end(), tail.begin(), tail.end());

        // merge_stderr stays false: ffmpeg logs to stderr, and merging it into
        // stdout would splice log text straight into the PCM as noise.
        std::shared_ptr<ChildProcess> child = ChildProcess::spawn(argv);
        if (!child) return nullptr;

        std::unique_ptr<FfmpegSource> src(new FfmpegSource());
        src->child_ = child;
        src->slot_ = slot;
        if (slot) slot->set(child);
        return src;
    }

    ~FfmpegSource() override {
        if (child_) child_->wait();
        if (slot_) slot_->set(nullptr);
    }

    size_t read(float* dst, size_t max_samples) override {
        const size_t cap_bytes = max_samples * sizeof(float);
        for (;;) {
            // Capped so `floats` below can never exceed max_samples.
            const size_t room = std::min(buf_.size(), cap_bytes) - carry_len_;
            if (room == 0) return 0;
            const long long n = child_->read_stdout(buf_.data() + carry_len_, room);
            if (n <= 0) return 0;   // end of stream, or the child was killed

            const size_t total  = carry_len_ + static_cast<size_t>(n);
            const size_t floats = total / sizeof(float);
            // Emit whole FRAMES only. Any leftover float simply stays in the
            // byte carry, so there is no second, float-level holding buffer to
            // keep in sync -- at most (channels*4 - 1) bytes are ever carried.
            const size_t usable = (floats / kAudioChannels) * kAudioChannels;
            const size_t used   = usable * sizeof(float);

            if (usable > 0) {
                std::memcpy(dst, buf_.data(), used);
                carry_len_ = total - used;
                if (carry_len_ > 0) std::memmove(buf_.data(), buf_.data() + used, carry_len_);
                return usable;
            }
            // Fewer than one whole frame so far: keep reading rather than
            // returning 0, which the caller would read as end of stream.
            carry_len_ = total;
        }
    }

private:
    FfmpegSource() { buf_.fill(0); }

    std::shared_ptr<ChildProcess> child_;
    ChildSlot* slot_ = nullptr;
    std::array<char, 65536> buf_{};
    size_t carry_len_ = 0;
};

enum class Backend { Unknown, Miniaudio, Ffmpeg };

// True for an Ogg stream whose codec is Opus.
//
// This check is load-bearing, and its absence was a real bug. miniaudio's
// built-in Ogg support is stb_vorbis, which recognises the OGG CONTAINER and
// so lets ma_decoder_init SUCCEED on an Ogg-Opus file -- then decodes a few
// seconds of noise and stops, because the packets inside are not Vorbis.
// Measured on a 90 s test file: init succeeded, 4.18 s came out, and the peak
// sample was 1.36 against a true level of 0.088.
//
// The old dispatcher fell back to ffmpeg only when init FAILED, so it never
// got the chance to. That matters here specifically because .opus is what
// yt-dlp caches, i.e. every online track. Sniffing the codec rather than
// trusting init is what routes those to ffmpeg, where they belong.
bool is_ogg_opus(const fs::path& path) {
#ifdef _WIN32
    FILE* f = _wfopen(path.wstring().c_str(), L"rb");
#else
    FILE* f = std::fopen(path.string().c_str(), "rb");
#endif
    if (!f) return false;
    unsigned char head[64] = {0};
    const size_t n = std::fread(head, 1, sizeof head, f);
    std::fclose(f);
    if (n < 36) return false;
    if (std::memcmp(head, "OggS", 4) != 0) return false;
    // "OpusHead" begins the first packet of an Ogg-Opus stream; it sits just
    // past the page header, whose length varies with the segment table.
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (std::memcmp(head + i, "OpusHead", 8) == 0) return true;
    }
    return false;
}

std::unique_ptr<DecodeSource> open_source(const fs::path& path, int64_t start_frame,
                                          Backend& backend, ChildSlot* slot) {
    if (backend == Backend::Unknown && is_ogg_opus(path)) backend = Backend::Ffmpeg;
    if (backend != Backend::Ffmpeg) {
        if (auto s = MiniaudioSource::open(path, start_frame)) {
            backend = Backend::Miniaudio;
            return s;
        }
        // Remembered, so a later seek does not pay for the failed miniaudio
        // probe all over again.
        backend = Backend::Ffmpeg;
    }
    return FfmpegSource::open(path, start_frame, slot);
}

constexpr size_t kChunkSamples = 8192;

} // namespace

// ---------------------------------------------------------------------------
// DecodeSession
// ---------------------------------------------------------------------------

DecodeSession::DecodeSession() : ring_(std::make_shared<PcmRing>()) {}
DecodeSession::~DecodeSession() { shutdown(); }

void DecodeSession::start(const fs::path& path, double start_sec, double total_seconds,
                          bool want_scanner) {
    if (started_.exchange(true)) return;
    total_samples_ = total_seconds > 0.0
                         ? static_cast<uint64_t>(total_seconds * 44100.0) * kAudioChannels
                         : 0;
    live_env_.reset(4096, total_samples_);
    scan_env_.reset(4096, total_samples_);

    ring_->begin_epoch(static_cast<int64_t>(std::max(0.0, start_sec) * 44100.0));
    producer_ = std::thread([this, path, start_sec] { producer_main(path, start_sec); });
    if (want_scanner) scanner_ = std::thread([this, path] { scanner_main(path); });
}

void DecodeSession::start_external(double total_seconds) {
    if (started_.exchange(true)) return;
    external_ = true;
    total_samples_ = total_seconds > 0.0
                         ? static_cast<uint64_t>(total_seconds * 44100.0) * kAudioChannels
                         : 0;
    live_env_.reset(4096, total_samples_);
    scan_env_.reset(4096, total_samples_);
    ring_->begin_epoch(0);
    // No producer and no scanner: whoever owns the stream drives this instead.
}

void DecodeSession::feed_envelope(uint64_t first_sample, const float* samples, size_t count) {
    live_env_.add(first_sample, samples, count);
}

void DecodeSession::shutdown() {
    ctl_.request_cancel();
    if (producer_.joinable()) producer_.join();
    if (scanner_.joinable()) scanner_.join();
}

void DecodeSession::request_seek(int64_t frame) { ctl_.request_seek(frame); }

std::vector<EnvelopeBin> DecodeSession::best_bins() const {
    if (scan_done_.load(std::memory_order_acquire)) return scan_env_.snapshot();
    return live_env_.snapshot();
}

// Feeds the audio ring, and is therefore held to playback speed by
// wait_for_room(). Every part of this loop assumes it can be told to stop or
// to jump at any moment.
void DecodeSession::producer_main(fs::path path, double start_sec) {
    Backend backend = Backend::Unknown;
    uint64_t served_seek = ctl_.seek_request.load(std::memory_order_acquire);
    int64_t start_frame = static_cast<int64_t>(std::max(0.0, start_sec) * 44100.0);
    std::vector<float> buf(kChunkSamples);

    const auto abort_pred = [this, &served_seek] {
        return ctl_.cancel.load(std::memory_order_acquire) ||
               ctl_.seek_request.load(std::memory_order_acquire) != served_seek;
    };

    for (;;) {
        if (ctl_.cancel.load(std::memory_order_acquire)) break;

        auto src = open_source(path, start_frame, backend, &ctl_.produce_child);
        if (!src) {
            ring_->decode_failed.store(true, std::memory_order_release);
            ring_->decode_done.store(true, std::memory_order_release);
            break;
        }

        uint64_t abs_sample = static_cast<uint64_t>(start_frame) * kAudioChannels;
        bool hit_eof = false;
        for (;;) {
            if (abort_pred()) break;
            if (!ring_->wait_for_room(kChunkSamples, abort_pred)) break;
            const size_t n = src->read(buf.data(), kChunkSamples);
            if (n == 0) { hit_eof = true; break; }
            ring_->append(buf.data(), n);
            live_env_.add(abs_sample, buf.data(), n);
            abs_sample += n;
        }
        src.reset();   // ma_decoder_uninit, or reap the ffmpeg child

        if (ctl_.cancel.load(std::memory_order_acquire)) break;

        if (hit_eof) {
            ring_->decode_done.store(true, std::memory_order_release);
            // Parked, NOT exited. The ring retains only ~23.8 s, so a backward
            // seek after decoding has finished still needs this thread alive
            // to restart the source. This is precisely the case that makes
            // cancellation mandatory rather than a nicety: without it, a
            // parked producer would outlive the track for good.
            while (!abort_pred()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (ctl_.cancel.load(std::memory_order_acquire)) break;
        }

        served_seek = ctl_.seek_request.load(std::memory_order_acquire);
        start_frame = ctl_.seek_target_frames.load(std::memory_order_acquire);
        ring_->begin_epoch(start_frame);
    }
}

// Builds the waveform on its own unthrottled pass.
//
// Without this the envelope would fill left-to-right across the length of the
// track, because the playback producer is now rate-limited by design. Today
// the waveform arrives early and complete, and the reveal animation in app.cpp
// depends on that. A second decode costs CPU and ~48 KiB, touches no ring, and
// actually delivers the envelope SOONER than before, since it no longer shares
// a thread with the audio feed.
void DecodeSession::scanner_main(fs::path path) {
    Backend backend = Backend::Unknown;
    auto src = open_source(path, 0, backend, &ctl_.scan_child);
    if (!src) return;

    std::vector<float> buf(kChunkSamples);
    uint64_t abs_sample = 0;
    for (;;) {
        if (ctl_.cancel.load(std::memory_order_acquire)) return;
        const size_t n = src->read(buf.data(), kChunkSamples);
        if (n == 0) break;
        scan_env_.add(abs_sample, buf.data(), n);
        abs_sample += n;
    }
    src.reset();
    if (ctl_.cancel.load(std::memory_order_acquire)) return;
    scan_env_.finalize(abs_sample);
    scan_done_.store(true, std::memory_order_release);
}

} // namespace muisc
