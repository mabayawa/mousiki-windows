#include "waveform.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace muisc {

BrailleColumn WaveformQuantizer::get_column(int level) {
    switch (level) {
        case 0: return {" ", "\u2836", " "};
        case 1: return {" ", "\u28FF", " "};
        case 2: return {"\u28C0", "\u28FF", "\u2809"};
        case 3: return {"\u28E4", "\u28FF", "\u281B"};
        case 4: return {"\u28F6", "\u28FF", "\u283F"};
        case 5: return {"\u28FF", "\u28FF", "\u28FF"};
        default: return {" ", "\u2836", " "};
    }
}

namespace {

// The back half of the envelope pipeline: smooth -> normalize by the global
// max -> contrast curve. Split out so that a per-bin RMS array reaches it by
// exactly one route, whether those bins were just computed from a resident
// PCM buffer or accumulated incrementally while the track streamed. That
// single path is what guarantees the two produce identical output.
std::vector<float> finish_envelope(const std::vector<float>& raw_rms, bool smooth) {
    const int resolution = static_cast<int>(raw_rms.size());
    std::vector<float> high_res(resolution, 0.0f);
    if (resolution <= 0) return high_res;

    const std::vector<float>* source = &raw_rms;
    std::vector<float> smoothed;
    if (smooth) {
        smoothed.assign(resolution, 0.0f);
        // Same narrow, center-weighted kernel as before -- a wider 5-tap
        // kernel spreads a loud bin's energy into its neighbors almost
        // as strongly as its own value, which is what made a sudden
        // drop look "extended" past where it actually happened.
        const float weights[3] = {0.15f, 0.70f, 0.15f};
        for (int i = 0; i < resolution; ++i) {
            float sum = 0.0f, weight_sum = 0.0f;
            for (int j = -1; j <= 1; ++j) {
                int idx = i + j;
                if (idx >= 0 && idx < resolution) {
                    sum += raw_rms[idx] * weights[j + 1];
                    weight_sum += weights[j + 1];
                }
            }
            smoothed[i] = sum / weight_sum;
        }
        source = &smoothed;
    }

    float global_max = 0.0001f;
    for (float v : *source) {
        if (v > global_max) global_max = v;
    }

    for (int i = 0; i < resolution; ++i) {
        float normalized = (*source)[i] / global_max;
        high_res[i] = std::clamp(std::pow(normalized, 2.5f), 0.0f, 1.0f);
    }

    return high_res;
}

} // namespace

std::vector<float> WaveformQuantizer::envelope_from_bins(const std::vector<EnvelopeBin>& bins,
                                                          bool smooth) {
    std::vector<float> raw_rms(bins.size(), 0.0f);
    for (size_t i = 0; i < bins.size(); ++i) {
        if (bins[i].count > 0) {
            raw_rms[i] = std::sqrt(bins[i].sum_sq / static_cast<float>(bins[i].count));
        }
    }
    return finish_envelope(raw_rms, smooth);
}

std::vector<float> WaveformQuantizer::generate_high_res_envelope(const std::vector<float>& pcm_data,
                                                                   int resolution, bool smooth) {
    if (pcm_data.empty() || resolution <= 0) return std::vector<float>(std::max(0, resolution), 0.0f);

    size_t chunk_size = pcm_data.size() / static_cast<size_t>(resolution);
    if (chunk_size == 0) chunk_size = 1;

    std::vector<float> raw_rms(resolution, 0.0f);
    for (int i = 0; i < resolution; ++i) {
        // PERF: was `double sum_sq` — the float→double promotion on every
        // iteration prevented ARM NEON auto-vectorization and roughly halved
        // throughput vs. float on Termux/Android.  Float precision is more
        // than sufficient for a 6-level visual bar (the final output is
        // quantized to 0-5 anyway).
        float sum_sq = 0.0f;
        size_t start = static_cast<size_t>(i) * chunk_size;
        size_t end = std::min(start + chunk_size, pcm_data.size());
        size_t count = end - start;
        if (count > 0) {
            for (size_t j = start; j < end; ++j) {
                sum_sq += pcm_data[j] * pcm_data[j];
            }
            raw_rms[i] = std::sqrt(sum_sq / static_cast<float>(count));
        }
    }

    return finish_envelope(raw_rms, smooth);
}

std::vector<int> WaveformQuantizer::resample_for_ui(const std::vector<float>& high_res_model, int terminal_width) {
    std::vector<int> ui_waveform(std::max(0, terminal_width), 0);
    if (terminal_width <= 0 || high_res_model.empty()) return ui_waveform;

    float ratio = static_cast<float>(high_res_model.size()) / static_cast<float>(terminal_width);

    for (int i = 0; i < terminal_width; ++i) {
        int start_idx = static_cast<int>(i * ratio);
        int end_idx = static_cast<int>((i + 1) * ratio);
        start_idx = std::clamp(start_idx, 0, static_cast<int>(high_res_model.size()));
        end_idx = std::clamp(end_idx, start_idx, static_cast<int>(high_res_model.size()));
        if (end_idx == start_idx && start_idx < static_cast<int>(high_res_model.size())) {
            end_idx = start_idx + 1; // resolution > terminal_width in every realistic case, but guard the edge anyway
        }

        // Peak decimation: the loudest bin in this column's bucket wins,
        // so a brief transient never gets averaged away into nothing —
        // it's why this needs the pre-computed high-res model in the
        // first place rather than a plain low-res RMS pass at whatever
        // width happened to be current at load time.
        float local_peak = 0.0f;
        for (int j = start_idx; j < end_idx; ++j) {
            if (high_res_model[j] > local_peak) local_peak = high_res_model[j];
        }

        if (local_peak < 0.08f) local_peak = 0.0f;
        ui_waveform[i] = static_cast<int>(std::round(local_peak * 5.0f));
    }

    return ui_waveform;
}

// Primary decode path: miniaudio's own built-in decoder, entirely
// in-process — no subprocess, no shell, nothing that depends on where
// (or whether) a shell binary happens to live on this device. Covers
// WAV/MP3/FLAC/OGG directly. This is the same approach as the reference
// implementation that prompted this rewrite: ma_decoder_init_file() +
// ma_decoder_read_pcm_frames() in a loop, chunk by chunk, which is what
// lets it start producing frames almost immediately with no process-spawn
// overhead at all.
} // namespace muisc
