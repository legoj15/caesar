#include "Cwav.hpp"
#include "Common.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace std;

const int8_t nibbles[] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };

Cwav::Cwav(const string& fileName, uint8_t* data, streamoff length, ParseContext& ctx) : Ctx(ctx), FileName(fileName)
{
	Length = length;

	// The parent wave archive already holds these bytes -- the span its
	// just-written .bcwav was serialised from -- so borrow them directly instead
	// of re-opening the file we just wrote. A zero-length span is the same
	// degenerate condition the old file-path constructor rejected on an empty
	// re-read (RequireOpen on length <= 0); preserve that error identically, and
	// fire it before the Push echo so the stdout stream stays byte-for-byte
	// unchanged.
	Ctx.RequireOpen(true, Length, FileName);

	Data = data;

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);
}

Cwav::~Cwav()
{
	Ctx.Pop();

	// Data is borrowed from the parent wave archive's buffer (freed by the Cwar,
	// after this child); do not delete it here.
}

bool Cwav::Parse()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43574156, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	Version = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x2, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x7000, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t infoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x7001, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t dataOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t dataLength = Ctx.ReadFixLen(pos, 4);

	// Record the raw DATA-section payload as an offset+length window into Data
	// (no bytes copied; the parent buffer backs it). Stage 1's buffer drop must
	// decide here whether to copy or keep these bytes.
	DataSpanOffset = dataOffset;
	DataSpanLength = dataLength;

	pos = Data + infoOffset;

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	Codec = Ctx.ReadFixLen(pos, 1);
	SampleMode = Ctx.ReadFixLen(pos, 1);

	if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

	uint32_t sampleRate = Ctx.ReadFixLen(pos, 4);
	uint32_t loopStart = Ctx.ReadFixLen(pos, 4);
	uint32_t loopEnd = Ctx.ReadFixLen(pos, 4);
	UnalignedLoopStart = Ctx.ReadFixLen(pos, 4);
	uint16_t chanCount = Ctx.ReadFixLen(pos, 2);

	if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

	// A malformed wave with zero channels would later index channel 0 on an empty
	// vector; reject it cleanly instead of over-reading.
	if (chanCount == 0)
	{
		Ctx.Error(Data + infoOffset + 8, "at least one channel", chanCount);

		return false;
	}

	ChannelInfo.clear();
	ChannelInfo.reserve(chanCount);

	for (uint16_t i = 0; i < chanCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x7100, Ctx.ReadFixLen(pos, 4))) { return false; }

		CwavChan chan{};
		chan.InfoOffset = infoOffset + 28 + Ctx.ReadFixLen(pos, 4);

		ChannelInfo.push_back(chan);
	}

	// One decoded-PCM vector per channel; the decode writes straight into these.
	Channels.assign(chanCount, {});

	for (uint16_t i = 0; i < chanCount; ++i)
	{
		pos = Data + ChannelInfo[i].InfoOffset;

		if (!Ctx.Assert(pos, 0x1F00, Ctx.ReadFixLen(pos, 4))) { return false; }

		ChannelInfo[i].SampOffset = dataOffset + 8 + Ctx.ReadFixLen(pos, 4);
		ChannelInfo[i].AdpcmType = Ctx.ReadFixLen(pos, 4);
		uint32_t adpcmOffset = Ctx.ReadFixLen(pos, 4);

		vector<int16_t>& pcm = Channels[i];

		switch (Codec)
		{
			case 0:
			{
				pos = Data + ChannelInfo[i].SampOffset;

				for (uint32_t j = 0; j < loopEnd; ++j)
				{
					pcm.push_back(Ctx.ReadFixLen(pos, 1) << 8);
				}

				break;
			}

			case 1:
			{
				pos = Data + ChannelInfo[i].SampOffset;

				for (uint32_t j = 0; j < loopEnd; ++j)
				{
					pcm.push_back(Ctx.ReadFixLen(pos, 2, true, true));
				}

				break;
			}

			case 2:
			{
				ChannelInfo[i].AdpcmOffset = ChannelInfo[i].InfoOffset + adpcmOffset;

				pos = Data + ChannelInfo[i].AdpcmOffset;

				for (uint8_t j = 0; j < 16; ++j)
				{
					ChannelInfo[i].DspCoeffs[j] = Ctx.ReadFixLen(pos, 2, true, true);
				}

				DspContext dspCntx{};
				dspCntx.PredScal = Ctx.ReadFixLen(pos, 1);

				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 1))) { return false; }

				dspCntx.SampHist1 = Ctx.ReadFixLen(pos, 2, true, true);
				dspCntx.SampHist2 = Ctx.ReadFixLen(pos, 2, true, true);

				DspContext dspLoopCntx{};
				dspLoopCntx.PredScal = Ctx.ReadFixLen(pos, 1);

				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 1))) { return false; }

				dspLoopCntx.SampHist1 = Ctx.ReadFixLen(pos, 2, true, true);
				dspLoopCntx.SampHist2 = Ctx.ReadFixLen(pos, 2, true, true);

				ChannelInfo[i].DspCntx = dspCntx;
				ChannelInfo[i].DspLoopCntx = dspLoopCntx;

				pos = Data + ChannelInfo[i].SampOffset;

				int8_t predScal = ChannelInfo[i].DspCntx.PredScal;
				int16_t hist1 = ChannelInfo[i].DspCntx.SampHist1;
				int16_t hist2 = ChannelInfo[i].DspCntx.SampHist2;

				for (uint32_t j = 0; j < ceil(loopEnd / 14.0f); ++j)
				{
					predScal = Ctx.ReadFixLen(pos, 1);
					// The predictor selects one of 8 coefficient pairs, so mask to 3
					// bits: valid data is already 0-7 (identical result), while a
					// corrupt high nibble of 8-15 would otherwise index DspCoeffs
					// (16 entries) out of bounds at pred*2+1.
					int32_t pred = (predScal >> 4) & 0x7;
					int32_t scal = 1 << (predScal & 0xF);
					int16_t coef1 = ChannelInfo[i].DspCoeffs[pred * 2];
					int16_t coef2 = ChannelInfo[i].DspCoeffs[(pred * 2) + 1];

					uint32_t samplesToRead = min<uint32_t>(14, static_cast<uint32_t>(loopEnd - pcm.size()));

					for (uint32_t k = 0; k < samplesToRead; ++k)
					{
						int32_t adpcm = k % 2 == 0 ? nibbles[*pos >> 4] : nibbles[Ctx.ReadFixLen(pos, 1) & 0xF];
						int32_t distance = (scal * adpcm) << 11;
						int32_t predicted = (coef1 * hist1) + (coef2 * hist2);
						int32_t corrected = predicted + distance;
						int32_t scaled = (corrected + 1024) >> 11;

						if (scaled < -32768)
						{
							pcm.push_back(-32768);
						}
						else if (scaled > 32767)
						{
							pcm.push_back(32767);
						}
						else
						{
							pcm.push_back(scaled);
						}

						hist2 = hist1;
						hist1 = pcm.back();
					}
				}

				break;
			}

			case 3:
			{
				Ctx.Warning(Data + infoOffset + 8, "IMA ADPCM decoding not implemented", "waves left silent (IMA-ADPCM codec not implemented)");

				break;
			}

			default:
			{
				Ctx.Error(Data + infoOffset + 8, "A valid codec identifier", Codec);

				return false;
			}
		}
	}

	// Publish the SF2-path fields the live object exposes (Cbnk reads these).
	ChanCount = chanCount;
	SampleRate = sampleRate;
	LoopStart = loopStart;
	LoopEnd = loopEnd;

	Converted = true;

	return true;
}

void Cwav::ExportWav()
{
	uint32_t fmtLength = 16;
	uint16_t waveCodec = 1;
	uint16_t bitsPerSample = 16;
	uint32_t byteRate = (SampleRate * ChanCount) * (bitsPerSample / 8);
	uint16_t blockAlign = ChanCount * (bitsPerSample / 8);
	uint32_t waveDataLength = static_cast<uint32_t>((Channels[0].size() * ChanCount) * (bitsPerSample / 8));
	uint32_t length = 36 + waveDataLength;

	uint32_t smplLength = 60;
	uint32_t zero = 0;
	uint32_t sampleLoops = 1;

	if ((SampleMode % 2) != 0)
	{
		length += 8 + smplLength;
	}

	string wavName = FileName.substr(0, FileName.length() - 5).append("wav");
	ofstream ofs(wavName, ofstream::binary);

	ofs.write("RIFF", 4);
	ofs.write(reinterpret_cast<const char*>(&length), 4);
	ofs.write("WAVE", 4);
	ofs.write("fmt ", 4);
	ofs.write(reinterpret_cast<const char*>(&fmtLength), 4);
	ofs.write(reinterpret_cast<const char*>(&waveCodec), 2);
	ofs.write(reinterpret_cast<const char*>(&ChanCount), 2);
	ofs.write(reinterpret_cast<const char*>(&SampleRate), 4);
	ofs.write(reinterpret_cast<const char*>(&byteRate), 4);
	ofs.write(reinterpret_cast<const char*>(&blockAlign), 2);
	ofs.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
	ofs.write("data", 4);
	ofs.write(reinterpret_cast<const char*>(&waveDataLength), 4);

	for (size_t i = 0; i < Channels[0].size(); ++i)
	{
		for (uint16_t j = 0; j < ChanCount; ++j)
		{
			ofs.write(reinterpret_cast<const char*>(&Channels[j][i]), 2);
		}
	}

	if ((SampleMode % 2) != 0)
	{
		ofs.write("smpl", 4);
		ofs.write(reinterpret_cast<const char*>(&smplLength), 4);

		for (uint8_t i = 0; i < 7; ++i)
		{
			ofs.write(reinterpret_cast<const char*>(&zero), 4);
		}

		ofs.write(reinterpret_cast<const char*>(&sampleLoops), 4);

		for (uint8_t i = 0; i < 3; ++i)
		{
			ofs.write(reinterpret_cast<const char*>(&zero), 4);
		}

		ofs.write(reinterpret_cast<const char*>(&LoopStart), 4);
		ofs.write(reinterpret_cast<const char*>(&LoopEnd), 4);

		for (uint8_t i = 0; i < 2; ++i)
		{
			ofs.write(reinterpret_cast<const char*>(&zero), 4);
		}
	}

	ofs.close();

	// The ofstream was never checked before, so a failed or truncated write still
	// reported success. This only fires on a real I/O error, never on healthy data.
	if (!ofs)
	{
		throw runtime_error("could not write file (write failed or incomplete): " + wavName);
	}
}
