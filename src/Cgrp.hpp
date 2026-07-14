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
	uint8_t* Offset;
	uint32_t Length;
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

	// Csar hands over the span its just-written .bcgrp was serialised from (a
	// pointer + length into Csar's own already-loaded buffer), so the group no
	// longer re-reads the file it was just written from. FileName stays the full
	// output path (its parent directory is the archive folder the group extracts
	// into). Data is borrowed, never owned: the sole construction site is a
	// stack-local in Csar::Extract, and Csar::Data outlives it, so the destructor
	// never frees it. The group's own children (Cbnk/Cseq borrow spans into this
	// Data, i.e. windows into Csar::Data; the group-resident Cwar copies) are all
	// freed before this borrowed Data would notionally end, so the chain is safe.
	Cgrp(const std::string& fileName, uint8_t* data, std::streamoff length, std::map<int, Cwar*>* cwars, const std::map<int, bool>& cseqsFromCsar, const std::map<int, std::string>& namesFromCsar, const Options& opts, ParseContext& ctx);
	~Cgrp();
	bool Extract();
};
