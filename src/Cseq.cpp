#include "Cseq.hpp"
#include "Common.hpp"

#include "libsmfc/libsmfc.h"
#include "libsmfc/libsmfcx.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stack>
#include <string>
#include <vector>

using namespace std;

// Meta event type 0x06 = Marker. libsmfc has no named constant for it; it is
// used below to write the "loopStart"/"loopEnd" markers that express a
// whole-song loop, which loop-aware players (e.g. foobar2000's foo_midi) honor
// and DAWs display on the timeline.
static constexpr int SMF_META_MARKER = 0x06;

// The libsmfc writer silently returns false and drops an event when an argument
// is outside MIDI range (a control/program/master-volume value above 127, a
// pitch bend beyond +/-8192, a tempo out of the 24-bit range). The sequence
// stream can carry such values, so without checking the return the note or
// controller just vanishes with no trace. These wrappers surface a rejected
// event: they count a default-visible notice (grouped by kind) and, under -w,
// print the positional detail. Each returns the writer's own result so callers
// are unaffected. The category strings are shared so counts merge across sites.
// emitCtrl covers the channel controllers plus the value-carrying master volume,
// pitch bend and tempo events (all "parameter" writes), so its wording stays
// general rather than claiming every one is a MIDI control-change message.
static bool emitCtrl(bool ok, uint8_t* pos)
{
	if (!ok)
	{
		Common::Warning(pos, "control/parameter event out of MIDI range; dropped",
			"MIDI control/parameter events dropped (value out of range)");
	}

	return ok;
}

static bool emitProgram(bool ok, uint8_t* pos)
{
	if (!ok)
	{
		Common::Warning(pos, "program number out of MIDI range; dropped",
			"MIDI program changes dropped (value out of range)");
	}

	return ok;
}

// A plain Uint8 argument is genuine sequence data in 0-255 while the MIDI
// control is 7-bit, so 128-255 clamps to 127 (surfaced as an approximation
// notice) rather than dropping the write. A Rnd/Var-prefixed argument is not
// evaluated yet (the range minimum / variable index stands in for the value),
// so an out-of-range value there is garbage and keeps dropping through the
// emitCtrl notice instead of being baked in as a plausible-looking 127.
static int32_t clampPlainCtrl(const CseqCmd& cmd, uint8_t* pos)
{
	if ((cmd.Suffix1 == SuffixType::None) && (cmd.Args[0] > 127))
	{
		Common::Warning(pos, "control/parameter value above MIDI range; clamped to 127",
			"MIDI control/parameter values clamped to 127 (above range)");

		return 127;
	}

	return cmd.Args[0];
}

vector<int32_t> ReadArgs(uint8_t*& pos, ArgType argType)
{
	if (argType == ArgType::Uint8)
	{
		return { ReadFixLen(pos, 1) };
	}
	else if (argType == ArgType::Int8)
	{
		return { ReadFixLen(pos, 1, false, true) };
	}
	else if (argType == ArgType::Uint16)
	{
		return { ReadFixLen(pos, 2, false) };
	}
	else if (argType == ArgType::Int16)
	{
		return { ReadFixLen(pos, 2, false, true) };
	}
	else if (argType == ArgType::Rnd)
	{
		return { ReadFixLen(pos, 2, false, true), ReadFixLen(pos, 2, false, true) };
	}
	else if (argType == ArgType::Var)
	{
		return { ReadFixLen(pos, 1) };
	}
	else if (argType == ArgType::VarLen)
	{
		return { ReadVarLen(pos) };
	}

	return { };
}

Cseq::Cseq(const char* fileName) : FileName(fileName)
{
	ifstream ifs(FileName, ios::binary | ios::ate);

	Length = ifs.tellg();
	Common::RequireOpen(ifs.good(), Length, FileName);
	Data = new uint8_t[Length];

	Common::Push(filesystem::path(FileName).filename().string(), Data, Length);

	ifs.seekg(0, ios::beg);
	ifs.read(reinterpret_cast<char*>(Data), Length);
	ifs.close();
}

Cseq::~Cseq()
{
	Common::Pop();

	delete[] Data;
}

// The set of command offsets from which the walk reaches a note (any Cmd < 0x80),
// following calls and unconditional jumps. `followConditional` controls whether
// conditional [If] jumps are treated as branchable:
//   false -> skip them, exactly what Convert emits today; answers "would the
//            current converter produce a note starting here" (used to decide
//            whether the plain fall-through of a dispatcher stays silent).
//   true  -> also follow their targets; answers "can this offset reach a note by
//            some branch" (used to decide whether a dispatcher branch is worth
//            taking, even when the note hides behind a further [If] jump).
// Two notions are needed: a single any-path set would report the fall-through as
// note-reaching via a branch we never take, so no dispatcher would ever fire.
// Computed by seeding every note command and walking predecessor edges, so
// cycles (loops) resolve correctly.
static set<uint32_t> ReachableNotes(const map<uint32_t, CseqCmd>& commands, bool followConditional)
{
	map<uint32_t, vector<uint32_t>> preds;
	vector<uint32_t> work;
	set<uint32_t> reaches;

	for (auto it = commands.begin(); it != commands.end(); ++it)
	{
		uint32_t offset = it->first;
		const CseqCmd& cmd = it->second;
		auto nextIt = next(it, 1);
		uint32_t nextOffset = (nextIt != commands.end()) ? nextIt->first : 0xFFFFFFFF;

		// Record that reaching `succ` also reaches `offset` (a reverse edge).
		auto edge = [&](uint32_t succ)
		{
			if (commands.count(succ))
			{
				preds[succ].push_back(offset);
			}
		};

		if (!cmd.Extended && (cmd.Cmd < 0x80))
		{
			// A note: a source of the reachability we are propagating backwards.
			reaches.insert(offset);
			work.push_back(offset);
		}
		else if (!cmd.Extended && ((cmd.Cmd == 0xFF) || (cmd.Cmd == 0xFD)))
		{
			// Fin / Return: this path ends here, so there are no successors.
		}
		else if (!cmd.Extended && (cmd.Cmd == 0x89) && (cmd.Suffix3 != SuffixType::If))
		{
			edge(cmd.Args[0]);          // unconditional jump: control follows it
		}
		else if (!cmd.Extended && (cmd.Cmd == 0x89) && (cmd.Suffix3 == SuffixType::If))
		{
			edge(nextOffset);           // conditional jump: the not-taken path, and
			if (followConditional)
			{
				edge(cmd.Args[0]);      // optionally the taken branch as well
			}
		}
		else if (!cmd.Extended && (cmd.Cmd == 0x8A))
		{
			edge(cmd.Args[0]);          // call into the subroutine, or
			edge(nextOffset);           // continue after it returns
		}
		else
		{
			edge(nextOffset);           // everything else
		}
	}

	while (!work.empty())
	{
		uint32_t succ = work.back();
		work.pop_back();

		auto p = preds.find(succ);

		if (p == preds.end())
		{
			continue;
		}

		for (uint32_t pred : p->second)
		{
			if (reaches.insert(pred).second)
			{
				work.push_back(pred);
			}
		}
	}

	return reaches;
}

// The set of track indices this entry uses, marked into trackUsed[16]. These
// archives pack many independent sequences into one shared DATA blob, so the
// tracks are gathered by following only the control flow reachable from this
// entry's start offset -- calls, jumps, conditional branches, and the tracks
// opened by 0x88 -- until each path ends at Fin/Return. A bank-wide scan would
// fold in OpenTracks belonging to unrelated entries: for a one-track sound
// effect sharing a bank with 16-track songs that wrongly reports 16 tracks in
// use, which would relocate track 9 or emit a rhythm-off SysEx and materialise
// empty SMF tracks the old converter never wrote. Track 0 (the entry's own
// start) is always in use. Over-approximates only along dead conditional
// branches, which is harmless for channel assignment.
static void collectEntryTracks(const map<uint32_t, CseqCmd>& commands, uint32_t startOffset, bool trackUsed[16])
{
	trackUsed[0] = true;

	set<uint32_t> visited;
	vector<uint32_t> stack;
	stack.push_back(startOffset);

	while (!stack.empty())
	{
		auto it = commands.find(stack.back());
		stack.pop_back();

		while ((it != commands.end()) && !visited.count(it->first))
		{
			visited.insert(it->first);

			const CseqCmd& cmd = it->second;
			auto nextIt = next(it, 1);

			if (!cmd.Extended && (cmd.Cmd == 0x88))
			{
				if (!cmd.Args.empty() && (cmd.Args[0] >= 0) && (cmd.Args[0] < 16))
				{
					trackUsed[cmd.Args[0]] = true;     // OpenTrack: this index plays
				}
				if (cmd.Args.size() >= 2)
				{
					stack.push_back(cmd.Args[1]);      // explore the opened track
				}
				it = nextIt;
			}
			else if (!cmd.Extended && (cmd.Cmd == 0x89) && (cmd.Suffix3 != SuffixType::If))
			{
				it = commands.find(cmd.Args[0]);       // unconditional jump
			}
			else if (!cmd.Extended && (cmd.Cmd == 0x89) && (cmd.Suffix3 == SuffixType::If))
			{
				stack.push_back(cmd.Args[0]);          // conditional: the branch and
				it = nextIt;                           // the fall-through
			}
			else if (!cmd.Extended && (cmd.Cmd == 0x8A))
			{
				stack.push_back(cmd.Args[0]);          // call the subroutine, and
				it = nextIt;                           // continue after it returns
			}
			else if (!cmd.Extended && ((cmd.Cmd == 0xFF) || (cmd.Cmd == 0xFD)))
			{
				break;                                 // Fin / Return: path ends
			}
			else
			{
				it = nextIt;
			}
		}
	}
}

bool Cseq::Convert(uint32_t startOffset)
{
	uint8_t* pos = Data;

	if (!Common::Assert(pos, 0x43534551, ReadFixLen(pos, 4, false))) { return false; }
	if (!Common::Assert(pos, 0xFEFF, ReadFixLen(pos, 2))) { return false; }
	if (!Common::Assert(pos, 0x40, ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cseqVersion = ReadFixLen(pos, 4);

	if (!Common::Assert<uint64_t>(pos, Length, ReadFixLen(pos, 4))) { return false; }
	if (!Common::Assert(pos, 0x2, ReadFixLen(pos, 4))) { return false; }
	if (!Common::Assert(pos, 0x5000, ReadFixLen(pos, 4))) { return false; }

	uint32_t dataOffset = ReadFixLen(pos, 4);
	uint32_t dataLength = ReadFixLen(pos, 4);

	if (!Common::Assert(pos, 0x5001, ReadFixLen(pos, 4))) { return false; }

	uint32_t lablOffset = ReadFixLen(pos, 4);
	uint32_t lablLength = ReadFixLen(pos, 4);

	pos = Data + lablOffset;

	if (!Common::Assert(pos, 0x4C41424C, ReadFixLen(pos, 4, false))) { return false; }
	if (!Common::Assert<uint32_t>(pos, lablLength, ReadFixLen(pos, 4))) { return false; }

	uint32_t lablCount = ReadFixLen(pos, 4);

	vector<uint8_t*> lablOffsets;

	for (uint32_t i = 0; i < lablCount; ++i)
	{
		if (!Common::Assert(pos, 0x5100, ReadFixLen(pos, 4))) { return false; }

		lablOffsets.push_back(Data + lablOffset + 8 + ReadFixLen(pos, 4));
	}

	map<uint8_t*, CseqLabl> labls;

	for (uint32_t i = 0; i < lablCount; ++i)
	{
		pos = lablOffsets[i];

		if (!Common::Assert(pos, 0x1F00, ReadFixLen(pos, 4))) { return false; }

		CseqLabl labl;
		labl.Offset = Data + dataOffset + 8 + ReadFixLen(pos, 4);
		uint32_t lablLength = ReadFixLen(pos, 4);
		Common::CheckBounds(pos, lablLength);
		labl.Label = string(reinterpret_cast<const char*>(pos), lablLength);

		labls[labl.Offset] = labl;
	}

	pos = Data + dataOffset;

	if (!Common::Assert(pos, 0x44415441, ReadFixLen(pos, 4, false))) { return false; }
	if (!Common::Assert<uint32_t>(pos, dataLength, ReadFixLen(pos, 4))) { return false; }

	map<uint32_t, CseqCmd> commands;

	while (pos < (Data + dataOffset + dataLength))
	{
		uint32_t offset = static_cast<uint32_t>(pos - 8 - dataOffset - Data);
		CseqCmd cmd;

		if (labls.count(pos))
		{
			cmd.Label = labls[pos].Label;
		}

		uint8_t statusByte = ReadFixLen(pos, 1);

		if (statusByte == 0xA2)
		{
			cmd.Suffix3 = SuffixType::If;

			statusByte = ReadFixLen(pos, 1);
		}

		if (statusByte == 0xA3)
		{
			cmd.Suffix2 = SuffixType::Time;
			cmd.Arg2 = ArgType::Int16;

			statusByte = ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA4)
		{
			cmd.Suffix2 = SuffixType::TimeRnd;
			cmd.Arg2 = ArgType::Rnd;

			statusByte = ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA5)
		{
			cmd.Suffix2 = SuffixType::TimeVar;
			cmd.Arg2 = ArgType::Var;

			statusByte = ReadFixLen(pos, 1);
		}

		if (statusByte == 0xA0)
		{
			cmd.Suffix1 = SuffixType::Rnd;
			cmd.Arg1 = ArgType::Rnd;

			statusByte = ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA1)
		{
			cmd.Suffix1 = SuffixType::Var;
			cmd.Arg1 = ArgType::Var;

			statusByte = ReadFixLen(pos, 1);
		}

		cmd.Cmd = statusByte;

		if (statusByte < 0x80)
		{
			cmd.Args.push_back(ReadFixLen(pos, 1));

			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::VarLen;
			}

			vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if ((statusByte == 0x80) || (statusByte == 0x81))
		{
			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::VarLen;
			}

			vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if (statusByte == 0x88)
		{
			cmd.Args.push_back(ReadFixLen(pos, 1));
			cmd.Args.push_back(ReadFixLen(pos, 3, false));
		}
		else if ((statusByte == 0x89) || (statusByte == 0x8A))
		{
			cmd.Args.push_back(ReadFixLen(pos, 3, false));
		}
		else if (statusByte == 0x90)
		{
			Common::Analyse("Cseq Cmd 0x90", ReadFixLen(pos, 2, false));
		}
		else if (statusByte == 0x96)
		{
			Common::Analyse("Cseq Cmd 0x96", ReadFixLen(pos, 2, false));
		}
		else if ((statusByte >= 0xB0) && (statusByte <= 0xDF))
		{
			// if ((statusByte != 0xB7) && (statusByte != 0xB8) && (statusByte != 0xB9) && (statusByte != 0xBA) && (statusByte != 0xBB) && (statusByte != 0xBC) && (statusByte != 0xDE))
			if ((statusByte == 0xB1) || (statusByte == 0xC3) || (statusByte == 0xC4) || (statusByte == 0xD0) || (statusByte == 0xD1) || (statusByte == 0xD2) || (statusByte == 0xD3))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int8;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else if ((statusByte == 0xB2) || (statusByte == 0xBF) || (statusByte == 0xC7) || (statusByte == 0xC8) || (statusByte == 0xC9) || (statusByte == 0xCE) || (statusByte == 0xDF))
			{
				cmd.Args.push_back(ReadFixLen(pos, 1));
			}
			else if (statusByte == 0xCC)
			{
				cmd.Args.push_back(ReadFixLen(pos, 1));

				if (cmd.Args.back() > 2)
				{
					Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

					return false;
				}
			}
			else if (statusByte == 0xD6)
			{
				vector<int32_t> args = ReadArgs(pos, ArgType::Var);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint8;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}

			if (cmd.Arg2 != ArgType::None)
			{
				vector<int32_t> args = ReadArgs(pos, cmd.Arg2);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
		}
		else if ((statusByte == 0xE0) || (statusByte == 0xE1) || (statusByte == 0xE3) || (statusByte == 0xE4))
		{
			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::Int16;
			}

			vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if (statusByte == 0xF0)
		{
			cmd.Extended = true;

			statusByte = ReadFixLen(pos, 1);

			if (((statusByte >= 0x80) && (statusByte <= 0x8B)) || ((statusByte >= 0x90) && (statusByte <= 0x95)))
			{
				vector<int32_t> args1 = ReadArgs(pos, ArgType::Var);

				cmd.Args.insert(cmd.Args.end(), args1.begin(), args1.end());

				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int16;
				}

				vector<int32_t> args2 = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args2.begin(), args2.end());
			}
			else if (statusByte == 0xA4)
			{
				cmd.Args.push_back(ReadFixLen(pos, 1));

				if (cmd.Args.back() > 2)
				{
					Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

					return false;
				}
			}
			else if (statusByte == 0xAA)
			{
				cmd.Args.push_back(ReadFixLen(pos, 1));

				if (cmd.Args.back() > 2)
				{
					Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

					return false;
				}
			}
			else if (statusByte == 0xB0)
			{
				cmd.Args.push_back(ReadFixLen(pos, 1));

				if (cmd.Args.back() > 2)
				{
					Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

					return false;
				}
			}
			else if ((statusByte >= 0xA0) && (statusByte <= 0xB1))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint8;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else if (statusByte == 0xE0)
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint16;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else if ((statusByte >= 0xE1) && (statusByte <= 0xE6))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int16;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else
			{
				Common::Error(pos - 1, "A valid extended command", statusByte);

				return false;
			}
		}
		else if ((statusByte == 0xFB) || (statusByte == 0xFC) || (statusByte == 0xFD) || (statusByte == 0xFF))
		{
			// These commands take no arguments
		}
		else if (statusByte == 0xFE)
		{
			cmd.Args.push_back(ReadFixLen(pos, 2, false));
		}
		else
		{
			Common::Error(pos - 1, "A valid command", statusByte);

			return false;
		}

		commands[offset] = cmd;
	}

	// Offsets from which a note is reachable (see ReachableNotes). `noteOnFall`
	// follows only the plain (skip-[If]) flow, so it reports whether a
	// dispatcher's fall-through stays silent; `noteViaBranch` also follows [If]
	// branches, so it reports whether a branch target eventually reaches a note
	// even through further conditional jumps. Together they let the walk resolve
	// note-less [If] dispatchers below.
	set<uint32_t> noteOnFall = ReachableNotes(commands, false);
	set<uint32_t> noteViaBranch = ReachableNotes(commands, true);

	Smf* smf = smfCreate();
	uint32_t absTime = 0;
	uint8_t track = 0;
	bool noteWait = false;
	// Whether the current track has emitted a note yet. A note-less [If]
	// dispatcher is only followed while this is false, so any track that already
	// produces sound converts exactly as before.
	bool trackHasNote = false;
	// Set by any handler that redirects the walk (jump, call, return, track
	// change) so the loop lands on the freshly-found command instead of stepping
	// past it. This replaces the old "find(target); --i;" idiom, which was
	// undefined behaviour whenever the target was the first command (offset 0) --
	// exactly the case for the note blocks these dispatchers jump into.
	bool redirected = false;
	uint32_t trackOffsets[16] = { 0 };
	stack<uint32_t> sp;
	[[maybe_unused]] bool trackEnabled[16] = { false };

	// Absolute MIDI time at which each command offset was reached in the current
	// track, so a backward jump can place its loop-start marker at the tick where
	// the loop target originally played. Cleared at every track boundary.
	map<uint32_t, uint32_t> offsetTime;

	// Begin at this entry's start offset within the shared bank. If it does not
	// land on a command boundary (a malformed offset, or an archive that stores
	// it differently), fall back to the top rather than skip the sequence.
	auto i = commands.find(startOffset);

	if (i == commands.end())
	{
		if (startOffset != 0)
		{
			Common::Warning(Data + dataOffset + 8, "sequence start offset is not a command boundary; starting from the top");
		}

		i = commands.begin();
	}

	// MIDI channel assignment. The CSEQ track index has been used directly as the
	// MIDI channel on every emitted event, so a sequence's 10th track (index 9)
	// lands on channel 9 -- the channel GM/GS players reserve for percussion --
	// and its melodic notes render as a drum kit, or as silence since caesar SF2s
	// carry no bank-128 drum preset. Decouple the channel from the SMF track
	// number (which stays equal to the track index, so the file's track layout is
	// unchanged and any sequence that never uses track 9 converts byte-identically):
	// keep every channel equal to its index, but relocate track 9 to a free
	// channel whenever this entry leaves one. Only when the entry uses all 16
	// tracks -- no channel is free -- does track 9 stay on channel 9, and then a
	// GS "rhythm part off" SysEx (emitted lazily below, at track 9's first note)
	// keeps it melodic on GS-aware players (FluidSynth). trackUsed is scoped to
	// this entry (from i->first, the resolved start), not the shared bank.
	static constexpr int DRUM_CHANNEL = 9;

	bool trackUsed[16] = { false };
	collectEntryTracks(commands, i->first, trackUsed);

	int channelOf[16];

	for (int t = 0; t < 16; ++t)
	{
		channelOf[t] = t;
	}

	bool rhythmOffPending = false;

	if (trackUsed[DRUM_CHANNEL])
	{
		int freeChannel = -1;

		for (int c = 0; c < 16; ++c)
		{
			if (!trackUsed[c])
			{
				freeChannel = c;
				break;
			}
		}

		if (freeChannel >= 0)
		{
			channelOf[DRUM_CHANNEL] = freeChannel;
		}
		else
		{
			rhythmOffPending = true;
		}
	}

	// Roland GS DT1 (F0 41 10 42 12 <addr> <data> <sum> F7): address 40 10 15 is
	// part 10's "Use for Rhythm Part", data 00 = OFF (a normal melodic part);
	// checksum 1B = (128 - ((0x40+0x10+0x15+0x00) & 0x7F)) & 0x7F. Emitted lazily
	// (see the note handler), just before track 9's first note, so it never
	// materialises an SMF track the note itself would not have created.
	static const uint8_t gsRhythmPartOff[] =
		{ 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, 0x1B, 0xF7 };

	// End the current track and move to the next allocated one (registered by an
	// earlier 0x88 in trackOffsets). Shared by three "this track is done here"
	// cases: Fin (0xFF), a whole-song loop's loop-back, and a stray Return.
	// Returns false when no further track remains, i.e. the whole sequence has
	// finished and the caller should stop the walk.
	auto advanceToNextTrack = [&]() -> bool
	{
		smfSetEndTimingOfTrack(smf, track, absTime);

		for (uint8_t j = track + 1; j < 16; ++j)
		{
			if (trackOffsets[j] != 0)
			{
				absTime = 0;
				track = j;
				noteWait = false;
				trackHasNote = false;
				offsetTime.clear();

				i = commands.find(trackOffsets[j]);
				redirected = true;

				return true;
			}
		}

		return false;
	};

	while (i != commands.end())
	{
		offsetTime[i->first] = absTime;

		// Position of the command now being emitted, for any dropped-event notice.
		uint8_t* here = Data + dataOffset + 8 + i->first;

		// MIDI channel for this track's channel-voice messages (see channelOf).
		// The SMF track number stays == track, so only the channel nibble moves.
		int chan = channelOf[track];

		if (!i->second.Label.empty())
		{
			smfInsertMetaText(smf, absTime, track, SMF_META_TEXT, i->second.Label.c_str());
		}

		if (!i->second.Extended)
		{
			if (i->second.Cmd < 0x80)
			{
				// This entry saturates all 16 tracks, so track 9 could not be
				// relocated off the GM drum channel; emit the GS rhythm-part-off
				// SysEx immediately before track 9's first note. Doing it lazily
				// (rather than up front) means a track 9 that is opened but plays
				// no note never gains the SysEx or a stray SMF track, so such a
				// sequence stays byte-identical. The SysEx is inserted first at
				// this tick, so libsmfc's stable equal-tick ordering keeps it
				// ahead of the note.
				if (rhythmOffPending && (track == DRUM_CHANNEL))
				{
					smfInsertSysex(smf, absTime, 0, track, gsRhythmPartOff, sizeof(gsRhythmPartOff));
					rhythmOffPending = false;
				}

				// key is Cmd (< 0x80, always valid); a note is only rejected on its
				// velocity. Velocity 0 is a legitimate silent note (the writer skips
				// it and timing below still advances), so surface a drop only when
				// the velocity is genuinely out of MIDI range, not merely zero.
				if (!smfInsertNote(smf, absTime, chan, track, i->second.Cmd, i->second.Args[0], i->second.Args[1])
					&& i->second.Args[0] > 127)
				{
					Common::Warning(here, "note velocity out of MIDI range; note dropped",
						"MIDI notes dropped (velocity out of range)");
				}

				trackHasNote = true;

				if (noteWait)
				{
					absTime += i->second.Args[1];
				}
			}
			else if (i->second.Cmd == 0x80)
			{
				absTime += i->second.Args[0];
			}
			else if (i->second.Cmd == 0x81)
			{
				// Args[0] is a flat instrument index that can exceed 127. A MIDI
				// program change only addresses 0-127, so a banked voice must be
				// reached with a bank select + a masked program: bank = Args[0] / 128,
				// program = Args[0] % 128 -- the same split Cbnk now uses to place the
				// instrument (bank i/128, preset i%128), so the two line up.
				//
				// The bank goes in the MSB control (CC0). Most SoundFont players
				// (FluidSynth's default GS mode, GM) take the SF2 bank from CC0 and
				// ignore the LSB, so the previous code -- which put the bank in the LSB
				// (CC32) and left CC0 at 0 -- selected bank 0 and played the wrong
				// instrument on those players; only XG/MMA-mode players read the LSB.
				// CC0 = bank, CC32 = 0 makes the common players correct. For a normal
				// (unbanked) voice, bank == 0, so this emits CC0=0 CC32=0 exactly as
				// before -- unbanked program changes stay byte-identical.
				//
				// A Rnd/Var prefix can make Args[0] negative; a negative value keeps
				// its sign through /128 and %128, so the emit* wrappers still surface
				// anything the writer rejects.
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BANKSELM, (i->second.Args[0] / 128) % 128), here);
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BANKSELL, 0), here);
				emitProgram(smfInsertProgram(smf, absTime, chan, track, i->second.Args[0] % 128), here);
			}
			else if (i->second.Cmd == 0x88)
			{
				// OpenTrack: register a sibling track's start offset. The track
				// index is a full byte, but the format only has 16 tracks (the
				// enable mask at 0xFE is 16-bit, and every track array is [16]).
				// No real archive uses an index >= 16 (verified across the corpus),
				// so guard the array write rather than let a malformed one corrupt
				// the stack, and surface it like other skipped content.
				if (i->second.Args[0] < 16)
				{
					trackOffsets[i->second.Args[0]] = i->second.Args[1];
				}
				else
				{
					Common::Warning(here, "OpenTrack index out of range (>= 16); track ignored",
						"OpenTrack index out of range");
				}
			}
			else if (i->second.Cmd == 0x89)
			{
				uint32_t target = i->second.Args[0];

				if (i->second.Suffix3 == SuffixType::If)
				{
					// Conditional (dispatcher) jump. The runtime variable it tests
					// is not modelled, so by default we take the branch-not-taken
					// path and fall through -- correct for spin-waits and for any
					// track that already carries its own notes. But many sequences
					// (notably Animal Crossing's GardenSound) build a whole track as
					// a note-less dispatcher: the body is nothing but conditional
					// jumps and every note lives behind one, so falling through
					// yields a silent MIDI. When this track has emitted no notes and
					// its plain continuation stays silent, follow the first branch
					// that actually reaches a note -- the default (variable == 0)
					// section the sound engine would pick -- rather than drop the
					// track. The forward-only test (target not yet played) keeps
					// backward spin-wait loops on the fall-through path.
					auto nextIt = next(i, 1);
					uint32_t fallThrough = (nextIt != commands.end()) ? nextIt->first : 0xFFFFFFFF;
					bool fallSilent = (fallThrough == 0xFFFFFFFF) || !noteOnFall.count(fallThrough);

					if (!trackHasNote && !offsetTime.count(target) && fallSilent && noteViaBranch.count(target))
					{
						i = commands.find(target);
						redirected = true;
					}
					else
					{
						Common::Warning(Data + dataOffset + 8 + i->first, "conditional jump skipped (conditions not implemented)");
					}
				}
				else if (offsetTime.count(target))
				{
					// The target has already played in this track, so following the
					// jump would replay it forever: this is the format's whole-song
					// loop. A MIDI file cannot loop on its own, so mark the loop span
					// (target..here) with "loopStart"/"loopEnd" and end the track
					// instead of following the jump.
					smfInsertMetaText(smf, offsetTime[target], track, SMF_META_MARKER, "loopStart");
					smfInsertMetaText(smf, absTime, track, SMF_META_MARKER, "loopEnd");

					if (!advanceToNextTrack())
					{
						break;
					}
				}
				else
				{
					// The target has not played yet in this track: this is a goto to
					// new code (a forward skip, or a jump into a shared block that
					// several tracks reuse, which will end on its own Fin). Follow it,
					// like a call with no return address. Any real loop this leads
					// into is caught above once it revisits played code.
					auto n = commands.find(target);

					if (n != commands.end())
					{
						i = n;
						redirected = true;
					}
					else
					{
						Common::Warning(Data + dataOffset + 8 + i->first, "jump target out of range");
					}
				}
			}
			else if (i->second.Cmd == 0x8A)
			{
				sp.push(next(i, 1)->first);

				i = commands.find(i->second.Args[0]);
				redirected = true;
			}
			else if (i->second.Cmd == 0xB0)
			{
				smfSetTimebase(smf, i->second.Args[0]);
			}
			else if (i->second.Cmd == 0xB1)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "envelope hold not implemented");
			}
			else if (i->second.Cmd == 0xB2)
			{
				smfInsertControl(smf, absTime, chan, track, i->second.Args[0] ? SMF_CONTROL_MONO : SMF_CONTROL_POLY, 0);
			}
			else if (i->second.Cmd == 0xB3)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "velocity range not implemented");
			}
			else if (i->second.Cmd == 0xB4)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "biquad type not implemented");
			}
			else if (i->second.Cmd == 0xB5)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "biquad value not implemented");
			}
			else if (i->second.Cmd == 0xB6)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "bank select not implemented");
			}
			else if (i->second.Cmd == 0xBD)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod phase not implemented");
			}
			else if (i->second.Cmd == 0xBE)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod curve not implemented");
			}
			else if (i->second.Cmd == 0xBF)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "front bypass not implemented");
			}
			else if (i->second.Cmd == 0xC0)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PANPOT, clampPlainCtrl(i->second, here)), here);
			}
			else if (i->second.Cmd == 0xC1)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VOLUME, clampPlainCtrl(i->second, here)), here);
			}
			else if (i->second.Cmd == 0xC2)
			{
				emitCtrl(smfInsertMasterVolume(smf, absTime, 0, track, clampPlainCtrl(i->second, here)), here);
			}
			else if (i->second.Cmd == 0xC3)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNM, 0);
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNL, 2);
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DATAENTRYM, i->second.Args[0] + 64), here);
			}
			else if (i->second.Cmd == 0xC4)
			{
				emitCtrl(smfInsertPitchBend(smf, absTime, chan, track, i->second.Args[0] * 64), here);
			}
			else if (i->second.Cmd == 0xC5)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNM, 0);
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNL, 0);
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DATAENTRYM, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xC6)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "priority not implemented");
			}
			else if (i->second.Cmd == 0xC7)
			{
				noteWait = i->second.Args[0];
			}
			else if (i->second.Cmd == 0xC8)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "tie not implemented");
			}
			else if (i->second.Cmd == 0xC9)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTOCTRL, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xCA)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_MODULATION, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xCB)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATORATE, (i->second.Args[0] / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xCC)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod type not implemented");
			}
			else if (i->second.Cmd == 0xCD)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODEPTH, (i->second.Args[0] / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xCE)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTO, i->second.Args[0] ? 127 : 0);
			}
			else if (i->second.Cmd == 0xCF)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTOTIME, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xD0)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_ATTACKTIME, (i->second.Args[0] / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD1)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DECAYTIME, (i->second.Args[0] / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD2)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "sustain not implemented");
			}
			else if (i->second.Cmd == 0xD3)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RELEASETIME, (i->second.Args[0] / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD4)
			{
				// 0xD4 = loop start; Args[0] is the U8 repeat count. Emit it as the
				// EMIDI CC116 (loop-start) value so a loop-aware player repeats the
				// section that many times instead of forever. The CTR and EMIDI
				// counts line up exactly -- both are total-plays with 0 meaning
				// "loop forever" (verified against GotaSequenceLib's CtrCafe
				// playback and the Apogee EMIDI v1.1 spec) -- so a literal count
				// passes straight through, 0 included. A count above the 7-bit CC
				// range clamps to 127 (still finite/"many") rather than being
				// dropped by the writer, which would lose the loop marker outright.
				// A Rnd/Var-prefixed count is not evaluated yet, so keep the old
				// 0 (= forever) stand-in instead of baking a raw range minimum /
				// variable index in as a bogus finite count.
				int32_t count = 0;

				if (i->second.Suffix1 == SuffixType::None)
				{
					count = i->second.Args[0];

					if (count > 127)
					{
						Common::Warning(here, "loop repeat count above MIDI range; clamped to 127",
							"MIDI loop repeat counts clamped to 127 (above range)");

						count = 127;
					}
				}

				smfInsertControl(smf, absTime, chan, track, 116, count);
			}
			else if (i->second.Cmd == 0xD5)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_EXPRESSION, clampPlainCtrl(i->second, here)), here);
			}
			else if (i->second.Cmd == 0xD6)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "print var not implemented");
			}
			else if (i->second.Cmd == 0xD7)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "span not implemented");
			}
			else if (i->second.Cmd == 0xD8)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "lpf cutoff not implemented");
			}
			else if (i->second.Cmd == 0xD9)
			{
				// FX send A -> reverb depth (CC91). The aux-send level is a 7-bit
				// value in these sequences (observed 0-120 across MeetSound);
				// clamp defensively so an unexpected value can't push the control
				// out of MIDI range and get silently dropped by the writer.
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_REVERB, clamp(i->second.Args[0], 0, 127));
			}
			else if (i->second.Cmd == 0xDA)
			{
				// FX send B -> chorus depth (CC93), same 0-127 convention.
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_CHORUS, clamp(i->second.Args[0], 0, 127));
			}
			else if (i->second.Cmd == 0xDB)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "main send not implemented");
			}
			else if (i->second.Cmd == 0xDC)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "init pan not implemented");
			}
			else if (i->second.Cmd == 0xDD)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mute not implemented");
			}
			else if (i->second.Cmd == 0xDF)
			{
			emitCtrl(smfInsertControl(smf, absTime, chan, track, 64, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xE0)
			{
				if (i->second.Suffix1 == SuffixType::None)
				{
					// Mod delay: time from note-on before the track LFO engages,
					// in 5 ms units (NW4R reads it as lfoParam.delay = arg * 5 ms;
					// NW4C is its port). CC78 "vibrato delay" is relative to the
					// patch default (64 = no change), and these SF2s program no
					// LFO delay, so 64 is the 0 ms baseline: scale the delay into
					// the upper half, saturating at 1000 ms (corpus p99 = 500 ms,
					// max = 1150 ms). The old (x/2)+64 treated the time as a
					// signed +/-64 parameter and pushed delays >= 640 ms out of
					// MIDI range entirely.
					int32_t ms = max(i->second.Args[0], 0) * 5;

					smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODELAY, 64 + min(ms * 63 / 1000, 63));
				}
				else
				{
					// Unevaluated Rnd/Var stand-in: keep the raw-value path so
					// out-of-range garbage still drops with the notice instead of
					// being scaled into a plausible-looking delay.
					emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODELAY, (i->second.Args[0] / 2) + 64), here);
				}
			}
			else if (i->second.Cmd == 0xE1)
			{
				// The tempo argument decodes as signed 16-bit, so garbage (or an
				// unevaluated Rnd/Var stand-in) can be <= 0. bpm == 0 makes
				// libsmfcx's 60000000 / bpm infinite and the int cast of that is
				// UB; guard caller-side to keep the vendored copy pristine. The
				// drop still surfaces through emitCtrl's notice.
				emitCtrl((i->second.Args[0] > 0) && smfInsertTempoBPM(smf, absTime, track, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xE3)
			{
				// Sweep pitch: a signed intra-note pitch ramp (1/64-semitone
				// units) that glides from the offset to the note's nominal
				// pitch, independent of (and additive with) portamento. No
				// static MIDI control expresses it — the faithful form is a
				// pitch-bend ramp, which is player/stage-2 territory. It was
				// previously mis-emitted as CC78 "vibrato delay", where sweeps
				// of two semitones or more also fell out of MIDI range.
				Common::Warning(here, "sweep pitch (intra-note pitch ramp) has no MIDI equivalent; dropped",
					"sweep pitch dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xE4)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod_period not implemented");
			}
			else if (i->second.Cmd == 0xFB)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "envelope reset not implemented");
			}
			else if (i->second.Cmd == 0xFC)
			{
				// 0xFC = loop end -> EMIDI CC117 (loop-end marker). Its value is
				// fixed at 127 by the EMIDI v1.1 spec (it carries no count; a
				// reader jumps back to the matching CC116, whose value holds the
				// repeat count), so emit 127 rather than the old 0 to complete a
				// valid loop pair.
				smfInsertControl(smf, absTime, chan, track, 117, 127);
			}
			else if (i->second.Cmd == 0xFD)
			{
				if (!sp.empty())
				{
					i = commands.find(sp.top());
					redirected = true;

					sp.pop();
				}
				else
				{
					// A Return with no matching Call. These archives pack many
					// sequence entries into one shared bank and the converter
					// currently starts every entry at the bank's first byte,
					// which is often a helper subroutine that ends in Return
					// (see the ROADMAP note on shared-bank start offsets). Rather
					// than discard the whole in-progress MIDI, treat the stray
					// Return as end-of-track, exactly like Fin: close this track
					// and move to the next so the sequence still yields a file.
					Common::Warning(Data + dataOffset + 8 + i->first, "Return with empty call stack; ending track");

					if (!advanceToNextTrack())
					{
						break;
					}
				}
			}
			else if (i->second.Cmd == 0xFE)
			{
				for (uint8_t j = 0; j < 16; ++j)
				{
					trackEnabled[j] = (i->second.Args[0] >> j) & 0x1;
				}
			}
			else if (i->second.Cmd == 0xFF)
			{
				if (!advanceToNextTrack())
				{
					break;
				}
			}
		}
		else
		{
			if (i->second.Cmd == 0x80)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "setvar not implemented");
			}
			else if (i->second.Cmd == 0x81)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "addvar not implemented");
			}
			else if (i->second.Cmd == 0x82)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "subvar not implemented");
			}
			else if (i->second.Cmd == 0x83)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mulvar not implemented");
			}
			else if (i->second.Cmd == 0x84)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "divvar not implemented");
			}
			else if (i->second.Cmd == 0x85)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "shiftvar not implemented");
			}
			else if (i->second.Cmd == 0x86)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "randvar not implemented");
			}
			else if (i->second.Cmd == 0x87)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "andvar not implemented");
			}
			else if (i->second.Cmd == 0x88)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "orvar not implemented");
			}
			else if (i->second.Cmd == 0x89)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "xorvar not implemented");
			}
			else if (i->second.Cmd == 0x8A)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "notvar not implemented");
			}
			else if (i->second.Cmd == 0x8B)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "modvar not implemented");
			}
			else if (i->second.Cmd == 0x90)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_eq not implemented");
			}
			else if (i->second.Cmd == 0x91)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_ge not implemented");
			}
			else if (i->second.Cmd == 0x92)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_gt not implemented");
			}
			else if (i->second.Cmd == 0x93)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_le not implemented");
			}
			else if (i->second.Cmd == 0x94)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_lt not implemented");
			}
			else if (i->second.Cmd == 0x95)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "cmp_ne not implemented");
			}
			else if (i->second.Cmd == 0xA0)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_curve not implemented");
			}
			else if (i->second.Cmd == 0xA1)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_phase not implemented");
			}
			else if (i->second.Cmd == 0xA2)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_depth not implemented");
			}
			else if (i->second.Cmd == 0xA3)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_speed not implemented");
			}
			else if (i->second.Cmd == 0xA4)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_type not implemented");
			}
			else if (i->second.Cmd == 0xA5)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_range not implemented");
			}
			else if (i->second.Cmd == 0xA6)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_curve not implemented");
			}
			else if (i->second.Cmd == 0xA7)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_phase not implemented");
			}
			else if (i->second.Cmd == 0xA8)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_depth not implemented");
			}
			else if (i->second.Cmd == 0xA9)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_speed not implemented");
			}
			else if (i->second.Cmd == 0xAA)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_type not implemented");
			}
			else if (i->second.Cmd == 0xAB)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_range not implemented");
			}
			else if (i->second.Cmd == 0xAC)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_range not implemented");
			}
			else if (i->second.Cmd == 0xAD)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_curve not implemented");
			}
			else if (i->second.Cmd == 0xAE)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_phase not implemented");
			}
			else if (i->second.Cmd == 0xAF)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_depth not implemented");
			}
			else if (i->second.Cmd == 0xB0)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_speed not implemented");
			}
			else if (i->second.Cmd == 0xB1)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_type not implemented");
			}
			else if (i->second.Cmd == 0xE0)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "userproc not implemented");
			}
			else if (i->second.Cmd == 0xE1)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_delay not implemented");
			}
			else if (i->second.Cmd == 0xE2)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod2_period not implemented");
			}
			else if (i->second.Cmd == 0xE3)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_delay not implemented");
			}
			else if (i->second.Cmd == 0xE4)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod3_period not implemented");
			}
			else if (i->second.Cmd == 0xE5)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_delay not implemented");
			}
			else if (i->second.Cmd == 0xE6)
			{
				Common::Warning(Data + dataOffset + 8 + i->first, "mod4_period not implemented");
			}
		}

		// Advance to the next command, unless a handler above redirected the walk
		// (jump/call/return/track change), in which case i already points at the
		// target and must be left there.
		if (redirected)
		{
			redirected = false;
		}
		else
		{
			++i;
		}
	}

	if (smf->timebase == 0)
	{
		smfSetTimebase(smf, 48);
	}

	smfWriteFile(smf, FileName.substr(0, FileName.length() - 5).append("mid").c_str());
	smfDelete(smf);

	return true;
}
