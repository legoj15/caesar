#pragma once

#include <cstdint>
#include <ios>
#include <string>
#include <vector>

struct ParseContext;

struct DspContext
{
	uint8_t PredScal;
	int16_t SampHist1;
	int16_t SampHist2;
};

struct CwavChan
{
	uint8_t* Offset;

	uint8_t* SampOffset;
	uint32_t AdpcmType;
	uint8_t* AdpcmOffset;

	int16_t DspCoeffs[16];
	DspContext DspCntx;
	DspContext DspLoopCntx;

	std::vector<int16_t> PcmSamples;
};

struct Cwav
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	uint8_t SampleMode;

	uint16_t ChanCount;
	uint32_t SampleRate;
	uint32_t LoopStart;
	uint32_t LoopEnd;

	std::vector<std::vector<int16_t>> Channels;

	bool Converted = false;

	// The owning wave archive hands over the span its just-written .bcwav was
	// serialised from (a pointer + length into the Cwar's buffer), so the child
	// no longer re-reads the file it was just written from. FileName stays the
	// full output path (the .wav is written beside it). Data is borrowed, never
	// owned: the Cwar frees its Cwav children before its own buffer, so the span
	// outlives this Cwav regardless of whether the Cwar owns or borrows it.
	Cwav(const std::string& fileName, uint8_t* data, std::streamoff length, ParseContext& ctx);
	~Cwav();
	bool Convert();
};
