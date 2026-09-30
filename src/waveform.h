#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "envelope_accumulator.h"

namespace muisc {

namespace fs = std::filesystem;

struct BrailleColumn {
    std::string top;
    std::string mid;
    std::string bot;
};

class WaveformQuantizer {
public:
    static BrailleColumn get_column(int level);

    // "Backend" pass — run ONCE when a track loads, regardless of
    // terminal width. Downsamples raw PCM to `resolution` RMS-energy
    // bins (default 4096 — far finer than any realistic terminal width
    // could need, since our own W is clamped to 200 columns max) and
    // normalizes + applies the visual contrast curve. `smooth=true`
    // applies a light center-weighted blur across neighboring bins
    // first (takes the edge off per-sample noise); `smooth=false` keeps
    // the raw per-bin RMS — every real transient shows up exactly where
    // it happened, at the cost of looking a little more jagged. Returns
    // normalized values in [0,1]; NOT yet quantized to braille levels —
    // that happens per-column in resample_for_ui, since the noise gate
    // needs to see the post-decimation peak, not each raw high-res bin.
    static std::vector<float> generate_high_res_envelope(const std::vector<float>& pcm_data,
                                                           int resolution = 4096, bool smooth = true);

    // Same output as the above, from an EnvelopeAccumulator's raw bins instead
    // of resident PCM. This is the form the streaming path uses: once audio is
    // held in a bounded ring rather than a whole-track buffer, the samples a
    // resize would need to re-bin are long gone, but the 48 KiB of raw bins
    // are still here and reproduce the envelope exactly -- for any `smooth`
    // setting, at any terminal width.
    static std::vector<float> envelope_from_bins(const std::vector<EnvelopeBin>& bins,
                                                  bool smooth = true);

    // "Frontend" pass — cheap enough to run every frame (or at least on
    // every resize): resamples the fixed high-res envelope down to
    // whatever the terminal's CURRENT width actually is via peak
    // decimation (the loudest bin in each terminal column's bucket,
    // so transients never get averaged away), then noise-gates and
    // quantizes to a 0-5 braille level. This is what makes the waveform
    // correct at any width instead of the old fixed-100-column data
    // just going blank past column 100 on a wide terminal, or showing
    // an un-rescaled partial slice of the track on a narrow one.
    static std::vector<int> resample_for_ui(const std::vector<float>& high_res_model, int terminal_width);
};

// The streaming decoders that used to live here now belong to DecodeSession
// (src/decode_session.h), which owns the threads, the cancellation and the
// seek-restart they need. This header is back to being purely a quantizer.

} // namespace muisc
