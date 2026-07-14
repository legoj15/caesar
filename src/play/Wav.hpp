#pragma once

// A minimal 16-bit PCM RIFF/WAVE writer, the same chunk shape Cwav::ExportWav
// emits (canonical 44-byte header: RIFF / WAVE / fmt (PCM, 16-bit) / data), with
// no smpl chunk. The player writes interleaved little-endian int16 frames.

#include <cstdint>
#include <string>
#include <vector>

namespace play
{
	// Write `interleaved` (chanCount samples per frame, interleaved) as a 16-bit
	// PCM WAV at `sampleRate`. Returns false on a failed/incomplete write.
	bool writeWavPcm(const std::string& path, const std::vector<int16_t>& interleaved,
		uint16_t chanCount, uint32_t sampleRate);
}
