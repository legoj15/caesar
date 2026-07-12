#include "Cseq.hpp"
#include "Common.hpp"

#include "libsmfc/libsmfc.h"
#include "libsmfc/libsmfcx.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
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
// evaluated yet (the range midpoint / variable index stands in for the value),
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

// A voice's pan is the SUM of three independent terms on hardware, not a single
// value: the bank note's own pan, the track's init pan (0xDC) and the track's pan
// (0xC0). The engine stores the latter two identically -- NW4R's MML parser reads
// each as a signed offset from centre (`pan = arg - 64`, `initPan = arg - 64`) and
// adds both into the voice's pan, so neither command overrides the other. caesar
// already exports the note term as the SF2 kPan generator, and a SoundFont player
// sums kPan with CC10 at the generator summing node -- the same additive model --
// so CC10 must carry exactly the other two terms and must not re-add the note pan.
// Writing either command's raw value straight to CC10 would clobber the other,
// which is why 0xDC needs this rather than a write of its own. The clamp is
// two-sided because two offsets in the same direction can push the sum past either
// end (each end is reached 54 times in the corpus), as the engine's own pan clamp
// does.
static int32_t combinePan(int32_t pan, int32_t initPan)
{
	return clamp(pan + initPan - 64, 0, 127);
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
		// A random range: two s16 bounds in file order (NOT necessarily
		// low-then-high -- corpus files carry both orders); the engine rolls a
		// fresh value between them per execution. A deterministic converter
		// must pick one stand-in, and it used to be the raw pair -- callers
		// took Args[0], the FIRST bound, silently biasing 196k volumes, 177k
		// pitch bends and 94k rest durations (timing!) toward that end
		// corpus-wide. The midpoint is the honest deterministic choice until
		// real randomness lands with the convert-time VM (symmetric, so the
		// unsorted order is irrelevant; C++ truncation toward zero; callers
		// see exactly one value, so every consumer inherits it).
		int32_t rndA = ReadFixLen(pos, 2, false, true);
		int32_t rndB = ReadFixLen(pos, 2, false, true);

		return { (rndA + rndB) / 2 };
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
		// 0x90 and 0x96 are not CTR opcodes either (they exist only as
		// extended second-bytes; verified against CtrCafe.cs). The original
		// author probed them here with a guessed 2-byte length, which -- like
		// 0xB7-0xBC below -- could only paper over an upstream desync. They
		// now fall through to the unknown-command error at the end.
		else if ((statusByte >= 0xB0) && (statusByte <= 0xDF))
		{
			// 0xB7-0xBC are not CTR opcodes -- the plain command map jumps
			// 0xB6 -> 0xBD (verified against CtrCafe.cs). They used to be a
			// 1-byte catch-all here, but consuming a guessed length for a byte
			// that cannot legitimately appear only perpetuates whatever
			// upstream desync produced it: fail fast like any other unknown
			// byte. Zero corpus occurrences.
			if ((statusByte >= 0xB7) && (statusByte <= 0xBC))
			{
				Common::Error(pos - 1, "A valid command", statusByte);

				return false;
			}

			// Every command in this range reads one argument through ReadArgs,
			// honouring a Rnd/Var prefix's argument form; only the default type
			// differs. The old code gave 0xB2/0xBF/0xC7/0xC8/0xC9/0xCC/0xCE/
			// 0xDF a bare 1-byte read that ignored the prefix, so a Rnd's
			// 4-byte range was read as 1 byte and every later command in the
			// track misframed (the second wrong-arg-count hazard; zero corpus
			// occurrences).
			if ((statusByte == 0xB1) || (statusByte == 0xC3) || (statusByte == 0xC4) || (statusByte == 0xD0) || (statusByte == 0xD1) || (statusByte == 0xD2) || (statusByte == 0xD3))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int8;
				}
			}
			else if (statusByte == 0xD6)
			{
				// Print var names a variable slot, so the plain argument is a
				// var index.
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Var;
				}
			}
			else if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::Uint8;
			}

			vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());

			// Mod type (0xCC) selects the track LFO target. Only a literal
			// argument is a target byte this validation can apply to; an
			// unevaluated Rnd/Var stand-in is handled with a notice at emit.
			if ((statusByte == 0xCC) && (cmd.Suffix1 == SuffixType::None) && (cmd.Args.back() > 2))
			{
				Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

				return false;
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

			// Record the extended opcode itself. Cmd used to stay 0xF0 here,
			// which matched nothing in the walk's Extended branch, so every
			// extended command (353k setvar, 210k cmp_eq, ... corpus-wide)
			// fell through it silently -- not even its "not implemented"
			// warning could fire.
			cmd.Cmd = statusByte;

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
			else if ((statusByte >= 0xA0) && (statusByte <= 0xB1))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint8;
				}

				vector<int32_t> args = ReadArgs(pos, cmd.Arg1);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());

				// mod2/3/4 type (0xA4/0xAA/0xB0): the same literal-only
				// validation as 0xCC. These three used to read a bare byte
				// that ignored a Rnd/Var prefix's argument form -- the same
				// wrong-arg-count desync as the plain fixed-1-byte group.
				if (((statusByte == 0xA4) || (statusByte == 0xAA) || (statusByte == 0xB0))
					&& (cmd.Suffix1 == SuffixType::None) && (cmd.Args.back() > 2))
				{
					Common::Error(pos - 1, "A valid modulation type", cmd.Args.back());

					return false;
				}
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

		// The Time (_t) suffix appends a trailing s16 ramp duration (or its
		// Rnd/Var form) AFTER the command's own arguments -- for any command
		// the prefix byte can precede, not just 0xB0-0xDF, which is all the
		// old placement inside that branch consumed. A _t on a note, tempo,
		// sweep or extended command left those 2+ bytes unread and misframed
		// every later command in the track (the first wrong-arg-count hazard;
		// zero corpus occurrences -- all ~473k observed _t sit in the safe
		// range). Error paths above return before reaching this, as before.
		if (cmd.Arg2 != ArgType::None)
		{
			vector<int32_t> args = ReadArgs(pos, cmd.Arg2);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
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
	// The current track's two pan terms, in raw command units (64 = centre), which
	// the engine sums into one voice pan (see combinePan). Both start at the value
	// the engine initialises them to, so a track that never sends init pan emits
	// CC10 == its pan and converts byte-identically.
	int32_t trackPan = 64;
	int32_t trackInitPan = 64;
	// The track LFO's target (0xCC): 0 = pitch (the engine default), 1 = volume
	// (tremolo), 2 = pan (auto-pan). The pitch-vibrato CCs (CC1/76/77/78) only
	// describe the LFO while it targets pitch; on a volume/pan span they render
	// tremolo/auto-pan as pitch wobble -- CC1 in particular drives an audible
	// vibrato via the SF2 default mod-wheel modulator -- so they are suppressed
	// while trackModType != 0. The engine keeps ONE persistent parameter block
	// (depth/speed/range/delay) beside a separately retargetable target field,
	// so a value commanded on any span survives a target switch: modShadow holds
	// the last literal commanded value per CC and modWire the value actually on
	// the MIDI wire, letting a return to pitch restore what the engine would
	// still be playing. Slots: 0 = CC1 depth, 1 = CC76 rate, 2 = CC77 range,
	// 3 = CC78 delay; -1 = never commanded/emitted.
	int32_t trackModType = 0;
	int32_t modShadow[4] = { -1, -1, -1, -1 };
	int32_t modWire[4] = { -1, -1, -1, -1 };
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
				trackPan = 64;
				trackInitPan = 64;
				trackModType = 0;

				for (int32_t slot = 0; slot < 4; ++slot)
				{
					modShadow[slot] = -1;
					modWire[slot] = -1;
				}

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

		// The argument machinery the converter does not evaluate yet (the
		// convert-time VM is the roadmap fix) used to mangle values with no trace:
		// an [If] prefix on anything but a jump executes unconditionally (33k
		// conditional Returns corpus-wide can truncate tracks), a Rnd argument
		// collapses to its range midpoint, and a Var argument emits the variable
		// INDEX as if it were the value. Surface each execution as a notice; the
		// behaviour itself is unchanged here. Conditional jumps are excluded --
		// the 0x89 handler resolves or reports those itself.
		if ((i->second.Suffix3 == SuffixType::If) && (i->second.Extended || (i->second.Cmd != 0x89)))
		{
			Common::Warning(here, "[If] prefix not evaluated; command executes unconditionally",
				"[If]-prefixed commands executed unconditionally (conditions not evaluated)");
		}

		if (i->second.Suffix1 == SuffixType::Rnd)
		{
			Common::Warning(here, "Rnd argument not evaluated; range midpoint stands in",
				"Rnd-valued arguments not evaluated (range midpoint stands in)");
		}
		else if (i->second.Suffix1 == SuffixType::Var)
		{
			Common::Warning(here, "Var argument not evaluated; variable index stands in for the value",
				"Var-valued arguments not evaluated (variable index stands in)");
		}

		// A _t ramp commands the engine to glide from the parameter's current
		// value to the target over the trailing duration; caesar emits the
		// target at the command tick instead (375k volume fades corpus-wide,
		// previously silent). Real interpolation is stage-2/5 flattening work.
		if (i->second.Suffix2 != SuffixType::None)
		{
			Common::Warning(here, "ramped (_t) change flattened to an instant jump at the command tick",
				"ramped (_t) changes flattened to instant jumps");
		}

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
						Common::Warning(here, "conditional jump skipped (conditions not evaluated)",
							"conditional jumps skipped (conditions not evaluated)");
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
						Common::Warning(here, "jump target out of range; jump ignored",
							"jump targets out of range (jump ignored)");
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
				// NW4C ADSHR hold-stage override; no GM/GS controller exists for it.
				Common::Warning(here, "envelope hold has no MIDI equivalent; dropped",
					"envelope hold dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB2)
			{
				// Mono/poly: the engine's per-track voice-allocation flag (mono =
				// a new note steals the track's previous voice; toggling it does
				// nothing to already-sounding voices). This used to be emitted as
				// CC126/CC127, but those are Channel MODE messages, not channel
				// controls: the MIDI 1.0 spec mandates an implicit All Notes Off
				// on CC124-127, so a mid-track toggle -- 56 of the 249 corpus
				// executions, 35 of them after the track has already sounded
				// notes -- chops every ringing note on the channel. Real players
				// are no gentler: FluidSynth honours these CCs only on a basic
				// channel (channel 0), where they reconfigure the whole
				// poly/mono channel-group and can disable every other channel
				// outright; most GM players ignore them entirely. There is no
				// per-channel MIDI control with the engine's semantics, so drop
				// with a notice. The future player reads the flag from the
				// sequence itself; nothing is lost for suite stage 2.
				Common::Warning(here, "mono/poly is a voice-allocation flag with no MIDI equivalent; dropped",
					"mono/poly dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB3)
			{
				Common::Warning(here, "velocity range not implemented; dropped",
					"velocity range dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xB4)
			{
				// Voice biquad response select; no audible MIDI target.
				Common::Warning(here, "biquad type has no MIDI equivalent; dropped",
					"biquad filter dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB5)
			{
				Common::Warning(here, "biquad value has no MIDI equivalent; dropped",
					"biquad filter dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB6)
			{
				// A real fidelity gap (wrong instrument wherever a track switches
				// banks mid-sequence), but the emitted CC0 must be co-designed with
				// Cbnk's SF2 bank layout -- see the roadmap item.
				Common::Warning(here, "mid-sequence bank select not implemented; instrument may be wrong",
					"bank select dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xBD)
			{
				Common::Warning(here, "mod phase has no MIDI equivalent; dropped",
					"LFO phase/curve dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xBE)
			{
				Common::Warning(here, "mod curve has no MIDI equivalent; dropped",
					"LFO phase/curve dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xBF)
			{
				// Surround-path routing flag (bypass the front virtualization);
				// only meaningful under the console's Surround output mode, and
				// MIDI has no surround axis at all.
				Common::Warning(here, "front bypass is surround routing with no MIDI equivalent; dropped",
					"front bypass dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xC0)
			{
				int32_t pan = clampPlainCtrl(i->second, here);

				// Fold in whatever init pan (0xDC) the track is carrying. With none --
				// every sequence that does not use 0xDC -- this is the pan value itself,
				// so those files are untouched. An unevaluated Rnd/Var stand-in is not a
				// pan, so it keeps being written raw as before rather than being summed
				// into a plausible-looking position.
				if (i->second.Suffix1 == SuffixType::None)
				{
					trackPan = pan;
					pan = combinePan(trackPan, trackInitPan);
				}

				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PANPOT, pan), here);
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
				// Voice-steal priority: engine scheduling state with no meaning in
				// MIDI (the future player must preserve it; nothing is lost here).
				Common::Warning(here, "voice priority has no MIDI equivalent; dropped",
					"priority dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xC7)
			{
				noteWait = i->second.Args[0];
			}
			else if (i->second.Cmd == 0xC8)
			{
				// Real articulation loss: tied notes re-attack instead of merging
				// into one sustained note (the roadmap's v0.5.1 stretch item).
				Common::Warning(here, "tie mode not implemented; tied notes re-attack",
					"tie mode dropped (tied notes re-attack)");
			}
			else if (i->second.Cmd == 0xC9)
			{
				emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTOCTRL, i->second.Args[0]), here);
			}
			else if (i->second.Cmd == 0xCA)
			{
				if (i->second.Suffix1 == SuffixType::None)
				{
					modShadow[0] = i->second.Args[0];
				}

				if (trackModType == 0)
				{
					if (emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_MODULATION, i->second.Args[0]), here))
					{
						modWire[0] = i->second.Args[0];
					}
				}
				else
				{
					Common::Warning(here, "mod depth while the track LFO targets volume/pan; CC1 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xCB)
			{
				int32_t rate = (i->second.Args[0] / 2) + 64;

				if (i->second.Suffix1 == SuffixType::None)
				{
					modShadow[1] = rate;
				}

				if (trackModType == 0)
				{
					if (emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATORATE, rate), here))
					{
						modWire[1] = rate;
					}
				}
				else
				{
					Common::Warning(here, "mod speed while the track LFO targets volume/pan; CC76 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xCC)
			{
				// Track LFO target: 0 = pitch, 1 = volume (tremolo), 2 = pan
				// (auto-pan); parse already rejected anything above 2. Retargeting
				// only re-routes the engine's LFO -- its parameters persist -- so in
				// MIDI terms leaving pitch must silence a live CC1 (the SF2 default
				// mod-wheel modulator keeps wobbling pitch otherwise) and returning
				// to pitch must restore whatever the persistent parameters now hold.
				// An unevaluated Rnd/Var stand-in is not a target, so it never
				// latches (and tremolo/auto-pan itself has no MIDI equivalent).
				if (i->second.Suffix1 == SuffixType::None)
				{
					int32_t newType = i->second.Args[0];

					if (newType != trackModType)
					{
						trackModType = newType;

						if (newType != 0)
						{
							if (modWire[0] > 0)
							{
								smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_MODULATION, 0);
								modWire[0] = 0;

								Common::Warning(here, "track LFO retargeted to volume/pan; tremolo/auto-pan not rendered",
									"tremolo/auto-pan LFO dropped (no MIDI equivalent)");
							}
						}
						else
						{
							const int32_t modCtrl[4] =
								{ SMF_CONTROL_MODULATION, SMF_CONTROL_VIBRATORATE, SMF_CONTROL_VIBRATODEPTH, SMF_CONTROL_VIBRATODELAY };

							for (int32_t slot = 0; slot < 4; ++slot)
							{
								if ((modShadow[slot] >= 0) && (modShadow[slot] != modWire[slot]))
								{
									if (emitCtrl(smfInsertControl(smf, absTime, chan, track, modCtrl[slot], modShadow[slot]), here))
									{
										modWire[slot] = modShadow[slot];
									}
								}
							}
						}
					}
				}
				else
				{
					Common::Warning(here, "mod type is an unevaluated Rnd/Var; LFO target not tracked",
						"mod type dropped (unevaluated Rnd/Var)");
				}
			}
			else if (i->second.Cmd == 0xCD)
			{
				int32_t range = (i->second.Args[0] / 2) + 64;

				if (i->second.Suffix1 == SuffixType::None)
				{
					modShadow[2] = range;
				}

				if (trackModType == 0)
				{
					if (emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODEPTH, range), here))
					{
						modWire[2] = range;
					}
				}
				else
				{
					Common::Warning(here, "mod range while the track LFO targets volume/pan; CC77 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
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
				// ADSR sustain LEVEL (not the pedal -- that is 0xDF); no GM2/GS
				// controller exists for it.
				Common::Warning(here, "envelope sustain level has no MIDI equivalent; dropped",
					"sustain level dropped (no MIDI equivalent)");
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
				// 0 (= forever) stand-in instead of baking a range midpoint /
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
				// A debug print of a sequence variable; nothing to render.
				Common::Warning(here, "print var is a debug command with no MIDI equivalent; dropped",
					"print var dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xD7)
			{
				// SurroundPan: the front/rear axis of the DSP's quad voice-gain
				// matrix. Audible on console only under the System Settings
				// "Surround" output mode (console-confirmed 2026-07-11); MIDI has
				// no surround axis in any mode, so there is nothing to map it to.
				// The future player must model it -- see the suite plan.
				Common::Warning(here, "span (front/rear surround pan) has no MIDI equivalent; dropped",
					"span dropped (no MIDI surround axis)");
			}
			else if (i->second.Cmd == 0xD8)
			{
				// LPF cutoff -> CC74 (brightness). Both are relative controls centred on
				// 64: NW4R's SeqTrack::InitParam starts lpfFreq at 64, and the command
				// scales the voice's low-pass cutoff by (value / 64), so 0-64 maps
				// straight through -- 0 is fully closed, 64 fully open.
				//
				// Above 64 the two diverge, and passing the value through would invent
				// sound the console never made: Voice::SetLpfFreq clamps that scale to
				// [0,1], so every byte >= 64 is identical to 64 (filter open, i.e. the
				// sample's own tone), whereas CC74 above 64 tells the synth to brighten
				// PAST the sample's tone. Clamp to the engine's own ceiling instead --
				// 261 commands in the corpus sit above it.
				//
				// The remaining approximation is the curve: hardware steps 187.5 cents
				// per unit (an exponential 31.25 Hz - 32 kHz sweep) against GM2's 150,
				// so a cut reads about 20% shallow. Direction, neutral point and the
				// closed/open ends are all right, which is what a relative control can
				// carry. A Rnd/Var stand-in is not a cutoff, so it keeps dropping.
				if (i->second.Suffix1 == SuffixType::None)
				{
					emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BRIGHTNESS, clamp(i->second.Args[0], 0, 64)), here);
				}
				else
				{
					Common::Warning(here, "lpf cutoff is Rnd/Var-valued; dropped",
						"MIDI brightness changes dropped (unevaluated Rnd/Var lpf cutoff)");
				}
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
				Common::Warning(here, "main (dry) send not implemented; dropped",
					"main send dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xDC)
			{
				// Init pan: a second pan offset the engine ADDS to the track pan rather
				// than replacing it (see combinePan), so it cannot be written to CC10 on
				// its own -- that would clobber any 0xC0 pan, which 76 tracks in the
				// corpus set alongside it. Track it and emit the combined position.
				//
				// The name is misleading: hardware latches init pan at note-on and does
				// not move notes already sounding, and 74% of the corpus' uses fire
				// mid-track, after notes have started. CC10 moves the whole channel,
				// sounding notes included; MIDI has no per-note pan, so that difference
				// is not expressible and the tick is the honest place to put it.
				//
				// A Rnd/Var stand-in would poison the combined pan for every later note
				// on the track, not just its own event, so it keeps dropping.
				if (i->second.Suffix1 == SuffixType::None)
				{
					trackInitPan = clampPlainCtrl(i->second, here);

					emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PANPOT, combinePan(trackPan, trackInitPan)), here);
				}
				else
				{
					Common::Warning(here, "init pan is Rnd/Var-valued; dropped",
						"MIDI pan changes dropped (unevaluated Rnd/Var init pan)");
				}
			}
			else if (i->second.Cmd == 0xDD)
			{
				Common::Warning(here, "track mute not implemented; notes keep sounding",
					"mute dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xDE)
			{
				// FX send C: the third aux bus. A real CTR command (zero corpus
				// occurrences) that used to fall off the end of this chain with no
				// trace at all. Buses A/B map to CC91/93; GM has no third effects
				// send, so C drops -- but now visibly.
				Common::Warning(here, "fx send C (third aux bus) has no MIDI equivalent; dropped",
					"fx send C dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xDF)
			{
				// Damper pedal -> CC64 (hold 1). The engine thresholds this Uint8
				// exactly the way MIDI does -- NW4R sets damperFlag = (u8)arg >= 64,
				// and a held damper suppresses the note's release -- so the raw value
				// was already right for 0-127. Applying the engine's own threshold
				// extends that to the argument's full 0-255 domain: a value above 127
				// is pedal-DOWN on hardware, but the writer drops any control outside
				// 7-bit range, so the pedal event used to vanish. Normalizing keeps the
				// value in range by construction, hence no emitCtrl guard.
				// (Gota's table types this Bool, but his player never runs the command;
				// the threshold is what the matching NW4R decomps and the corpus show.)
				smfInsertControl(smf, absTime, chan, track, 64, (i->second.Args[0] >= 64) ? 127 : 0);
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
					int32_t delay = 64 + min(ms * 63 / 1000, 63);

					modShadow[3] = delay;

					if (trackModType == 0)
					{
						smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODELAY, delay);
						modWire[3] = delay;
					}
					else
					{
						Common::Warning(here, "mod delay while the track LFO targets volume/pan; CC78 suppressed",
							"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
					}
				}
				else if (trackModType == 0)
				{
					// Unevaluated Rnd/Var stand-in: keep the raw-value path so
					// out-of-range garbage still drops with the notice instead of
					// being scaled into a plausible-looking delay.
					if (emitCtrl(smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODELAY, (i->second.Args[0] / 2) + 64), here))
					{
						modWire[3] = (i->second.Args[0] / 2) + 64;
					}
				}
				else
				{
					Common::Warning(here, "mod delay while the track LFO targets volume/pan; CC78 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
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
				Common::Warning(here, "mod period has no MIDI equivalent; dropped",
					"LFO period dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xFB)
			{
				Common::Warning(here, "envelope reset not implemented; dropped",
					"envelope reset dropped (not implemented)");
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
			else
			{
				// Anything the chain above does not handle used to vanish with no
				// trace (0xDE sat here for years). Parse already vets the byte, so
				// reaching this means a command was parsed but never wired up --
				// surface it instead of silently perpetuating the gap.
				char msg[64];
				snprintf(msg, sizeof(msg), "unhandled sequence command 0x%02X; dropped", i->second.Cmd);
				Common::Warning(here, msg, "unhandled sequence commands dropped");
			}
		}
		else
		{
			// Extended (0xF0-prefixed) command space: nothing here is implemented
			// yet. The variable/comparison ops await the convert-time VM (see the
			// roadmap); the mod2-4 multi-LFO family has zero corpus occurrences.
			// One name table instead of 42 branches, in CtrCafe byte order -- the
			// old chain's mod4 labels (0xAC-0xB1) were scrambled against the map,
			// which never showed because the chain was dead code (see the parse
			// fix that records the extended opcode in Cmd).
			static const map<uint8_t, const char*> extendedNames =
			{
				{ 0x80, "setvar" },     { 0x81, "addvar" },      { 0x82, "subvar" },
				{ 0x83, "mulvar" },     { 0x84, "divvar" },      { 0x85, "shiftvar" },
				{ 0x86, "randvar" },    { 0x87, "andvar" },      { 0x88, "orvar" },
				{ 0x89, "xorvar" },     { 0x8A, "notvar" },      { 0x8B, "modvar" },
				{ 0x90, "cmp_eq" },     { 0x91, "cmp_ge" },      { 0x92, "cmp_gt" },
				{ 0x93, "cmp_le" },     { 0x94, "cmp_lt" },      { 0x95, "cmp_ne" },
				{ 0xA0, "mod2_curve" }, { 0xA1, "mod2_phase" },  { 0xA2, "mod2_depth" },
				{ 0xA3, "mod2_speed" }, { 0xA4, "mod2_type" },   { 0xA5, "mod2_range" },
				{ 0xA6, "mod3_curve" }, { 0xA7, "mod3_phase" },  { 0xA8, "mod3_depth" },
				{ 0xA9, "mod3_speed" }, { 0xAA, "mod3_type" },   { 0xAB, "mod3_range" },
				{ 0xAC, "mod4_curve" }, { 0xAD, "mod4_phase" },  { 0xAE, "mod4_depth" },
				{ 0xAF, "mod4_speed" }, { 0xB0, "mod4_type" },   { 0xB1, "mod4_range" },
				{ 0xE0, "userproc" },
				{ 0xE1, "mod2_delay" }, { 0xE2, "mod2_period" },
				{ 0xE3, "mod3_delay" }, { 0xE4, "mod3_period" },
				{ 0xE5, "mod4_delay" }, { 0xE6, "mod4_period" },
			};

			uint8_t ext = i->second.Cmd;
			auto name = extendedNames.find(ext);
			char msg[64];

			if (name == extendedNames.end())
			{
				// Parse vets extended bytes, so this is the same safety net as the
				// plain chain's final else: parsed but never wired up.
				snprintf(msg, sizeof(msg), "unhandled extended command 0x%02X; dropped", ext);
				Common::Warning(here, msg, "unhandled sequence commands dropped");
			}
			else
			{
				const char* category =
					(ext <= 0x8B) ? "variable ops dropped (variables not evaluated)" :
					(ext <= 0x95) ? "comparison ops dropped (variables not evaluated)" :
					(ext == 0xE0) ? "userproc dropped (no MIDI equivalent)" :
					"multi-LFO (mod2-4) commands dropped (not implemented)";

				snprintf(msg, sizeof(msg), "%s not implemented; dropped", name->second);
				Common::Warning(here, msg, category);
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
