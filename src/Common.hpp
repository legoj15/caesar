#pragma once

#include <cstdint>
#include <iomanip>
#include <ios>
#include <iostream>
#include <map>
#include <stack>
#include <string>
#include <vector>

// A loaded file buffer and its extent, used to bounds-check reads.
struct Range
{
	uint8_t* base;
	uint8_t* end;
	std::string name;
};

// Normalize an archive-supplied output name (context-free; see the definition
// in Common.cpp for the rationale).
std::string TypedName(const std::string& name, const std::string& type);

// The mutable state a single parse threads through every reader, together with
// the helpers that read and diagnose against it: the file-name and base-offset
// stacks that diagnostics are positioned against, the loaded buffer extents
// CheckBounds validates against, the analysis Log, the per-input
// dropped/approximated Notices tally, and the -w flag. One instance is created
// in main and passed by reference (Csar -> Cgrp -> Cbnk/Cwar/Cwav/Cseq, mirroring
// the Options injection), which is what makes the readers reentrant. Its lifetime
// today still spans the whole run, so cross-input behaviour is unchanged;
// per-input scoping is a later, deliberate, output-changing step.
struct ParseContext
{
	bool ShowWarnings = false;
	std::stack<std::string> FileNames;
	std::stack<uint8_t*> Offsets;
	std::vector<Range> Buffers;
	std::vector<std::string> Log;

	// Counts, per top-level input, of content that was skipped or approximated
	// (keyed by a human-readable category). Populated regardless of -w so the
	// summary in FlushNotices can be shown by default; the per-item positional
	// detail behind those counts still only prints with -w.
	std::map<std::string, size_t> Notices;

	template<typename T>
	bool Assert(uint8_t* pos, T expected, T found)
	{
		if (found != expected)
		{
			std::cerr << std::hex << std::setfill('0') << std::uppercase << std::endl;
			std::cerr << "ERROR IN\t" << FileNames.top() << std::endl;
			std::cerr << "AT POSITION\t0x" << std::setw(8) << pos - Offsets.top() << std::endl;
			std::cerr << "EXPECTED\t0x" << std::setw(8) << expected << std::endl;
			std::cerr << "INSTEAD GOT\t0x" << std::setw(8) << found << std::endl;
			std::cerr << std::endl;

			return false;
		}

		return true;
	}

	template<typename T>
	void Error(uint8_t* pos, std::string expected, T found)
	{
		std::cerr << std::hex << std::setfill('0') << std::uppercase << std::endl;
		std::cerr << "ERROR IN\t" << FileNames.top() << std::endl;
		std::cerr << "AT POSITION\t0x" << std::setw(8) << pos - Offsets.top() << std::endl;
		std::cerr << "EXPECTED\t" << expected << std::endl;
		std::cerr << "INSTEAD GOT\t0x" << std::setw(8) << found << std::endl;
		std::cerr << std::endl;
	}

	void Warning(uint8_t* pos, std::string msg, const std::string& noticeCategory = "");
	void FlushNotices(const std::string& inputName);
	void Push(std::string fileName, uint8_t* data, std::streamoff length);
	void Pop();
	void Reset();
	void RequireOpen(bool streamOk, std::streamoff length, const std::string& fileName);
	void CheckBounds(uint8_t* pos, size_t bytes);
	void Analyse(std::string tag, uint32_t val);
	void Dump(std::string fileName);

	int32_t ReadFixLen(uint8_t*& pos, size_t bytes, bool littleEndian = true, bool isSigned = false);
	int32_t ReadVarLen(uint8_t*& pos);
};
