#pragma once

#include "Cwar.hpp"
#include "Options.hpp"

#include <ios>
#include <cstdint>
#include <string>
#include <map>
#include <utility>
#include <vector>

struct ParseContext;

// The raw sample reference the CWAV table parses: which wave-archive (Cwar) and
// which sample within it (Id). Key is the note's root key, filled in by the
// instrument walk when a note references this sample. The decoded PCM, sample
// rate, channel count and loop points are NOT stored here -- they are resolved
// live from the owning Cwav at SF2-emit time, so the parser never reaches into
// live Cwav objects (see Cbnk::Export's sample-creation loop). Cwar is the war
// index resolved as (raw first table word - 0x5000000); Serialize re-adds the
// 0x5000000 to reproduce the stored word.
struct CbnkCwav
{
	uint32_t Cwar = 0;
	uint32_t Id = 0;
	uint32_t Key = 0;
};

struct WaveSmpl
{
	uint32_t CuePointId;
	uint32_t Type;
	uint32_t Start;
	uint32_t End;
	uint32_t Fraction;
	uint32_t PlayCount;
};

struct CbnkNote
{
	bool Exists = true;

	// The note's file-order entry in the instrument's note-offset table: the raw
	// first word (0x5901 on an existing note, whatever the file carried otherwise)
	// and the body offset relative to the instrument body + 8. Retained so
	// Serialize rebuilds the table verbatim, including non-existent entries.
	uint32_t TableMagic = 0;
	uint32_t TableOffset = 0;

	// This note body's absolute offset within the .bcbnk (relative to Data), the
	// span-relative replacement for the old raw uint8_t* Offset. Parse locates the
	// body through it and the release-127 warning positions against it.
	uint32_t BodyOffset = 0;

	// The note body's first word. 0x6001 gates the four extra Word6001 words below;
	// any other value is a plain single-note body. Retained because NoteCount alone
	// cannot tell a layered 0x6001 note from a plain one at the same key.
	uint32_t Id = 0;

	// The raw CWAV index as stored in the note body. Kept separate from the resolved
	// Cwav pointer: the out-of-range substitution path repoints Cwav to the first
	// sample, so the original index would otherwise be lost to Serialize.
	uint32_t CwavIndex = 0;

	CbnkCwav* Cwav = nullptr;
	uint8_t StartNote = 0;
	uint8_t EndNote = 0;
	uint32_t RootKey = 0;
	uint32_t Volume = 0;
	uint32_t Pan = 0;
	float Tune = 1.0f;
	uint8_t Interpolation = 0;
	uint8_t Attack = 0;
	uint8_t Decay = 0;
	uint8_t Sustain = 0;
	uint8_t Hold = 0;
	uint8_t Release = 0;

	// Note words the parse walk reads and logs (Ctx.Analyse) but does not yet act
	// on, retained here so the model is lossless for the stage-1 round-trip. The
	// 0x6001 quartet is only present on layered (0x6001) notes; Flags is the note
	// flags word at 0x14 (0x21F on every observed bank); DataRef2C/30/34 are the
	// self-referential ADSHR DataRef chain (see the tune history in HISTORY.md).
	uint32_t Word08 = 0;
	uint32_t Word0C = 0;
	uint32_t Word6001_10 = 0;
	uint32_t Word6001_14 = 0;
	uint32_t Word6001_18 = 0;
	uint32_t Word6001_1C = 0;
	uint32_t Flags = 0;
	uint16_t Word28 = 0;
	uint32_t DataRef2C = 0;
	uint32_t DataRef30 = 0;
	uint32_t DataRef34 = 0;
};

struct CbnkInst
{
	bool Exists = true;

	// The instrument's file-order entry in the bank's instrument-offset table: the
	// raw first word (0x5900 on an existing instrument) and the body offset relative
	// to InfoOffset + 24. Retained so Serialize rebuilds the table verbatim.
	uint32_t TableMagic = 0;
	uint32_t TableOffset = 0;

	// This instrument body's absolute offset within the .bcbnk (relative to Data),
	// the span-relative replacement for the old raw uint8_t* Offset.
	uint32_t BodyOffset = 0;

	// The instrument-type discriminator word: 0x6000 (a single implicit note over
	// the whole keyboard), 0x6001 (a layered key-split list), or 0x6002 (a drum
	// kit, one note per key). Retained because the three serialize with different
	// body headers and 0x6000 is not recoverable from a 1-note NoteCount alone.
	uint32_t Type = 0;

	uint32_t NoteCount = 0;
	std::vector<CbnkNote> Notes;

	bool IsDrumKit = false;
};

struct Cbnk
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	std::map<int, Cwar*>* Cwars;
	Options Opts;

	// CBNK/INFO framing words retained for Serialize's exact reconstruction. The
	// file length is Length (the file-length header word equals it, asserted at
	// parse); Version is the 0x20-header version word, previously read and
	// discarded. CwavOffset/InstOffset are the INFO sub-table pointers.
	uint32_t Version = 0;
	uint32_t InfoOffset = 0;
	uint32_t InfoLength = 0;
	uint32_t CwavOffset = 0;
	uint32_t InstOffset = 0;

	// The parsed model, promoted from Convert's function locals so it survives the
	// Parse -> Export/Serialize split (the offsets on CbnkInst/CbnkNote reference
	// into Data, so this owner must outlive any use of them).
	std::vector<CbnkCwav> Cwavs;
	std::vector<CbnkInst> Insts;

	// Bytes the parser never reads -- note-body tails and inter-body padding the
	// offset tables skip over -- captured verbatim (offset relative to Data) so
	// Serialize reproduces the exact file layout. Only runs that contain a non-zero
	// byte are stored (Serialize zero-fills the rest); empty for all but a handful
	// of banks (corpus-wide only ~1.5 KB across 5 banks carry any unread byte), see
	// the round-trip write-up. Positional reconstruction plus this overlay is what
	// makes the region lossless despite the out-of-order, gapped body layout.
	std::vector<std::pair<uint32_t, std::vector<uint8_t>>> GapSpans;

	// The parent hands over the span its just-written .bcbnk was serialised from
	// (a pointer + length into the parent's already-loaded buffer), so the child
	// no longer re-reads the file it was just written from. FileName stays the
	// full output path (its directory is where the .sf2 is written). Data is
	// borrowed, never owned: every construction site's parent (Csar's own buffer
	// for a direct bank, the stack-local Cgrp's buffer for a group-resident one)
	// provably outlives this Cbnk, so the destructor never frees it.
	Cbnk(const std::string& fileName, uint8_t* data, std::streamoff length, std::map<int, Cwar*>* cwars, const Options& opts, ParseContext& ctx);
	~Cbnk();

	// Parse the CBNK/INFO headers and build the model (cwav table, instrument and
	// note bodies), then capture the unread-byte GapSpans. No SF2 output; every
	// parse-phase Assert/Error/Analyse/Warning fires here. Call once before Export
	// or Serialize.
	bool Parse();

	// Build and write the .sf2 from the parsed model, resolving live Cwav samples
	// from the shared Cwars. The per-sample <id>.wav stdout echo, the release-127
	// warning, and the missing-sample throw fire here. Call only after Parse().
	bool Export();

	// Re-serialize the parsed model back to the exact source .bcbnk bytes: the
	// inverse of Parse, reading ONLY model state (never Data) -- so it proves the
	// model is a lossless representation of the file. The body region is
	// reconstructed positionally (each instrument/note body written at its retained
	// offset into a Length-sized zero buffer) because the offset tables place bodies
	// out of order with gaps; the retained GapSpans overlay the few unread runs. The
	// stage-1 round-trip harness (caesar-roundtrip) compares the result against a
	// saved copy of the source span. Call only after Parse().
	std::vector<uint8_t> Serialize();
};
