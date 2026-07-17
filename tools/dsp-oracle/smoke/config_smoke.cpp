// dsp_oracle_config_smoke — firmware-free correctness check for the commit-2
// SourceConfiguration / DspConfiguration byte builders.
//
// The oracle writes raw bytes directly into DSP shared memory, so the whole
// dry-source deliverable rides on the struct byte-offsets and the middle-endian
// (u32_dsp) encoding being exactly right. This smoke builds both configs into
// local buffers and asserts the key fields land at the documented offsets with
// the documented encodings — no Teakra, no firmware, deterministic. Same spirit
// as smoke.cpp: prove the standalone tree is internally consistent before the
// firmware-dependent oracle run.
//
// protocol reference: the offsets/encodings checked here are transcribed as
// layout documentation from Azahar audio_core/hle/shared_memory.h (GPL) in
// src/shared_mem.h; this file is ORIGINAL caesar code (GPLv3).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shared_mem.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

float ReadF32LE(const std::uint8_t* p) {
    std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 24);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
std::uint16_t ReadU16LE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}
std::uint32_t ReadU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
// Decode a u32_dsp field: bytes are little-endian of the halves-swapped value.
std::uint32_t ReadU32Dsp(const std::uint8_t* p) {
    std::uint32_t stored = ReadU32LE(p);
    return (stored << 16) | (stored >> 16);
}

} // namespace

int main() {
    using namespace dsp_oracle;

    // --- fixed sizes -----------------------------------------------------
    Check(kSourceConfigStride == 192, "SourceConfiguration::Configuration is 192 bytes");
    Check(kDspConfigSize == 196, "DspConfiguration is 196 bytes");
    Check(kReverbEffectSize == 52, "ReverbEffect is 52 bytes");
    Check(kFinalMixSamplesPerFrame == 160, "FinalMixSamples holds 160 stereo pairs");
    Check(kEmbFlags1_MonoPCM16 == 0x0005, "flags1 = Mono(bit0) | PCM16(bit2) == 0x0005");
    Check(DspCfgOffset::sync_mode == 0xBC, "DspConfiguration.sync_mode offset == 0xBC");

    // --- u32_dsp middle-endian round-trip --------------------------------
    {
        std::uint8_t buf[4];
        const std::uint32_t v = 0x20008000; // an FCRAM physical address
        PutU32Dsp(buf, v);
        // stored value must be the halves-swapped form, little-endian on disk.
        Check(ReadU32LE(buf) == 0x80002000, "u32_dsp stores halves-swapped LE");
        Check(ReadU32Dsp(buf) == v, "u32_dsp decodes back to the original u32");
    }

    // --- SourceConfiguration builder -------------------------------------
    {
        std::uint8_t cfg[kSourceConfigStride];
        std::memset(cfg, 0xAA, sizeof(cfg)); // poison to catch un-set bytes
        const std::uint32_t phys = 0x20008000, len = 64;
        BuildDrySourceConfig(cfg, phys, len);

        const std::uint32_t want_dirty = SrcDirty::format | SrcDirty::mono_stereo |
                                         SrcDirty::enable | SrcDirty::interpolation |
                                         SrcDirty::rate | SrcDirty::play_position |
                                         SrcDirty::gain0 | SrcDirty::gain1 | SrcDirty::gain2 |
                                         SrcDirty::embedded;
        Check(ReadU32LE(cfg + SrcCfgOffset::dirty_raw) == want_dirty, "src dirty_raw mask");
        Check(ReadF32LE(cfg + SrcCfgOffset::gain + 0 * 4) == 1.0f, "gain[0][0] (front L) == 1.0");
        Check(ReadF32LE(cfg + SrcCfgOffset::gain + 1 * 4) == 1.0f, "gain[0][1] (front R) == 1.0");
        Check(ReadF32LE(cfg + SrcCfgOffset::gain + 2 * 4) == 0.0f, "gain[0][2] (rear L) == 0.0");
        Check(ReadF32LE(cfg + SrcCfgOffset::gain + 4 * 4) == 0.0f, "gain[1][0] (aux0) == 0.0");
        Check(ReadF32LE(cfg + SrcCfgOffset::rate_multiplier) == 1.0f, "rate_multiplier == 1.0");
        Check(cfg[SrcCfgOffset::interpolation] == kInterpNone, "interpolation == None(2)");
        Check(cfg[SrcCfgOffset::enable] == 1, "enable == 1");
        Check(ReadU32Dsp(cfg + SrcCfgOffset::play_position) == 0, "play_position == 0");
        Check(ReadU32Dsp(cfg + SrcCfgOffset::emb_phys_addr) == phys, "embedded physical_address");
        Check(ReadU32Dsp(cfg + SrcCfgOffset::emb_length) == len, "embedded length (samples)");
        Check(ReadU16LE(cfg + SrcCfgOffset::emb_flags1) == kEmbFlags1_MonoPCM16,
              "embedded flags1 == Mono/PCM16");
        Check(ReadU16LE(cfg + SrcCfgOffset::emb_flags2) == 0, "embedded flags2 (not looping)");
        Check(ReadU16LE(cfg + SrcCfgOffset::buffers_dirty) == 0, "buffers_dirty == 0 (embedded only)");
    }

    // --- DspConfiguration builder ----------------------------------------
    {
        std::uint8_t cfg[kDspConfigSize];
        std::memset(cfg, 0xAA, sizeof(cfg));
        BuildDryDspConfig(cfg);

        const std::uint32_t want_dirty = DspDirty::master_volume | DspDirty::output_format |
                                         DspDirty::aux_bus_enable0 | DspDirty::aux_bus_enable1;
        Check(ReadU32LE(cfg + DspCfgOffset::dirty_raw) == want_dirty, "dsp dirty_raw mask");
        Check(ReadF32LE(cfg + DspCfgOffset::master_volume) == 1.0f, "master_volume == 1.0");
        Check(ReadU16LE(cfg + DspCfgOffset::output_format) == kOutputFormatStereo,
              "output_format == Stereo(1)");
        Check(ReadU16LE(cfg + DspCfgOffset::aux_bus_enable + 0) == 0, "aux_bus_enable[0] == 0");
        Check(ReadU16LE(cfg + DspCfgOffset::aux_bus_enable + 2) == 0, "aux_bus_enable[1] == 0");
        // reverb_effect blocks must be untouched (all zero) — effects OFF.
        bool reverb_zero = true;
        for (std::size_t i = 0; i < 2 * kReverbEffectSize; ++i)
            if (cfg[DspCfgOffset::reverb_effect + i] != 0) reverb_zero = false;
        Check(reverb_zero, "reverb_effect[2] left zero (reverb bypassed)");
    }

    if (g_failures == 0) {
        std::printf("config builders: all offset/encoding checks passed.\n");
        std::printf("OK\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
