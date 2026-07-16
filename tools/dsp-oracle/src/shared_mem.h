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

} // namespace dsp_oracle
