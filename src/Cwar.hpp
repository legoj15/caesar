#pragma once

#include "Cwav.hpp"

#include <cstdint>
#include <ios>
#include <string>
#include <vector>

struct ParseContext;

struct CwarCwav
{
	// Offset is stored relative to the .bcwar span base (Cwar::Data), not as a
	// raw pointer, so the record stays valid across a future buffer drop
	// (stage 1). Export reconstructs the read cursor as Data + Offset.
	uint32_t Offset;
	uint32_t Length;
};

struct Cwar
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;
	bool OwnsData = false;

	// Header word parsed off the CWAR/0x40 block, retained for round-trip.
	uint32_t Version = 0;              // cwarVersion

	// The FILE section as an offset+length window into Data (borrowed, never
	// copied: Data outlives this Cwar). A stage-1 buffer drop must make an
	// explicit copy-or-keep decision here, as the per-record Offsets below all
	// resolve against this same span.
	uint32_t FileSpanOffset = 0;
	uint32_t FileSpanLength = 0;

	// The INFO cwav table: one offset+length record per sub-file, built by
	// Parse and consumed by Export. Retained so the model survives past the walk.
	std::vector<CwarCwav> CwavRecords;

	std::vector<Cwav*> Cwavs;

	// The parent hands over the span its just-written .bcwar was serialised
	// from (a pointer + length into the parent's already-loaded buffer), so the
	// child no longer re-reads the file it was just written from. FileName stays
	// the full output path (its directory is where the child .bcwav files are
	// written). ownsData = false borrows the parent's bytes (only where the
	// parent provably outlives this Cwar); true takes a private copy.
	Cwar(const std::string& fileName, uint8_t* data, std::streamoff length, bool ownsData, ParseContext& ctx);
	~Cwar();

	// Parse the CWAR/INFO/FILE headers into the model (the cwav table + FILE
	// span). No file output; every Assert fires here.
	bool Parse();

	// Write each sub-file's .bcwav from the parsed model and recurse into the
	// child Cwavs (construct -> Parse -> ExportWav), per file. Call only after a
	// successful Parse().
	bool Export();
};
