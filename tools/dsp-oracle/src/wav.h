// dsp-oracle — minimal 16-bit stereo PCM WAV writer.
//
// Part of caesar's offline DSP reverb oracle (suite stage 3). This file is
// original caesar code (GPLv3, same as the repo). It links against no external
// audio library — the oracle deliberately ships nothing but fitted coefficients
// and a golden IR wav, so a tiny self-contained writer is all we need.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsp_oracle {

// Writes `interleaved` (L,R,L,R,... signed 16-bit) as a canonical 44-byte-header
// PCM WAV at `path`. `interleaved.size()` must be even (whole stereo frames);
// an odd tail sample is dropped with the count reported by the caller.
// Returns false if the file cannot be opened/written.
bool WriteWav16Stereo(const std::string& path, const std::vector<std::int16_t>& interleaved,
                      std::uint32_t sample_rate);

} // namespace dsp_oracle
