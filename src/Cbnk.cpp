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
#include <utility>
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

// Walk the CBNK/INFO headers, the CWAV reference table, and every instrument and
// note body into the retained model (Cwavs/Insts). No SF2 output happens here;
// every Assert/Error/Analyse/Warning of the parse phase fires exactly as the old
// single-pass Convert() did, in the same order, because Export is called straight
// after this per file. The instrument/note bodies are located through the offset
// tables (BodyOffset is the file offset relative to Data), and the raw table words
// (TableMagic/TableOffset), the instrument type, the note id, and the raw CWAV
// index are retained so the model is a lossless representation of the file.
bool Cbnk::Parse()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43424E4B, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x20, Ctx.ReadFixLen(pos, 2))) { return false; }

	Version = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x1, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x5800, Ctx.ReadFixLen(pos, 4))) { return false; }

	InfoOffset = Ctx.ReadFixLen(pos, 4);
	InfoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, InfoLength, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x100, Ctx.ReadFixLen(pos, 4))) { return false; }

	CwavOffset = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x101, Ctx.ReadFixLen(pos, 4))) { return false; }

	InstOffset = Ctx.ReadFixLen(pos, 4);

	pos = Data + InfoOffset + 8 + CwavOffset;

	uint32_t cwavCount = Ctx.ReadFixLen(pos, 4);

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		pos = Data + InfoOffset + 8 + CwavOffset + 4 + (i * 8);

		// The parse -> model seam: this walk records only the raw sample reference
		// (war id, sample id). Resolving the live Cwav -- decoded PCM, sample rate,
		// loop points, existence -- is the SF2 exporter's job below, not the
		// parser's. Value-initialize: Key is only assigned when a note references
		// this sample, but every sample with Id < 0xF000 is still emitted (with Key
		// as its shdr byOriginalKey). Without this, an unreferenced sample would
		// emit an uninitialized byte, making SF2 output non-deterministic. Cwar is
		// the war index resolved as (raw word - 0x5000000); Serialize re-adds it.
		CbnkCwav cwav{};
		cwav.Cwar = Ctx.ReadFixLen(pos, 4) - 0x5000000;
		cwav.Id = Ctx.ReadFixLen(pos, 4);

		Cwavs.push_back(cwav);
	}

	pos = Data + InfoOffset + 8 + InstOffset;

	uint32_t instCount = Ctx.ReadFixLen(pos, 4);

	for (uint32_t i = 0; i < instCount; ++i)
	{
		CbnkInst inst{};

		// Retain the raw table word (0x5900 marks a present instrument; a null
		// entry carries a different word) so Serialize rebuilds the table verbatim,
		// including the non-existent entries whose bodies are never parsed.
		inst.TableMagic = Ctx.ReadFixLen(pos, 4);

		if (inst.TableMagic != 0x5900)
		{
			inst.Exists = false;
		}

		inst.TableOffset = Ctx.ReadFixLen(pos, 4);
		inst.BodyOffset = InfoOffset + 24 + inst.TableOffset;

		Insts.push_back(inst);
	}

	for (uint32_t i = 0; i < instCount; ++i)
	{
		if (!Insts[i].Exists)
		{
			continue;
		}

		pos = Data + Insts[i].BodyOffset;

		Insts[i].Type = Ctx.ReadFixLen(pos, 4);

		if (!Ctx.Assert(pos, 0x8, Ctx.ReadFixLen(pos, 4))) { return false; }

		switch (Insts[i].Type)
		{
			case 0x6000:
			{
				Insts[i].NoteCount = 1;

				CbnkNote note{};
				note.StartNote = 0;
				note.EndNote = 127;

				Insts[i].Notes.push_back(note);

				break;
			}

			case 0x6001:
			{
				Insts[i].NoteCount = Ctx.ReadFixLen(pos, 4);

				for (uint32_t j = 0; j < Insts[i].NoteCount; ++j)
				{
					CbnkNote note{};
					note.StartNote = j == 0 ? 0 : Insts[i].Notes[j - 1].EndNote + 1;
					note.EndNote = Ctx.ReadFixLen(pos, 1);

					Insts[i].Notes.push_back(note);
				}

				uint8_t padding = Insts[i].NoteCount % 4;

				if (padding)
				{
					if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 4 - padding))) { return false; }
				}

				break;
			}

			case 0x6002:
			{
				Insts[i].NoteCount = Ctx.ReadFixLen(pos, 2, false) + 1;

				for (uint32_t j = 0; j < Insts[i].NoteCount; ++j)
				{
					CbnkNote note{};
					note.StartNote = j;
					note.EndNote = j;

					Insts[i].Notes.push_back(note);
				}

				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 2))) { return false; }

				Insts[i].IsDrumKit = true;

				break;
			}

			default:
			{
				Ctx.Error(pos - 8, "A valid instrument type", Insts[i].Type);

				return false;
			}
		}

		for (uint32_t j = 0; j < Insts[i].NoteCount; ++j)
		{
			// As with the instrument table, retain the raw note-table word (0x5901
			// marks a present note) plus the offset, so the table round-trips.
			Insts[i].Notes[j].TableMagic = Ctx.ReadFixLen(pos, 4);

			if (Insts[i].Notes[j].TableMagic != 0x5901)
			{
				Insts[i].Notes[j].Exists = false;
			}

			Insts[i].Notes[j].TableOffset = Ctx.ReadFixLen(pos, 4);
			Insts[i].Notes[j].BodyOffset = Insts[i].BodyOffset + 8 + Insts[i].Notes[j].TableOffset;
		}

		for (uint32_t j = 0; j < Insts[i].NoteCount; ++j)
		{
			if (!Insts[i].Notes[j].Exists)
			{
				continue;
			}

			pos = Data + Insts[i].Notes[j].BodyOffset;

			// The note body's first word gates the layered-note quartet below and is
			// retained: a 0x6001-id note is not recoverable from NoteCount alone.
			Insts[i].Notes[j].Id = Ctx.ReadFixLen(pos, 4);

			if (!Ctx.Assert(pos, 0x8, Ctx.ReadFixLen(pos, 4))) { return false; }

			// Read each retained word into a local, then Analyse (feeding .log,
			// unchanged) and store it in the model. Reading into the local keeps the
			// exact read order and value the inline Analyse argument had.
			uint32_t note08 = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x08", note08);
			Insts[i].Notes[j].Word08 = note08;

			uint32_t note0C = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x0C", note0C);
			Insts[i].Notes[j].Word0C = note0C;

			if (Insts[i].Notes[j].Id == 0x6001)
			{
				uint32_t note6001_10 = Ctx.ReadFixLen(pos, 4);
				Ctx.Analyse("Note 0x6001 0x10", note6001_10);
				Insts[i].Notes[j].Word6001_10 = note6001_10;

				uint32_t note6001_14 = Ctx.ReadFixLen(pos, 4);
				Ctx.Analyse("Note 0x6001 0x14", note6001_14);
				Insts[i].Notes[j].Word6001_14 = note6001_14;

				uint32_t note6001_18 = Ctx.ReadFixLen(pos, 4);
				Ctx.Analyse("Note 0x6001 0x18", note6001_18);
				Insts[i].Notes[j].Word6001_18 = note6001_18;

				uint32_t note6001_1C = Ctx.ReadFixLen(pos, 4);
				Ctx.Analyse("Note 0x6001 0x1C", note6001_1C);
				Insts[i].Notes[j].Word6001_1C = note6001_1C;
			}

			uint32_t cwav = Ctx.ReadFixLen(pos, 4);

			// Retain the raw index: the out-of-range substitution below repoints Cwav
			// to the first sample, which would otherwise lose the original index.
			Insts[i].Notes[j].CwavIndex = cwav;

			if (cwav < Cwavs.size())
			{
				Insts[i].Notes[j].Cwav = &Cwavs[cwav];
			}
			else if (!Cwavs.empty())
			{
				Ctx.Warning(pos - 4, "CWAV " + to_string(cwav) + " does not exist", "instrument notes referencing a missing sample (substituted the first sample)");

				Insts[i].Notes[j].Cwav = &Cwavs[0];
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
			Insts[i].Notes[j].Flags = noteFlags;

			// For ordinary (non-0x6001) notes this word is the note's flags, which
			// is 0x21F across every observed bank; the field layout parsed below is
			// hardcoded for that value. Surface a note whose flags differ, since its
			// envelope/pitch/pan fields may then sit at other offsets and misparse.
			if (Insts[i].Notes[j].Id != 0x6001 && noteFlags != 0x21F)
			{
				ostringstream flagsMsg;
				flagsMsg << "note flags 0x" << hex << uppercase << noteFlags << " (expected 0x21F)";

				Ctx.Warning(pos - 4, flagsMsg.str(), "bank notes with an unrecognized flags word (envelope/pitch/pan may be misparsed)");
			}

			Insts[i].Notes[j].RootKey = Ctx.ReadFixLen(pos, 4);
			Insts[i].Notes[j].Cwav->Key = Insts[i].Notes[j].RootKey;
			Insts[i].Notes[j].Volume = Ctx.ReadFixLen(pos, 4);
			Insts[i].Notes[j].Pan = Ctx.ReadFixLen(pos, 4);

			// Note 0x24 is an f32 pitch ratio (frequency multiplier on top of the
			// root key), stored as raw little-endian bits. Reinterpret those bits
			// as a float and keep it; emitted as SF2 tune generators below.
			uint32_t tuneBits = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x24", tuneBits);

			float tune;
			memcpy(&tune, &tuneBits, sizeof(tune));
			Insts[i].Notes[j].Tune = tune;

			uint16_t note28 = static_cast<uint16_t>(Ctx.ReadFixLen(pos, 2));
			Ctx.Analyse("Note 0x28", note28);
			Insts[i].Notes[j].Word28 = note28;

			Insts[i].Notes[j].Interpolation = Ctx.ReadFixLen(pos, 1);

			if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 1))) { return false; }

			uint32_t note2C = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x2C", note2C);
			Insts[i].Notes[j].DataRef2C = note2C;

			uint32_t note30 = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x30", note30);
			Insts[i].Notes[j].DataRef30 = note30;

			uint32_t note34 = Ctx.ReadFixLen(pos, 4);
			Ctx.Analyse("Note 0x34", note34);
			Insts[i].Notes[j].DataRef34 = note34;

			Insts[i].Notes[j].Attack = Ctx.ReadFixLen(pos, 1);
			Insts[i].Notes[j].Decay = Ctx.ReadFixLen(pos, 1);
			Insts[i].Notes[j].Sustain = Ctx.ReadFixLen(pos, 1);
			Insts[i].Notes[j].Hold = Ctx.ReadFixLen(pos, 1);
			Insts[i].Notes[j].Release = Ctx.ReadFixLen(pos, 1);

			if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 3))) { return false; }
		}
	}

	return true;
}

// Build the SoundFont from the parsed model and write the .sf2 beside FileName,
// resolving each sample live from its owning Cwar at emit time. Reads model state
// and the shared Cwars only. The per-sample <id>.wav stdout echo, the release-127
// warning, and the missing-sample throw stay at their original positions in the
// output stream. Call only after a successful Parse().
bool Cbnk::Export()
{
	uint32_t cwavCount = static_cast<uint32_t>(Cwavs.size());
	uint32_t instCount = static_cast<uint32_t>(Insts.size());

	SoundFont sf2;
	sf2.set_sound_engine("EMU8000");
	sf2.set_bank_name(filesystem::path(FileName).stem().string());
	sf2.set_rom_name("ROM");
	sf2.set_software("Caesar");

	map<uint32_t, shared_ptr<SFSample>> leftSamples;
	map<uint32_t, shared_ptr<SFSample>> rightSamples;

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		if (Cwavs[i].Id >= 0xF000)
		{
			continue;
		}

		// Resolve the live Cwav here, in the exporter -- the parse walk above only
		// recorded the raw reference. The sample was decoded in memory when its
		// wave-archive was extracted; read it back from the live Cwav instead of
		// re-opening the .wav. The positional lookup locates that wave-archive
		// (advance begin() by Cwar). Guard the cases that were previously a
		// missing-file throw or a crash: past the map's end, an absent wave-archive
		// (nullptr), an out-of-range id, or a sample that never converted. On healthy
		// data none of these fire, so this matches the old RequireOpen-throws contract.
		size_t k = 0;
		auto it = Cwars->begin();

		for (; it != Cwars->end(); ++it, ++k)
		{
			if (k == Cwavs[i].Cwar)
			{
				break;
			}
		}

		if (it == Cwars->end() || it->second == nullptr || Cwavs[i].Id >= it->second->Cwavs.size() || !it->second->Cwavs[Cwavs[i].Id]->Converted)
		{
			throw runtime_error("could not open or read file (missing, empty, or unreadable): " + to_string(Cwavs[i].Id) + ".wav");
		}

		Cwav* src = it->second->Cwavs[Cwavs[i].Id];

		// Keep the per-sample stdout echo (empty range: no reads happen here now).
		// Relocated with the resolution from the parse-phase CWAV walk; the echo is
		// the only stdout inside a Cbnk conversion, so its overall order is unchanged.
		Ctx.Push(to_string(Cwavs[i].Id) + ".wav", nullptr, 0);

		uint16_t chanCount = src->ChanCount;
		uint32_t sampleRate = src->SampleRate;

		vector<int16_t> leftPcm;
		vector<int16_t> rightPcm;

		if (src->ChanCount == 1)
		{
			leftPcm = src->Channels[0];
		}
		else if (src->ChanCount == 2)
		{
			leftPcm = src->Channels[0];
			rightPcm = src->Channels[1];
		}
		else
		{
			// The old read-back pushed every 2-byte word into LeftSamples unless the
			// wave had exactly two channels, so a >2-channel wave collapsed into one
			// frame-interleaved LeftSamples stream. Reproduce that.
			for (size_t s = 0; s < src->Channels[0].size(); ++s)
			{
				for (uint16_t c = 0; c < src->ChanCount; ++c)
				{
					leftPcm.push_back(src->Channels[c][s]);
				}
			}
		}

		// A smpl chunk is written only when SampleMode is odd; it carried the raw
		// loop points, which the old code recovered even when the decoded PCM was
		// empty (the IMA-ADPCM case: a smpl chunk but zero samples).
		uint32_t loopStart;
		uint32_t loopEnd;

		if ((src->SampleMode % 2) != 0)
		{
			loopStart = src->LoopStart;
			loopEnd = src->LoopEnd;
		}
		else
		{
			loopStart = 0;
			loopEnd = static_cast<uint32_t>(leftPcm.size());
		}

		Ctx.Pop();

		if (chanCount == 1)
		{
			leftSamples[Cwavs[i].Id] = sf2.NewSample(to_string(Cwavs[i].Id), std::move(leftPcm), loopStart, loopEnd, sampleRate, Cwavs[i].Key, 0);
		}
		else
		{
			leftSamples[Cwavs[i].Id] = sf2.NewSample(to_string(Cwavs[i].Id) + "l", std::move(leftPcm), loopStart, loopEnd, sampleRate, Cwavs[i].Key, 0);
			rightSamples[Cwavs[i].Id] = sf2.NewSample(to_string(Cwavs[i].Id) + "r", std::move(rightPcm), loopStart, loopEnd, sampleRate, Cwavs[i].Key, 0);

			leftSamples[Cwavs[i].Id]->set_link(rightSamples[Cwavs[i].Id]);
			rightSamples[Cwavs[i].Id]->set_link(leftSamples[Cwavs[i].Id]);

			leftSamples[Cwavs[i].Id]->set_type(SFSampleLink::kLeftSample);
			rightSamples[Cwavs[i].Id]->set_type(SFSampleLink::kRightSample);
		}
	}

	vector<shared_ptr<SFInstrument>> instruments;

	for (uint32_t i = 0; i < instCount; ++i)
	{
		if (Insts[i].Exists)
		{
			vector<SFInstrumentZone> instrumentZones;

			for (uint32_t j = 0; j < Insts[i].NoteCount; ++j)
			{
				if ((Insts[i].Notes[j].Exists) && (Insts[i].Notes[j].Cwav->Id < 0xF000))
				{
					size_t k = 0;
					auto it = Cwars->begin();

					for (; it != Cwars->end(); ++it, ++k)
					{
						if (k == Insts[i].Notes[j].Cwav->Cwar)
						{
							break;
						}
					}

					SFGeneratorItem keyRange(SFGenerator::kKeyRange, RangesType(Insts[i].Notes[j].StartNote, Insts[i].Notes[j].EndNote));
					SFGeneratorItem overridingRootKey(SFGenerator::kOverridingRootKey, Insts[i].Notes[j].RootKey);
					SFGeneratorItem initialAttenuation(SFGenerator::kInitialAttenuation, static_cast<int16_t>(ConvertVolume(Insts[i].Notes[j].Volume)));
					SFGeneratorItem pan(SFGenerator::kPan, static_cast<int16_t>(ConvertPan(Insts[i].Notes[j].Pan)));
					SFGeneratorItem attackVolEnv(SFGenerator::kAttackVolEnv, static_cast<int16_t>(ConvertAttack(Insts[i].Notes[j].Attack)));
					SFGeneratorItem holdVolEnv(SFGenerator::kHoldVolEnv, static_cast<int16_t>(ConvertHold(Insts[i].Notes[j].Hold)));
					SFGeneratorItem decayVolEnv(SFGenerator::kDecayVolEnv, static_cast<int16_t>(ConvertDecay(Insts[i].Notes[j].Decay, Insts[i].Notes[j].Sustain)));
					SFGeneratorItem releaseVolEnv(SFGenerator::kReleaseVolEnv, static_cast<int16_t>(ConvertRelease(Insts[i].Notes[j].Release, Insts[i].Notes[j].Sustain, Opts.PadSustainSeconds)));
					SFGeneratorItem sustainVolEnv(SFGenerator::kSustainVolEnv, static_cast<int16_t>(ConvertSustain(Insts[i].Notes[j].Sustain)));

					// A release-127 voice stops instantly on hardware; its audible tail is
					// DSP reverb, which no soundfont can carry. Say so either way: by
					// default the tail is simply absent (the sequence's CC91 send needs a
					// reverb-capable player), and under --pad-sustain it is faked. The
					// position is reconstructed from the note's retained BodyOffset, which
					// is exactly the pointer the old model field held (Data + BodyOffset).
					if (Insts[i].Notes[j].Release == 127)
					{
						Ctx.Warning(Data + Insts[i].Notes[j].BodyOffset, "instrument " + to_string(i) + " note " + to_string(j) + " has release 127",
							Opts.PadSustainSeconds > 0
								? "instrument tails faked with a held release (--pad-sustain; not hardware behaviour)"
								: "instrument tails that are console DSP reverb, not release (play the MIDI's CC91 send through a reverb-capable player, or approximate it with --pad-sustain)");
					}
					SFGeneratorItem sampleModes(SFGenerator::kSampleModes, it->second->Cwavs[Insts[i].Notes[j].Cwav->Id]->SampleMode);

					// Emit the per-note tune (Note 0x24) as SF2 coarse (semitone) + fine
					// (cent) tune, split so the fine part stays within +/-50 cents and the
					// rest is whole semitones. A note at exactly 1.0 (>99% of notes) yields
					// 0/0 and adds no generator, so its output stays byte-identical.
					vector<SFGeneratorItem> tuneGens;

					{
						double tuneCents = ConvertTune(Insts[i].Notes[j].Tune);

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

					// Channel count comes from the live Cwav (resolved via the same
					// positional lookup as sampleModes above), not the raw model record.
					if (it->second->Cwavs[Insts[i].Notes[j].Cwav->Id]->ChanCount == 1)
					{
						instrumentZones.push_back(SFInstrumentZone(leftSamples[Insts[i].Notes[j].Cwav->Id], zoneGens(pan), vector<SFModulatorItem> { }));
					}
					else
					{
						if (!Opts.Pan)
						{
							SFGeneratorItem left(SFGenerator::kPan, -500);
							SFGeneratorItem right(SFGenerator::kPan, 500);

							instrumentZones.push_back(SFInstrumentZone(leftSamples[Insts[i].Notes[j].Cwav->Id], zoneGens(left), vector<SFModulatorItem> { }));
							instrumentZones.push_back(SFInstrumentZone(rightSamples[Insts[i].Notes[j].Cwav->Id], zoneGens(right), vector<SFModulatorItem> { }));
						}
						else
						{
							SFGeneratorItem left(SFGenerator::kPan, static_cast<int16_t>(((static_cast<double>(Insts[i].Notes[j].Pan) / 128.0f) * 500) - 500));
							SFGeneratorItem right(SFGenerator::kPan, static_cast<int16_t>((static_cast<double>(Insts[i].Notes[j].Pan) / 128.0f) * 500));

							instrumentZones.push_back(SFInstrumentZone(leftSamples[Insts[i].Notes[j].Cwav->Id], zoneGens(left), vector<SFModulatorItem> { }));
							instrumentZones.push_back(SFInstrumentZone(rightSamples[Insts[i].Notes[j].Cwav->Id], zoneGens(right), vector<SFModulatorItem> { }));
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
		if (Insts[i].Exists && (instruments[i] != nullptr))
		{
			// Instruments are indexed 0..N-1, but a MIDI program change only
			// addresses 0-127, so an instrument at index >= 128 is unreachable
			// unless it is placed in a higher SF2 bank. Split the index the same
			// way the sequence's bank-select does: bank = i / 128, preset = i % 128
			// (drum kits keep the GM drum bank 128 as their base). A sequence
			// selecting instrument i then emits bank i/128 + program i%128 and lands
			// on the matching preset. Banks with < 128 instruments are unaffected
			// (i/128 == 0, i%128 == i), so their SF2 output is unchanged.
			sf2.NewPreset(instruments[i]->name(), i % 128, (!Insts[i].IsDrumKit ? 0 : 128) + i / 128, vector<SFPresetZone> { SFPresetZone(instruments[i]) });
		}
	}

	ofstream ofs(FileName.substr(0, FileName.length() - 5).append("sf2"), ios::binary);
	sf2.Write(ofs);
	ofs.close();

	return true;
}
