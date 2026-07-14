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

Cwav::Cwav(const char* fileName, ParseContext& ctx) : Ctx(ctx), FileName(fileName)
{
	ifstream ifs(FileName, ios::binary | ios::ate);

	Length = ifs.tellg();
	Ctx.RequireOpen(ifs.good(), Length, FileName);
	Data = new uint8_t[Length];

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);

	ifs.seekg(0, ios::beg);
	ifs.read(reinterpret_cast<char*>(Data), Length);
	ifs.close();
}

Cwav::~Cwav()
{
	Ctx.Pop();

	delete[] Data;
}

bool Cwav::Convert()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43574156, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cwavVersion = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x2, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x7000, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t infoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x7001, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t dataOffset = Ctx.ReadFixLen(pos, 4);
	[[maybe_unused]] uint32_t dataLength = Ctx.ReadFixLen(pos, 4);

	pos = Data + infoOffset;

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint8_t codec = Ctx.ReadFixLen(pos, 1);
	SampleMode = Ctx.ReadFixLen(pos, 1);

	if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

	uint32_t sampleRate = Ctx.ReadFixLen(pos, 4);
	uint32_t loopStart = Ctx.ReadFixLen(pos, 4);
	uint32_t loopEnd = Ctx.ReadFixLen(pos, 4);
	[[maybe_unused]] uint32_t unalignedLoopStart = Ctx.ReadFixLen(pos, 4);
	uint16_t chanCount = Ctx.ReadFixLen(pos, 2);

	if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

	// A malformed wave with zero channels would later index chans[0] on an empty
	// vector; reject it cleanly instead of over-reading.
	if (chanCount == 0)
	{
		Ctx.Error(Data + infoOffset + 8, "at least one channel", chanCount);

		return false;
	}

	vector<CwavChan> chans;

	for (uint16_t i = 0; i < chanCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x7100, Ctx.ReadFixLen(pos, 4))) { return false; }

		CwavChan chan{};
		chan.Offset = Data + infoOffset + 28 + Ctx.ReadFixLen(pos, 4);

		chans.push_back(chan);
	}

	for (uint16_t i = 0; i < chanCount; ++i)
	{
		pos = chans[i].Offset;

		if (!Ctx.Assert(pos, 0x1F00, Ctx.ReadFixLen(pos, 4))) { return false; }

		chans[i].SampOffset = Data + dataOffset + 8 + Ctx.ReadFixLen(pos, 4);
		chans[i].AdpcmType = Ctx.ReadFixLen(pos, 4);
		uint32_t adpcmOffset = Ctx.ReadFixLen(pos, 4);

		switch (codec)
		{
			case 0:
			{
				pos = chans[i].SampOffset;

				for (uint32_t j = 0; j < loopEnd; ++j)
				{
					chans[i].PcmSamples.push_back(Ctx.ReadFixLen(pos, 1) << 8);
				}

				break;
			}

			case 1:
			{
				pos = chans[i].SampOffset;

				for (uint32_t j = 0; j < loopEnd; ++j)
				{
					chans[i].PcmSamples.push_back(Ctx.ReadFixLen(pos, 2, true, true));
				}

				break;
			}

			case 2:
			{
				chans[i].AdpcmOffset = chans[i].Offset + adpcmOffset;

				pos = chans[i].AdpcmOffset;

				for (uint8_t j = 0; j < 16; ++j)
				{
					chans[i].DspCoeffs[j] = Ctx.ReadFixLen(pos, 2, true, true);
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

				chans[i].DspCntx = dspCntx;
				chans[i].DspLoopCntx = dspLoopCntx;

				pos = chans[i].SampOffset;

				int8_t predScal = chans[i].DspCntx.PredScal;
				int16_t hist1 = chans[i].DspCntx.SampHist1;
				int16_t hist2 = chans[i].DspCntx.SampHist2;

				for (uint32_t j = 0; j < ceil(loopEnd / 14.0f); ++j)
				{
					predScal = Ctx.ReadFixLen(pos, 1);
					// The predictor selects one of 8 coefficient pairs, so mask to 3
					// bits: valid data is already 0-7 (identical result), while a
					// corrupt high nibble of 8-15 would otherwise index DspCoeffs
					// (16 entries) out of bounds at pred*2+1.
					int32_t pred = (predScal >> 4) & 0x7;
					int32_t scal = 1 << (predScal & 0xF);
					int16_t coef1 = chans[i].DspCoeffs[pred * 2];
					int16_t coef2 = chans[i].DspCoeffs[(pred * 2) + 1];

					uint32_t samplesToRead = min<uint32_t>(14, static_cast<uint32_t>(loopEnd - chans[i].PcmSamples.size()));

					for (uint32_t k = 0; k < samplesToRead; ++k)
					{
						int32_t adpcm = k % 2 == 0 ? nibbles[*pos >> 4] : nibbles[Ctx.ReadFixLen(pos, 1) & 0xF];
						int32_t distance = (scal * adpcm) << 11;
						int32_t predicted = (coef1 * hist1) + (coef2 * hist2);
						int32_t corrected = predicted + distance;
						int32_t scaled = (corrected + 1024) >> 11;

						if (scaled < -32768)
						{
							chans[i].PcmSamples.push_back(-32768);
						}
						else if (scaled > 32767)
						{
							chans[i].PcmSamples.push_back(32767);
						}
						else
						{
							chans[i].PcmSamples.push_back(scaled);
						}

						hist2 = hist1;
						hist1 = chans[i].PcmSamples.back();
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
				Ctx.Error(Data + infoOffset + 8, "A valid codec identifier", codec);

				return false;
			}
		}
	}

	uint32_t fmtLength = 16;
	uint16_t waveCodec = 1;
	uint16_t bitsPerSample = 16;
	uint32_t byteRate = (sampleRate * chanCount) * (bitsPerSample / 8);
	uint16_t blockAlign = chanCount * (bitsPerSample / 8);
	uint32_t waveDataLength = static_cast<uint32_t>((chans[0].PcmSamples.size() * chanCount) * (bitsPerSample / 8));
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
	ofs.write(reinterpret_cast<const char*>(&chanCount), 2);
	ofs.write(reinterpret_cast<const char*>(&sampleRate), 4);
	ofs.write(reinterpret_cast<const char*>(&byteRate), 4);
	ofs.write(reinterpret_cast<const char*>(&blockAlign), 2);
	ofs.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
	ofs.write("data", 4);
	ofs.write(reinterpret_cast<const char*>(&waveDataLength), 4);

	for (size_t i = 0; i < chans[0].PcmSamples.size(); ++i)
	{
		for (uint16_t j = 0; j < chanCount; ++j)
		{
			ofs.write(reinterpret_cast<const char*>(&chans[j].PcmSamples[i]), 2);
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

		ofs.write(reinterpret_cast<const char*>(&loopStart), 4);
		ofs.write(reinterpret_cast<const char*>(&loopEnd), 4);

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

	// Retain the decoded data so Cbnk can read it from the live object instead of
	// re-opening the .wav. Move the per-channel PCM out now that the .wav is written.
	ChanCount = chanCount;
	SampleRate = sampleRate;
	LoopStart = loopStart;
	LoopEnd = loopEnd;

	Channels.reserve(chanCount);

	for (uint16_t i = 0; i < chanCount; ++i)
	{
		Channels.push_back(std::move(chans[i].PcmSamples));
	}

	Converted = true;

	return true;
}
