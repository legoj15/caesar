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

struct CgrpFile
{
	uint32_t Id;
	uint8_t* Offset;
	uint32_t Length;
};

struct Cgrp
{
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

	Cgrp(const char* fileName, std::map<int, Cwar*>* cwars, const std::map<int, bool>& cseqsFromCsar, const std::map<int, std::string>& namesFromCsar, const Options& opts);
	~Cgrp();
	bool Extract();
};
