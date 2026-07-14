#pragma once

#include <cstdint>
#include <ios>
#include <string>
#include <utility>
#include <vector>

enum class SuffixType { None, Rnd, Var, Time, TimeRnd, TimeVar, If };
enum class ArgType { None, Uint8, Int8, Uint16, Int16, Rnd, Var, VarLen };

// Reads one argument of the given form, advancing pos. For a random range
// (ArgType::Rnd) the two raw s16 bounds are handed back through rndBounds (when
// non-null) so the model keeps them losslessly; the returned vector then carries
// the first bound as the slot's inert placeholder value -- never read as a value,
// since the exporter's midpoint stand-in is computed at emit (see Cseq::Convert).
std::vector<int32_t> ReadArgs(uint8_t*& pos, ArgType argType,
	std::pair<int32_t, int32_t>* rndBounds = nullptr);

struct CseqCmd
{
	SuffixType Suffix1 = SuffixType::None;
	SuffixType Suffix2 = SuffixType::None;
	SuffixType Suffix3 = SuffixType::None;
	bool Extended = false;
	uint8_t Cmd;
	std::vector<int32_t> Args;

	ArgType Arg1 = ArgType::None;
	ArgType Arg2 = ArgType::None;

	// Raw random-range bounds, in file order (UNSORTED -- the hardware stores
	// them unsorted and both byte orders occur in the corpus; never sort or
	// normalize them). A Rnd prefix (0xA0 -> Suffix1 == Rnd) fills Arg1Rnd for
	// the Arg1-typed slot (a note length, rest, program or 0xB0-0xE4 parameter,
	// or an extended op's operand), and a TimeRnd prefix (0xA4 -> Suffix2 ==
	// TimeRnd) fills Arg2Rnd for the trailing _t ramp. Both default to (0, 0)
	// and are meaningful only when the matching suffix marks the slot Rnd. The
	// model retains the raw pair so it reconstructs the original bytes; the
	// exporter's midpoint stand-in ((lo + hi) / 2) is computed at emit, not
	// welded in at parse.
	std::pair<int32_t, int32_t> Arg1Rnd = { 0, 0 };
	std::pair<int32_t, int32_t> Arg2Rnd = { 0, 0 };

	std::string Label;
};

struct CseqLabl
{
	uint8_t* Offset;
	std::string Label;
};

struct Cseq
{
	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	Cseq(const char* fileName);
	~Cseq();

	// startOffset is the entry's start position within the sequence data,
	// relative to DATA+8 (the same space the command map is keyed in). These
	// archives pack many entries into one shared sequence bank, each beginning
	// at its own offset; 0 (the default) starts from the top of the data.
	bool Convert(uint32_t startOffset = 0);
};
