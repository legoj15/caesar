#pragma once

// caesar_play — the suite stage-2 offline dry player. Public surface shared by
// the loader, the voice DSP, the sequencer and the CLI. Nothing here reaches
// into caesar_core's export paths: the engine is a parallel consumer of the
// same parsed models (Csar/Cbnk/Cseq/Cwav), so the shipped converter's A/B and
// round-trip guards keep watching caesar_core unchanged.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ParseContext;
struct Csar;
struct Cbnk;
struct Cseq;

namespace play
{
	// The DSP's native mix rate and frame size (Azahar/Citra audio_types.h:
	// native_sample_rate = 32728, samples_per_frame = 160 -> 204.55 frames/s,
	// 4.889 ms/frame). Every voice is resampled to this bus; a single final
	// sinc upsample to the output rate is provably transparent (Nyquist 16.364 kHz).
	constexpr uint32_t kNativeRate = 32728;
	constexpr uint32_t kFrameSamples = 160;

	// One renderable sequence enumerated from an archive's INFO table. Only
	// Type 0x2203 in-archive ("Convertible") sequences appear -- external
	// streams (0x2201) and CWSD wave-sound blocks (0x2202) are not sequences.
	struct SequenceInfo
	{
		uint32_t index = 0;        // 0-based over the renderable sequences (the --seq index)
		std::string name;          // the INFO symbol name
		uint32_t bankIndex = 0;    // index into Csar::CbnkRecords: the bank this seq binds to
		uint32_t startOffset = 0;  // entry start within the shared sequence bank (DATA+8-relative)
		uint32_t recordIndex = 0;  // index into Csar::CseqRecords
	};

	// The loaded, resolved object graph for one archive plus (optionally) one
	// chosen sequence. Members are declared in dependency order so their reverse
	// destruction pops the ParseContext frames in the safe order: the seq/bank
	// (pushed last) release before the Csar (whose ~Csar frees the wave archives
	// it constructed), and the ParseContext outlives every object that holds a
	// reference to it. The wave archives themselves live in Csar::Cwars, decoded
	// in-memory by the loader (never written to disk).
	struct LoadedArchive
	{
		std::unique_ptr<ParseContext> ctx;
		std::unique_ptr<Csar> csar;
		std::unique_ptr<Cbnk> bank;  // the chosen sequence's bank; null until resolveSequence
		std::unique_ptr<Cseq> seq;   // the chosen sequence; null until resolveSequence
		uint32_t bankIndex = 0;      // the sequence's default bank (index into CbnkRecords)

		// C10: additional banks a 0xB6 mid-sequence switch selects, built on demand and
		// cached by CbnkRecords index (null = not built / not in-archive). Declared
		// after `seq` so it destructs BEFORE ~Csar frees the wave archives these banks
		// resolve against (the same ordered-teardown rule the members above follow).
		std::vector<std::unique_ptr<Cbnk>> switchBanks;

		uint32_t startOffset = 0;    // the chosen entry's start offset

		LoadedArchive();
		~LoadedArchive();

		LoadedArchive(const LoadedArchive&) = delete;
		LoadedArchive& operator=(const LoadedArchive&) = delete;
	};

	// Parse an archive read-only (Csar::Parse, no extraction/Export) and
	// construct+parse every internal wave archive into Csar::Cwars, decoding its
	// samples in memory. No files are written. Returns null on failure, with a
	// message already printed to stderr. Diagnostics the parse fires (Analyse
	// rows, notices) are collateral of loading a new surface and are discarded.
	std::unique_ptr<LoadedArchive> loadArchive(const std::string& path);

	// Enumerate the renderable sequences of a loaded archive, in INFO order.
	std::vector<SequenceInfo> listSequences(const Csar& csar);

	// Resolve one chosen sequence: construct+parse its bank (Csar::CbnkRecords
	// [bankIndex]) and its own .bcseq, filling arch.bank/arch.seq/arch.startOffset.
	// The bank's samples resolve live against the already-decoded Csar::Cwars, so
	// no disk round-trip happens. Returns false on failure (message to stderr).
	bool resolveSequence(LoadedArchive& arch, const SequenceInfo& info);

	// Return the parsed bank at CbnkRecords index `bankIndex`, building + caching it
	// on demand -- a 0xB6 mid-sequence bank switch's target (C10). The sequence's own
	// default bank is returned directly; others are constructed against the already-
	// decoded Cwars, exactly like resolveSequence's bank. Null if the index is out of
	// range or the bank has no in-archive data (an external bank).
	Cbnk* getBank(LoadedArchive& arch, uint32_t bankIndex);

	// --- The voice DSP + native mix bus (C2) --------------------------------

	// The native-rate (32,728 Hz) float stereo mix bus. Voices accumulate into
	// l/r; a single final resample takes it to the output rate.
	struct StereoBus
	{
		std::vector<float> l;
		std::vector<float> r;

		void ensure(size_t frames)
		{
			if (l.size() < frames)
			{
				l.resize(frames, 0.0f);
				r.resize(frames, 0.0f);
			}
		}

		size_t frames() const { return l.size(); }
	};

	// --- The per-track parameter timeline (C7 `_t` ramp synthesis) -----------
	//
	// The reusable timeline-flattener the blueprint says stage 5's `.it` export
	// shares. A ParamCurve is a piecewise-linear function of ABSOLUTE bus sample
	// position, in the engine's CONTROL-VALUE domain (a 0..127 volume/pan byte, a
	// signed bend/transpose in semitones, an LFO parameter). NW4R ramps the STORED
	// control value linearly and converts it to amplitude / pan angle / pitch ratio
	// downstream each frame -- so a `_t` glide is linear in THIS domain, not in dB
	// (documented + flagged in HISTORY; the engine's MoveValue precedent). A plain
	// (non-`_t`) set is a zero-duration segment (an instant step). `set` chains a
	// new ramp from the curve's CURRENT interpolated value, so an interrupted ramp
	// re-anchors correctly. Segments are appended in non-decreasing `start` order
	// (the frame clock only advances); the renderer samples valueAt per frame.
	struct ParamCurve
	{
		struct Seg
		{
			uint32_t start;  // absolute bus sample the ramp begins
			uint32_t dur;    // ramp length in samples (0 = instant step)
			float from;      // value at `start`
			float to;        // value at `start + dur` and after
		};

		std::vector<Seg> segs;
		float initial = 0.0f;  // the value before the first segment

		void reset(float v)
		{
			initial = v;
			segs.clear();
		}

		// The control value at absolute sample `s`. Before the first segment it is
		// `initial`; within a segment it interpolates linearly; after, it holds the
		// segment's target. O(log n) over the segments.
		float valueAt(uint32_t s) const
		{
			if (segs.empty() || s < segs.front().start)
			{
				return initial;
			}

			// The last segment whose start <= s (hand-rolled upper_bound - 1 so the
			// header stays dependency-light; segments are start-sorted by construction).
			size_t lo = 0;
			size_t hi = segs.size();

			while (lo < hi)
			{
				size_t mid = (lo + hi) / 2;

				if (segs[mid].start <= s)
				{
					lo = mid + 1;
				}
				else
				{
					hi = mid;
				}
			}

			const Seg& g = segs[lo - 1];

			if (g.dur == 0 || s >= g.start + g.dur)
			{
				return g.to;
			}

			float t = static_cast<float>(s - g.start) / static_cast<float>(g.dur);

			return g.from + (g.to - g.from) * t;
		}

		// Command the parameter to `target` over `durSamples` (0 = instant), gliding
		// from wherever it sits NOW (the chained-ramp anchor).
		void set(uint32_t s, float target, uint32_t durSamples)
		{
			segs.push_back({ s, durSamples, valueAt(s), target });
		}

		bool active() const { return !segs.empty(); }
	};

	// The live per-track parameters a voice follows each frame (C7+). Volumes/pans
	// are 0..127 byte curves (127 = unity, 64 = centre); bend is the 0xC4 signed
	// value, bendRange the 0xC5 semitone span (default 2), transpose the 0xC3 coarse
	// tune. Each starts at the engine's SeqTrack::InitParam default and every `_t`
	// command adds a ramp segment. The LFO block (C9) and biquad LPF (C10) extend it.
	struct TrackTimeline
	{
		ParamCurve volume;      // 0xC1
		ParamCurve expression;  // 0xD5
		ParamCurve pan;         // 0xC0
		ParamCurve initPan;     // 0xDC
		ParamCurve bend;        // 0xC4 (signed)
		ParamCurve bendRange;   // 0xC5 (semitones)
		ParamCurve transpose;   // 0xC3 (semitones)

		// The persistent LFO parameter block (C9). ONE block per track, retargetable
		// via `lfoTarget` (0 = pitch, 1 = volume, 2 = pan). depth 0xCA / rate 0xCB /
		// range 0xCD / delay 0xE0 (5 ms units). Retargeting preserves the block, so a
		// value commanded on any target survives a target switch (the converter's
		// wire/shadow model). Pitch LFO cents = depth x range.
		ParamCurve lfoDepth;    // 0xCA (0..127)
		ParamCurve lfoRate;     // 0xCB
		ParamCurve lfoRange;    // 0xCD
		ParamCurve lfoDelayMs;  // 0xE0 x 5 ms (stored as ms)
		ParamCurve lfoTarget;   // 0xCC (0/1/2)

		// The voice biquad low-pass cutoff (C10). 0xD8, 0..127; 64 = open (no cut).
		ParamCurve lpfCutoff;   // 0xD8

		// The damper pedal (C10). 0xDF; >= 64 = down, which suppresses a note's release
		// until it lifts (a sounding voice's note-off is deferred).
		ParamCurve damper;      // 0xDF (0..127)

		void reset()
		{
			volume.reset(127.0f);
			expression.reset(127.0f);
			pan.reset(64.0f);
			initPan.reset(64.0f);
			bend.reset(0.0f);
			bendRange.reset(2.0f);
			transpose.reset(0.0f);
			lfoDepth.reset(0.0f);
			lfoRate.reset(0.0f);
			lfoRange.reset(0.0f);
			lfoDelayMs.reset(0.0f);
			lfoTarget.reset(0.0f);
			lpfCutoff.reset(64.0f);
			damper.reset(0.0f);
		}
	};

	// A resolved voice: the static synthesis parameters for one note, pointing at
	// decoded PCM owned by a live Cwav (in Csar::Cwars). The read `step` folds the
	// sample rate, the key-vs-root-key semitone ratio and the note's Tune together.
	struct VoiceSpec
	{
		const std::vector<int16_t>* chan0 = nullptr;  // channel 0 (mono uses this for both)
		const std::vector<int16_t>* chan1 = nullptr;  // channel 1 (stereo); == chan0 for mono
		double step = 1.0;                            // source samples advanced per native-rate output sample
		float gainL = 1.0f;
		float gainR = 1.0f;
		bool looped = false;
		uint32_t loopStart = 0;
		uint32_t loopEnd = 0;
		uint32_t sampleCount = 0;

		// The NW4R ADSHR envelope bytes for this note (resolved from the CbnkNote,
		// or overridden by the track's 0xB1/0xD0-0xD3 commands). The player runs the
		// real NW4R EnvGenerator over these (see Dsp.cpp) -- NOT Cbnk's SF2 timecent
		// approximations. Each byte is 0..127 in the engine's own units; 127 on decay
		// or release is the FASTEST rate (instant), the release-127 correction.
		uint8_t envAttack = 127;   // 127 = instant attack
		uint8_t envHold = 0;       // 0 = no hold
		uint8_t envDecay = 127;    // 127 = instant decay to sustain
		uint8_t envSustain = 127;  // 127 = full level (0 dB)
		uint8_t envRelease = 127;  // 127 = instant release

		// The note zone's own pan (CbnkNote.Pan, 0..127, 64 = centre); the additive
		// base the track's live 0xC0/0xDC pans offset each frame (C7, via VoiceMod).
		uint8_t notePan = 64;

		// The note's own root key vs its played key, in semitones (C8 portamento
		// glides from the previous key TO this note's key, so the target pitch is
		// this note's nominal). Retained so a live pitch modifier knows the nominal.
		int key = 60;
	};

	// The live modulation a sounding voice follows per frame (C7+). `track` points
	// at the voice's track timeline (volume/pan/pitch/LFO/LPF curves), `master` at
	// the sequence-wide master-volume curve, `noteOnSample` anchors the LFO delay.
	// When null, renderVoice runs the voice with static gain/pan/step (the C2
	// single-note proof path). The per-note sweep/portamento fields are additive
	// semitone offsets that glide to zero over their duration (C8).
	struct VoiceMod
	{
		const TrackTimeline* track = nullptr;
		const ParamCurve* master = nullptr;
		uint32_t noteOnSample = 0;

		// 0xE3 sweep: a signed intra-note ramp gliding from `sweepFromSemis` to the
		// note's nominal pitch (0 offset) over `sweepDurSamples`. Independent of and
		// additive with portamento.
		float sweepFromSemis = 0.0f;
		uint32_t sweepDurSamples = 0;

		// 0xC9/0xCE/0xCF portamento: glide from the previous key (as an offset
		// `portaFromSemis` relative to this note) to this note's key over
		// `portaDurSamples`.
		float portaFromSemis = 0.0f;
		uint32_t portaDurSamples = 0;

		// 0xC8 tie: an additive semitone-offset curve for a tie region's ONE
		// continuous voice. The first tied note sets the base pitch; each later tied
		// note steps this curve to (newKey - firstKey), retuning the LIVE voice with
		// no re-attack. Null for a non-tied voice. Owned by the sequencer for the
		// render's lifetime.
		const ParamCurve* voicePitch = nullptr;
	};

	// Convert a 0..127 volume/expression byte to a linear amplitude gain, through the
	// engine's decibel-square domain: amp = 10^(DecibelSquareTable[byte]/400). This is
	// the SAME table+conversion the envelope sustain uses (verified: it tracks
	// byte/127 to <1% across all 128 levels). Multiplying these per-source amplitudes
	// equals summing their decibel contributions -- the NW4R volume model. (C6)
	float volumeByteToAmp(int byte);

	// Convert a control-value byte (0..127, fractional allowed for a mid-ramp value)
	// to a linear amplitude gain, interpolating the DecibelSquareTable in its stored
	// decibel domain then applying amp = 10^(value/400) -- the float-domain form of
	// volumeByteToAmp a `_t` volume ramp needs. Integer bytes match volumeByteToAmp
	// exactly. (C7)
	float decibelSquareAmp(float byte);

	// Resolve (program, key, velocity) against the loaded bank and its wave
	// archives, the way Cbnk's SF2 emit walk resolves a note zone -> sample: pick
	// the instrument's key-split zone containing `key`, take its root key / volume
	// / tune, then resolve the live Cwav through the positional Cwars index.
	// Returns false for a note with no matching instrument, zone, or sample
	// (a dropped note), leaving `out` untouched. `bank` is the track's CURRENT bank
	// (the sequence default, or a 0xB6-switched one), so a mid-sequence bank change
	// re-points the instrument lookup (C10).
	bool resolveVoice(const Cbnk& bank, uint32_t program, int key, int velocity, VoiceSpec& out);

	// Render one voice into the native-rate bus: loop-aware, linearly-interpolated
	// sample fetch, shaped by the NW4R EnvGenerator (attack/hold/decay/sustain, then
	// release once the note-off at `gateSamples` is crossed), accumulated (+=) at
	// `startSample`. The voice renders past `gateSamples` for its release tail until
	// the envelope is silent. `stopSample` (absolute, on the bus) force-stops the
	// voice early -- a C5 steal / mono re-trigger / the render cap; UINT32_MAX = no
	// early stop. Deterministic; the caller drives the accumulation order by the
	// order it renders voices. `mod` (optional) makes the voice follow its track's
	// live volume/pan/pitch/LFO/LPF each frame plus its per-note sweep/portamento
	// (C7-C10); null renders the static voice (the C2 single-note proof path).
	void renderVoice(StereoBus& bus, const VoiceSpec& v, uint32_t startSample, uint32_t gateSamples,
		uint32_t stopSample = UINT32_MAX, const VoiceMod* mod = nullptr);

	// The absolute sample at which this voice falls silent (note-on at `startSample`,
	// note-off at `startSample + gateSamples`): the max of its envelope release tail
	// and its one-shot sample exhaustion, clamped to `capSample`. Frame-quantised.
	// The C5 voice allocator uses this to know when a pool slot frees; renderVoice
	// uses it to size the bus. Pure function of the VoiceSpec envelope + loop.
	uint32_t voiceEndSample(const VoiceSpec& v, uint32_t startSample, uint32_t gateSamples, uint32_t capSample);

	// One final band-limited resample of the native-rate bus to `outRate`, then
	// clamp + round to interleaved 16-bit stereo PCM (ready for writeWavPcm).
	std::vector<int16_t> finalizeToPcm(const StereoBus& busNative, uint32_t outRate);

	// C2 DSP proof: find the first playable instrument zone in the resolved bank
	// and render that one note at its root key into `bus` (1 s gate + 1 s tail,
	// velocity 100). Deterministic. Returns false if the bank has no playable note.
	bool renderSingleNote(const LoadedArchive& arch, StereoBus& bus);

	// --- The sequencer spine (C3) -------------------------------------------

	// Summary of one sequence render, for the CLI/report and audibility checks.
	struct RenderStats
	{
		uint32_t notesFired = 0;      // note events scheduled
		uint32_t notesDropped = 0;    // notes with no resolvable instrument/zone/sample
		uint32_t tracksOpened = 0;    // distinct tracks that ran
		uint64_t tickLength = 0;      // total sequence ticks stepped
		uint64_t nativeSamples = 0;   // rendered length on the 32,728 Hz bus
		double tempoBpm = 120.0;      // last tempo in force
		uint32_t timebase = 48;       // last timebase in force
		bool loopDetected = false;    // a whole-song loop-back ended a track (rendered once)
		bool cappedByMaxSeconds = false;

		// The 24-voice priority pool (C5). notesRefused: allocation refused because
		// the pool was full of higher-priority voices (the note silently did not
		// sound). voicesStolen: a sounding voice cut short because a higher-priority
		// note stole its slot. monoRetriggers: a mono track's previous voice released
		// early for a new note. maxConcurrent: peak simultaneous voices (<= 24).
		uint32_t notesRefused = 0;
		uint32_t voicesStolen = 0;      // total voices force-stopped by a steal
		uint32_t voicesStolenHeld = 0;  // of those, ones still held (audibly cut)
		uint32_t monoRetriggers = 0;
		uint32_t maxConcurrent = 0;

		// C7: `_t` ramp segments applied (a volume/pan/pitch glide over a non-zero
		// duration). instantSets counts the plain (0-duration) parameter steps.
		// longestRampTicks is the biggest single ramp duration seen (a clean fade
		// shows a large value on few ramps).
		uint32_t rampsApplied = 0;
		uint32_t instantSets = 0;
		uint32_t longestRampTicks = 0;

		// C8: tieVoices = tie regions opened (each ONE continuous voice); tieRetunes =
		// tied notes that retuned a live voice with no re-attack (folded into a region,
		// so not counted in notesFired). A tie repro shows many retunes on few voices.
		uint32_t tieVoices = 0;
		uint32_t tieRetunes = 0;

		// C9: track-LFO parameter commands executed (0xCA/CB/CC/CD/E0). A vibrato /
		// tremolo / auto-pan track shows a positive count.
		uint32_t lfoCommands = 0;

		// C10: mid-sequence bank switches executed (0xB6). A SoundData1 bank-switcher
		// shows many; the notes that were dropped without them now resolve.
		uint32_t bankSwitches = 0;

		// Opcodes the walk safe-skipped (never desyncing time): plain command byte,
		// or 0x100 | ext for an extended (0xF0-prefixed) op. For the handoff report.
		std::vector<uint32_t> skippedOps;
	};

	// --- The 24-voice priority pool (C5) ------------------------------------

	// One note competing for the 24-voice pool. `naturalEnd` is voiceEndSample for
	// the (start, gate) note; the allocator fills `stopAt` (a steal / mono re-trigger
	// force-stop, else UINT32_MAX = natural) and `sounds` (false = the pool refused
	// it). Priority is playerPriority(64) + the track's 0xC6 value. Public so the
	// allocator is independently unit-testable (the C5 stress + refuse proofs).
	struct PoolVoice
	{
		VoiceSpec v;
		uint32_t start = 0;
		uint32_t gate = 0;
		uint32_t naturalEnd = 0;
		uint32_t stopAt = UINT32_MAX;
		int priority = 64;
		int track = 0;
		bool mono = false;
		bool sounds = true;

		// C8 per-note pitch modifiers carried to the render's VoiceMod (the allocator
		// ignores them). tieRegion >= 0 selects the region pitch curve.
		float sweepFromSemis = 0.0f;
		uint32_t sweepDurSamples = 0;
		float portaFromSemis = 0.0f;
		uint32_t portaDurSamples = 0;
		int tieRegion = -1;

		uint32_t effectiveEnd() const { return (stopAt < naturalEnd) ? stopAt : naturalEnd; }
	};

	// Run the single 24-voice priority pool over `voices` (which MUST be in
	// non-decreasing `start` order, as the sequencer emits them), filling each
	// voice's `stopAt`/`sounds` and the pool RenderStats. The steal policy is
	// verbatim from docs/NW4C-disasm-handoff.md session 3: reuse a free voice; else
	// take the FRONT of the priority-sorted active list (lowest current priority,
	// released voices dropped to priority 1); REFUSE if the front outranks the
	// requester; else evict the front. `capSample` bounds a mono re-trigger's
	// recomputed release tail. Deterministic.
	void allocateVoicePool(std::vector<PoolVoice>& voices, uint32_t capSample, RenderStats& stats);

	// Render a resolved sequence (arch.seq bound to arch.bank) to the native-rate
	// bus. Runs the concurrent per-tick VM (notes/rests/program/tempo/timebase/
	// control-flow, noteWait default on) against the 160-sample frame clock,
	// producing note events that drive voices through resolveVoice/renderVoice.
	// `maxSeconds` caps a forever-looping sequence. Returns false only on a hard
	// setup error; an empty/instant sequence still returns true with 0 notes.
	// Non-const: a 0xB6 bank switch lazily builds + caches its target bank (C10).
	bool renderSequence(LoadedArchive& arch, StereoBus& bus, uint32_t maxSeconds, RenderStats& stats);
}
