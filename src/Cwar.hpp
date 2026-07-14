#pragma once

#include "Cwav.hpp"

#include <cstdint>
#include <ios>
#include <string>
#include <vector>

struct ParseContext;

struct CwarCwav
{
	uint8_t* Offset;
	uint32_t Length;
};

struct Cwar
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;
	bool OwnsData = false;

	std::vector<Cwav*> Cwavs;

	// The parent hands over the span its just-written .bcwar was serialised
	// from (a pointer + length into the parent's already-loaded buffer), so the
	// child no longer re-reads the file it was just written from. FileName stays
	// the full output path (its directory is where the child .bcwav files are
	// written). ownsData = false borrows the parent's bytes (only where the
	// parent provably outlives this Cwar); true takes a private copy.
	Cwar(const std::string& fileName, uint8_t* data, std::streamoff length, bool ownsData, ParseContext& ctx);
	~Cwar();
	bool Extract();
};
