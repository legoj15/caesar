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
	// Offsets are stored relative to the .bcwav span base (Cwav::Data), not as
	// raw pointers, so the record stays valid across a future buffer drop
	// (stage 1). Parse reconstructs a read cursor as Data + <offset>.
	uint32_t InfoOffset;   // channel info block, INFO section
	uint32_t SampOffset;   // first sample, DATA section

	uint32_t AdpcmType;
	uint32_t AdpcmOffset;  // ADPCM info block, INFO section; 0 unless DSP-ADPCM

	int16_t DspCoeffs[16];
	DspContext DspCntx;
	DspContext DspLoopCntx;
};

struct Cwav
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	// Header words parsed off the CWAV/INFO headers, retained for round-trip.
	uint32_t Version = 0;              // cwavVersion (0x40-block version word)
	uint32_t UnalignedLoopStart = 0;  // INFO loop start before block alignment

	// Raw DATA-section payload as an offset+length window into Data. Borrowed,
	// never copied: Data is the parent Cwar's buffer, freed after this child, so
	// the span outlives the object. A stage-1 buffer drop must make an explicit
	// copy-or-keep decision here (offset+length alone no longer resolves once the
	// backing buffer is gone).
	uint32_t DataSpanOffset = 0;
	uint32_t DataSpanLength = 0;       // == the 0x7001 dataLength header word

	uint8_t Codec = 0;
	uint8_t SampleMode = 0;

	uint16_t ChanCount = 0;
	uint32_t SampleRate = 0;
	uint32_t LoopStart = 0;
	uint32_t LoopEnd = 0;

	// Per-channel parse records (span-relative offsets + DSP context/coefficients).
	std::vector<CwavChan> ChannelInfo;

	// Decoded PCM, one vector per channel. The SF2 path (Cbnk) consumes this live.
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

	// Parse the INFO header and decode PCM into the model. No file output; every
	// Assert, the IMA-ADPCM notice, and the codec/channel errors fire here.
	bool Parse();

	// Write the .wav beside FileName purely from the decoded model. Throws on a
	// failed or incomplete write. Call only after a successful Parse().
	void ExportWav();
};
