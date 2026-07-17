// dsp-oracle — DSP shared-memory map: the pipe-2 region-address table and the
// struct-offset constants the harness needs right now, plus the road to
// commit-2 (SourceConfiguration) and commit-3 (DspConfiguration/ReverbEffect).
//
// protocol reference: Citra/Azahar audio_core/hle/shared_memory.h and
// audio_core/hle/hle.cpp (GPL); teakra (MIT). This is ORIGINAL caesar code
// (GPLv3) informed by, not copied from, those sources.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dsp_oracle {

// ---------------------------------------------------------------------------
// Word-vs-byte addressing (VERIFIED against lle.cpp + libctru ndsp.c)
// ---------------------------------------------------------------------------
// The Teak DSP is word-addressed (1 word = 16 bits). Every address the firmware
// hands the ARM11 — pipe_base_waddr, the region-address table, PipeStatus
// waddress — is a WORD address. Teakra lays DSP data memory out starting at byte
// offset 0x40000 inside GetDspMemory(); a data word W therefore lives at byte
// offset kDspDataOffset + W*2. (lle.cpp GetDspDataPointer + the *2 scaling on
// pipe_base_waddr and PipeStatus.waddress.) libctru's
// DSP_ConvertProcessAddressFromDspDram computes the ARM-visible address as
// (W << 1) + (DSP_RAM_VADDR + 0x40000), i.e. the same W*2 + 0x40000, confirming
// both the shift and the offset.
constexpr std::uint32_t kDspDataOffset = 0x40000; // byte offset of DATA space in GetDspMemory()
constexpr std::uint32_t kDspMemorySize = 0x80000; // full Teak address space (Teakra::DspMemorySize)

// Convert a DSP data-space WORD address to a byte offset into GetDspMemory().
constexpr std::uint32_t DataWordToByte(std::uint16_t word_addr) {
    return kDspDataOffset + static_cast<std::uint32_t>(word_addr) * 2u;
}

// ---------------------------------------------------------------------------
// Frame-parity double-banking (THE trap — read before commit-2/3)
// ---------------------------------------------------------------------------
// The application-visible shared region is DOUBLE-BANKED. The firmware exposes
// two full copies:
//     region 0  @ word 0x8000   (byte 0x50000 in GetDspMemory)
//     region 1  @ word 0x18000  (byte 0x70000)     == region-0 word | 0x10000
// Every entry in the region-address table below is the region-0 word address;
// the region-1 address is that word OR 0x10000 (ndsp.c does exactly:
// ConvertProcessAddressFromDspDram(vars[i] | 0x10000, ...)).
//
// Each frame the DSP reads the bank whose Frame Counter (table index 0) is
// higher (with wraparound handling — see hle.cpp CurrentRegionIndex) and writes
// its results (final mix, source status, dsp status) into the OTHER bank. The
// ARM11 must therefore write next-frame config into the bank it is about to
// bump the counter on, then bump that bank's counter. Writing config and the
// counter into the wrong bank for the parity => the DSP reads stale data and
// your impulse never reaches the mixer. libctru tracks this with
// ndspBufferCurId = ndspFrameId & 1 and alternates each frame.
constexpr std::uint32_t kRegion0Offset = 0x50000; // byte offset in GetDspMemory()
constexpr std::uint32_t kRegion1Offset = 0x70000;
// A region-1 word address is the region-0 word OR'd with 0x10000 (does not fit
// in u16, so region-1 addresses are always formed and held in u32).
constexpr std::uint32_t kRegion1WordOr = 0x10000u;

// ---------------------------------------------------------------------------
// Pipe-2 (Audio) region-address table
// ---------------------------------------------------------------------------
// After the ARM11 sends the Audio-pipe Initialize message and kicks the DSP
// (SetSemaphore 0x4000), the firmware writes this table into pipe 2 (DSPtoCPU):
//   [0]      u16 count (== kRegionTableCount, 15 on every audio firmware seen)
//   [1..N]   u16 region-0 word address of each region, in the fixed order below
// The ORDER (the pipe index) is stable across the real firmware and Citra/Azahar
// HLE; only the concrete word addresses vary between firmware builds, which is
// exactly why we read them at runtime instead of hardcoding. Index meanings
// (matches libctru ndspVars[i] and hle.cpp AudioPipeWriteStructAddresses):
enum RegionTableIndex : std::size_t {
    kIdxFrameCounter = 0,        // u16 counter, last word of the bank (double-buffer selector)
    kIdxSourceConfiguration = 1, // SourceConfiguration::Configuration[24], 192 B each  (COMMIT 2)
    kIdxSourceStatus = 2,        // SourceStatus::Status[24], 12 B each (DSP-written)
    kIdxAdpcmCoefficients = 3,   // AdpcmCoefficients (COMMIT 2, for ADPCM sources)
    kIdxDspConfiguration = 4,    // DspConfiguration, 196 B: master vol, aux, delay, REVERB (COMMIT 3)
    kIdxDspStatus = 5,           // DspStatus: [+0]=?, [+1 word]=dropped_frames
    kIdxFinalMixSamples = 6,     // FinalMixSamples: s16 pcm16[160][2] — the wet output, DSP-written
    kIdxIntermediateMix = 7,     // IntermediateMixSamples (mix1/mix2 pcm32)
    kIdxCompressor = 8,          // Compressor table (application-owned)
    kIdxDspDebug = 9,            // DSP debug info
    kIdxSurround10 = 10,         // surround-related
    kIdxSurround11 = 11,
    kIdxSurround12 = 12,
    kIdxSurround13 = 13,
    kIdxSurround14 = 14,
    kRegionTableCount = 15,
};

// ---------------------------------------------------------------------------
// Struct sizes / offsets the later commits will need (bytes). Kept here so the
// road is visible now; commit-1 only touches kIdxFrameCounter.
// ---------------------------------------------------------------------------
// COMMIT 2 — one dry source at the final mix:
//   SourceConfiguration::Configuration = 192 B/source, 24 sources at table[1].
//     dirty_raw(u32) @0x00; gain[3][4] float @0x04; rate_multiplier float @0x34;
//     interpolation_mode(u8) @0x38; embedded-buffer physical_address(u32_dsp)
//     near the tail. Set enable + a dirty mask each frame; the DSP clears dirty.
//   Sample data is fetched over AHBM from FCRAM: LoadComponent's AHBM callback
//   maps ARM physical (addr - 0x20000000) into a backing buffer. For a click we
//   point one source's embedded buffer at an FCRAM address we've filled. The DSP
//   pulls it via AHBMRead16/32. (See main.cpp SetAHBMCallback in lle.cpp.)
//   u32_dsp fields are MIDDLE-endian (halves swapped) — see u32_dsp in
//   shared_memory.h; the pcm16 final mix and float gains are plain little-endian.
//
// COMMIT 3 — engage reverb:
//   DspConfiguration @ table[4], 196 B. master_volume float @0x04;
//   aux_return_volume[2] @0x08; delay_effect[2] (20 B, fully mapped) then
//   reverb_effect[2] (52 B == 26 opaque DSP words) live in the effect array.
//   Set the reverb_effect dirty bit (dirty_raw bit12/bit13) + aux routing, write
//   the 26 words we recover from Mii Plaza's ARM11 code.bin, and capture the wet
//   tail from the audio callback. A malformed 52-byte block is silently bypassed
//   (dry) — that is SUITE-DESIGN risk #2; a known-good replayed config de-risks
//   it before sweeping the 26 words.
constexpr std::size_t kSourceConfigStride = 192;
constexpr std::size_t kNumSources = 24;
constexpr std::size_t kDspConfigSize = 196;
constexpr std::size_t kReverbEffectSize = 52; // 26 DSP words, opaque
constexpr std::size_t kFinalMixSamplesPerFrame = 160;

// ---------------------------------------------------------------------------
// COMMIT 2 — SourceConfiguration::Configuration byte map + dry-source builder
// ---------------------------------------------------------------------------
// Byte offsets within one 192-byte source slot, transcribed as PROTOCOL
// DOCUMENTATION (layout facts only — no code copied) from Azahar
// `src/audio_core/hle/shared_memory.h`, `struct SourceConfiguration::Configuration`
// (lines 117-309; `ASSERT_DSP_STRUCT(..., 192)` line 309). Individual fields
// cited by line inline. These offsets were also cross-checked by walking the
// struct field-by-field against that layout.
struct SrcCfgOffset {
    static constexpr std::size_t dirty_raw = 0x00;       // u32 LE, dirty bitmask (ln 121-147)
    static constexpr std::size_t gain = 0x04;            // float_le gain[3][4] (ln 156)
    static constexpr std::size_t rate_multiplier = 0x34; // float_le (ln 161)
    static constexpr std::size_t interpolation = 0x38;   // u8 InterpolationMode (ln 169)
    static constexpr std::size_t filters_enabled = 0x3A; // u16 (ln 202-206)
    static constexpr std::size_t buffers_dirty = 0x4A;   // u16 (ln 246)
    static constexpr std::size_t buffers = 0x4C;         // Buffer[4], 20 B each (ln 247)
    static constexpr std::size_t loop_related = 0x9C;    // u32_dsp (ln 251)
    static constexpr std::size_t enable = 0xA0;          // u8 (ln 252)
    static constexpr std::size_t sync_count = 0xA2;      // u16 (ln 254)
    static constexpr std::size_t play_position = 0xA4;   // u32_dsp, samples (ln 255)
    static constexpr std::size_t emb_phys_addr = 0xAC;   // u32_dsp, FCRAM phys (ln 262)
    static constexpr std::size_t emb_length = 0xB0;      // u32_dsp, samples (ln 266)
    static constexpr std::size_t emb_flags1 = 0xB4;      // u16 mono/stereo + format (ln 279-284)
    static constexpr std::size_t emb_adpcm_ps = 0xB6;    // u16 (ln 287-291)
    static constexpr std::size_t emb_adpcm_yn = 0xB8;    // u16[2] (ln 294)
    static constexpr std::size_t emb_flags2 = 0xBC;      // u16 adpcm_dirty/is_looping (ln 296-300)
    static constexpr std::size_t emb_buffer_id = 0xBE;   // u16 (ln 304)
};

// dirty_raw bit positions (Azahar shared_memory.h ln 121-147). The DSP re-reads
// only the fields whose dirty bit is set, then clears the mask each frame.
struct SrcDirty {
    static constexpr std::uint32_t format = 1u << 0;
    static constexpr std::uint32_t mono_stereo = 1u << 1;
    static constexpr std::uint32_t enable = 1u << 16;
    static constexpr std::uint32_t interpolation = 1u << 17;
    static constexpr std::uint32_t rate = 1u << 18;
    static constexpr std::uint32_t play_position = 1u << 21;
    static constexpr std::uint32_t gain0 = 1u << 25;
    static constexpr std::uint32_t gain1 = 1u << 26;
    static constexpr std::uint32_t gain2 = 1u << 27;
    static constexpr std::uint32_t embedded = 1u << 30;
};

// emb_flags1: mono_or_stereo = BitField<0,2> (Mono=1, ln 268-271); format =
// BitField<2,2> (PCM16=1, ln 273-277).
constexpr std::uint16_t kEmbFlags1_MonoPCM16 = (1u << 0) | (1u << 2); // 0x0005
constexpr std::uint8_t kInterpNone = 2; // InterpolationMode::None (ln 163-167)

// DspConfiguration byte map (Azahar shared_memory.h ln 326-432; sizeof==196
// asserted ln 428, offsetof(sync_mode)==0xBC ln 431, offsetof(dirty2_raw)==0xC0
// ln 432).
struct DspCfgOffset {
    static constexpr std::size_t dirty_raw = 0x00;         // u32 LE dirty bitmask (ln 329-352)
    static constexpr std::size_t master_volume = 0x04;     // float_le (ln 356)
    static constexpr std::size_t aux_return_volume = 0x08; // float_le[2] (ln 357)
    static constexpr std::size_t output_format = 0x16;     // u16 OutputFormat (ln 368)
    static constexpr std::size_t aux_bus_enable = 0x28;    // u16[2] (ln 378)
    static constexpr std::size_t reverb_effect = 0x54;     // ReverbEffect[2], 52 B each (ln 414-418)
    static constexpr std::size_t sync_mode = 0xBC;         // u16 (ln 420)
};
struct DspDirty {
    static constexpr std::uint32_t aux_bus_enable0 = 1u << 8;
    static constexpr std::uint32_t aux_bus_enable1 = 1u << 9;
    static constexpr std::uint32_t reverb_effect0 = 1u << 12;
    static constexpr std::uint32_t reverb_effect1 = 1u << 13;
    static constexpr std::uint32_t master_volume = 1u << 16;
    static constexpr std::uint32_t output_format = 1u << 26;
};
constexpr std::uint16_t kOutputFormatStereo = 1; // OutputFormat::Stereo (ln 362-366)

// --- little-/middle-endian field writers into a raw byte buffer -------------
inline void PutU16LE(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void PutU32LE(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
// u32_dsp: the DSP reads 32-bit values with the 16-bit halves swapped (middle-
// endian). Azahar u32_dsp::Convert = (v<<16)|(v>>16), ln 56-58; store that
// swapped value little-endian so the firmware reads back the true u32.
inline void PutU32Dsp(std::uint8_t* p, std::uint32_t v) {
    PutU32LE(p, (v << 16) | (v >> 16));
}
inline void PutF32LE(std::uint8_t* p, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    PutU32LE(p, bits);
}

// Build one dry, unity-gain, PCM16-mono source that plays an embedded buffer at
// the native rate (no SRC) straight to the MAIN mixer's front L/R — reverb/aux
// buses left at zero. `out` must have >= kSourceConfigStride bytes.
//   phys_addr      — FCRAM physical byte address of the PCM16 buffer.
//   length_samples — number of PCM16 samples in that buffer.
inline void BuildDrySourceConfig(std::uint8_t* out, std::uint32_t phys_addr,
                                 std::uint32_t length_samples) {
    std::memset(out, 0, kSourceConfigStride);
    PutU32LE(out + SrcCfgOffset::dirty_raw,
             SrcDirty::format | SrcDirty::mono_stereo | SrcDirty::enable |
                 SrcDirty::interpolation | SrcDirty::rate | SrcDirty::play_position |
                 SrcDirty::gain0 | SrcDirty::gain1 | SrcDirty::gain2 | SrcDirty::embedded);
    // gain[mixer][channel]: mixer 0 = MAIN (feeds the final DAC mix directly);
    // mixers 1,2 are the aux/effect buses. Front L = [0][0], front R = [0][1].
    // Leave rear ([0][2],[0][3]) and the aux mixers at 0 -> no reverb/aux path.
    PutF32LE(out + SrcCfgOffset::gain + 0 * 4, 1.0f); // gain[0][0] front L
    PutF32LE(out + SrcCfgOffset::gain + 1 * 4, 1.0f); // gain[0][1] front R
    PutF32LE(out + SrcCfgOffset::rate_multiplier, 1.0f);
    out[SrcCfgOffset::interpolation] = kInterpNone;
    out[SrcCfgOffset::enable] = 1;
    PutU32Dsp(out + SrcCfgOffset::play_position, 0);
    PutU32Dsp(out + SrcCfgOffset::emb_phys_addr, phys_addr);
    PutU32Dsp(out + SrcCfgOffset::emb_length, length_samples);
    PutU16LE(out + SrcCfgOffset::emb_flags1, kEmbFlags1_MonoPCM16);
    PutU16LE(out + SrcCfgOffset::emb_buffer_id, 1); // non-zero id
}

// Build a minimal DspConfiguration: master volume unity, stereo out, aux/effect
// buses OFF (reverb bypassed). `out` must have >= kDspConfigSize bytes.
inline void BuildDryDspConfig(std::uint8_t* out) {
    std::memset(out, 0, kDspConfigSize);
    PutU32LE(out + DspCfgOffset::dirty_raw,
             DspDirty::master_volume | DspDirty::output_format |
                 DspDirty::aux_bus_enable0 | DspDirty::aux_bus_enable1);
    PutF32LE(out + DspCfgOffset::master_volume, 1.0f);
    PutU16LE(out + DspCfgOffset::output_format, kOutputFormatStereo);
    // aux_bus_enable[0]=aux_bus_enable[1]=0 (memset) -> effect buses disabled.
}

} // namespace dsp_oracle
