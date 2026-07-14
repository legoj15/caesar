#include "Cbnk.hpp"
#include "Common.hpp"
#include "Cwar.hpp"

#include <sf2cute.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sf2cute;
using namespace std;

const double AttackTable[] = { 13122, 6546, 4356, 3261, 2604, 2163, 1851, 1617, 1434, 1287, 1167, 1068, 984, 912, 849, 795, 747, 702, 666, 630, 600, 570, 543, 519, 498, 477, 459, 441, 426, 411, 396, 384, 372, 360, 348, 336, 327, 318, 309, 300, 294, 285, 279, 270, 264, 258, 252, 246, 240, 234, 231, 225, 219, 216, 210, 207, 201, 198, 195, 192, 186, 183, 180, 177, 174, 171, 168, 165, 162, 159, 156, 153.5, 153, 150, 147, 144, 141.5, 141, 138, 135.5, 135, 132, 129.5, 129, 126, 123.5, 123, 120.5, 120, 117, 114.5, 114, 111.5, 111, 108.5, 108, 105.7, 105.35, 105, 102.5, 102, 99.5, 99, 96.7, 96.35, 96, 93.5, 93, 90, 87, 81, 75, 72, 69, 63, 60, 54, 48, 45, 39, 36, 30, 24, 21, 15, 12, 9, 6.1e-6 };
const double HoldTable[] = { 6e-6, 1, 2, 4, 6, 9, 12, 16, 20, 25, 30, 36, 42, 49, 56, 64, 72, 81, 90, 100, 110, 121, 132, 144, 156, 169, 182, 196, 210, 225, 240, 256, 272, 289, 306, 324, 342, 361, 380, 400, 420, 441, 462, 484, 506, 529, 552, 576, 600, 625, 650, 676, 702, 729, 756, 784, 812, 841, 870, 900, 930, 961, 992, 1024, 1056, 1089, 1122, 1156, 1190, 1225, 1260, 1296, 1332, 1369, 1406, 1444, 1482, 1521, 1560, 1600, 1640, 1681, 1722, 1764, 1806, 1849, 1892, 1936, 1980, 2025, 2070, 2116, 2162, 2209, 2256, 2304, 2352, 2401, 2450, 2500, 2550, 2601, 2652, 2704, 2756, 2809, 2862, 2916, 2970, 3025, 3080, 3136, 3192, 3249, 3306, 3364, 3422, 3481, 3540, 3600, 3660, 3721, 3782, 3844, 3906, 3969, 4032, 4096 };
// Envelope decay/release RATE table, indexed by the 0-126 parameter byte (127
// is the "instant" sentinel, special-cased in ConvertDecay/ConvertRelease and
// never read here). Values are a negative slope; larger magnitude = faster
// fade. Over indices 50-126 the curve is exactly -1.2 / (126 - index); below 50
// it is a gentler near-linear ramp that meets it continuously. Eight tail
// entries were historically mistyped 10x too large (a decimal shifted one place
// right), which made those instruments fade ~10x too fast: indices 114, 120,
// 121, 122, 123, 124, 125, 126 now read -0.1, -0.2, -0.24, -0.3, -0.4, -0.6,
// -1.2, -2.4 (were -1, -2, -2.4, -3, -4, -6, -12, -24). Keep values on the
// curve when editing.
const double DecayTable[] = { -0.00016, -0.00047, -0.00078, -0.00109, -0.00141, -0.00172, -0.00203, -0.00234, -0.00266, -0.00297, -0.00328, -0.00359, -0.00391, -0.00422, -0.00453, -0.00484, -0.00516, -0.00547, -0.00578, -0.00609, -0.00641, -0.00672, -0.00703, -0.00734, -0.00766, -0.00797, -0.00828, -0.00859, -0.00891, -0.00922, -0.00953, -0.00984, -0.01016, -0.01047, -0.01078, -0.01109, -0.01141, -0.01172, -0.01203, -0.01234, -0.01266, -0.01297, -0.01328, -0.01359, -0.01391, -0.01422, -0.01453, -0.01484, -0.01516, -0.01547, -0.01579, -0.016, -0.01622, -0.01644, -0.01667, -0.0169, -0.01714, -0.01739, -0.01765, -0.01791, -0.01818, -0.01846, -0.01875, -0.01905, -0.01935, -0.01967, -0.02, -0.02034, -0.02069, -0.02105, -0.02143, -0.02182, -0.02222, -0.02264, -0.02308, -0.02353, -0.024, -0.02449, -0.025, -0.02553, -0.02609, -0.02667, -0.02727, -0.02791, -0.02857, -0.02927, -0.03, -0.03077, -0.03158, -0.03243, -0.03333, -0.03429, -0.03529, -0.03636, -0.0375, -0.03871, -0.04, -0.04138, -0.04286, -0.04444, -0.04615, -0.048, -0.05, -0.05217, -0.05455, -0.05714, -0.06, -0.06316, -0.06667, -0.07059, -0.075, -0.08, -0.08571, -0.09231, -0.1, -0.10909, -0.12, -0.13333, -0.15, -0.17143, -0.2, -0.24, -0.3, -0.4, -0.6, -1.2, -2.4, -65535 };

double ChangeLogBase(double x, double base)
{
	return log(x) / log(base);
}

double ConvertTime(double time)
{
	double timeCents = 1200 * ChangeLogBase(time, 2);

	return timeCents < -12000 ? -12000 : timeCents;
}

double ConvertVolume(uint32_t volume)
{
	return 200 * abs(ChangeLogBase(pow((static_cast<double>(volume) / 127.0f), 2), 10));
}

double ConvertPan(uint32_t pan)
{
	double sf2Pan = (static_cast<double>(pan) - 64.0f) * (500.0f / 63.0f);

	return sf2Pan < -500 ? -500 : sf2Pan;
}

// AttackTable/HoldTable/DecayTable each hold 128 entries indexed by the 0-127
// parameter byte; the parameters are read as raw 0-255 bytes, so a corrupt bank
// could index past the table. Clamp to the last entry (127 is the "instant"
// sentinel the decay/release curves already special-case). A no-op on valid data.
double ConvertAttack(uint8_t attack)
{
	return ConvertTime(AttackTable[attack > 127 ? 127 : attack] / 1000);
}

double ConvertHold(uint8_t hold)
{
	return ConvertTime(HoldTable[hold > 127 ? 127 : hold] / 1000);
}

double ConvertDecay(uint8_t decay, uint8_t sustain)
{
	if (decay > 127) { decay = 127; }

	double sustainVolume = 20 * ChangeLogBase(pow((static_cast<double>(sustain) / 127.0f), 2), 10);

	if (decay == 127)
	{
		return -12000;
	}
	else
	{
		if (sustain == 0)
		{
			return ConvertTime(-90.25 / DecayTable[decay] / 1000);
		}
		else
		{
			return ConvertTime(sustainVolume / DecayTable[decay] / 1000);
		}
	}
}

double ConvertRelease(uint8_t release, uint8_t sustain, double padSustainSeconds)
{
	if (release > 127) { release = 127; }

	double sustainVolume = 20 * ChangeLogBase(pow((static_cast<double>(sustain) / 127.0f), 2), 10);

	if (release == 127)
	{
		// 127 is the fastest-rate sentinel, i.e. an effectively instant cutoff.
		// The NW4C rate conversion (Mii Plaza code.bin @ 0x201E3C) is the Wii
		// NW4R EnvGenerator::CalcRelease verbatim, including
		// `if (x == 127) return 65535.0f;` -- 65535 being the fastest per-ms
		// rate. The seconds-long tail heard on R=127 pads is the DSP reverb,
		// which the sequence carries as a CC91 send, not note release.
		//
		// --pad-sustain deliberately breaks that to fake the tail; see Options.
		return padSustainSeconds > 0 ? ConvertTime(padSustainSeconds) : -12000;
	}
	else
	{
		if (sustain == 0)
		{
			return ConvertTime(-90.25 / DecayTable[release] / 1000);
		}
		else
		{
			return ConvertTime((-90.25 - sustainVolume) / DecayTable[release] / 1000);
		}
	}
}

double ConvertSustain(uint8_t sustain)
{
	if (sustain == 0)
	{
		return 900;
	}
	else
	{
		return 200 * abs(ChangeLogBase(pow((static_cast<double>(sustain) / 127.0f), 2), 10));
	}
}

// The per-note tune field (Cbnk Note 0x24) is an f32 frequency ratio applied on
// top of the root key -- 1.0 for almost every note, but some carry a genuine
// detune (e.g. 0.9828 -> -30 cents, 1.0087 -> +15 cents). Convert the ratio to a
// pitch offset in cents the SF2 coarse/fine tune generators can carry:
// cents = 1200 * log2(ratio).
double ConvertTune(float tune)
{
	return 1200 * ChangeLogBase(tune, 2);
}

Cbnk::Cbnk(const string& fileName, uint8_t* data, streamoff length, map<int, Cwar*>* cwars, const Options& opts, ParseContext& ctx) : Ctx(ctx), FileName(fileName), Cwars(cwars), Opts(opts)
{
	Length = length;

	// The parent already holds these bytes -- the span its just-written .bcbnk
	// was serialised from -- so borrow them directly instead of re-opening the
	// file we just wrote. A zero-length span is the same degenerate condition
	// the old file-path constructor rejected on an empty re-read (RequireOpen on
	// length <= 0); preserve that error identically, and fire it before the Push
	// echo so the stdout stream stays byte-for-byte unchanged.
	Ctx.RequireOpen(true, Length, FileName);

	Data = data;

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);
}

Cbnk::~Cbnk()
{
	Ctx.Pop();

	// Data is borrowed from the parent's buffer (freed by the parent, after this
	// child); do not delete it here.
}

bool Cbnk::Convert()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43424E4B, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x20, Ctx.ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cbnkVersion = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x1, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x5800, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t infoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x100, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t cwavOffset = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x101, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t instOffset = Ctx.ReadFixLen(pos, 4);

	pos = Data + infoOffset + 8 + cwavOffset;

	uint32_t cwavCount = Ctx.ReadFixLen(pos, 4);

	vector<CbnkCwav> cwavs;

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		pos = Data + infoOffset + 8 + cwavOffset + 4 + (i * 8);

		// Value-initialize: Key is only assigned when a note references this
		// sample, but every sample with Id < 0xF000 is still emitted (with Key as
		// its shdr byOriginalKey). Without this, an unreferenced sample wrote an
		// uninitialized byte, making SF2 output non-deterministic.
		CbnkCwav cwav{};
		cwav.Cwar = Ctx.ReadFixLen(pos, 4) - 0x5000000;
		cwav.Id = Ctx.ReadFixLen(pos, 4);

		size_t j = 0;
		auto it = Cwars->begin();

		for (; it != Cwars->end(); ++it, ++j)
		{
			if (j == cwav.Cwar)
			{
				break;
			}
		}

		if (cwav.Id < 0xF000)
		{
			// The sample was already decoded in memory when its wave-archive was
			// extracted; read it back from the live Cwav instead of re-opening the
			// .wav. The positional lookup above located that wave-archive (advance
			// begin() by Cwar). Guard the cases that were previously a missing-file
			// throw or a crash: past the map's end, an absent wave-archive (nullptr),
			// an out-of-range id, or a sample that never converted. On healthy data
			// none of these fire, so this matches the old RequireOpen-throws contract.
			if (it == Cwars->end() || it->second == nullptr || cwav.Id >= it->second->Cwavs.size() || !it->second->Cwavs[cwav.Id]->Converted)
			{
				throw runtime_error("could not open or read file (missing, empty, or unreadable): " + to_string(cwav.Id) + ".wav");
			}

			Cwav* src = it->second->Cwavs[cwav.Id];

			// Keep the per-sample stdout echo (empty range: no reads happen here now).
			Ctx.Push(to_string(cwav.Id) + ".wav", nullptr, 0);

			cwav.ChanCount = src->ChanCount;
			cwav.SampleRate = src->SampleRate;

			if (src->ChanCount == 1)
			{
				cwav.LeftSamples = src->Channels[0];
			}
			else if (src->ChanCount == 2)
			{
				cwav.LeftSamples = src->Channels[0];
				cwav.RightSamples = src->Channels[1];
			}
			else
			{
				// The old read-back pushed every 2-byte word into LeftSamples unless
				// the wave had exactly two channels, so a >2-channel wave collapsed
				// into one frame-interleaved LeftSamples stream. Reproduce that.
				for (size_t s = 0; s < src->Channels[0].size(); ++s)
				{
					for (uint16_t c = 0; c < src->ChanCount; ++c)
					{
						cwav.LeftSamples.push_back(src->Channels[c][s]);
					}
				}
			}

			// A smpl chunk is written only when SampleMode is odd; it carried the raw
			// loop points, which the old code recovered even when the decoded PCM was
			// empty (the IMA-ADPCM case: a smpl chunk but zero samples).
			if ((src->SampleMode % 2) != 0)
			{
				cwav.Loop = true;
				cwav.LoopStart = src->LoopStart;
				cwav.LoopEnd = src->LoopEnd;
			}
			else
			{
				cwav.LoopStart = 0;
				cwav.LoopEnd = static_cast<uint32_t>(cwav.LeftSamples.size());
			}

			Ctx.Pop();
		}

		cwavs.push_back(cwav);
	}

	pos = Data + infoOffset + 8 + instOffset;

	uint32_t instCount = Ctx.ReadFixLen(pos, 4);

	vector<CbnkInst> insts;

	for (uint32_t i = 0; i < instCount; ++i)
	{
		CbnkInst inst{};

		if (Ctx.ReadFixLen(pos, 4) != 0x5900)
		{
			inst.Exists = false;
		}

		inst.Offset = Data + infoOffset + 24 + Ctx.ReadFixLen(pos, 4);

		insts.push_back(inst);
	}

	for (uint32_t i = 0; i < instCount; ++i)
	{
		if (!insts[i].Exists)
		{
			continue;
		}

		pos = insts[i].Offset;

		uint32_t instType = Ctx.ReadFixLen(pos, 4);

		if (!Ctx.Assert(pos, 0x8, Ctx.ReadFixLen(pos, 4))) { return false; }

		switch (instType)
		{
			case 0x6000:
			{
				insts[i].NoteCount = 1;

				CbnkNote note{};
				note.StartNote = 0;
				note.EndNote = 127;

				insts[i].Notes.push_back(note);

				break;
			}

			case 0x6001:
			{
				insts[i].NoteCount = Ctx.ReadFixLen(pos, 4);

				for (uint32_t j = 0; j < insts[i].NoteCount; ++j)
				{
					CbnkNote note{};
					note.StartNote = j == 0 ? 0 : insts[i].Notes[j - 1].EndNote + 1;
					note.EndNote = Ctx.ReadFixLen(pos, 1);

					insts[i].Notes.push_back(note);
				}

				uint8_t padding = insts[i].NoteCount % 4;

				if (padding)
				{
					if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 4 - padding))) { return false; }
				}

				break;
			}

			case 0x6002:
			{
				insts[i].NoteCount = Ctx.ReadFixLen(pos, 2, false) + 1;

				for (uint32_t j = 0; j < insts[i].NoteCount; ++j)
				{
					CbnkNote note{};
					note.StartNote = j;
					note.EndNote = j;

					insts[i].Notes.push_back(note);
				}

				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

				insts[i].IsDrumKit = true;

				break;
			}

			default:
			{
				Ctx.Error(pos - 8, "A valid instrument type", instType);

				return false;
			}
		}

		for (uint32_t j = 0; j < insts[i].NoteCount; ++j)
		{
			if (Ctx.ReadFixLen(pos, 4) != 0x5901)
			{
				insts[i].Notes[j].Exists = false;
			}

			insts[i].Notes[j].Offset = insts[i].Offset + 8 + Ctx.ReadFixLen(pos, 4);
		}

		for (uint32_t j = 0; j < insts[i].NoteCount; ++j)
		{
			if (!insts[i].Notes[j].Exists)
			{
				continue;
			}

			pos = insts[i].Notes[j].Offset;

			uint32_t id = Ctx.ReadFixLen(pos, 4);

			if (!Ctx.Assert(pos, 0x8, Ctx.ReadFixLen(pos, 4))) { return false; }
			Ctx.Analyse("Note 0x08", Ctx.ReadFixLen(pos, 4));
			Ctx.Analyse("Note 0x0C", Ctx.ReadFixLen(pos, 4));

			if (id == 0x6001)
			{
				Ctx.Analyse("Note 0x6001 0x10", Ctx.ReadFixLen(pos, 4));
				Ctx.Analyse("Note 0x6001 0x14", Ctx.ReadFixLen(pos, 4));
				Ctx.Analyse("Note 0x6001 0x18", Ctx.ReadFixLen(pos, 4));
				Ctx.Analyse("Note 0x6001 0x1C", Ctx.ReadFixLen(pos, 4));
			}

			uint32_t cwav = Ctx.ReadFixLen(pos, 4);

			if (cwav < cwavs.size())
			{
				insts[i].Notes[j].Cwav = &cwavs[cwav];
			}
			else if (!cwavs.empty())
			{
				Ctx.Warning(pos - 4, "CWAV " + to_string(cwav) + " does not exist", "instrument notes referencing a missing sample (substituted the first sample)");

				insts[i].Notes[j].Cwav = &cwavs[0];
			}
			else
			{
				// The bank has no samples at all, so there is nothing to substitute
				// and every following field dereferences Cwav. Fail this bank cleanly
				// (the per-input handler isolates it) rather than deref an empty vector.
				Ctx.Error(pos - 4, "a bank containing at least one sample", cwav);

				return false;
			}

			uint32_t noteFlags = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x14", noteFlags);

			// For ordinary (non-0x6001) notes this word is the note's flags, which
			// is 0x21F across every observed bank; the field layout parsed below is
			// hardcoded for that value. Surface a note whose flags differ, since its
			// envelope/pitch/pan fields may then sit at other offsets and misparse.
			if (id != 0x6001 && noteFlags != 0x21F)
			{
				ostringstream flagsMsg;
				flagsMsg << "note flags 0x" << hex << uppercase << noteFlags << " (expected 0x21F)";

				Ctx.Warning(pos - 4, flagsMsg.str(), "bank notes with an unrecognized flags word (envelope/pitch/pan may be misparsed)");
			}

			insts[i].Notes[j].RootKey = Ctx.ReadFixLen(pos, 4);
			insts[i].Notes[j].Cwav->Key = insts[i].Notes[j].RootKey;
			insts[i].Notes[j].Volume = Ctx.ReadFixLen(pos, 4);
			insts[i].Notes[j].Pan = Ctx.ReadFixLen(pos, 4);

			// Note 0x24 is an f32 pitch ratio (frequency multiplier on top of the
			// root key), stored as raw little-endian bits. Reinterpret those bits
			// as a float and keep it; emitted as SF2 tune generators below.
			uint32_t tuneBits = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x24", tuneBits);

			float tune;
			memcpy(&tune, &tuneBits, sizeof(tune));
			insts[i].Notes[j].Tune = tune;

			Ctx.Analyse("Note 0x28", Ctx.ReadFixLen(pos, 2));

			insts[i].Notes[j].Interpolation = Ctx.ReadFixLen(pos, 1);

			if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 1))) { return false; }
			Ctx.Analyse("Note 0x2C", Ctx.ReadFixLen(pos, 4));
			Ctx.Analyse("Note 0x30", Ctx.ReadFixLen(pos, 4));
			Ctx.Analyse("Note 0x34", Ctx.ReadFixLen(pos, 4));

			insts[i].Notes[j].Attack = Ctx.ReadFixLen(pos, 1);
			insts[i].Notes[j].Decay = Ctx.ReadFixLen(pos, 1);
			insts[i].Notes[j].Sustain = Ctx.ReadFixLen(pos, 1);
			insts[i].Notes[j].Hold = Ctx.ReadFixLen(pos, 1);
			insts[i].Notes[j].Release = Ctx.ReadFixLen(pos, 1);

			if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 3))) { return false; }
		}
	}

	SoundFont sf2;
	sf2.set_sound_engine("EMU8000");
	sf2.set_bank_name(filesystem::path(FileName).stem().string());
	sf2.set_rom_name("ROM");
	sf2.set_software("Caesar");

	map<uint32_t, shared_ptr<SFSample>> leftSamples;
	map<uint32_t, shared_ptr<SFSample>> rightSamples;

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		if (cwavs[i].Id >= 0xF000)
		{
			continue;
		}

		if (cwavs[i].ChanCount == 1)
		{
			leftSamples[cwavs[i].Id] = sf2.NewSample(to_string(cwavs[i].Id), cwavs[i].LeftSamples, cwavs[i].LoopStart, cwavs[i].LoopEnd, cwavs[i].SampleRate, cwavs[i].Key, 0);
		}
		else
		{
			leftSamples[cwavs[i].Id] = sf2.NewSample(to_string(cwavs[i].Id) + "l", cwavs[i].LeftSamples, cwavs[i].LoopStart, cwavs[i].LoopEnd, cwavs[i].SampleRate, cwavs[i].Key, 0);
			rightSamples[cwavs[i].Id] = sf2.NewSample(to_string(cwavs[i].Id) + "r", cwavs[i].RightSamples, cwavs[i].LoopStart, cwavs[i].LoopEnd, cwavs[i].SampleRate, cwavs[i].Key, 0);

			leftSamples[cwavs[i].Id]->set_link(rightSamples[cwavs[i].Id]);
			rightSamples[cwavs[i].Id]->set_link(leftSamples[cwavs[i].Id]);

			leftSamples[cwavs[i].Id]->set_type(SFSampleLink::kLeftSample);
			rightSamples[cwavs[i].Id]->set_type(SFSampleLink::kRightSample);
		}
	}

	vector<shared_ptr<SFInstrument>> instruments;

	for (uint32_t i = 0; i < instCount; ++i)
	{
		if (insts[i].Exists)
		{
			vector<SFInstrumentZone> instrumentZones;

			for (uint32_t j = 0; j < insts[i].NoteCount; ++j)
			{
				if ((insts[i].Notes[j].Exists) && (insts[i].Notes[j].Cwav->Id < 0xF000))
				{
					size_t k = 0;
					auto it = Cwars->begin();

					for (; it != Cwars->end(); ++it, ++k)
					{
						if (k == insts[i].Notes[j].Cwav->Cwar)
						{
							break;
						}
					}

					SFGeneratorItem keyRange(SFGenerator::kKeyRange, RangesType(insts[i].Notes[j].StartNote, insts[i].Notes[j].EndNote));
					SFGeneratorItem overridingRootKey(SFGenerator::kOverridingRootKey, insts[i].Notes[j].RootKey);
					SFGeneratorItem initialAttenuation(SFGenerator::kInitialAttenuation, static_cast<int16_t>(ConvertVolume(insts[i].Notes[j].Volume)));
					SFGeneratorItem pan(SFGenerator::kPan, static_cast<int16_t>(ConvertPan(insts[i].Notes[j].Pan)));
					SFGeneratorItem attackVolEnv(SFGenerator::kAttackVolEnv, static_cast<int16_t>(ConvertAttack(insts[i].Notes[j].Attack)));
					SFGeneratorItem holdVolEnv(SFGenerator::kHoldVolEnv, static_cast<int16_t>(ConvertHold(insts[i].Notes[j].Hold)));
					SFGeneratorItem decayVolEnv(SFGenerator::kDecayVolEnv, static_cast<int16_t>(ConvertDecay(insts[i].Notes[j].Decay, insts[i].Notes[j].Sustain)));
					SFGeneratorItem releaseVolEnv(SFGenerator::kReleaseVolEnv, static_cast<int16_t>(ConvertRelease(insts[i].Notes[j].Release, insts[i].Notes[j].Sustain, Opts.PadSustainSeconds)));
					SFGeneratorItem sustainVolEnv(SFGenerator::kSustainVolEnv, static_cast<int16_t>(ConvertSustain(insts[i].Notes[j].Sustain)));

					// A release-127 voice stops instantly on hardware; its audible tail is
					// DSP reverb, which no soundfont can carry. Say so either way: by
					// default the tail is simply absent (the sequence's CC91 send needs a
					// reverb-capable player), and under --pad-sustain it is faked.
					if (insts[i].Notes[j].Release == 127)
					{
						Ctx.Warning(insts[i].Notes[j].Offset, "instrument " + to_string(i) + " note " + to_string(j) + " has release 127",
							Opts.PadSustainSeconds > 0
								? "instrument tails faked with a held release (--pad-sustain; not hardware behaviour)"
								: "instrument tails that are console DSP reverb, not release (play the MIDI's CC91 send through a reverb-capable player, or approximate it with --pad-sustain)");
					}
					SFGeneratorItem sampleModes(SFGenerator::kSampleModes, it->second->Cwavs[insts[i].Notes[j].Cwav->Id]->SampleMode);

					// Emit the per-note tune (Note 0x24) as SF2 coarse (semitone) + fine
					// (cent) tune, split so the fine part stays within +/-50 cents and the
					// rest is whole semitones. A note at exactly 1.0 (>99% of notes) yields
					// 0/0 and adds no generator, so its output stays byte-identical.
					vector<SFGeneratorItem> tuneGens;

					{
						double tuneCents = ConvertTune(insts[i].Notes[j].Tune);

						// A corrupt bank could store tune <= 0 or NaN, which makes the log
						// non-finite and lround() undefined; treat that as no detune.
						if (isfinite(tuneCents))
						{
							long coarse = lround(tuneCents / 100.0);
							long fine = lround(tuneCents - coarse * 100.0);

							if (coarse != 0)
							{
								tuneGens.push_back(SFGeneratorItem(SFGenerator::kCoarseTune, static_cast<int16_t>(coarse)));
							}

							if (fine != 0)
							{
								tuneGens.push_back(SFGeneratorItem(SFGenerator::kFineTune, static_cast<int16_t>(fine)));
							}
						}
					}

					// sf2cute sorts generators into their spec order on write, so appending
					// the (possibly empty) tune generators here does not affect the byte
					// layout of the fixed generators above.
					auto zoneGens = [&](const SFGeneratorItem& panGen)
					{
						vector<SFGeneratorItem> gens { keyRange, overridingRootKey, initialAttenuation, panGen, attackVolEnv, holdVolEnv, decayVolEnv, releaseVolEnv, sustainVolEnv, sampleModes };
						gens.insert(gens.end(), tuneGens.begin(), tuneGens.end());

						return gens;
					};

					if (insts[i].Notes[j].Cwav->ChanCount == 1)
					{
						instrumentZones.push_back(SFInstrumentZone(leftSamples[insts[i].Notes[j].Cwav->Id], zoneGens(pan), vector<SFModulatorItem> { }));
					}
					else
					{
						if (!Opts.Pan)
						{
							SFGeneratorItem left(SFGenerator::kPan, -500);
							SFGeneratorItem right(SFGenerator::kPan, 500);

							instrumentZones.push_back(SFInstrumentZone(leftSamples[insts[i].Notes[j].Cwav->Id], zoneGens(left), vector<SFModulatorItem> { }));
							instrumentZones.push_back(SFInstrumentZone(rightSamples[insts[i].Notes[j].Cwav->Id], zoneGens(right), vector<SFModulatorItem> { }));
						}
						else
						{
							SFGeneratorItem left(SFGenerator::kPan, static_cast<int16_t>(((static_cast<double>(insts[i].Notes[j].Pan) / 128.0f) * 500) - 500));
							SFGeneratorItem right(SFGenerator::kPan, static_cast<int16_t>((static_cast<double>(insts[i].Notes[j].Pan) / 128.0f) * 500));

							instrumentZones.push_back(SFInstrumentZone(leftSamples[insts[i].Notes[j].Cwav->Id], zoneGens(left), vector<SFModulatorItem> { }));
							instrumentZones.push_back(SFInstrumentZone(rightSamples[insts[i].Notes[j].Cwav->Id], zoneGens(right), vector<SFModulatorItem> { }));
						}
					}
				}
			}

			if (!instrumentZones.empty())
			{
				instruments.push_back(sf2.NewInstrument(to_string(i), instrumentZones));
			}
			else
			{
				instruments.push_back(nullptr);
			}
		}
		else
		{
			instruments.push_back(nullptr);
		}
	}

	for (uint32_t i = 0; i < instCount; ++i)
	{
		if (insts[i].Exists && (instruments[i] != nullptr))
		{
			// Instruments are indexed 0..N-1, but a MIDI program change only
			// addresses 0-127, so an instrument at index >= 128 is unreachable
			// unless it is placed in a higher SF2 bank. Split the index the same
			// way the sequence's bank-select does: bank = i / 128, preset = i % 128
			// (drum kits keep the GM drum bank 128 as their base). A sequence
			// selecting instrument i then emits bank i/128 + program i%128 and lands
			// on the matching preset. Banks with < 128 instruments are unaffected
			// (i/128 == 0, i%128 == i), so their SF2 output is unchanged.
			sf2.NewPreset(instruments[i]->name(), i % 128, (!insts[i].IsDrumKit ? 0 : 128) + i / 128, vector<SFPresetZone> { SFPresetZone(instruments[i]) });
		}
	}

	ofstream ofs(FileName.substr(0, FileName.length() - 5).append("sf2"), ios::binary);
	sf2.Write(ofs);
	ofs.close();

	return true;
}
