#pragma once

#include <cstdint>
#include <ios>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct ParseContext;

enum class SuffixType { None, Rnd, Var, Time, TimeRnd, TimeVar, If };
enum class ArgType { None, Uint8, Int8, Uint16, Int16, Rnd, Var, VarLen };

// Reads one argument of the given form, advancing pos. For a random range
// (ArgType::Rnd) the two raw s16 bounds are handed back through rndBounds (when
// non-null) so the model keeps them losslessly; the returned vector then carries
// the first bound as the slot's inert placeholder value -- never read as a value,
// since the exporter's midpoint stand-in is computed at emit (see Cseq::Export).
std::vector<int32_t> ReadArgs(ParseContext& Ctx, uint8_t*& pos, ArgType argType,
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

	// This command's own source offset within the DATA command space, relative
	// to DATA+8 -- the same value the command map is keyed on. Carried on the
	// record so the emit walk can locate a diagnostic (its DATA-relative
	// AT POSITION is DataOffset + 8 + Offset) from the model alone, without
	// reconstructing a raw pointer into the live buffer, and so the model stays
	// self-locating across a stage-1 buffer drop.
	uint32_t Offset = 0;
};

struct CseqLabl
{
	uint8_t* Offset;
	std::string Label;
};

struct Cseq
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	// Header words parsed off the CSEQ/DATA headers, retained for the emit phase
	// and round-trip. DataOffset locates the DATA section; the command map is
	// keyed relative to DataOffset + 8, and Export derives every diagnostic
	// position from DataOffset alone rather than from Data. Version is the CSEQ
	// 0x40-block version word (cseqVersion), previously read and discarded.
	uint32_t DataOffset = 0;
	uint32_t Version = 0;

	// The parsed command stream: offset (relative to DATA+8) -> command, built by
	// Parse and consumed by Export. Retained on the model so the walk locates
	// itself through stored offsets, not the live buffer -- the point of the
	// Parse/Export split.
	std::map<uint32_t, CseqCmd> Commands;

	// The parent hands over the span its just-written .bcseq was serialised from
	// (a pointer + length into the parent's already-loaded buffer), so the child
	// no longer re-reads the file it was just written from. FileName stays the
	// full output path (the .mid is written beside it). Data is borrowed, never
	// owned: every construction site's parent (Csar's own buffer for a direct
	// sequence, the stack-local Cgrp's buffer for a group-resident one) provably
	// outlives this Cseq, so the destructor never frees it.
	Cseq(const std::string& fileName, uint8_t* data, std::streamoff length, ParseContext& ctx);
	~Cseq();

	// Parse the CSEQ/LABL/DATA headers and build the command map into the model.
	// No file output; every Assert/Error fires here. Call once before Export.
	bool Parse();

	// Walk the parsed model (the convert-time VM + control-flow interpreter) and
	// write the .mid beside FileName. Reads only model state (Commands,
	// DataOffset), never Data -- so every diagnostic position is the true
	// DATA-relative offset regardless of what frame tops the shared stack.
	// startOffset is the entry's start position within the sequence data,
	// relative to DATA+8 (the same space the command map is keyed in). These
	// archives pack many entries into one shared sequence bank, each beginning
	// at its own offset; 0 (the default) starts from the top of the data. Call
	// only after a successful Parse().
	bool Export(uint32_t startOffset = 0);
};
