#include "CaesarPlay.hpp"

#include "Cseq.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

using namespace std;

namespace play
{
	namespace
	{
		// Player defaults. A sequence that never issues 0xE1/0xB0 plays at the MIDI
		// convention (120 BPM) on the converter's default timebase (48), which is
		// exactly what Cseq::Export falls back to (smfSetTimebase(smf, 48)).
		constexpr double kDefaultTempoBpm = 120.0;
		constexpr uint32_t kDefaultTimebase = 48;

		// Per-track execution backstop, per tick (mirrors Cseq::Export's
		// kTrackExecBudget intent): a Call cycle or an all-zero-length spin would
		// otherwise never yield. When exceeded the track ends.
		constexpr uint32_t kTrackExecBudget = 1u << 20;
		constexpr uint32_t kCondRetakeBudget = 1024;

		// One scheduled note: everything the DSP needs, resolved later per voice.
		// envOverride carries the track's 0xB1/0xD0-0xD3 ADSHR overrides as they stood
		// at note-on (-1 = use the note's own byte). Order: attack, hold, decay,
		// sustain, release. Applied in Phase B on top of the resolved note bytes.
		struct NoteEvent
		{
			uint32_t startSample;
			uint32_t gateSamples;
			uint32_t program;
			int key;
			int velocity;
			int envOverride[5] = { -1, -1, -1, -1, -1 };

			// The 24-voice pool inputs (C5), captured at note-on. priority is
			// playerPriority(64) + the track's 0xC6 priority; the track index and mono
			// flag drive mono re-trigger. Released voices drop to priority 1 in the sim.
			int priority = 64;
			int track = 0;
			bool mono = false;

			// The C6 native mix params, latched at note-on. volAmp is the product of
			// the track/master/expression volume amplitudes; panOffset is the track
			// 0xC0 + 0xDC pan as offsets from centre; pitchSemitones is bend + transpose.
			float volAmp = 1.0f;
			int panOffset = 0;
			double pitchSemitones = 0.0;
		};

		// A 48-slot variable file with the converter's scoping: 0-31 are shared
		// across the (single) player (player-local 0-15 + global 16-31), 32-47 are
		// per track. cmpFlag/track vars live on TrackState; the shared half lives on
		// the Runtime.
		struct TrackState
		{
			bool active = false;
			bool ranAtAll = false;
			map<uint32_t, CseqCmd>::const_iterator cursor;

			uint32_t waitTicks = 0;
			bool noteWait = true;
			uint32_t program = 0;

			// Per-track ADSHR envelope overrides (0xB1 hold, 0xD0 attack, 0xD1 decay,
			// 0xD2 sustain, 0xD3 release; 0xFB resets all). -1 = no override (use the
			// note's own byte). Order: attack, hold, decay, sustain, release. The
			// converter drops these (no MIDI equivalent); the player honours them.
			int envOverride[5] = { -1, -1, -1, -1, -1 };

			// Voice-allocation state (C5). trackPriority: the track's 0xC6 priority
			// (added to the player's 64). mono: 0xB2 monophonic flag (a new note
			// re-triggers the track's single voice). Both drop the converter on the
			// floor; the player uses them.
			int trackPriority = 0;
			bool mono = false;

			// C6 native mix state. Volumes are 0..127 bytes (127 = unity), pans are
			// 0..127 (64 = centre); bend is the 0xC4 signed value, bendRange the 0xC5
			// semitone span (RPN 0,0; default 2), transpose the 0xC3 coarse tune
			// (RPN 0,2). The converter maps these to CC7/CC11/CC10/pitch-bend; the
			// player folds them straight into per-voice gain / pan / step.
			int trackVolume = 127;   // 0xC1
			int expression = 127;    // 0xD5
			int trackPan = 64;       // 0xC0
			int initPan = 64;        // 0xDC
			int bend = 0;            // 0xC4
			int bendRange = 2;       // 0xC5 (semitones)
			int transpose = 0;       // 0xC3 (semitones)

			bool cmpFlag = true;
			int16_t vars[16] = { 0 };        // slots 32-47 (track-local)
			bool varWritten[16] = { false };

			vector<uint32_t> callStack;

			// Per-track control-flow bookkeeping (cleared implicitly by this track
			// never being reset -- each track runs once). offsetVersion doubles as
			// the "already reached" set for loop detection.
			map<uint32_t, uint64_t> offsetVersion;
			map<uint32_t, uint32_t> condRetakes;
			uint32_t execs = 0;
		};

		struct Runtime
		{
			const map<uint32_t, CseqCmd>& commands;

			double tempoBpm = kDefaultTempoBpm;
			uint32_t timebase = kDefaultTimebase;

			int masterVolume = 127;   // 0xC2, sequence-wide (C6)

			int16_t globalVars[32] = { 0 };      // slots 0-31 (shared)
			bool globalWritten[32] = { false };
			uint64_t vmVersion = 0;

			TrackState tracks[16];

			vector<NoteEvent> events;
			RenderStats* stats = nullptr;
			set<uint32_t> skipped;   // safe-skipped opcodes (for the report)

			explicit Runtime(const map<uint32_t, CseqCmd>& cmds) : commands(cmds) {}

			bool anyActive() const
			{
				for (const TrackState& t : tracks)
				{
					if (t.active)
					{
						return true;
					}
				}

				return false;
			}

			int16_t readVar(int track, int32_t idx)
			{
				if (idx < 32)
				{
					return globalVars[idx];
				}

				return tracks[track].vars[idx - 32];
			}

			bool varWritten(int track, int32_t idx) const
			{
				if (idx < 32)
				{
					return globalWritten[idx];
				}

				return tracks[track].varWritten[idx - 32];
			}

			void writeVar(int track, int32_t idx, int32_t value)
			{
				int16_t v = static_cast<int16_t>(value);

				if (idx < 32)
				{
					if (globalVars[idx] != v || !globalWritten[idx])
					{
						globalVars[idx] = v;
						globalWritten[idx] = true;
						++vmVersion;
					}
				}
				else
				{
					TrackState& t = tracks[track];

					if (t.vars[idx - 32] != v || !t.varWritten[idx - 32])
					{
						t.vars[idx - 32] = v;
						t.varWritten[idx - 32] = true;
						++vmVersion;
					}
				}
			}
		};

		double samplesPerTick(const Runtime& rt)
		{
			double bpm = (rt.tempoBpm > 0.0) ? rt.tempoBpm : kDefaultTempoBpm;
			uint32_t tb = (rt.timebase > 0) ? rt.timebase : kDefaultTimebase;

			// samples/tick = (native samples/sec) / (ticks/sec); ticks/sec = bpm/60 * tb.
			return static_cast<double>(kNativeRate) * 60.0 / (bpm * static_cast<double>(tb));
		}

		// Resolve a command's Suffix1-governed argument the way Cseq::Export does:
		// a Var slot reads the live VM (out-of-range -> drop), a Rnd slot stands in
		// the range midpoint (PRNG-free; stage 4 wires the real LCG), else the raw.
		int32_t resolveArg(Runtime& rt, int track, const CseqCmd& cmd, int slot, bool& drop)
		{
			int32_t raw = cmd.Args[slot];

			if (cmd.Suffix1 == SuffixType::Var)
			{
				if (raw < 0 || raw >= 48)
				{
					drop = true;
					return 0;
				}

				return rt.readVar(track, raw);
			}

			if (cmd.Suffix1 == SuffixType::Rnd)
			{
				return (cmd.Arg1Rnd.first + cmd.Arg1Rnd.second) / 2;
			}

			return raw;
		}

		// The extended (0xF0) variable + comparison ops -- ported verbatim from
		// Cseq::Export's convert-time VM so [If] gates resolve exactly. All instant
		// (no time), so a track never desyncs on them.
		void execExtendedVm(Runtime& rt, int track, const CseqCmd& cmd)
		{
			uint8_t ext = cmd.Cmd;
			TrackState& t = rt.tracks[track];

			if (ext >= 0x80 && ext <= 0x8B)
			{
				int32_t idx = cmd.Args[0];

				if (idx < 0 || idx >= 48)
				{
					return;
				}

				bool drop = false;
				int32_t op = resolveArg(rt, track, cmd, 1, drop);

				if (drop)
				{
					return;
				}

				int32_t cur = rt.readVar(track, idx);

				switch (ext)
				{
					case 0x80: rt.writeVar(track, idx, op); break;
					case 0x81: rt.writeVar(track, idx, cur + op); break;
					case 0x82: rt.writeVar(track, idx, cur - op); break;
					case 0x83: rt.writeVar(track, idx, cur * op); break;
					case 0x84: if (op != 0) { rt.writeVar(track, idx, cur / op); } break;
					case 0x85:
					{
						int32_t v = cur;

						if (op >= 0)
						{
							v = (op >= 16) ? 0 : static_cast<int32_t>(static_cast<uint32_t>(v) << op);
						}
						else
						{
							int32_t rs = -op;
							v = v >> ((rs >= 16) ? 15 : rs);
						}

						rt.writeVar(track, idx, v);
						break;
					}
					case 0x86: rt.writeVar(track, idx, op / 2); break;  // randvar -> midpoint
					case 0x87: rt.writeVar(track, idx, cur & op); break;
					case 0x88: rt.writeVar(track, idx, cur | op); break;
					case 0x89: rt.writeVar(track, idx, cur ^ op); break;
					case 0x8A: rt.writeVar(track, idx, ~static_cast<uint16_t>(op)); break;
					case 0x8B: if (op != 0) { rt.writeVar(track, idx, cur % op); } break;
					default: break;
				}
			}
			else if (ext >= 0x90 && ext <= 0x95)
			{
				int32_t idx = cmd.Args[0];

				if (idx < 0 || idx >= 48)
				{
					return;
				}

				bool drop = false;
				int32_t op = resolveArg(rt, track, cmd, 1, drop);

				if (drop)
				{
					return;
				}

				int32_t lhs = rt.readVar(track, idx);
				bool result = t.cmpFlag;

				switch (ext)
				{
					case 0x90: result = (lhs == op); break;
					case 0x91: result = (lhs >= op); break;
					case 0x92: result = (lhs >  op); break;
					case 0x93: result = (lhs <= op); break;
					case 0x94: result = (lhs <  op); break;
					case 0x95: result = (lhs != op); break;
					default: break;
				}

				if (result != t.cmpFlag)
				{
					t.cmpFlag = result;
					++rt.vmVersion;
				}
			}
			else
			{
				// mod2-4 / userproc: no engine effect here (stage 4), instant.
				rt.skipped.insert(0x100u | ext);
			}
		}

		// Activate a track at `offset` with fresh per-track state (SeqTrack::InitParam).
		void openTrack(Runtime& rt, int idx, uint32_t offset)
		{
			auto it = rt.commands.find(offset);

			if (it == rt.commands.end())
			{
				return;
			}

			TrackState& t = rt.tracks[idx];
			t = TrackState{};
			t.active = true;
			t.cursor = it;
			t.waitTicks = 0;
		}

		// Run one track until it blocks (sets waitTicks > 0) or ends. currentSample
		// is the frame-quantised sample position note-ons are stamped at.
		void processTrack(Runtime& rt, int track, uint32_t currentSample, uint32_t maxSamples)
		{
			TrackState& t = rt.tracks[track];

			while (t.active)
			{
				if (++t.execs > kTrackExecBudget)
				{
					t.active = false;
					return;
				}

				if (t.cursor == rt.commands.end())
				{
					t.active = false;   // ran off the end of the bank
					return;
				}

				const uint32_t here = t.cursor->first;
				const CseqCmd& cmd = t.cursor->second;

				t.ranAtAll = true;
				t.offsetVersion[here] = rt.vmVersion;

				// The [If] gate: exactly Cseq::Export's cmpFlag dispatch. 0xFE
				// alloc-track is consumed regardless of the flag.
				bool allocTrack = !cmd.Extended && cmd.Cmd == 0xFE;

				if (cmd.Suffix3 == SuffixType::If && !t.cmpFlag && !allocTrack)
				{
					++t.cursor;
					continue;
				}

				if (cmd.Extended)
				{
					execExtendedVm(rt, track, cmd);
					++t.cursor;
					continue;
				}

				uint8_t c = cmd.Cmd;

				// Note.
				if (c < 0x80)
				{
					bool drop = false;
					int32_t len = (cmd.Args.size() > 1) ? resolveArg(rt, track, cmd, 1, drop) : 0;

					if (drop)
					{
						++t.cursor;
						continue;
					}

					if (len < 0)
					{
						len = 0;
					}

					int velocity = cmd.Args.empty() ? 0 : cmd.Args[0];

					if (velocity > 0 && velocity <= 127)
					{
						double spt = samplesPerTick(rt);
						uint64_t gate = static_cast<uint64_t>(static_cast<double>(len) * spt + 0.5);

						// Clamp the gate so a single long note cannot grow the bus past
						// the safety cap (plus a short tail).
						uint64_t capEnd = static_cast<uint64_t>(maxSamples) + 2ull * kNativeRate;

						if (static_cast<uint64_t>(currentSample) + gate > capEnd)
						{
							gate = (capEnd > currentSample) ? (capEnd - currentSample) : 0;
						}

						NoteEvent ev;
						ev.startSample = currentSample;
						ev.gateSamples = static_cast<uint32_t>(gate);
						ev.program = t.program;
						ev.key = static_cast<int>(c);
						ev.velocity = velocity;

						for (int e = 0; e < 5; ++e)
						{
							ev.envOverride[e] = t.envOverride[e];
						}

						// The 24-voice pool inputs, latched at note-on.
						ev.priority = clamp<int>(64 + t.trackPriority, 0, 255);
						ev.track = track;
						ev.mono = t.mono;

						// The C6 native mix params, latched at note-on. Track/master/
						// expression volumes multiply as amplitudes; the two track pans
						// combine as offsets from centre; bend (scaled by its range) plus
						// transpose give the semitone offset.
						ev.volAmp = volumeByteToAmp(t.trackVolume)
							* volumeByteToAmp(rt.masterVolume)
							* volumeByteToAmp(t.expression);
						ev.panOffset = (t.trackPan - 64) + (t.initPan - 64);
						ev.pitchSemitones = (static_cast<double>(t.bend) / 128.0) * static_cast<double>(t.bendRange)
							+ static_cast<double>(t.transpose);

						rt.events.push_back(ev);

						if (rt.stats)
						{
							++rt.stats->notesFired;
						}
					}

					++t.cursor;

					if (t.noteWait)
					{
						t.waitTicks = static_cast<uint32_t>(len);

						if (len > 0)
						{
							return;
						}
					}

					continue;
				}

				// Rest.
				if (c == 0x80)
				{
					bool drop = false;
					int32_t wait = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					++t.cursor;

					if (drop)
					{
						continue;
					}

					if (wait < 0)
					{
						wait = 0;
					}

					t.waitTicks = static_cast<uint32_t>(wait);

					if (wait > 0)
					{
						return;
					}

					continue;
				}

				// Program change.
				if (c == 0x81)
				{
					bool drop = false;
					int32_t prog = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop && prog >= 0)
					{
						t.program = static_cast<uint32_t>(prog);
					}

					++t.cursor;
					continue;
				}

				// OpenTrack.
				if (c == 0x88)
				{
					if (cmd.Args.size() >= 2 && cmd.Args[0] >= 0 && cmd.Args[0] < 16)
					{
						openTrack(rt, cmd.Args[0], static_cast<uint32_t>(cmd.Args[1]));
					}

					++t.cursor;
					continue;
				}

				// Jump.
				if (c == 0x89)
				{
					uint32_t target = static_cast<uint32_t>(cmd.Args.empty() ? 0 : cmd.Args[0]);
					auto n = rt.commands.find(target);

					if (n == rt.commands.end())
					{
						++t.cursor;   // bad target: skip
						continue;
					}

					if (cmd.Suffix3 == SuffixType::If)
					{
						// Conditional jump; the gate above already proved cmpFlag true.
						if (!t.offsetVersion.count(target))
						{
							t.cursor = n;   // forward / first entry: take it
							continue;
						}

						// Backward: a counted/spin loop. Take it only while VM state has
						// moved since the target last ran and the budget holds (matches
						// Cseq::Export's vmVersion/condRetakes bound).
						bool stateChanged = (rt.vmVersion != t.offsetVersion[target]);
						bool budgetOk = (t.condRetakes[here] < kCondRetakeBudget) && (t.execs < kTrackExecBudget);

						if (stateChanged && budgetOk)
						{
							++t.condRetakes[here];
							t.cursor = n;
							continue;
						}

						++t.cursor;   // loop done: fall through
						continue;
					}

					// Unconditional jump.
					if (t.offsetVersion.count(target))
					{
						// Backward to already-played code: the format's whole-song loop.
						// Render the body once and end the track (Cseq::Export marks the
						// loop and advances; the player has no MIDI loop marker, so it
						// simply stops -- the max-seconds cap catches anything else).
						if (rt.stats)
						{
							rt.stats->loopDetected = true;
						}

						t.active = false;
						return;
					}

					t.cursor = n;   // forward goto into new code
					continue;
				}

				// Call.
				if (c == 0x8A)
				{
					uint32_t target = static_cast<uint32_t>(cmd.Args.empty() ? 0 : cmd.Args[0]);
					auto n = rt.commands.find(target);

					if (n == rt.commands.end())
					{
						++t.cursor;
						continue;
					}

					auto ret = t.cursor;
					++ret;

					if (ret != rt.commands.end())
					{
						t.callStack.push_back(ret->first);
					}

					t.cursor = n;
					continue;
				}

				// Return.
				if (c == 0xFD)
				{
					if (!t.callStack.empty())
					{
						uint32_t retOff = t.callStack.back();
						t.callStack.pop_back();

						auto n = rt.commands.find(retOff);

						if (n != rt.commands.end())
						{
							t.cursor = n;
							continue;
						}
					}

					t.active = false;   // stray Return: end track (as Cseq::Export does)
					return;
				}

				// Fin.
				if (c == 0xFF)
				{
					t.active = false;
					return;
				}

				// Timebase.
				if (c == 0xB0)
				{
					bool drop = false;
					int32_t tb = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop && tb > 0)
					{
						rt.timebase = static_cast<uint32_t>(tb);
					}

					++t.cursor;
					continue;
				}

				// Tempo.
				if (c == 0xE1)
				{
					bool drop = false;
					int32_t bpm = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop && bpm > 0)
					{
						rt.tempoBpm = static_cast<double>(bpm);
					}

					++t.cursor;
					continue;
				}

				// Note-wait toggle.
				if (c == 0xC7)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.noteWait = (v != 0);
					}

					++t.cursor;
					continue;
				}

				// AllocTrack mask -- consumed, no scheduling effect (OpenTrack activates).
				if (c == 0xFE)
				{
					++t.cursor;
					continue;
				}

				// Per-track ADSHR envelope overrides (C4). 0xB1 hold, 0xD0 attack,
				// 0xD1 decay, 0xD2 sustain, 0xD3 release: latch the resolved value
				// (plain / Var / Rnd) into the track override slot so the next note
				// uses it instead of its own byte. 0xFB resets all overrides. The
				// value is an engine ADSHR byte (0..127); clamp defensively.
				if (c == 0xB1 || c == 0xD0 || c == 0xD1 || c == 0xD2 || c == 0xD3)
				{
					bool drop = false;
					int32_t val = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						int slot = (c == 0xD0) ? 0 : (c == 0xB1) ? 1 : (c == 0xD1) ? 2 : (c == 0xD2) ? 3 : 4;
						t.envOverride[slot] = clamp<int32_t>(val, 0, 127);
					}

					++t.cursor;
					continue;
				}

				// Envelope reset: back to the note's own ADSHR bytes.
				if (c == 0xFB)
				{
					for (int e = 0; e < 5; ++e)
					{
						t.envOverride[e] = -1;
					}

					++t.cursor;
					continue;
				}

				// Voice-steal priority (C5): the track's priority, added to the
				// player's 64 at note-on. The converter drops this; the player's pool
				// uses it to decide which voices to steal.
				if (c == 0xC6)
				{
					bool drop = false;
					int32_t val = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.trackPriority = clamp<int32_t>(val, 0, 255);
					}

					++t.cursor;
					continue;
				}

				// Mono/poly (C5): a per-track voice-allocation flag MIDI cannot carry
				// (the converter demotes it to a notice). Nonzero = mono: a new note on
				// the track re-triggers its single voice (the previous note releases at
				// the new note-on). Toggling does not touch already-sounding voices.
				if (c == 0xB2)
				{
					bool drop = false;
					int32_t val = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.mono = (val != 0);
					}

					++t.cursor;
					continue;
				}

				// Native volume / pan / pitch (C6). These bear no time, so they only
				// affect notes started AFTER them (the params are latched at note-on).
				// The converter maps them to CC7/CC11/CC10/master-vol/pitch-bend; the
				// player folds them straight into per-voice gain / pan / step.
				if (c == 0xC1 || c == 0xC2 || c == 0xD5 || c == 0xC0 || c == 0xDC
					|| c == 0xC4 || c == 0xC5 || c == 0xC3)
				{
					bool drop = false;
					int32_t val = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						switch (c)
						{
							case 0xC1: t.trackVolume = clamp<int32_t>(val, 0, 127); break;  // track volume
							case 0xC2: rt.masterVolume = clamp<int32_t>(val, 0, 127); break; // master volume
							case 0xD5: t.expression = clamp<int32_t>(val, 0, 127); break;   // expression
							case 0xC0: t.trackPan = clamp<int32_t>(val, 0, 127); break;     // track pan
							case 0xDC: t.initPan = clamp<int32_t>(val, 0, 127); break;      // init pan
							case 0xC4: t.bend = clamp<int32_t>(val, -128, 127); break;      // pitch bend (signed)
							case 0xC5: t.bendRange = clamp<int32_t>(val, 0, 127); break;    // bend range (semitones)
							case 0xC3: t.transpose = clamp<int32_t>(val, -64, 63); break;   // coarse tune (semitones)
							default: break;
						}
					}

					++t.cursor;
					continue;
				}

				// Everything else is a parameter/effect command that does NOT bear
				// time (volume, pan, pitch bend, LFO, fx sends, ...). Safe-skip it so
				// the cursor never desyncs; native rendering of these is C6+.
				rt.skipped.insert(c);
				++t.cursor;
			}
		}

		void stepOneTick(Runtime& rt, uint32_t currentSample, uint32_t maxSamples)
		{
			for (int t = 0; t < 16; ++t)
			{
				if (!rt.tracks[t].active)
				{
					continue;
				}

				if (rt.tracks[t].waitTicks > 0)
				{
					--rt.tracks[t].waitTicks;
				}

				if (rt.tracks[t].waitTicks == 0)
				{
					processTrack(rt, t, currentSample, maxSamples);
				}
			}
		}
	}

	void allocateVoicePool(vector<PoolVoice>& voices, uint32_t capSample, RenderStats& stats)
	{
		// A single pool of 24 voices (byte-confirmed three ways in the disasm). A slot
		// holds an index into `voices` (-1 = free). Released voices (note-off passed)
		// drop to the release priority so they are stolen before any held note.
		constexpr int kVoicePool = 24;
		constexpr int kReleasePriority = 1;

		int slots[kVoicePool];

		for (int s = 0; s < kVoicePool; ++s)
		{
			slots[s] = -1;
		}

		// The current priority of a slot's voice at time t: a voice whose note-off has
		// passed is releasing, and drops to the release priority.
		auto slotPriority = [&](int vi, uint32_t t) -> int
		{
			const PoolVoice& p = voices[static_cast<size_t>(vi)];

			if (p.start + p.gate <= t)
			{
				return kReleasePriority;
			}

			return p.priority;
		};

		for (size_t i = 0; i < voices.size(); ++i)
		{
			PoolVoice& cur = voices[i];
			uint32_t t = cur.start;

			// 1. Reclaim voices that fell silent at or before this note-on.
			for (int s = 0; s < kVoicePool; ++s)
			{
				if (slots[s] >= 0 && voices[static_cast<size_t>(slots[s])].effectiveEnd() <= t)
				{
					slots[s] = -1;
				}
			}

			// 2. Mono: the track's previous voice releases at this note-on. Shorten its
			// gate to end here so it drops into release, then reclaim its slot if that
			// release finishes by now.
			if (cur.mono)
			{
				for (int s = 0; s < kVoicePool; ++s)
				{
					if (slots[s] < 0)
					{
						continue;
					}

					PoolVoice& prev = voices[static_cast<size_t>(slots[s])];

					if (prev.track == cur.track && prev.stopAt == UINT32_MAX)
					{
						uint32_t newGate = (t > prev.start) ? (t - prev.start) : 0;

						if (newGate < prev.gate)
						{
							prev.gate = newGate;
							prev.naturalEnd = voiceEndSample(prev.v, prev.start, newGate, capSample);
							++stats.monoRetriggers;

							if (prev.effectiveEnd() <= t)
							{
								slots[s] = -1;
							}
						}
					}
				}
			}

			// 3. Find a free slot.
			int chosen = -1;

			for (int s = 0; s < kVoicePool; ++s)
			{
				if (slots[s] < 0)
				{
					chosen = s;
					break;
				}
			}

			// 4. Pool full: find the front (lowest current priority; among a tie, the
			// oldest = lowest allocation index, since events are start-ordered -- this
			// is the deterministic FIFO-within-priority tie-break, flagged for Net-B).
			if (chosen < 0)
			{
				int frontSlot = -1;
				int frontPrio = 0;
				int frontIdx = 0;

				for (int s = 0; s < kVoicePool; ++s)
				{
					int vi = slots[s];
					int prio = slotPriority(vi, t);

					if (frontSlot < 0 || prio < frontPrio || (prio == frontPrio && vi < frontIdx))
					{
						frontSlot = s;
						frontPrio = prio;
						frontIdx = vi;
					}
				}

				// Refuse if the front outranks the requester (the NW4R bgt -> NULL guard,
				// verbatim): the note silently does not sound.
				if (frontPrio > cur.priority)
				{
					cur.sounds = false;
					++stats.notesRefused;
					continue;
				}

				// Steal the front: force-stop it here and take its slot. Every steal
				// truncates a render -- a still-held note is cut audibly, a releasing
				// one loses (largely inaudible) tail. Count both; the report notes how
				// many were still held.
				PoolVoice& victim = voices[static_cast<size_t>(slots[frontSlot])];

				++stats.voicesStolen;

				if (victim.start + victim.gate > t)
				{
					++stats.voicesStolenHeld;
				}

				victim.stopAt = t;
				slots[frontSlot] = -1;
				chosen = frontSlot;
			}

			slots[chosen] = static_cast<int>(i);

			// Peak concurrency (occupied slots after this allocation).
			uint32_t occupied = 0;

			for (int s = 0; s < kVoicePool; ++s)
			{
				if (slots[s] >= 0)
				{
					++occupied;
				}
			}

			if (occupied > stats.maxConcurrent)
			{
				stats.maxConcurrent = occupied;
			}
		}
	}

	bool renderSequence(const LoadedArchive& arch, StereoBus& bus, uint32_t maxSeconds, RenderStats& stats)
	{
		if (!arch.seq)
		{
			return false;
		}

		Runtime rt(arch.seq->Commands);
		rt.stats = &stats;

		// Start the conductor track at the entry's start offset (fall back to the
		// top if it is not a command boundary, as Cseq::Export does).
		auto start = rt.commands.find(arch.startOffset);

		if (start == rt.commands.end())
		{
			start = rt.commands.begin();
		}

		if (start == rt.commands.end())
		{
			// An empty sequence: nothing to render, but not an error.
			stats.tempoBpm = rt.tempoBpm;
			stats.timebase = rt.timebase;
			return true;
		}

		rt.tracks[0].active = true;
		rt.tracks[0].cursor = start;

		const uint32_t maxSamples = maxSeconds * kNativeRate;

		// Phase A: run the concurrent tick VM to produce note events.
		uint64_t frame = 0;
		double tickAccum = 0.0;
		uint64_t totalTicks = 0;

		while (rt.anyActive() && frame * kFrameSamples < maxSamples)
		{
			uint32_t currentSample = static_cast<uint32_t>(frame * kFrameSamples);

			double ticksPerSecond = (rt.tempoBpm > 0.0 ? rt.tempoBpm : kDefaultTempoBpm) / 60.0
				* static_cast<double>(rt.timebase > 0 ? rt.timebase : kDefaultTimebase);

			tickAccum += ticksPerSecond * (static_cast<double>(kFrameSamples) / static_cast<double>(kNativeRate));

			int steps = static_cast<int>(tickAccum);
			tickAccum -= steps;

			for (int s = 0; s < steps && rt.anyActive(); ++s)
			{
				stepOneTick(rt, currentSample, maxSamples);
				++totalTicks;
			}

			++frame;
		}

		if (rt.anyActive() && frame * kFrameSamples >= maxSamples)
		{
			stats.cappedByMaxSeconds = true;
		}

		// Phase B: resolve each note to a voice, run the 24-voice priority pool over
		// the note-ons in time order (they are pushed in non-decreasing startSample),
		// then render the surviving voices. A hard cap bounds each voice's envelope
		// release tail (a release byte 0 is a ~19-minute tail) so the bus cannot grow
		// unbounded past the render window.
		const uint32_t renderCap = maxSamples + 2u * kNativeRate;

		vector<PoolVoice> voices;
		voices.reserve(rt.events.size());

		for (const NoteEvent& ev : rt.events)
		{
			VoiceSpec v;

			if (!resolveVoice(arch, ev.program, ev.key, ev.velocity, v))
			{
				++stats.notesDropped;
				continue;
			}

			// Apply the track's ADSHR overrides (captured at note-on) on top of the
			// note's own envelope bytes. Order: attack, hold, decay, sustain, release.
			if (ev.envOverride[0] >= 0) v.envAttack = static_cast<uint8_t>(ev.envOverride[0]);
			if (ev.envOverride[1] >= 0) v.envHold = static_cast<uint8_t>(ev.envOverride[1]);
			if (ev.envOverride[2] >= 0) v.envDecay = static_cast<uint8_t>(ev.envOverride[2]);
			if (ev.envOverride[3] >= 0) v.envSustain = static_cast<uint8_t>(ev.envOverride[3]);
			if (ev.envOverride[4] >= 0) v.envRelease = static_cast<uint8_t>(ev.envOverride[4]);

			// C6: fold the track/master/expression volume, additive pan, and bend +
			// transpose pitch (all latched at note-on) into this voice's gain/pan/step.
			applyMixParams(v, ev.volAmp, ev.panOffset, ev.pitchSemitones);

			PoolVoice p;
			p.v = v;
			p.start = ev.startSample;
			p.gate = ev.gateSamples;
			p.naturalEnd = voiceEndSample(v, ev.startSample, ev.gateSamples, renderCap);
			p.priority = ev.priority;
			p.track = ev.track;
			p.mono = ev.mono;

			voices.push_back(p);
		}

		allocateVoicePool(voices, renderCap, stats);

		// Render the surviving voices in event order (deterministic accumulation).
		for (const PoolVoice& p : voices)
		{
			if (!p.sounds)
			{
				continue;
			}

			renderVoice(bus, p.v, p.start, p.gate, (p.stopAt < renderCap) ? p.stopAt : renderCap);
		}

		// Stats.
		for (const TrackState& t : rt.tracks)
		{
			if (t.ranAtAll)
			{
				++stats.tracksOpened;
			}
		}

		stats.tickLength = totalTicks;
		stats.nativeSamples = bus.frames();
		stats.tempoBpm = rt.tempoBpm;
		stats.timebase = rt.timebase;
		stats.skippedOps.assign(rt.skipped.begin(), rt.skipped.end());

		return true;
	}
}
