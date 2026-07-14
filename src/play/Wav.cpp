#include "Wav.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

namespace play
{
	namespace
	{
		void put32(ofstream& ofs, uint32_t v)
		{
			uint8_t b[4] = {
				static_cast<uint8_t>(v & 0xFF),
				static_cast<uint8_t>((v >> 8) & 0xFF),
				static_cast<uint8_t>((v >> 16) & 0xFF),
				static_cast<uint8_t>((v >> 24) & 0xFF) };
			ofs.write(reinterpret_cast<const char*>(b), 4);
		}

		void put16(ofstream& ofs, uint16_t v)
		{
			uint8_t b[2] = {
				static_cast<uint8_t>(v & 0xFF),
				static_cast<uint8_t>((v >> 8) & 0xFF) };
			ofs.write(reinterpret_cast<const char*>(b), 2);
		}
	}

	bool writeWavPcm(const string& path, const vector<int16_t>& interleaved,
		uint16_t chanCount, uint32_t sampleRate)
	{
		const uint16_t bitsPerSample = 16;
		const uint16_t blockAlign = static_cast<uint16_t>(chanCount * (bitsPerSample / 8));
		const uint32_t byteRate = sampleRate * blockAlign;
		const uint32_t dataLength = static_cast<uint32_t>(interleaved.size()) * (bitsPerSample / 8);
		const uint32_t riffLength = 36 + dataLength;

		ofstream ofs(path, ofstream::binary);

		ofs.write("RIFF", 4);
		put32(ofs, riffLength);
		ofs.write("WAVE", 4);
		ofs.write("fmt ", 4);
		put32(ofs, 16);                 // fmt chunk length
		put16(ofs, 1);                  // PCM
		put16(ofs, chanCount);
		put32(ofs, sampleRate);
		put32(ofs, byteRate);
		put16(ofs, blockAlign);
		put16(ofs, bitsPerSample);
		ofs.write("data", 4);
		put32(ofs, dataLength);

		// Write the samples explicitly little-endian (put16) so the WAV bytes are
		// identical on a big-endian host too -- the golden hash must not depend on
		// the build's byte order.
		for (int16_t s : interleaved)
		{
			put16(ofs, static_cast<uint16_t>(s));
		}

		ofs.close();

		return static_cast<bool>(ofs);
	}
}
