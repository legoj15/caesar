// dsp-oracle — minimal 16-bit stereo PCM WAV writer (implementation).
#define _CRT_SECURE_NO_WARNINGS
#include "wav.h"

#include <cstdio>
#include <cstring>

namespace dsp_oracle {

namespace {
void PutU32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
void PutU16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
} // namespace

bool WriteWav16Stereo(const std::string& path, const std::vector<std::int16_t>& interleaved,
                      std::uint32_t sample_rate) {
    constexpr std::uint16_t kChannels = 2;
    constexpr std::uint16_t kBitsPerSample = 16;
    const std::uint16_t block_align = kChannels * (kBitsPerSample / 8); // 4 bytes/frame
    const std::uint32_t byte_rate = sample_rate * block_align;

    // Drop an odd trailing sample so we always emit whole stereo frames.
    const std::size_t sample_count = interleaved.size() & ~static_cast<std::size_t>(1);
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(sample_count * sizeof(std::int16_t));

    std::uint8_t header[44];
    std::memcpy(header + 0, "RIFF", 4);
    PutU32(header + 4, 36 + data_bytes); // RIFF chunk size = 4 + (8+16) + (8+data)
    std::memcpy(header + 8, "WAVE", 4);
    std::memcpy(header + 12, "fmt ", 4);
    PutU32(header + 16, 16);              // fmt chunk size (PCM)
    PutU16(header + 20, 1);               // audio format = PCM
    PutU16(header + 22, kChannels);
    PutU32(header + 24, sample_rate);
    PutU32(header + 28, byte_rate);
    PutU16(header + 32, block_align);
    PutU16(header + 34, kBitsPerSample);
    std::memcpy(header + 36, "data", 4);
    PutU32(header + 40, data_bytes);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
    if (ok && data_bytes != 0) {
        ok = std::fwrite(interleaved.data(), sizeof(std::int16_t), sample_count, f) == sample_count;
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

} // namespace dsp_oracle
