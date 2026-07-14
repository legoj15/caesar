#pragma once

#include "Cwar.hpp"
#include "Options.hpp"

#include <ios>
#include <cstdint>
#include <string>
#include <map>
#include <vector>

struct ParseContext;

// The raw sample reference the CWAV table parses: which wave-archive (Cwar) and
// which sample within it (Id). Key is the note's root key, filled in by the
// instrument walk when a note references this sample. The decoded PCM, sample
// rate, channel count and loop points are NOT stored here -- they are resolved
// live from the owning Cwav at SF2-emit time, so the parser never reaches into
// live Cwav objects (see Cbnk::Convert's sample-creation loop).
struct CbnkCwav
{
	uint32_t Cwar;
	uint32_t Id;
	uint32_t Key;
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
	uint8_t* Offset;

	CbnkCwav* Cwav;
	uint8_t StartNote;
	uint8_t EndNote;
	uint32_t RootKey;
	uint32_t Volume;
	uint32_t Pan;
	float Tune = 1.0f;
	uint8_t Interpolation;
	uint8_t Attack;
	uint8_t Decay;
	uint8_t Sustain;
	uint8_t Hold;
	uint8_t Release;

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
	uint8_t* Offset;

	uint32_t NoteCount;
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

	// The parent hands over the span its just-written .bcbnk was serialised from
	// (a pointer + length into the parent's already-loaded buffer), so the child
	// no longer re-reads the file it was just written from. FileName stays the
	// full output path (its directory is where the .sf2 is written). Data is
	// borrowed, never owned: every construction site's parent (Csar's own buffer
	// for a direct bank, the stack-local Cgrp's buffer for a group-resident one)
	// provably outlives this Cbnk, so the destructor never frees it.
	Cbnk(const std::string& fileName, uint8_t* data, std::streamoff length, std::map<int, Cwar*>* cwars, const Options& opts, ParseContext& ctx);
	~Cbnk();
	bool Convert();
};
