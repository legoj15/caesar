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

			// The C7 live-modulation anchor: the voice follows track `track`'s live
			// timeline (volume/pan/pitch/LFO/LPF) and the master-volume curve from
			// `startSample` onward -- no per-note latch of volume/pan/pitch anymore.
			// noteOnSample == startSample; kept explicit for the LFO delay reference.

			// C8 per-note pitch modifiers, captured at note-on. sweep (0xE3) and
			// portamento (0xC9/CE/CF) are additive semitone offsets that glide to 0
			// over their duration. tieRegion >= 0 marks this event as a tie region's
			// ONE continuous voice; its pitch retune curve lives in Runtime::tieRegions
			// and its gate is finalised when the region closes.
			float sweepFromSemis = 0.0f;
			uint32_t sweepDurSamples = 0;
			float portaFromSemis = 0.0f;
			uint32_t portaDurSamples = 0;
			int tieRegion = -1;

			// C10: the track's CURRENT bank (0xB6) at note-on -- the voice resolves its
			// instrument against this bank, so a mid-sequence switch re-points it.
			uint32_t bankIndex = 0;
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

			// C7 native mix state as LIVE per-frame curves (volume 0xC1 / expression
			// 0xD5 / pan 0xC0 / init pan 0xDC / bend 0xC4 / bend range 0xC5 / transpose
			// 0xC3), each in the engine's control-value domain. A `_t`-suffixed command
			// glides the curve from its current value to the target over the trailing
			// duration; a plain command is an instant step. Sounding voices sample these
			// each frame, so a mid-note change follows the note (the engine model) rather
			// than latching at onset. The LFO block + LPF (0xD8) extend the same struct.
			TrackTimeline timeline;

			// C8 tie (0xC8): one continuous voice across tied notes. tieOn latches the
			// flag; while a region is open, tieRegion indexes Runtime::tieRegions and
			// tieFirstKey is the base pitch each later tied note steps the region's
			// pitch curve to (newKey - firstKey), retuning the LIVE voice with no
			// re-attack. Both edges of 0xC8, a Fin, or the track end close the region.
			bool tieOn = false;
			int tieRegion = -1;
			int tieFirstKey = 60;

			// C8 sweep (0xE3): a signed pitch offset (semitones) pending for the NEXT
			// note, gliding to 0 over that note's gate. 0 = none.
			float pendingSweepSemis = 0.0f;

			// C8 portamento (0xC9 set-start+enable / 0xCE on-off / 0xCF time). A note
			// glides from the origin key (portaStartKey if set by 0xC9, else the
			// previous note's key) to its own key over a portaTime-scaled duration.
			bool portaOn = false;
			int portaStartKey = -1;   // 0xC9 one-shot origin; -1 = use lastNoteKey
			int portaTime = 0;        // 0xCF
			int lastNoteKey = -1;     // previous note's key (default glide origin)

			// C10 track features. bankIndex (0xB6) re-points the instrument lookup to
			// another CbnkRecords bank mid-sequence; velRange (0xB3) scales note-on
			// velocities (127 = identity); muted (0xDD) suppresses this track's notes
			// (time still advances). Damper (0xDF) and the LPF (0xD8) live as curves on
			// the timeline (they affect a sounding voice per frame / at note-off).
			uint32_t bankIndex = 0;
			int velRange = 127;
			bool muted = false;

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

			uint32_t defaultBankIndex = 0;   // the sequence's bound bank (0xB6 default)

			ParamCurve masterVolume;   // 0xC2, sequence-wide, live (C7)

			int16_t globalVars[32] = { 0 };      // slots 0-31 (shared)
			bool globalWritten[32] = { false };
			uint64_t vmVersion = 0;

			TrackState tracks[16];

			// C8 tie regions: one continuous voice per region, keyed to a NoteEvent by
			// index. `pitch` is the retune curve (a step per tied note, offset from the
			// region's first key). Stored on the Runtime so a Phase-B VoiceMod can point
			// at it for the whole render.
			struct TieRegion
			{
				ParamCurve pitch;
				size_t eventIndex = 0;
			};

			vector<TieRegion> tieRegions;

			vector<NoteEvent> events;
			RenderStats* stats = nullptr;
			set<uint32_t> skipped;   // safe-skipped opcodes (for the report)

			explicit Runtime(const map<uint32_t, CseqCmd>& cmds) : commands(cmds)
			{
				masterVolume.reset(127.0f);

				for (TrackState& t : tracks)
				{
					t.timeline.reset();
				}
			}

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

		// A `_t`-suffixed command appends a trailing s16 ramp DURATION (in ticks) after
		// its own arguments (parse: Suffix2 Time/TimeRnd/TimeVar). Resolve it the way
		// the primary arg resolves -- a TimeVar reads the live VM, a TimeRnd stands in
		// the range midpoint (PRNG-free), a plain Time is the raw s16. Returns 0 (an
		// instant set) for a non-`_t` command or a negative/dropped duration.
		uint32_t rampDurationTicks(Runtime& rt, int track, const CseqCmd& cmd)
		{
			if (cmd.Suffix2 == SuffixType::None || cmd.Args.empty())
			{
				return 0;
			}

			int32_t last = static_cast<int32_t>(cmd.Args.size()) - 1;
			int32_t dur = cmd.Args[last];

			if (cmd.Suffix2 == SuffixType::TimeRnd)
			{
				dur = (cmd.Arg2Rnd.first + cmd.Arg2Rnd.second) / 2;
			}
			else if (cmd.Suffix2 == SuffixType::TimeVar)
			{
				int32_t idx = cmd.Args[last];

				if (idx < 0 || idx >= 48)
				{
					return 0;
				}

				dur = rt.readVar(track, idx);
			}

			return (dur > 0) ? static_cast<uint32_t>(dur) : 0;
		}

		// Command a live track/master parameter: resolve the target (Args[0], honouring
		// a Var/Rnd prefix) and the `_t` ramp duration, convert the duration from ticks
		// to samples at the current tempo, and glide the curve there. A non-`_t` command
		// is an instant step (0 duration). A dropped Var arg leaves the curve unchanged.
		void setParam(Runtime& rt, int track, const CseqCmd& cmd, uint32_t currentSample,
			ParamCurve& curve, float lo, float hi)
		{
			bool drop = false;
			int32_t val = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

			if (drop)
			{
				return;
			}

			float target = static_cast<float>(clamp<int32_t>(val, static_cast<int32_t>(lo), static_cast<int32_t>(hi)));

			uint32_t durTicks = rampDurationTicks(rt, track, cmd);
			uint32_t durSamples = durTicks
				? static_cast<uint32_t>(static_cast<double>(durTicks) * samplesPerTick(rt) + 0.5)
				: 0;

			curve.set(currentSample, target, durSamples);

			if (rt.stats)
			{
				if (durSamples > 0)
				{
					++rt.stats->rampsApplied;

					if (durTicks > rt.stats->longestRampTicks)
					{
						rt.stats->longestRampTicks = durTicks;
					}
				}
				else
				{
					++rt.stats->instantSets;
				}
			}
		}

		// Close a track's open tie region (C8): finalise its ONE continuous voice's
		// gate to end at `endSample`. Called on 0xC8 (both edges), Fin, and track end.
		void closeTie(Runtime& rt, TrackState& t, uint32_t endSample)
		{
			if (t.tieRegion < 0)
			{
				return;
			}

			NoteEvent& ev = rt.events[rt.tieRegions[static_cast<size_t>(t.tieRegion)].eventIndex];
			ev.gateSamples = (endSample > ev.startSample) ? (endSample - ev.startSample) : 0;
			t.tieRegion = -1;
		}

		// C10 damper (0xDF): if the track's damper is down (>= 64) at the note-off,
		// defer the release until it lifts. Returns the effective gate (note-on to the
		// deferred note-off). A damper that never engaged returns the original gate.
		uint32_t damperGate(const ParamCurve& damper, uint32_t start, uint32_t gate, uint32_t cap)
		{
			uint32_t off = start + gate;

			if (damper.valueAt(off) < 64.0f)
			{
				return gate;   // pedal up at note-off: normal release
			}

			// Held: the first later step where the damper drops below the threshold.
			for (const ParamCurve::Seg& seg : damper.segs)
			{
				if (seg.start > off && seg.to < 64.0f)
				{
					uint32_t end = (seg.start < cap) ? seg.start : cap;
					return (end > start) ? (end - start) : gate;
				}
			}

			return (cap > start) ? (cap - start) : gate;   // never lifts: hold to the cap
		}

		// The portamento glide duration for a 0xCF time value, in samples. The exact
		// CTR portaTime -> duration mapping is not byte-confirmed in the disasm (the
		// doc records no portamento address), so this uses the NW4R precedent that the
		// time value scales the glide and treats it as a tick count at the current
		// tempo (a distance-independent glide). FLAGGED for the console capture -- one
		// constant to recalibrate.
		uint32_t portaDurationSamples(Runtime& rt, int portaTime)
		{
			if (portaTime <= 0)
			{
				return 0;
			}

			return static_cast<uint32_t>(static_cast<double>(portaTime) * samplesPerTick(rt) + 0.5);
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
			t.timeline.reset();   // fresh SeqTrack::InitParam defaults (assignment above cleared it)
			t.bankIndex = rt.defaultBankIndex;   // inherit the sequence's bank until 0xB6
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
					closeTie(rt, t, currentSample);
					t.active = false;
					return;
				}

				if (t.cursor == rt.commands.end())
				{
					closeTie(rt, t, currentSample);
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

					int rawVel = cmd.Args.empty() ? 0 : cmd.Args[0];

					// C10 velocity range (0xB3): scale the note-on velocity (127 =
					// identity). The note is still gated on the RAW velocity, so a
					// velocity-0 note stays a silent-but-timed note as before.
					int velocity = clamp<int>(rawVel * t.velRange / 127, 0, 127);

					// C10 mute (0xDD): a muted track produces no notes; time still
					// advances below.
					if (!t.muted && rawVel > 0 && rawVel <= 127)
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

						int key = static_cast<int>(c);

						if (t.tieOn && t.tieRegion >= 0)
						{
							// C8 tie retune: step the region's ONE voice to the new key
							// (no re-attack). The note-length still advances track time
							// below; it plays no audio role here.
							rt.tieRegions[static_cast<size_t>(t.tieRegion)].pitch.set(
								currentSample, static_cast<float>(key - t.tieFirstKey), 0);

							if (rt.stats)
							{
								++rt.stats->tieRetunes;
							}
						}
						else
						{
							NoteEvent ev;
							ev.startSample = currentSample;
							ev.gateSamples = static_cast<uint32_t>(gate);
							ev.program = t.program;
							ev.key = key;
							ev.velocity = velocity;

							for (int e = 0; e < 5; ++e)
							{
								ev.envOverride[e] = t.envOverride[e];
							}

							// The 24-voice pool inputs, latched at note-on. Volume/pan/pitch
							// are NOT latched -- the voice follows track `track`'s live
							// timeline each frame (C7), so a mid-note change reaches it.
							ev.priority = clamp<int>(64 + t.trackPriority, 0, 255);
							ev.track = track;
							ev.mono = t.mono;
							ev.bankIndex = t.bankIndex;   // C10: resolve against the current bank

							// C8 sweep (0xE3): a pending signed pitch offset gliding to 0
							// over this note's gate.
							if (t.pendingSweepSemis != 0.0f)
							{
								ev.sweepFromSemis = t.pendingSweepSemis;
								ev.sweepDurSamples = static_cast<uint32_t>(gate);
							}

							// C8 portamento (0xC9/CE/CF): glide from the origin key (0xC9's
							// key, else the previous note's) to this note's key.
							if (t.portaOn)
							{
								int origin = (t.portaStartKey >= 0) ? t.portaStartKey : t.lastNoteKey;

								if (origin >= 0 && origin != key)
								{
									ev.portaFromSemis = static_cast<float>(origin - key);
									ev.portaDurSamples = portaDurationSamples(rt, t.portaTime);
								}
							}

							// C8 tie OPEN: this note starts a continuous voice; later tied
							// notes retune it. Its gate is finalised when the region closes.
							if (t.tieOn)
							{
								Runtime::TieRegion region;
								region.pitch.reset(0.0f);
								region.eventIndex = rt.events.size();
								rt.tieRegions.push_back(region);
								t.tieRegion = static_cast<int>(rt.tieRegions.size() - 1);
								t.tieFirstKey = key;
								ev.tieRegion = t.tieRegion;

								if (rt.stats)
								{
									++rt.stats->tieVoices;
								}
							}

							rt.events.push_back(ev);

							if (rt.stats)
							{
								++rt.stats->notesFired;
							}
						}

						// Sweep is consumed by the note; 0xC9's one-shot origin too; the
						// previous-key origin for the next portamento is this note's key.
						t.pendingSweepSemis = 0.0f;
						t.portaStartKey = -1;
						t.lastNoteKey = key;
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

						closeTie(rt, t, currentSample);
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

					closeTie(rt, t, currentSample);
					t.active = false;   // stray Return: end track (as Cseq::Export does)
					return;
				}

				// Fin.
				if (c == 0xFF)
				{
					closeTie(rt, t, currentSample);
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

				// Tie on/off (C8, 0xC8). Both edges release the sounding voice on
				// hardware, so either edge closes an open region; a nonzero value arms
				// the next note to start one continuous voice.
				if (c == 0xC8)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						closeTie(rt, t, currentSample);
						t.tieOn = (v != 0);
					}

					++t.cursor;
					continue;
				}

				// Sweep pitch (C8, 0xE3): a signed intra-note ramp in 1/64-semitone
				// units, pending for the next note (glides from the offset to nominal).
				if (c == 0xE3)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.pendingSweepSemis = static_cast<float>(v) / 64.0f;
					}

					++t.cursor;
					continue;
				}

				// Portamento (C8). 0xC9 sets the glide start key AND enables; 0xCE
				// toggles on/off; 0xCF sets the glide time.
				if (c == 0xC9)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.portaStartKey = clamp<int32_t>(v, 0, 127);
						t.portaOn = true;
					}

					++t.cursor;
					continue;
				}

				if (c == 0xCE)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.portaOn = (v != 0);
					}

					++t.cursor;
					continue;
				}

				if (c == 0xCF)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.portaTime = clamp<int32_t>(v, 0, 127);
					}

					++t.cursor;
					continue;
				}

				// The persistent track LFO (C9): ONE parameter block, retargetable. 0xCA
				// depth / 0xCB rate / 0xCD range / 0xCC target (0 pitch, 1 volume, 2 pan)
				// each latch (or `_t`-glide) their own curve; the block persists across a
				// target switch, so a value commanded on any target survives it -- exactly
				// the converter's wire/shadow model, but trivially, since the player keeps
				// the values. Applied per frame in renderVoice; pitch cents = depth x range.
				if (c == 0xCA || c == 0xCB || c == 0xCC || c == 0xCD)
				{
					ParamCurve& curve =
						(c == 0xCA) ? t.timeline.lfoDepth :
						(c == 0xCB) ? t.timeline.lfoRate :
						(c == 0xCD) ? t.timeline.lfoRange : t.timeline.lfoTarget;

					setParam(rt, track, cmd, currentSample, curve, 0, 127);

					if (rt.stats) { ++rt.stats->lfoCommands; }

					++t.cursor;
					continue;
				}

				// Mod delay (0xE0): time from note-on before the LFO engages, in 5 ms
				// units (NW4R lfoParam.delay = arg * 5 ms). Stored as ms.
				if (c == 0xE0)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						int32_t ms = (v > 0 ? v : 0) * 5;
						t.timeline.lfoDelayMs.set(currentSample, static_cast<float>(ms), 0);

						if (rt.stats) { ++rt.stats->lfoCommands; }
					}

					++t.cursor;
					continue;
				}

				// C10 track features. Bank switch (0xB6): re-point the track's instrument
				// lookup to another CbnkRecords bank; captured per note at note-on.
				if (c == 0xB6)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop && v >= 0)
					{
						t.bankIndex = static_cast<uint32_t>(v);

						if (rt.stats) { ++rt.stats->bankSwitches; }
					}

					++t.cursor;
					continue;
				}

				// Velocity range (0xB3): scale note-on velocities (127 = identity).
				if (c == 0xB3)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.velRange = clamp<int32_t>(v, 0, 127);
					}

					++t.cursor;
					continue;
				}

				// Track mute (0xDD): nonzero suppresses this track's notes; time still
				// advances (so the rest of the sequence stays in sync).
				if (c == 0xDD)
				{
					bool drop = false;
					int32_t v = cmd.Args.empty() ? 0 : resolveArg(rt, track, cmd, 0, drop);

					if (!drop)
					{
						t.muted = (v != 0);
					}

					++t.cursor;
					continue;
				}

				// LPF cutoff (0xD8) + damper (0xDF): live curves. The LPF cuts a sounding
				// voice per frame; the damper defers a note's release while it is down.
				if (c == 0xD8) { setParam(rt, track, cmd, currentSample, t.timeline.lpfCutoff, 0, 127); ++t.cursor; continue; }
				if (c == 0xDF) { setParam(rt, track, cmd, currentSample, t.timeline.damper,    0, 127); ++t.cursor; continue; }

				// Biquad type / value (0xB4 / 0xB5): a general voice biquad. The disasm
				// doc records no filter topology, so these fold into the same LPF path
				// (0xB5 as an added cut) and are FLAGGED -- the exact type mapping is a
				// console-capture item. Tracked so they are audible, not dropped.
				if (c == 0xB5) { setParam(rt, track, cmd, currentSample, t.timeline.lpfCutoff, 0, 127); ++t.cursor; continue; }
				if (c == 0xB4) { ++t.cursor; continue; }   // biquad type select (flagged; no distinct topology modelled)

				// Native volume / pan / pitch (C7), as LIVE curves. A `_t` suffix glides
				// the parameter from its current value to the target over the trailing
				// duration (ticks -> samples at the current tempo); a plain command is an
				// instant step. Sounding voices sample these each frame.
				if (c == 0xC1) { setParam(rt, track, cmd, currentSample, t.timeline.volume,     0, 127); ++t.cursor; continue; }
				if (c == 0xC2) { setParam(rt, track, cmd, currentSample, rt.masterVolume,       0, 127); ++t.cursor; continue; }
				if (c == 0xD5) { setParam(rt, track, cmd, currentSample, t.timeline.expression, 0, 127); ++t.cursor; continue; }
				if (c == 0xC0) { setParam(rt, track, cmd, currentSample, t.timeline.pan,        0, 127); ++t.cursor; continue; }
				if (c == 0xDC) { setParam(rt, track, cmd, currentSample, t.timeline.initPan,    0, 127); ++t.cursor; continue; }
				if (c == 0xC4) { setParam(rt, track, cmd, currentSample, t.timeline.bend,    -128, 127); ++t.cursor; continue; }
				if (c == 0xC5) { setParam(rt, track, cmd, currentSample, t.timeline.bendRange,   0, 127); ++t.cursor; continue; }
				if (c == 0xC3) { setParam(rt, track, cmd, currentSample, t.timeline.transpose, -64,  63); ++t.cursor; continue; }

				// Everything else is a parameter/effect command that does NOT bear
				// time (LFO, fx sends, tie, sweep, portamento, ...). Safe-skip it so the
				// cursor never desyncs; native rendering of these is C8+.
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

	bool renderSequence(LoadedArchive& arch, StereoBus& bus, uint32_t maxSeconds, RenderStats& stats)
	{
		if (!arch.seq)
		{
			return false;
		}

		Runtime rt(arch.seq->Commands);
		rt.stats = &stats;
		rt.defaultBankIndex = arch.bankIndex;   // C10: the bank 0xB6 switches away from

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
		rt.tracks[0].bankIndex = arch.bankIndex;

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

			// C10: resolve against the track's CURRENT bank (a 0xB6 switch may have
			// re-pointed it), built + cached on demand.
			Cbnk* bank = getBank(arch, ev.bankIndex);

			if (!bank || !resolveVoice(*bank, ev.program, ev.key, ev.velocity, v))
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

			// C7: volume/pan/pitch are NOT folded here -- the voice follows its track's
			// live timeline per frame in renderVoice (see the VoiceMod in the render
			// loop). The VoiceSpec keeps only the note's static base (velocity x zone
			// volume, note pan, key/root/tune step).

			// C10 damper: defer the note-off while the pedal is held.
			uint32_t gate = damperGate(rt.tracks[ev.track].timeline.damper,
				ev.startSample, ev.gateSamples, renderCap);

			PoolVoice p;
			p.v = v;
			p.start = ev.startSample;
			p.gate = gate;
			p.naturalEnd = voiceEndSample(v, ev.startSample, gate, renderCap);
			p.priority = ev.priority;
			p.track = ev.track;
			p.mono = ev.mono;

			// C8 pitch modifiers (sweep / portamento / tie region), for the VoiceMod.
			p.sweepFromSemis = ev.sweepFromSemis;
			p.sweepDurSamples = ev.sweepDurSamples;
			p.portaFromSemis = ev.portaFromSemis;
			p.portaDurSamples = ev.portaDurSamples;
			p.tieRegion = ev.tieRegion;

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

			VoiceMod mod;
			mod.track = &rt.tracks[p.track].timeline;
			mod.master = &rt.masterVolume;
			mod.noteOnSample = p.start;
			mod.sweepFromSemis = p.sweepFromSemis;
			mod.sweepDurSamples = p.sweepDurSamples;
			mod.portaFromSemis = p.portaFromSemis;
			mod.portaDurSamples = p.portaDurSamples;
			if (p.tieRegion >= 0) { mod.voicePitch = &rt.tieRegions[static_cast<size_t>(p.tieRegion)].pitch; }

			renderVoice(bus, p.v, p.start, p.gate, (p.stopAt < renderCap) ? p.stopAt : renderCap, &mod);
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
