#include "Cseq.hpp"
#include "Common.hpp"

#include "libsmfc/libsmfc.h"
#include "libsmfc/libsmfcx.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
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

// Convert-time variable-VM policy: all three sequence-variable scopes (player,
// global, track) start at 0. This is a deliberate CONVERTER policy, not the
// hardware power-on value -- NW4R's DEFAULT_VARIABLE_VALUE is -1 for every
// scope, and on hardware the globals are static, seeded by the game via
// SetGlobalVariable and persisting across songs, neither of which a standalone
// converter can reproduce. 0 is the "game at rest / default section" value, so
// game-driven globals default to the same section the retired dispatcher
// heuristic aimed for and every conversion is deterministic. Kept a named
// constant so the init-policy A/B is a one-line flip.
static constexpr int16_t kVarInit = 0;

// Safety budgets. kCondRetakeBudget caps the backward retakes per conditional
// jump-site (see the 0x89 [If] handling): a counted loop unrolls until its
// comparison flips cmpFlag, but a state-churning loop can spin without
// settling. A GENUINE counted loop above the budget is truncated at 1024
// passes -- the refusal notice surfaces it, and no corpus loop counts that
// high. kTrackExecBudget caps total commands executed per track, checked once
// per walk iteration: it is the terminal backstop for any cycle the other
// guards cannot see (a Call that reaches itself recursed unboundedly before
// this guard existed). Neither budget binds on real data.
static constexpr uint32_t kCondRetakeBudget = 1024;
static constexpr uint32_t kTrackExecBudget = 1u << 20;

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
static bool emitCtrl(ParseContext& Ctx, bool ok, uint8_t* pos)
{
	if (!ok)
	{
		Ctx.Warning(pos, "control/parameter event out of MIDI range; dropped",
			"MIDI control/parameter events dropped (value out of range)");
	}

	return ok;
}

static bool emitProgram(ParseContext& Ctx, bool ok, uint8_t* pos)
{
	if (!ok)
	{
		Ctx.Warning(pos, "program number out of MIDI range; dropped",
			"MIDI program changes dropped (value out of range)");
	}

	return ok;
}

// Clamp an already-resolved control/parameter value into 7-bit MIDI range,
// surfacing the approximation. A plain Uint8 argument is genuine sequence data
// in 0-255, so 128-255 clamps to 127 rather than dropping the write; the
// convert-time VM can also hand this a Var/Rnd-resolved s16 that is below 0,
// which clamps to 0. (Before the VM every argument reaching here was a literal
// value and only the high end was live -- the callers gated this on
// Suffix1 == None and let an unevaluated Rnd/Var stand-in drop through the
// emitCtrl notice instead; the value now arrives evaluated, so both ends are
// real and the caller no longer discriminates on the prefix.)
static int32_t clampCtrl(ParseContext& Ctx, int32_t value, uint8_t* pos)
{
	if (value > 127)
	{
		Ctx.Warning(pos, "control/parameter value above MIDI range; clamped to 127",
			"MIDI control/parameter values clamped to 127 (above range)");

		return 127;
	}

	if (value < 0)
	{
		Ctx.Warning(pos, "control/parameter value below MIDI range; clamped to 0",
			"MIDI control/parameter values clamped to 0 (below range)");

		return 0;
	}

	return value;
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

vector<int32_t> ReadArgs(ParseContext& Ctx, uint8_t*& pos, ArgType argType, pair<int32_t, int32_t>* rndBounds)
{
	if (argType == ArgType::Uint8)
	{
		return { Ctx.ReadFixLen(pos, 1) };
	}
	else if (argType == ArgType::Int8)
	{
		return { Ctx.ReadFixLen(pos, 1, false, true) };
	}
	else if (argType == ArgType::Uint16)
	{
		return { Ctx.ReadFixLen(pos, 2, false) };
	}
	else if (argType == ArgType::Int16)
	{
		return { Ctx.ReadFixLen(pos, 2, false, true) };
	}
	else if (argType == ArgType::Rnd)
	{
		// A random range: two s16 bounds in file order (NOT necessarily
		// low-then-high -- corpus files carry both orders); the engine rolls a
		// fresh value between them per execution. The raw pair is retained in
		// the model (handed back through rndBounds), keeping the bytes lossless
		// and leaving the exporter's midpoint decision to the emit walk rather
		// than welding it into the parsed model here. A deterministic converter
		// must still pick one stand-in; caesar's is the range midpoint
		// ((lo + hi) / 2, C++ truncation toward zero), computed at emit in
		// resolveArg -- symmetric, so the unsorted order is irrelevant, and the
		// VM surfaces it as an approximation notice at the command site. The
		// value stored in Args is the first bound as an inert placeholder that
		// keeps the slot count intact; no consumer reads a Rnd slot's Args value
		// (every Rnd read goes through resolveArg, which uses the bounds).
		int32_t rndA = Ctx.ReadFixLen(pos, 2, false, true);
		int32_t rndB = Ctx.ReadFixLen(pos, 2, false, true);

		if (rndBounds)
		{
			*rndBounds = { rndA, rndB };
		}

		return { rndA };
	}
	else if (argType == ArgType::Var)
	{
		return { Ctx.ReadFixLen(pos, 1) };
	}
	else if (argType == ArgType::VarLen)
	{
		return { Ctx.ReadVarLen(pos) };
	}

	return { };
}

Cseq::Cseq(const string& fileName, uint8_t* data, streamoff length, ParseContext& ctx) : Ctx(ctx), FileName(fileName)
{
	Length = length;

	// The parent already holds these bytes -- the span its just-written .bcseq
	// was serialised from -- so borrow them directly instead of re-opening the
	// file we just wrote. A zero-length span is the same degenerate condition
	// the old file-path constructor rejected on an empty re-read (RequireOpen on
	// length <= 0); preserve that error identically, and fire it before the Push
	// echo so the stdout stream stays byte-for-byte unchanged.
	Ctx.RequireOpen(true, Length, FileName);

	Data = data;

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);
}

Cseq::~Cseq()
{
	Ctx.Pop();

	// Data is borrowed from the parent's buffer (freed by the parent, after this
	// child); do not delete it here.
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

	if (!Ctx.Assert(pos, 0x43534551, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cseqVersion = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x2, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x5000, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t dataOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t dataLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x5001, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t lablOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t lablLength = Ctx.ReadFixLen(pos, 4);

	pos = Data + lablOffset;

	if (!Ctx.Assert(pos, 0x4C41424C, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, lablLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t lablCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> lablOffsets;

	for (uint32_t i = 0; i < lablCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x5100, Ctx.ReadFixLen(pos, 4))) { return false; }

		lablOffsets.push_back(Data + lablOffset + 8 + Ctx.ReadFixLen(pos, 4));
	}

	map<uint8_t*, CseqLabl> labls;

	for (uint32_t i = 0; i < lablCount; ++i)
	{
		pos = lablOffsets[i];

		if (!Ctx.Assert(pos, 0x1F00, Ctx.ReadFixLen(pos, 4))) { return false; }

		CseqLabl labl;
		labl.Offset = Data + dataOffset + 8 + Ctx.ReadFixLen(pos, 4);
		uint32_t lablLength = Ctx.ReadFixLen(pos, 4);
		Ctx.CheckBounds(pos, lablLength);
		labl.Label = string(reinterpret_cast<const char*>(pos), lablLength);

		labls[labl.Offset] = labl;
	}

	pos = Data + dataOffset;

	if (!Ctx.Assert(pos, 0x44415441, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, dataLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	map<uint32_t, CseqCmd> commands;

	while (pos < (Data + dataOffset + dataLength))
	{
		uint32_t offset = static_cast<uint32_t>(pos - 8 - dataOffset - Data);
		CseqCmd cmd;

		if (labls.count(pos))
		{
			cmd.Label = labls[pos].Label;
		}

		uint8_t statusByte = Ctx.ReadFixLen(pos, 1);

		if (statusByte == 0xA2)
		{
			cmd.Suffix3 = SuffixType::If;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}

		if (statusByte == 0xA3)
		{
			cmd.Suffix2 = SuffixType::Time;
			cmd.Arg2 = ArgType::Int16;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA4)
		{
			cmd.Suffix2 = SuffixType::TimeRnd;
			cmd.Arg2 = ArgType::Rnd;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA5)
		{
			cmd.Suffix2 = SuffixType::TimeVar;
			cmd.Arg2 = ArgType::Var;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}

		if (statusByte == 0xA0)
		{
			cmd.Suffix1 = SuffixType::Rnd;
			cmd.Arg1 = ArgType::Rnd;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}
		else if (statusByte == 0xA1)
		{
			cmd.Suffix1 = SuffixType::Var;
			cmd.Arg1 = ArgType::Var;

			statusByte = Ctx.ReadFixLen(pos, 1);
		}

		cmd.Cmd = statusByte;

		if (statusByte < 0x80)
		{
			cmd.Args.push_back(Ctx.ReadFixLen(pos, 1));

			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::VarLen;
			}

			vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if ((statusByte == 0x80) || (statusByte == 0x81))
		{
			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::VarLen;
			}

			vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if (statusByte == 0x88)
		{
			cmd.Args.push_back(Ctx.ReadFixLen(pos, 1));
			cmd.Args.push_back(Ctx.ReadFixLen(pos, 3, false));
		}
		else if ((statusByte == 0x89) || (statusByte == 0x8A))
		{
			cmd.Args.push_back(Ctx.ReadFixLen(pos, 3, false));
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
				Ctx.Error(pos - 1, "A valid command", statusByte);

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

			vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());

			// Mod type (0xCC) selects the track LFO target. This used to be a
			// hard error for values above 2, but that is stricter than the
			// hardware: NW4R stores the byte unvalidated and its routing
			// if-chain simply applies no LFO to an out-of-range target, so the
			// console plays such a file (LFO silent) where caesar refused it
			// outright. Notice and continue; the emit handler suppresses the
			// pitch-vibrato CCs for any non-pitch target, which is exactly the
			// audible result. Only a literal argument can be range-checked here;
			// a Var/Rnd target is known only at emit, where the VM's resolved
			// value drives the same suppression. Zero corpus occurrences.
			if ((statusByte == 0xCC) && (cmd.Suffix1 == SuffixType::None) && (cmd.Args.back() > 2))
			{
				Ctx.Warning(pos - 1, "mod type above 2; the engine applies no LFO",
					"mod type out of range (engine applies no LFO)");
			}
		}
		else if ((statusByte == 0xE0) || (statusByte == 0xE1) || (statusByte == 0xE3) || (statusByte == 0xE4))
		{
			if (cmd.Arg1 == ArgType::None)
			{
				cmd.Arg1 = ArgType::Int16;
			}

			vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}
		else if (statusByte == 0xF0)
		{
			cmd.Extended = true;

			statusByte = Ctx.ReadFixLen(pos, 1);

			// Record the extended opcode itself. Cmd used to stay 0xF0 here,
			// which matched nothing in the walk's Extended branch, so every
			// extended command (353k setvar, 210k cmp_eq, ... corpus-wide)
			// fell through it silently -- not even its "not implemented"
			// warning could fire.
			cmd.Cmd = statusByte;

			if (((statusByte >= 0x80) && (statusByte <= 0x8B)) || ((statusByte >= 0x90) && (statusByte <= 0x95)))
			{
				vector<int32_t> args1 = ReadArgs(Ctx, pos, ArgType::Var);

				cmd.Args.insert(cmd.Args.end(), args1.begin(), args1.end());

				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int16;
				}

				vector<int32_t> args2 = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

				cmd.Args.insert(cmd.Args.end(), args2.begin(), args2.end());
			}
			else if ((statusByte >= 0xA0) && (statusByte <= 0xB1))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint8;
				}

				vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());

				// mod2/3/4 type (0xA4/0xAA/0xB0) needs no range validation:
				// the engine stores the target unvalidated (out of range =
				// no LFO applied), and the whole mod2-4 family is dropped at
				// emit with a notice anyway. The old hard error was stricter
				// than hardware, and these three also used to read a bare
				// byte that ignored a Rnd/Var prefix's argument form -- the
				// same wrong-arg-count desync as the plain fixed-1-byte
				// group. Both fixed; the shared ReadArgs above is all that
				// is needed.
			}
			else if (statusByte == 0xE0)
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Uint16;
				}

				vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else if ((statusByte >= 0xE1) && (statusByte <= 0xE6))
			{
				if (cmd.Arg1 == ArgType::None)
				{
					cmd.Arg1 = ArgType::Int16;
				}

				vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg1, &cmd.Arg1Rnd);

				cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
			}
			else
			{
				Ctx.Error(pos - 1, "A valid extended command", statusByte);

				return false;
			}
		}
		else if ((statusByte == 0xFB) || (statusByte == 0xFC) || (statusByte == 0xFD) || (statusByte == 0xFF))
		{
			// These commands take no arguments
		}
		else if (statusByte == 0xFE)
		{
			cmd.Args.push_back(Ctx.ReadFixLen(pos, 2, false));
		}
		else
		{
			Ctx.Error(pos - 1, "A valid command", statusByte);

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
			vector<int32_t> args = ReadArgs(Ctx, pos, cmd.Arg2, &cmd.Arg2Rnd);

			cmd.Args.insert(cmd.Args.end(), args.begin(), args.end());
		}

		commands[offset] = cmd;
	}

	Smf* smf = smfCreate();
	uint32_t absTime = 0;
	uint8_t track = 0;
	// Note-wait starts ON: the engine default (NW4R SeqTrack ctor sets
	// noteWaitFlag = true), corroborated corpus-wide -- 92% of the corpus's
	// explicit 0xC7 commands are DISABLES (44,349 off vs 3,654 on), i.e. the
	// authoring tool escaping an on-default, and the no-rest tied sweeps
	// (SE_Map_WarpstarUp*) only produce their console sound with waits.
	// caesar shipped OFF for years, compressing every track that plays notes
	// before an explicit 0xC7 (~112k notes across 67 archives).
	bool noteWait = true;
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
	// Tie mode (0xC8). With tie on, the engine's track plays ONE continuous
	// voice: the first note attacks, each later note merely updates the
	// sounding voice's key/velocity (no re-attack), the note-length argument
	// is ignored for audio (NoteOn stores length -1, which never counts down,
	// so the voice sustains through gates and rests), and the voice releases
	// only at the next tie command -- BOTH edges release and free the track's
	// channels -- a Fin, or the track's end (NW4R SeqTrack::NoteOn /
	// UpdateChannelLength / MML_SET_TIE; note-wait still advances track time
	// by the length argument as usual). MIDI has no "retune the sounding
	// note" event a GM/SF2 player honours, so a tie region flattens to
	// back-to-back segments: one note per commanded pitch/velocity, each
	// running from its note-on to the next boundary -- gap-free, spanning
	// rests and gates, the last extended to the region's end. The remaining
	// approximation (a re-attack at each pitch change instead of one
	// continuous envelope) is surfaced once per region.
	bool tieOn = false;
	bool tieHeld = false;
	uint32_t tieStart = 0;
	uint32_t tieCmdOffset = 0;
	int32_t tieKey = 0;
	int32_t tieVel = 0;
	// The convert-time variable VM. vars[] is the engine's own s16 storage (all
	// op arithmetic wraps at 16 bits through it); the index map is
	// RESEARCH-CONFIRMED: 0-15 player-local, 16-31 global, 32-47 track-local.
	// Player/global slots persist across the sequential track walk (globals are
	// static on hardware; the sequential visibility of writes is the
	// deterministic stand-in for concurrent tracks), while track slots (32-47)
	// reset per track in advanceToNextTrack (SeqTrack::InitParam). varWritten
	// tracks which slots the sequence itself has written, so a read of a slot the
	// game would have seeded externally is surfaced honestly (init-0 stands in).
	// cmpFlag is the per-track comparison result the [If] gate reads; it starts
	// true (NW4R MmlSeqTrack ctor) and is written only by the six comparisons.
	int16_t vars[48];

	for (int32_t v = 0; v < 48; ++v)
	{
		vars[v] = kVarInit;
	}

	bool varWritten[48] = { false };
	bool cmpFlag = true;
	// Bumped only when VM state actually changes (a var write that changes the
	// stored value or flips varWritten, or a cmpFlag flip). offsetVersion stamps
	// the vmVersion in force when each offset was last reached, beside offsetTime;
	// together they let a conditional backward jump tell a still-counting loop
	// (state moved since the target ran) from a spin-wait (state unchanged).
	uint64_t vmVersion = 0;
	map<uint32_t, uint64_t> offsetVersion;
	// Conditional-loop budgets: revisits taken per jump-site, and total commands
	// executed this track. Both cleared per track (see advanceToNextTrack).
	map<uint32_t, uint32_t> condRetakes;
	uint32_t trackExecs = 0;
	// Re-roll loop escape bookkeeping. A common SE idiom is a self-contained RNG
	// wait loop -- randvar, compare, [If]-jump to the play block, unconditional
	// jump back -- which on hardware ALWAYS eventually exits (the sequence rolls
	// its own variable until the comparison clears; Pokemon niji_sound's ambient
	// SFX are 50 corpus files of exactly this). A PRNG-free midpoint can gate
	// that exit off permanently, and the loop's unconditional back-jump would
	// then read as a "whole-song loop" and end the track silent. So every
	// [If]-jump the gate skips is recorded (offset -> target), tagged with
	// whether the comparison that gated it read any never-written -- i.e.
	// game-seeded -- variable: a game-driven wait (Animal Crossing's spin-wait
	// dispatchers, 138 files) must NOT be escaped, because silence-until-the-
	// game-acts is its real at-rest behaviour, while a sequence-internal RNG
	// wait is escaped once at the loop-classification site (see the 0x89
	// handler). All cleared per track.
	map<uint32_t, pair<uint32_t, bool>> gatedExits;   // jump offset -> (target, gameDriven)
	set<uint32_t> gatedExitUsed;
	bool lastCmpGameDriven = false;
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
	// track, so an unconditional backward jump can place its loop-start marker at
	// the tick where the loop target originally played (and, for a conditional
	// backward jump, so offsetVersion has a companion key). Cleared at every
	// track boundary.
	map<uint32_t, uint32_t> offsetTime;

	// Begin at this entry's start offset within the shared bank. If it does not
	// land on a command boundary (a malformed offset, or an archive that stores
	// it differently), fall back to the top rather than skip the sequence.
	auto i = commands.find(startOffset);

	if (i == commands.end())
	{
		if (startOffset != 0)
		{
			Ctx.Warning(Data + dataOffset + 8, "sequence start offset is not a command boundary; starting from the top");
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

	// Close the current tie segment, sounding from its note-on to `end`. A
	// zero-length segment (a pitch update in the tick it was opened, or a
	// region closed at its own start) never had time to sound and is skipped,
	// matching the per-frame granularity of the hardware update. The insertion
	// tick lies in the past, which libsmfc's time-sorted writer handles the
	// same way the loop-start markers already do. A velocity out of MIDI range
	// surfaces exactly like a rejected plain note.
	auto finalizeTie = [&](uint32_t end)
	{
		if (!tieHeld)
		{
			return;
		}

		tieHeld = false;

		if ((end > tieStart)
			&& !smfInsertNote(smf, tieStart, channelOf[track], track, tieKey, tieVel, end - tieStart)
			&& (tieVel > 127))
		{
			Ctx.Warning(Data + dataOffset + 8 + tieCmdOffset, "note velocity out of MIDI range; note dropped",
				"MIDI notes dropped (velocity out of range)");
		}
	};

	// End the current track and move to the next allocated one (registered by an
	// earlier 0x88 in trackOffsets). Shared by three "this track is done here"
	// cases: Fin (0xFF), a whole-song loop's loop-back, and a stray Return.
	// Returns false when no further track remains, i.e. the whole sequence has
	// finished and the caller should stop the walk. A tie segment still sounding
	// closes at the track's end, exactly where the hardware releases it.
	auto advanceToNextTrack = [&]() -> bool
	{
		finalizeTie(absTime);

		smfSetEndTimingOfTrack(smf, track, absTime);

		for (uint8_t j = track + 1; j < 16; ++j)
		{
			if (trackOffsets[j] != 0)
			{
				absTime = 0;
				track = j;
				noteWait = true;
				trackPan = 64;
				trackInitPan = 64;
				trackModType = 0;
				tieOn = false;

				for (int32_t slot = 0; slot < 4; ++slot)
				{
					modShadow[slot] = -1;
					modWire[slot] = -1;
				}

				// Track-scoped VM state resets at every track init on hardware
				// (SeqTrack::InitParam): the 16 track-local vars to the init
				// policy, their written flags, and cmpFlag to true. Player and
				// global vars (0-31) and their written flags persist across the
				// walk. The conditional-loop bookkeeping is per-track too.
				for (int32_t v = 32; v < 48; ++v)
				{
					vars[v] = kVarInit;
					varWritten[v] = false;
				}

				cmpFlag = true;
				offsetVersion.clear();
				condRetakes.clear();
				trackExecs = 0;
				gatedExits.clear();
				gatedExitUsed.clear();
				lastCmpGameDriven = false;

				// The engine keeps the 0x8A/0xFD call stack per track (NW4R
				// callStack[]/callStackDepth). A track that ends inside a Call
				// (via Fin, a whole-song loop-back, or a stray Return) leaves
				// its return frame behind; if that frame carried into the next
				// track, that track's first unbalanced Return would jump back
				// into this code and replay it under the wrong index/channel.
				sp = {};

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
		offsetVersion[i->first] = vmVersion;

		// Position of the command now being emitted, for any dropped-event notice.
		uint8_t* here = Data + dataOffset + 8 + i->first;

		// MIDI channel for this track's channel-voice messages (see channelOf).
		// The SMF track number stays == track, so only the channel nibble moves.
		int chan = channelOf[track];

		// Labels are positional annotations on the stream, so they emit even for
		// a command the [If] gate below skips.
		if (!i->second.Label.empty())
		{
			smfInsertMetaText(smf, absTime, track, SMF_META_TEXT, i->second.Label.c_str());
		}

		// The per-track execution backstop. Conditional jumps are bounded by
		// offsetTime/condRetakes and unconditional backward jumps end the track,
		// but a Call cycle (a subroutine that reaches itself) has no such guard
		// and recursed forever before this check. Ending the track is the same
		// recovery every other "this track is done here" case uses.
		if (++trackExecs > kTrackExecBudget)
		{
			Ctx.Warning(here, "track exceeded the execution budget (likely a call cycle); ending track",
				"track execution budget exceeded (track ended)");

			if (!advanceToNextTrack())
			{
				break;
			}

			redirected = false;
			continue;
		}

		// The [If] gate. cmpFlag starts true per track and is written only by the
		// six comparison ops, so an [If]-free stream runs exactly as before; a
		// false flag skips the command outright -- no argument-as-value, no
		// notice, no latch, no emission, no time advance, no redirect -- the
		// engine's own doExecCommand = cmpFlag dispatch. DISASM-CONFIRMED against
		// the actual 3DS binary (MiiPlaza code.bin, Parse @0x2E3B80): EVERY
		// command is gated, INCLUDING Fin (0xFF gate @0x2E4000) and Return (0xFD
		// tail @0x2E403C), so [If] Fin / [If] Return with a false flag keep the
		// track playing -- real conditional-truncation behaviour. This
		// CONTRADICTS the NW4R/Wii decomp, where MML_FIN returns FINISH outside
		// the gate; the CTR port moved it inside. The lone exception is 0xFE
		// alloc-track, which CTR Parse consumes (the u16 mask) regardless of the
		// flag; caesar's latch is inert either way, so it stays ungated to mirror
		// that byte-consumption semantics.
		bool allocTrack = !i->second.Extended && (i->second.Cmd == 0xFE);

		if ((i->second.Suffix3 == SuffixType::If) && !cmpFlag && !allocTrack)
		{
			// Condition false: skip. A gated command never redirects, so the walk
			// just steps to the next command. A skipped plain jump is remembered
			// as a potential loop exit (see gatedExits): if an unconditional
			// backward jump later closes a loop over this offset, the gated exit
			// is where hardware would eventually leave a re-roll loop. Gated
			// calls are deliberately NOT recorded -- no corpus loop exits through
			// a call, and replaying one would need a return frame the loop never
			// pushed.
			if (!i->second.Extended && (i->second.Cmd == 0x89) && !i->second.Args.empty())
			{
				gatedExits[i->first] = { static_cast<uint32_t>(i->second.Args[0]), lastCmpGameDriven };
			}

			++i;
			continue;
		}

		// Execution-time argument resolution and the VM's read/write helpers. A
		// Var-prefixed slot holds a variable INDEX resolved against vars[] at this
		// instant (the whole point of the VM); a never-written slot surfaces the
		// init-0 default; an out-of-range index is the engine's GetVariablePtr ==
		// NULL no-op, mirrored here as a deterministic command drop.
		bool dropCommand = false;

		auto readVar = [&](int32_t idx) -> int32_t
		{
			// A read of a slot the sequence never wrote is where the init-0 policy
			// does its work (on hardware the game seeds such slots externally), so
			// surface it. Arithmetic ops that read-modify-write their own target do
			// NOT come through here; the honest read sites are Var arguments and a
			// comparison's target, per the VM design.
			if (!varWritten[idx])
			{
				char msg[80];
				snprintf(msg, sizeof(msg),
					"variable %d read before any write; converter default %d stands in", idx, kVarInit);
				Ctx.Warning(here, msg,
					"variables read before any write (converter init-0 default)");
			}

			return vars[idx];
		};

		auto resolveArg = [&](int32_t slot) -> int32_t
		{
			int32_t raw = i->second.Args[slot];

			if (i->second.Suffix1 == SuffixType::Var)
			{
				if ((raw < 0) || (raw >= 48))
				{
					Ctx.Warning(here, "variable index out of range; command dropped",
						"variable index out of range (command dropped)");
					dropCommand = true;

					return 0;
				}

				return readVar(raw);
			}

			if (i->second.Suffix1 == SuffixType::Rnd)
			{
				Ctx.Warning(here, "Rnd argument approximated by its range midpoint",
					"Rnd-valued arguments approximated (range midpoint)");

				// The exporter's midpoint stand-in, decided here at emit rather
				// than welded into the parsed model: the same (lo + hi) / 2 (C++
				// truncation toward zero) over the raw bounds ReadArgs retained in
				// Arg1Rnd. Symmetric, so the unsorted file order is irrelevant;
				// raw (Args[slot]) is only the first bound placeholder here.
				return (i->second.Arg1Rnd.first + i->second.Arg1Rnd.second) / 2;
			}

			return raw;
		};

		auto writeVar = [&](int32_t idx, int32_t value)
		{
			int16_t v = static_cast<int16_t>(value);   // s16 storage wraps at 16 bits

			if ((vars[idx] != v) || !varWritten[idx])
			{
				vars[idx] = v;
				varWritten[idx] = true;
				++vmVersion;
			}
		};

		// The command's Suffix1-governed primary argument, resolved once. Notes
		// carry velocity in Args[0] and the resolvable length in Args[1]; the
		// other value-carrying commands (0x80 rest, 0x81 program, the 0xB0-0xE4
		// parameters) use Args[0]. Structural commands (OpenTrack/Jump/Call/
		// AllocTrack), the no-argument commands, and the extended ops have no
		// primary slot here -- the extended ops resolve their own operand inside
		// their handler.
		int32_t primarySlot = -1;

		if (!i->second.Extended)
		{
			uint8_t c = i->second.Cmd;

			if (c < 0x80)                        { primarySlot = 1; }
			else if ((c == 0x80) || (c == 0x81)) { primarySlot = 0; }
			else if ((c >= 0xB0) && (c <= 0xE4)) { primarySlot = 0; }
		}

		int32_t arg = 0;

		if ((primarySlot >= 0) && (primarySlot < static_cast<int32_t>(i->second.Args.size())))
		{
			arg = resolveArg(primarySlot);
		}

		if (dropCommand)
		{
			++i;
			continue;
		}

		// A _t ramp commands the engine to glide from the parameter's current
		// value to the target over the trailing duration; caesar emits the
		// target at the command tick instead (375k volume fades corpus-wide,
		// previously silent). Real interpolation is stage-2/5 flattening work.
		if (i->second.Suffix2 != SuffixType::None)
		{
			Ctx.Warning(here, "ramped (_t) change flattened to an instant jump at the command tick",
				"ramped (_t) changes flattened to instant jumps");
		}

		if (!i->second.Extended)
		{
			if (i->second.Cmd < 0x80)
			{
				// Resolved note length (Args[1] may be a Var read; arg holds it). A
				// negative s16 length is meaningless as a duration -- treat it as a
				// zero-length note (immediate) and surface it. Velocity (Args[0])
				// stays a plain byte, never resolved.
				int32_t len = arg;

				if (len < 0)
				{
					Ctx.Warning(here, "negative note length from variable; treated as 0",
						"negative note length/rest from variable (treated as 0)");

					len = 0;
				}

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

				if (tieOn)
				{
					// One continuous voice: a key/velocity change closes the
					// current segment (its audio ran to this tick) and opens
					// the next; re-commanding the identical key and velocity
					// changes nothing on hardware, so the segment just
					// continues. The length argument plays no audio role here
					// (the note-wait advance below still uses it, as the
					// engine does).
					if (!tieHeld || (i->second.Cmd != tieKey) || (i->second.Args[0] != tieVel))
					{
						finalizeTie(absTime);

						tieHeld = true;
						tieStart = absTime;
						tieCmdOffset = i->first;
						tieKey = i->second.Cmd;
						tieVel = i->second.Args[0];
					}
				}
				// key is Cmd (< 0x80, always valid); a note is only rejected on its
				// velocity. Velocity 0 is a legitimate silent note (the writer skips
				// it and timing below still advances), so surface a drop only when
				// the velocity is genuinely out of MIDI range, not merely zero.
				else if (!smfInsertNote(smf, absTime, chan, track, i->second.Cmd, i->second.Args[0], len)
					&& i->second.Args[0] > 127)
				{
					Ctx.Warning(here, "note velocity out of MIDI range; note dropped",
						"MIDI notes dropped (velocity out of range)");
				}

				if (noteWait)
				{
					absTime += len;
				}
			}
			else if (i->second.Cmd == 0x80)
			{
				// Rest: advance track time by the resolved wait (Args[0] may be a
				// Var read). A negative s16 wait is meaningless -- treat it as 0.
				int32_t wait = arg;

				if (wait < 0)
				{
					Ctx.Warning(here, "negative rest duration from variable; treated as 0",
						"negative note length/rest from variable (treated as 0)");

					wait = 0;
				}

				absTime += wait;
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
				// The convert-time VM can make the resolved value negative (a Var/Rnd
				// s16); a negative value keeps its sign through /128 and %128, so the
				// emit* wrappers still surface anything the writer rejects.
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BANKSELM, (arg / 128) % 128), here);
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BANKSELL, 0), here);
				emitProgram(Ctx, smfInsertProgram(smf, absTime, chan, track, arg % 128), here);
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
					Ctx.Warning(here, "OpenTrack index out of range (>= 16); track ignored",
						"OpenTrack index out of range");
				}
			}
			else if (i->second.Cmd == 0x89)
			{
				uint32_t target = i->second.Args[0];

				if (i->second.Suffix3 == SuffixType::If)
				{
					// Conditional jump. The [If] gate at the top of the loop already
					// dropped this command when cmpFlag was false, so reaching here
					// means the condition evaluated TRUE and the jump is taken --
					// subject only to the loop-revisit rule below. This replaces the
					// old two-reachability dispatcher heuristic, which guessed a
					// note-reaching branch because the tested variable was unmodelled;
					// the VM now resolves the condition exactly, so there is nothing
					// to guess and no marker to place.
					auto n = commands.find(target);

					if (n == commands.end())
					{
						Ctx.Warning(here, "jump target out of range; jump ignored",
							"jump targets out of range (jump ignored)");
					}
					else if (!offsetTime.count(target))
					{
						// Not yet executed this track (a forward branch, or a first
						// entry into shared code): take it, like the unconditional
						// forward case below.
						i = n;
						redirected = true;
					}
					else
					{
						// The target already ran this track: a conditional backward
						// jump, i.e. a loop only the VM can bound. A counted loop
						// rewrites its counter each pass, so VM state has moved since
						// the target last ran (vmVersion != the offsetVersion stamped
						// there) and it unrolls until its comparison flips cmpFlag and
						// the gate falls through naturally; a spin-wait polls a
						// game-driven variable convert-time state can never change, so
						// its version never moves and the body plays exactly once.
						// Refuse the retake when state is unchanged or a budget is hit,
						// and fall through. No loop markers on a conditional loop --
						// those stay exclusive to the unconditional whole-song path.
						bool stateChanged = (vmVersion != offsetVersion[target]);
						bool budgetOk = (condRetakes[i->first] < kCondRetakeBudget) && (trackExecs < kTrackExecBudget);

						if (stateChanged && budgetOk)
						{
							++condRetakes[i->first];
							i = n;
							redirected = true;
						}
						else if (!stateChanged)
						{
							Ctx.Warning(here, "conditional backward jump with unchanged state; loop not repeated (spin-wait broken out)",
								"conditional loops broken (unchanged state / spin-wait)");
						}
						else
						{
							Ctx.Warning(here, "conditional loop exceeded the revisit budget; loop broken",
								"conditional loops broken (revisit budget exhausted)");
						}
					}
				}
				else if (offsetTime.count(target))
				{
					// The target has already played in this track, so following the
					// jump would replay it forever. Before calling it the whole-song
					// loop, check whether the span it closes contains an [If]-jump
					// the gate turned off whose comparison read only SEQUENCE-written
					// state: that is a self-contained RNG re-roll loop (randvar, cmp,
					// [If]-exit to the play block, jump back), and on hardware it
					// ALWAYS eventually leaves through that exit -- the sequence
					// re-rolls its own variable until the comparison clears. The
					// PRNG-free midpoint can never clear it, so take the exit once
					// instead of ending the track silent (Pokemon niji_sound: 50
					// ambient-SFX files, 403 notes, all guaranteed audible on
					// console). Taken at most once per exit: on a later pass the
					// loop really is "repeat from the file's viewpoint" and falls
					// through to the marker path below. A gated exit whose
					// comparison read a game-seeded (never-written) variable is NOT
					// an escape -- that loop is a game-driven spin-wait (Animal
					// Crossing's dispatcher SEs), and its honest at-rest rendering
					// is silence until the game acts.
					uint32_t rescueTarget = 0;
					bool rescued = false;

					for (auto g = gatedExits.lower_bound(target);
						(g != gatedExits.end()) && (g->first <= i->first); ++g)
					{
						if (!g->second.second && !gatedExitUsed.count(g->first)
							&& commands.count(g->second.first))
						{
							gatedExitUsed.insert(g->first);
							rescueTarget = g->second.first;
							rescued = true;
							break;
						}
					}

					if (rescued)
					{
						Ctx.Warning(here, "re-roll loop exit taken once (sequence-rolled condition always clears on hardware)",
							"re-roll loop exits taken once (PRNG-free stand-in never clears them)");

						i = commands.find(rescueTarget);
						redirected = true;
					}
					else
					{
						// The format's whole-song loop. A MIDI file cannot loop on
						// its own, so mark the loop span (target..here) with
						// "loopStart"/"loopEnd" and end the track instead of
						// following the jump.
						smfInsertMetaText(smf, offsetTime[target], track, SMF_META_MARKER, "loopStart");
						smfInsertMetaText(smf, absTime, track, SMF_META_MARKER, "loopEnd");

						if (!advanceToNextTrack())
						{
							break;
						}
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
						Ctx.Warning(here, "jump target out of range; jump ignored",
							"jump targets out of range (jump ignored)");
					}
				}
			}
			else if (i->second.Cmd == 0x8A)
			{
				// Call: resolve the target first. A target that is not a command
				// boundary (malformed) must not silently stop the whole walk -- the
				// while loop would exit and skip every remaining track -- so report
				// it and fall through, mirroring the 0x89 jump guard. When the
				// target is valid but next(i, 1) is end() (the Call is the last
				// command, so there is no return address), jump WITHOUT pushing: a
				// later Return then takes the honest empty-stack end-of-track path
				// rather than dereferencing end().
				auto target = commands.find(i->second.Args[0]);

				if (target == commands.end())
				{
					Ctx.Warning(here, "call target out of range; call ignored",
						"call targets out of range (call ignored)");
				}
				else
				{
					auto ret = next(i, 1);

					if (ret != commands.end())
					{
						sp.push(ret->first);
					}

					i = target;
					redirected = true;
				}
			}
			else if (i->second.Cmd == 0xB0)
			{
				smfSetTimebase(smf, arg);
			}
			else if (i->second.Cmd == 0xB1)
			{
				// NW4C ADSHR hold-stage override; no GM/GS controller exists for it.
				Ctx.Warning(here, "envelope hold has no MIDI equivalent; dropped",
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
				Ctx.Warning(here, "mono/poly is a voice-allocation flag with no MIDI equivalent; dropped",
					"mono/poly dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB3)
			{
				Ctx.Warning(here, "velocity range not implemented; dropped",
					"velocity range dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xB4)
			{
				// Voice biquad response select; no audible MIDI target.
				Ctx.Warning(here, "biquad type has no MIDI equivalent; dropped",
					"biquad filter dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB5)
			{
				Ctx.Warning(here, "biquad value has no MIDI equivalent; dropped",
					"biquad filter dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xB6)
			{
				// A real fidelity gap (wrong instrument wherever a track switches
				// banks mid-sequence), but the emitted CC0 must be co-designed with
				// Cbnk's SF2 bank layout -- see the roadmap item.
				Ctx.Warning(here, "mid-sequence bank select not implemented; instrument may be wrong",
					"bank select dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xBD)
			{
				Ctx.Warning(here, "mod phase has no MIDI equivalent; dropped",
					"LFO phase/curve dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xBE)
			{
				Ctx.Warning(here, "mod curve has no MIDI equivalent; dropped",
					"LFO phase/curve dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xBF)
			{
				// Surround-path routing flag (bypass the front virtualization);
				// only meaningful under the console's Surround output mode, and
				// MIDI has no surround axis at all.
				Ctx.Warning(here, "front bypass is surround routing with no MIDI equivalent; dropped",
					"front bypass dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xC0)
			{
				// Fold in whatever init pan (0xDC) the track is carrying. With none --
				// every sequence that does not use 0xDC -- this is the pan value itself,
				// so those files are untouched. The value is now always resolved (a plain
				// byte, a Var read or the Rnd midpoint), so the latch-and-combine runs
				// unconditionally rather than writing an unevaluated stand-in raw.
				int32_t pan = clampCtrl(Ctx, arg, here);

				trackPan = pan;
				pan = combinePan(trackPan, trackInitPan);

				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PANPOT, pan), here);
			}
			else if (i->second.Cmd == 0xC1)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VOLUME, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xC2)
			{
				emitCtrl(Ctx, smfInsertMasterVolume(smf, absTime, 0, track, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xC3)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNM, 0);
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNL, 2);
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DATAENTRYM, arg + 64), here);
			}
			else if (i->second.Cmd == 0xC4)
			{
				emitCtrl(Ctx, smfInsertPitchBend(smf, absTime, chan, track, arg * 64), here);
			}
			else if (i->second.Cmd == 0xC5)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNM, 0);
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RPNL, 0);
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DATAENTRYM, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xC6)
			{
				// Voice-steal priority: engine scheduling state with no meaning in
				// MIDI (the future player must preserve it; nothing is lost here).
				Ctx.Warning(here, "voice priority has no MIDI equivalent; dropped",
					"priority dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xC7)
			{
				// Note-wait latches the resolved flag (the VM retires the old known
				// bug where a Var/Rnd stand-in latched a bogus persistent flag).
				noteWait = (arg != 0);
			}
			else if (i->second.Cmd == 0xC8)
			{
				// Tie on/off. Both edges release the sounding voice on hardware
				// (MML_SET_TIE releases and frees the track's channels before
				// setting the flag), so either edge closes an open segment here.
				// The flag is now the resolved value (plain, Var read or Rnd
				// midpoint), so it always latches -- no stand-in to drop.
				finalizeTie(absTime);

				tieOn = (arg != 0);

				if (tieOn)
				{
					Ctx.Warning(here, "tie region approximated as back-to-back segments (single-envelope legato not expressible in MIDI)",
						"tie regions approximated (segments re-attack at pitch changes)");
				}
			}
			else if (i->second.Cmd == 0xC9)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTOCTRL, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xCA)
			{
				// Clamp once so the latched shadow and the emitted CC1 carry the same
				// value: the 0xCC restore path replays modShadow[0], so an unclamped
				// >127 there would drop on the replay even after clamping now. The
				// depth is always resolved now, so the shadow always latches.
				int32_t depth = clampCtrl(Ctx, arg, here);

				modShadow[0] = depth;

				if (trackModType == 0)
				{
					if (emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_MODULATION, depth), here))
					{
						modWire[0] = depth;
					}
				}
				else
				{
					Ctx.Warning(here, "mod depth while the track LFO targets volume/pan; CC1 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xCB)
			{
				int32_t rate = (arg / 2) + 64;

				modShadow[1] = rate;

				if (trackModType == 0)
				{
					if (emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATORATE, rate), here))
					{
						modWire[1] = rate;
					}
				}
				else
				{
					Ctx.Warning(here, "mod speed while the track LFO targets volume/pan; CC76 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xCC)
			{
				// Track LFO target: 0 = pitch, 1 = volume (tremolo), 2 = pan
				// (auto-pan). A value above 2 gets no LFO at all on hardware
				// (stored unvalidated, routed nowhere), so any non-zero target
				// rightly suppresses the pitch-vibrato CCs here. Retargeting
				// only re-routes the engine's LFO -- its parameters persist -- so in
				// MIDI terms leaving pitch must silence a live CC1 (the SF2 default
				// mod-wheel modulator keeps wobbling pitch otherwise) and returning
				// to pitch must restore whatever the persistent parameters now hold.
				// The target is now always resolved (plain, Var read or Rnd
				// midpoint), so it always tracks; tremolo/auto-pan itself still has
				// no MIDI equivalent.
				int32_t newType = arg;

				if (newType != trackModType)
				{
					trackModType = newType;

					if (newType != 0)
					{
						if (modWire[0] > 0)
						{
							smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_MODULATION, 0);
							modWire[0] = 0;

							if (newType <= 2)
							{
								Ctx.Warning(here, "track LFO retargeted to volume/pan; tremolo/auto-pan not rendered",
									"tremolo/auto-pan LFO dropped (no MIDI equivalent)");
							}
							else
							{
								Ctx.Warning(here, "track LFO target out of range; the engine applies no LFO",
									"mod type out of range (engine applies no LFO)");
							}
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
								if (emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, modCtrl[slot], modShadow[slot]), here))
								{
									modWire[slot] = modShadow[slot];
								}
							}
						}
					}
				}
			}
			else if (i->second.Cmd == 0xCD)
			{
				int32_t range = (arg / 2) + 64;

				modShadow[2] = range;

				if (trackModType == 0)
				{
					if (emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODEPTH, range), here))
					{
						modWire[2] = range;
					}
				}
				else
				{
					Ctx.Warning(here, "mod range while the track LFO targets volume/pan; CC77 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xCE)
			{
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTO, arg ? 127 : 0);
			}
			else if (i->second.Cmd == 0xCF)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PORTAMENTOTIME, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xD0)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_ATTACKTIME, (arg / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD1)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_DECAYTIME, (arg / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD2)
			{
				// ADSR sustain LEVEL (not the pedal -- that is 0xDF); no GM2/GS
				// controller exists for it.
				Ctx.Warning(here, "envelope sustain level has no MIDI equivalent; dropped",
					"sustain level dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xD3)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_RELEASETIME, (arg / 2) + 64), here);
			}
			else if (i->second.Cmd == 0xD4)
			{
				// 0xD4 = loop start; the resolved value is the repeat count. Emit it
				// as the EMIDI CC116 (loop-start) value so a loop-aware player repeats
				// the section that many times instead of forever. The CTR and EMIDI
				// counts line up exactly -- both are total-plays with 0 meaning
				// "loop forever" (verified against GotaSequenceLib's CtrCafe
				// playback and the Apogee EMIDI v1.1 spec) -- so a literal count
				// passes straight through, 0 included. A count above the 7-bit CC
				// range clamps to 127 (still finite/"many") rather than being
				// dropped by the writer, which would lose the loop marker outright;
				// a resolved NEGATIVE count (only reachable via a Var/Rnd s16)
				// clamps to 0, i.e. loop forever.
				int32_t count = arg;

				if (count > 127)
				{
					Ctx.Warning(here, "loop repeat count above MIDI range; clamped to 127",
						"MIDI loop repeat counts clamped to 127 (above range)");

					count = 127;
				}
				else if (count < 0)
				{
					Ctx.Warning(here, "loop repeat count below MIDI range; clamped to 0 (loop forever)",
						"MIDI loop repeat counts clamped to 0 (below range)");

					count = 0;
				}

				smfInsertControl(smf, absTime, chan, track, 116, count);
			}
			else if (i->second.Cmd == 0xD5)
			{
				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_EXPRESSION, clampCtrl(Ctx, arg, here)), here);
			}
			else if (i->second.Cmd == 0xD6)
			{
				// A debug print of a sequence variable; nothing to render.
				Ctx.Warning(here, "print var is a debug command with no MIDI equivalent; dropped",
					"print var dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xD7)
			{
				// SurroundPan: the front/rear axis of the DSP's quad voice-gain
				// matrix. Audible on console only under the System Settings
				// "Surround" output mode (console-confirmed 2026-07-11); MIDI has
				// no surround axis in any mode, so there is nothing to map it to.
				// The future player must model it -- see the suite plan.
				Ctx.Warning(here, "span (front/rear surround pan) has no MIDI equivalent; dropped",
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
				// carry. The resolved value drives the same clamp (a negative Var/Rnd
				// value lands at the fully-closed 0).
				int32_t cutoff = clamp(arg, 0, 64);

				// Only an actual cut carries the ~20% curve error; landing on the
				// neutral 64 is a no-op that needs no notice.
				if (cutoff != 64)
				{
					Ctx.Warning(here, "lpf cutoff approximated (CC74 cuts ~20% shallower than hardware)",
						"lpf cutoff approximated (CC74 curve reads ~20% shallow)");
				}

				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_BRIGHTNESS, cutoff), here);
			}
			else if (i->second.Cmd == 0xD9)
			{
				// FX send A -> reverb depth (CC91). The aux-send level is a 7-bit
				// value in these sequences (observed 0-120 across MeetSound);
				// clamp defensively so an unexpected value can't push the control
				// out of MIDI range and get silently dropped by the writer.
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_REVERB, clamp(arg, 0, 127));
			}
			else if (i->second.Cmd == 0xDA)
			{
				// FX send B -> chorus depth (CC93), same 0-127 convention.
				smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_CHORUS, clamp(arg, 0, 127));
			}
			else if (i->second.Cmd == 0xDB)
			{
				Ctx.Warning(here, "main (dry) send not implemented; dropped",
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
				// The init pan is now always the resolved value (plain, Var read or
				// Rnd midpoint), so it always latches and re-emits the combined pan.
				trackInitPan = clampCtrl(Ctx, arg, here);

				emitCtrl(Ctx, smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_PANPOT, combinePan(trackPan, trackInitPan)), here);
			}
			else if (i->second.Cmd == 0xDD)
			{
				Ctx.Warning(here, "track mute not implemented; notes keep sounding",
					"mute dropped (not implemented)");
			}
			else if (i->second.Cmd == 0xDE)
			{
				// FX send C: the third aux bus. A real CTR command (zero corpus
				// occurrences) that used to fall off the end of this chain with no
				// trace at all. Buses A/B map to CC91/93; GM has no third effects
				// send, so C drops -- but now visibly.
				Ctx.Warning(here, "fx send C (third aux bus) has no MIDI equivalent; dropped",
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
				smfInsertControl(smf, absTime, chan, track, 64, (arg >= 64) ? 127 : 0);
			}
			else if (i->second.Cmd == 0xE0)
			{
				// Mod delay: time from note-on before the track LFO engages,
				// in 5 ms units (NW4R reads it as lfoParam.delay = arg * 5 ms;
				// NW4C is its port). CC78 "vibrato delay" is relative to the
				// patch default (64 = no change), and these SF2s program no
				// LFO delay, so 64 is the 0 ms baseline: scale the delay into
				// the upper half, saturating at 1000 ms (corpus p99 = 500 ms,
				// max = 1150 ms). The old (x/2)+64 treated the time as a
				// signed +/-64 parameter and pushed delays >= 640 ms out of
				// MIDI range entirely. The value is now always resolved, so it
				// always runs the 5 ms-units path (a negative Var/Rnd value floors
				// at the 0 ms baseline via max).
				int32_t ms = max(arg, 0) * 5;
				int32_t delay = 64 + min(ms * 63 / 1000, 63);

				// CC78's upper half tops out at 1000 ms, but the corpus reaches
				// 1150 ms; delays past 1 s all flatten to 127, so surface the loss.
				if (ms > 1000)
				{
					Ctx.Warning(here, "mod delay above 1000 ms saturates CC78 (flattened to 127)",
						"mod delay saturated (CC78 caps at 1000 ms)");
				}

				modShadow[3] = delay;

				if (trackModType == 0)
				{
					smfInsertControl(smf, absTime, chan, track, SMF_CONTROL_VIBRATODELAY, delay);
					modWire[3] = delay;
				}
				else
				{
					Ctx.Warning(here, "mod delay while the track LFO targets volume/pan; CC78 suppressed",
						"pitch-vibrato CCs suppressed (track LFO targets volume/pan)");
				}
			}
			else if (i->second.Cmd == 0xE1)
			{
				// The resolved tempo can still be <= 0 (a signed-16-bit literal, or a
				// negative Var/Rnd value). bpm == 0 makes libsmfcx's 60000000 / bpm
				// infinite and the int cast of that is UB; guard caller-side to keep
				// the vendored copy pristine. The drop still surfaces through
				// emitCtrl's notice.
				emitCtrl(Ctx, (arg > 0) && smfInsertTempoBPM(smf, absTime, track, arg), here);
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
				Ctx.Warning(here, "sweep pitch (intra-note pitch ramp) has no MIDI equivalent; dropped",
					"sweep pitch dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xE4)
			{
				Ctx.Warning(here, "mod period has no MIDI equivalent; dropped",
					"LFO period dropped (no MIDI equivalent)");
			}
			else if (i->second.Cmd == 0xFB)
			{
				Ctx.Warning(here, "envelope reset not implemented; dropped",
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
					// sequence entries into one shared bank, and the converter
					// starts each entry at its own start offset -- which often
					// lands on a helper subroutine that ends in Return, so an
					// entry can reach a Return it never Called. Rather than
					// discard the whole in-progress MIDI, treat the stray Return
					// as end-of-track, exactly like Fin: close this track and
					// move to the next so the sequence still yields a file.
					Ctx.Warning(Data + dataOffset + 8 + i->first, "Return with empty call stack; ending track");

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
				Ctx.Warning(here, msg, "unhandled sequence commands dropped");
			}
		}
		else
		{
			// Extended (0xF0-prefixed) command space. The variable and comparison
			// ops (0x80-0x95) now EXECUTE in the convert-time VM; the mod2-4
			// multi-LFO family (0xA0-0xB1, 0xE1-0xE6; zero corpus occurrences) and
			// userproc (0xE0) still drop with a notice, served by the name table
			// below (in CtrCafe byte order) alongside the unknown catch-all.
			uint8_t ext = i->second.Cmd;

			if ((ext >= 0x80) && (ext <= 0x8B))
			{
				// The 12 arithmetic ops (RESEARCH-CONFIRMED NW4R
				// MmlParser::CommandProc). Args[0] is the target variable index (a
				// plain u8, never a Var read); Args[1] is the operand, itself
				// resolvable via a Var/Rnd prefix. Storage is s16 and wraps at 16
				// bits (writeVar). div/mod guard ÷0 (variable left unchanged, as the
				// engine does), notvar complements the OPERAND (not the variable),
				// and randvar stands in the operand midpoint (the VM is PRNG-free).
				int32_t idx = i->second.Args[0];

				if ((idx < 0) || (idx >= 48))
				{
					Ctx.Warning(here, "variable index out of range; command dropped",
						"variable index out of range (command dropped)");
				}
				else
				{
					int32_t op = resolveArg(1);

					if (!dropCommand)
					{
						switch (ext)
						{
							case 0x80: writeVar(idx, op); break;               // setvar
							case 0x81: writeVar(idx, vars[idx] + op); break;   // addvar
							case 0x82: writeVar(idx, vars[idx] - op); break;   // subvar
							case 0x83: writeVar(idx, vars[idx] * op); break;   // mulvar
							case 0x84:                                         // divvar
								if (op != 0)
								{
									writeVar(idx, vars[idx] / op);
								}
								else
								{
									Ctx.Warning(here, "divvar by zero; variable left unchanged",
										"variable divide/modulo by zero (skipped)");
								}
								break;
							case 0x85:                                         // shiftvar
							{
								// op >= 0 left-shifts, op < 0 arithmetic-right-shifts
								// (RESEARCH-CONFIRMED sign convention). The count is
								// clamped to the 16-bit width so the C++ shift stays
								// defined; that matches the ARM register-shift (count
								// truncated to its low byte) for every count below
								// 256, i.e. everything short of authoring nonsense --
								// past that the reference C is itself UB, so there is
								// no exact value to chase.
								int32_t v = vars[idx];

								if (op >= 0)
								{
									v = (op >= 16) ? 0 : static_cast<int32_t>(static_cast<uint32_t>(v) << op);
								}
								else
								{
									int32_t rs = -op;
									v = v >> ((rs >= 16) ? 15 : rs);
								}

								writeVar(idx, v);
								break;
							}
							case 0x86:                                         // randvar
								Ctx.Warning(here, "randvar approximated by the operand midpoint (VM is PRNG-free)",
									"randvar approximated (operand midpoint)");
								writeVar(idx, op / 2);
								break;
							case 0x87: writeVar(idx, vars[idx] & op); break;   // andvar
							case 0x88: writeVar(idx, vars[idx] | op); break;   // orvar
							case 0x89: writeVar(idx, vars[idx] ^ op); break;   // xorvar
							case 0x8A:                                         // notvar
								writeVar(idx, ~static_cast<uint16_t>(op));
								break;
							case 0x8B:                                         // modvar
								if (op != 0)
								{
									writeVar(idx, vars[idx] % op);
								}
								else
								{
									Ctx.Warning(here, "modvar by zero; variable left unchanged",
										"variable divide/modulo by zero (skipped)");
								}
								break;
						}
					}
				}
			}
			else if ((ext >= 0x90) && (ext <= 0x95))
			{
				// The 6 comparisons write the per-track cmpFlag the [If] gate reads
				// (RESEARCH-CONFIRMED order eq/ge/gt/le/lt/ne; signed s16 compare).
				// cmpFlag persists until the next comparison -- one compare gates
				// many [If]s -- and only a real flip bumps vmVersion.
				int32_t idx = i->second.Args[0];

				if ((idx < 0) || (idx >= 48))
				{
					Ctx.Warning(here, "variable index out of range; command dropped",
						"variable index out of range (command dropped)");
				}
				else
				{
					// Whether this comparison depends on game-seeded state: any
					// never-written participant (the target var, or a Var-prefixed
					// operand) means the result hinges on a variable only the game
					// writes at runtime. An [If]-jump this comparison gates off
					// inherits the tag (see gatedExits) so the loop-escape rule can
					// tell a sequence-internal RNG wait from a game-driven one.
					bool opVarUnwritten = (i->second.Suffix1 == SuffixType::Var)
						&& (i->second.Args[1] >= 0) && (i->second.Args[1] < 48)
						&& !varWritten[i->second.Args[1]];

					int32_t op = resolveArg(1);

					if (!dropCommand)
					{
						lastCmpGameDriven = !varWritten[idx] || opVarUnwritten;

						int32_t lhs = readVar(idx);
						bool result = cmpFlag;

						switch (ext)
						{
							case 0x90: result = (lhs == op); break;
							case 0x91: result = (lhs >= op); break;
							case 0x92: result = (lhs >  op); break;
							case 0x93: result = (lhs <= op); break;
							case 0x94: result = (lhs <  op); break;
							case 0x95: result = (lhs != op); break;
						}

						if (result != cmpFlag)
						{
							cmpFlag = result;
							++vmVersion;
						}
					}
				}
			}
			else
			{
				// Still-dropped families: mod2-4 multi-LFO (0xA0-0xB1, 0xE1-0xE6;
				// zero corpus occurrences), userproc (0xE0), and the unknown
				// catch-all. One name table in CtrCafe byte order -- the old chain's
				// mod4 labels (0xAC-0xB1) were scrambled against the map, which never
				// showed because the chain was dead code (see the parse fix that
				// records the extended opcode in Cmd).
				static const map<uint8_t, const char*> extendedNames =
				{
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

				auto name = extendedNames.find(ext);
				char msg[64];

				if (name == extendedNames.end())
				{
					// Parse vets extended bytes, so this is the same safety net as the
					// plain chain's final else: parsed but never wired up.
					snprintf(msg, sizeof(msg), "unhandled extended command 0x%02X; dropped", ext);
					Ctx.Warning(here, msg, "unhandled sequence commands dropped");
				}
				else
				{
					const char* category = (ext == 0xE0)
						? "userproc dropped (no MIDI equivalent)"
						: "multi-LFO (mod2-4) commands dropped (not implemented)";

					snprintf(msg, sizeof(msg), "%s not implemented; dropped", name->second);
					Ctx.Warning(here, msg, category);
				}
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

	// A track that runs off the end of the bank without a Fin exits the walk
	// here; every other exit closes the tie segment inside advanceToNextTrack.
	finalizeTie(absTime);

	if (smf->timebase == 0)
	{
		smfSetTimebase(smf, 48);
	}

	smfWriteFile(smf, FileName.substr(0, FileName.length() - 5).append("mid").c_str());
	smfDelete(smf);

	return true;
}
