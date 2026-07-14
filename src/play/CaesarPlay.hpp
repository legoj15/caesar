#pragma once

// caesar_play — the suite stage-2 offline dry player. Public surface shared by
// the loader, the voice DSP, the sequencer and the CLI. Nothing here reaches
// into caesar_core's export paths: the engine is a parallel consumer of the
// same parsed models (Csar/Cbnk/Cseq/Cwav), so the shipped converter's A/B and
// round-trip guards keep watching caesar_core unchanged.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ParseContext;
struct Csar;
struct Cbnk;
struct Cseq;

namespace play
{
	// The DSP's native mix rate and frame size (Azahar/Citra audio_types.h:
	// native_sample_rate = 32728, samples_per_frame = 160 -> 204.55 frames/s,
	// 4.889 ms/frame). Every voice is resampled to this bus; a single final
	// sinc upsample to the output rate is provably transparent (Nyquist 16.364 kHz).
	constexpr uint32_t kNativeRate = 32728;
	constexpr uint32_t kFrameSamples = 160;

	// One renderable sequence enumerated from an archive's INFO table. Only
	// Type 0x2203 in-archive ("Convertible") sequences appear -- external
	// streams (0x2201) and CWSD wave-sound blocks (0x2202) are not sequences.
	struct SequenceInfo
	{
		uint32_t index = 0;        // 0-based over the renderable sequences (the --seq index)
		std::string name;          // the INFO symbol name
		uint32_t bankIndex = 0;    // index into Csar::CbnkRecords: the bank this seq binds to
		uint32_t startOffset = 0;  // entry start within the shared sequence bank (DATA+8-relative)
		uint32_t recordIndex = 0;  // index into Csar::CseqRecords
	};

	// The loaded, resolved object graph for one archive plus (optionally) one
	// chosen sequence. Members are declared in dependency order so their reverse
	// destruction pops the ParseContext frames in the safe order: the seq/bank
	// (pushed last) release before the Csar (whose ~Csar frees the wave archives
	// it constructed), and the ParseContext outlives every object that holds a
	// reference to it. The wave archives themselves live in Csar::Cwars, decoded
	// in-memory by the loader (never written to disk).
	struct LoadedArchive
	{
		std::unique_ptr<ParseContext> ctx;
		std::unique_ptr<Csar> csar;
		std::unique_ptr<Cbnk> bank;  // the chosen sequence's bank; null until resolveSequence
		std::unique_ptr<Cseq> seq;   // the chosen sequence; null until resolveSequence
		uint32_t startOffset = 0;    // the chosen entry's start offset

		LoadedArchive();
		~LoadedArchive();

		LoadedArchive(const LoadedArchive&) = delete;
		LoadedArchive& operator=(const LoadedArchive&) = delete;
	};

	// Parse an archive read-only (Csar::Parse, no extraction/Export) and
	// construct+parse every internal wave archive into Csar::Cwars, decoding its
	// samples in memory. No files are written. Returns null on failure, with a
	// message already printed to stderr. Diagnostics the parse fires (Analyse
	// rows, notices) are collateral of loading a new surface and are discarded.
	std::unique_ptr<LoadedArchive> loadArchive(const std::string& path);

	// Enumerate the renderable sequences of a loaded archive, in INFO order.
	std::vector<SequenceInfo> listSequences(const Csar& csar);

	// Resolve one chosen sequence: construct+parse its bank (Csar::CbnkRecords
	// [bankIndex]) and its own .bcseq, filling arch.bank/arch.seq/arch.startOffset.
	// The bank's samples resolve live against the already-decoded Csar::Cwars, so
	// no disk round-trip happens. Returns false on failure (message to stderr).
	bool resolveSequence(LoadedArchive& arch, const SequenceInfo& info);
}
