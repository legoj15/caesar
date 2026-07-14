#pragma once

#include "Cwar.hpp"
#include "Options.hpp"

#include <cstdint>
#include <ios>
#include <map>
#include <string>
#include <vector>

struct ParseContext;

struct CsarStrg
{
	// Offset is span-relative (against Csar::Data), not a raw pointer, so the
	// record survives a stage-1 buffer drop; Parse resolves Data + Offset when it
	// reads the string bytes.
	uint32_t Offset;
	uint32_t Length;

	std::string String;
};

struct CsarFile
{
	// The INFO file table's internal/external discriminator, made explicit (it was
	// an Offset == nullptr sentinel): Internal is a 0x220C entry whose data span
	// lies inside this archive; Location (non-empty) is a 0x220D entry naming a
	// sibling file; neither is set for a 0xFFFFFFFF / out-of-range absent entry.
	bool Internal = false;      // 0x220C with an in-range data span (was Offset != nullptr)
	uint32_t Offset = 0;        // span-relative data start; valid only when Internal
	uint32_t Length = 0;        // data length; valid only when Internal

	std::string Location = "";  // sibling path for an external (0x220D) entry

	// A 0x220C entry carries a reserved third word that Parse read and logged
	// (Analyse "0x220C 0x08") then dropped; retained here as a typed field for the
	// stage-1 round-trip. Is220C marks the entries whose Analyse row Export replays.
	bool Is220C = false;
	uint32_t Word220C08 = 0;

	// The entry's own record offset within the INFO file table (span-relative
	// against Csar::Data): where this file's 0x220A-indexed record begins. Serialize
	// re-points the 0x220C data offset/length words here from the laid-out FILE
	// section (a 0x220D location record and an absent entry are copied through, so
	// only 0x220C entries are patched).
	uint32_t RecordOffset = 0;
};

struct CsarCwar
{
	uint32_t Offset;             // span-relative INFO record offset
	uint32_t Id;
	std::string FileName;

	// The record's second word, logged (Analyse "Cwar 0x04") and dropped before
	// the split; and the has-symbol-name flag that gates the string-table lookup.
	uint32_t Word04 = 0;
	bool HasFileName = false;
};

struct CsarCbnk
{
	uint32_t Offset;             // span-relative INFO record offset
	uint32_t Id;
	std::string FileName;

	// The three record words logged (Analyse "Cbnk 0x04/08/0C") and dropped.
	uint32_t Word04 = 0;
	uint32_t Word08 = 0;
	uint32_t Word0C = 0;
};

struct CsarCseq
{
	uint32_t Offset;             // span-relative INFO record offset
	uint32_t Id;
	std::string FileName;

	uint32_t Type = 0;           // 0x2201 external stream / 0x2202 CWSD / 0x2203 sequence
	uint32_t CbnkOffset = 0;     // offset of the record's bank-reference sub-structure

	// The three record words logged (Analyse "Cseq 0x04/08/14") and dropped.
	uint32_t Word04 = 0;
	uint32_t Word08 = 0;
	uint32_t Word14 = 0;

	// Span-relative position the 0x2201/0x2202 skip warning points at (the old
	// live `pos - 16`), reconstructed as Data + WarnOffset at export so the
	// pointer-form Warning keeps its exact current AT POSITION (pinned behaviour,
	// including the heap-nondeterminism this Csar site shares -- Known bugs).
	uint32_t WarnOffset = 0;

	// A 0x2203 sequence with in-archive data (Type == 0x2203 && Files[Id].Internal).
	// The bank-reference tail beyond CbnkOffset (StartOffset + BankIndex) is only
	// reached for these; the rest of that tail is unparsed and is covered by this
	// record's own Files[Id] data span for the stage-1 copy-through.
	bool Convertible = false;
	uint32_t StartOffset = 0;    // entry start within DATA+8 (bank-shared sequence entry)
	uint32_t BankIndex = 0;      // index into CbnkRecords whose directory hosts the .bcseq
};

struct CsarCgrp
{
	uint32_t Offset;             // span-relative INFO record offset (external-group warning position)
	uint32_t Id;
	std::string FileName;

	bool Valid = false;          // false for a 0xFFFFFFFF placeholder entry (skipped at export)
};

struct Csar
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	std::map<int, Cwar*> Cwars;
	Options Opts;

	// --- Retained parse model (the persistent archive record tree) ---
	//
	// Header words read and discarded before the split, retained for the stage-1
	// round-trip: Version (csarVersion); DeclaredLength (the 0x0C file-length word --
	// equals the physical Length for a self-contained archive, but is LARGER when the
	// archive references external content, e.g. Mario Kart 7's ctr_dash whose FILE
	// section is declared past its own end); the three section placements
	// (Strg/Info/File offset + length -- StrgOffset is 0xFFFFFFFF for the handful of
	// archives with no symbol table); FileLength (the 0x2002 FILE-section length
	// word, formerly [[maybe_unused]]); the STRG sub-offsets; the INFO end offset.
	uint32_t Version = 0;
	uint32_t DeclaredLength = 0;
	uint32_t StrgOffset = 0;
	uint32_t StrgLength = 0;
	uint32_t InfoOffset = 0;
	uint32_t InfoLength = 0;
	uint32_t FileOffset = 0;
	uint32_t FileLength = 0;
	uint32_t StrgStringsOffset = 0;
	uint32_t StrgUnknownOffset = 0;
	uint32_t InfoEndOffset = 0;

	// The INFO 8-entry reference block, in slot order (the seven sub-table ids plus
	// the 0x220B end marker). Parse switches on each id into a named offset; Serialize
	// re-emits the block in this exact slot order with recomputed offsets, so the
	// container round-trips even though the format does not fix the slot order.
	std::vector<uint32_t> InfoRefIds;

	// The INFO sub-table start offsets (span-relative against Csar::Data), the
	// Cseq/Cbnk/Cwar/Cgrp analogues of PlayerTableOffset/SetTableOffset below. Each
	// sub-table's record region (everything past its count + 0x220A-style entry
	// array) is copied through opaquely at Serialize -- the records carry unparsed
	// tails (the CSEQ bank-reference sub-structure) and per-entry reserved words, so
	// they are treated as spans, exactly like the player/set tables. Only the
	// framing (count + entry-offset array + the FILE table's 0x220C data offsets) is
	// recomputed.
	uint32_t CseqTableOffset = 0;
	uint32_t CbnkTableOffset = 0;
	uint32_t CwarTableOffset = 0;
	uint32_t CgrpTableOffset = 0;
	uint32_t FileTableOffset = 0;

	// The player (0x2102) and set (0x2104) tables are never parsed beyond their
	// entry-offset arrays; kept as opaque spans (the section start, span-relative,
	// plus the entry offsets Parse already walks). The per-entry record payload is
	// still unparsed -- stage 1 copies the section through; its byte length is not
	// recoverable from the header here (bounded by the next section / InfoEndOffset).
	uint32_t PlayerTableOffset = 0;
	std::vector<uint32_t> PlayerEntryOffsets;
	uint32_t SetTableOffset = 0;
	std::vector<uint32_t> SetEntryOffsets;

	std::vector<CsarStrg> Strgs;
	std::vector<CsarFile> Files;
	std::vector<CsarCwar> CwarRecords;
	std::vector<CsarCbnk> CbnkRecords;
	std::vector<CsarCseq> CseqRecords;
	std::vector<CsarCgrp> CgrpRecords;

	// File id -> the (type-prefixed) symbol name resolved at this level, for every
	// wave-archive/bank/sequence the INFO section enumerates. When a file's data
	// actually lives in a group, this level still names it (banks even get an empty
	// symbol-named directory), but the group's own file table carries only the
	// numeric id -- so this map is handed to Cgrp to carry the name across that
	// boundary and avoid a numeric-named duplicate.
	std::map<int, std::string> NamesById;

	// Which sequence ids Export converts directly at this (Csar) level; handed to
	// each Cgrp so a group does not re-extract a sequence already written here.
	// Populated by Export (after each sequence's conversion), consumed by the later
	// group walk -- the same lifecycle the former Extract-local map had.
	std::map<int, bool> CseqsFromCsar;

	Csar(const char* fileName, const Options& opts, ParseContext& ctx);
	~Csar();

	// outputDir is the base directory to extract into. When empty (the default),
	// each archive is extracted beside its own input file, into a folder named
	// after the archive (the historical behaviour). Otherwise every archive is
	// extracted into "<outputDir>/<archive-name>/". Composes the output directory,
	// then drives Parse then Export.
	bool Extract(const std::string& outputDir = "");

	// Parse the CSAR/STRG/INFO/FILE headers into the record tree above. No file
	// output, no directory creation, no child construction, no Analyse/Warning: on
	// well-formed input this phase is silent, and every Assert/Error fires here with
	// only the archive's own frame on the diagnostic stack. Call once before Export.
	bool Parse();

	// Walk the record tree in exactly the parse order and emit everything: the
	// Analyse (.log) rows replayed from the retained words (at the same stack state
	// the interleaved walk saw), the extracted sub-file blobs, the child
	// construction echoes, the Cwar/Cbnk/Cseq/Cgrp conversions, the skip/external
	// warnings, and finally Ctx.Dump. Reads child state from the model; the only
	// live-buffer reads are the child blob length + bounds checks the blob writes
	// need. Call only after a successful Parse.
	bool Export(const std::string& archiveDir);

	// Re-serialize the parsed container back to the exact source .bcsar bytes: the
	// inverse of Parse and the stage-1 proof criterion (parse -> Serialize -> sha
	// matches, corpus-wide). Rebuilds the CSAR header, the STRG string table, the
	// INFO section and the FILE section from the retained model; every section and
	// sub-table offset/length is recomputed from the laid-out content, child blobs
	// are copied through as spans (the deep BCSEQ/BCBNK re-embed is the optional
	// commit-5 capstone, not this), and a FILE entry whose data lies inside a CGRP
	// container blob is re-pointed into that copied container rather than emitted
	// twice. The only reads of Data are copy-through span reads (opaque record tails,
	// the STRG lookup tree, the 0x220B block, the child blobs); the round-trip
	// harness compares the result against a saved copy of the source. Call only after
	// a successful Parse.
	std::vector<uint8_t> Serialize();
};
