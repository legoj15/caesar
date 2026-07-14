#pragma once

#include "Cbnk.hpp"
#include "Cseq.hpp"
#include "Cwar.hpp"
#include "Options.hpp"

#include <cstdint>
#include <ios>
#include <map>
#include <string>
#include <vector>

struct ParseContext;

struct CgrpFile
{
	uint32_t Id;

	// The internal/external discriminator, made explicit (it was an Offset ==
	// nullptr sentinel): Present is a marker == 0x1F00 entry whose data span lies
	// inside this group; the offset field is consumed unconditionally at parse (a
	// non-0x1F00 marker still advances past it) but only meaningful when Present.
	bool Present = false;
	uint32_t Offset = 0;   // span-relative (against Cgrp::Data); valid only when Present
	uint32_t Length = 0;

	uint32_t Marker = 0;   // retained presence marker (0x1F00 for an embedded file)
};

struct Cgrp
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	std::map<int, Cwar*>* Cwars;
	std::vector<Cbnk*> Cbnks;
	std::vector<Cseq*> Cseqs;
	std::map<int, bool> CseqsFromCsar;
	// File id -> the symbol name the CSAR level gave that file. A group-resident
	// bank/wave-archive/sequence carries only a numeric id in the group's own file
	// table, but the CSAR INFO section names it (and, for banks, leaves an empty
	// symbol-named directory). Looking the id up here lets the group extract into
	// that name instead of a numeric duplicate.
	std::map<int, std::string> NamesFromCsar;
	Options Opts;

	// --- Retained parse model ---
	//
	// Header words read and discarded before the split, retained for the stage-1
	// round-trip: Version (cgrpVersion), FileLength (the 0x7801 FILE-chunk length),
	// and the INFX (0x7802) chunk kept as an opaque offset+length span (both come
	// straight from the chunk table, so this one is a true offset+length span).
	uint32_t Version = 0;
	uint32_t FileLength = 0;
	uint32_t InfxOffset = 0;
	uint32_t InfxLength = 0;

	// The INFO file table (persistent), built by Parse and walked by Export.
	std::vector<CgrpFile> Files;

	// Csar hands over the span its just-written .bcgrp was serialised from (a
	// pointer + length into Csar's own already-loaded buffer), so the group no
	// longer re-reads the file it was just written from. FileName stays the full
	// output path (its parent directory is the archive folder the group extracts
	// into). Data is borrowed, never owned: the sole construction site is a
	// stack-local in Csar::Export, and Csar::Data outlives it, so the destructor
	// never frees it. The group's own children (Cbnk/Cseq borrow spans into this
	// Data, i.e. windows into Csar::Data; the group-resident Cwar copies) are all
	// freed before this borrowed Data would notionally end, so the chain is safe.
	Cgrp(const std::string& fileName, uint8_t* data, std::streamoff length, std::map<int, Cwar*>* cwars, const std::map<int, bool>& cseqsFromCsar, const std::map<int, std::string>& namesFromCsar, const Options& opts, ParseContext& ctx);
	~Cgrp();

	// Parse the CGRP/INFO headers and the file table into the model. No file
	// output, no child construction; every Assert/Error fires here. Call once
	// before Export.
	bool Parse();

	// Walk the parsed file table: dump each embedded sub-file, construct the child
	// Cbnk/Cseq/Cwar objects (in the file-table walk), then convert them in the
	// deferred second loops -- that deferral is what makes group-path warnings fire
	// with a sibling frame on top, so it is preserved exactly. Call only after a
	// successful Parse.
	bool Export();
};
