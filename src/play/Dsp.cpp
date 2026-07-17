#include "CaesarPlay.hpp"

#include "Cbnk.hpp"
#include "Csar.hpp"
#include "Cwar.hpp"
#include "Cwav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace std;

namespace play
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;

		// --- The track LFO (C9) --------------------------------------------------
		//
		// The disasm doc records NO LFO/sine/rate address (session 4 stopped at the
		// variable VM), so the LFO uses the NW4R precedent and every constant below is
		// FLAGGED for the console capture. rate -> Hz is anchored so a mid rate (~64)
		// gives a musical ~5 Hz vibrato; the exact scaling is one constant to
		// recalibrate. A continuous four-quadrant sine stands in for NW4R's 32-step
		// quarter-sine table (its high-resolution limit; the table quantisation is
		// inaudible here and is the flagged detail). Pitch cents = depth x range (the
		// vibrato-gating memory), clamped to +/-1 octave defensively.
		constexpr float kLfoRateHz = 5.0f / 64.0f;     // Hz per rate unit (anchored, flagged)
		constexpr float kLfoMaxCents = 1200.0f;        // defensive clamp on depth x range

		// --- The voice low-pass filter (C10, 0xD8 / 0xB4 / 0xB5) ------------------
		//
		// The disasm doc records NO filter topology. The console battery (2026-07-14)
		// measured the 0xD8 filter at time byte 48: corner ~4.1 kHz with a ~6-7 dB/oct
		// slope (a 1-POLE filter). This is therefore a bilinear-transform 1-pole
		// low-pass -- NOT the earlier RBJ 2nd-order biquad, which placed byte 48 at
		// 2,890 Hz with a 12 dB/oct slope (half an octave too dark AND twice too steep).
		// The byte->corner curve is re-anchored on the measured point (byte 48 =
		// 4.1 kHz) and keeps the converter's 187.5-cents/unit slope (UNMEASURED off
		// byte 48, FLAGGED for battery v2); 64 = fully open (no filtering). The state is
		// still the 5-coefficient Biquad with b2 = a2 = 0, so the render loop is unchanged.
		constexpr double kLpfNyquistHz = static_cast<double>(kNativeRate) / 2.0;  // 16,364 Hz
		constexpr double kLpfByte48Hz = 4100.0;   // console-measured corner @ byte 48 (battery 2026-07-14)

		struct Biquad { double b0, b1, b2, a1, a2; };

		bool lpfBiquad(float cutoffByte, Biquad& q)
		{
			if (cutoffByte >= 64.0f)
			{
				return false;   // open: pass-through
			}

			float c = cutoffByte < 0.0f ? 0.0f : cutoffByte;

			// 187.5 cents/unit around the measured byte-48 anchor; the corner is a
			// fraction of Nyquist, clamped to (0, 1].
			double octaves = (c - 48.0) * 187.5 / 1200.0;
			double frac = (kLpfByte48Hz / kLpfNyquistHz) * pow(2.0, octaves);
			frac = frac < 1e-4 ? 1e-4 : (frac > 0.999 ? 0.999 : frac);

			// 1-pole (6 dB/oct) low-pass via the bilinear transform, prewarped so the
			// -3 dB point lands on the corner. y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
			// (b2 = a2 = 0), exactly what the biquad difference equation below computes.
			double w0 = kPi * frac;                // = 2*pi*(frac*Nyquist)/rate
			double k = tan(w0 / 2.0);
			double norm = 1.0 / (k + 1.0);

			q.b0 = k * norm;
			q.b1 = k * norm;
			q.b2 = 0.0;
			q.a1 = (k - 1.0) * norm;
			q.a2 = 0.0;

			return true;
		}

		// Read one source sample (normalised to [-1, 1)), honouring the loop for a
		// looped voice and returning silence past the end of a one-shot. idx is a
		// signed index so the interpolator's i0-1 / i0+1 neighbours are safe at the
		// edges.
		float sampleAt(const vector<int16_t>& chan, int64_t idx, const VoiceSpec& v)
		{
			if (v.looped)
			{
				int64_t loopLen = static_cast<int64_t>(v.loopEnd) - static_cast<int64_t>(v.loopStart);

				if (loopLen > 0)
				{
					while (idx >= static_cast<int64_t>(v.loopEnd))
					{
						idx -= loopLen;
					}
				}
			}
			else if (idx < 0 || idx >= static_cast<int64_t>(v.sampleCount))
			{
				return 0.0f;
			}

			if (idx < 0 || idx >= static_cast<int64_t>(chan.size()))
			{
				return 0.0f;
			}

			return static_cast<float>(chan[static_cast<size_t>(idx)]) / 32768.0f;
		}

		// Linear interpolation of `chan` at fractional position `pos`.
		float interp(const vector<int16_t>& chan, double pos, const VoiceSpec& v)
		{
			int64_t i0 = static_cast<int64_t>(floor(pos));
			float frac = static_cast<float>(pos - static_cast<double>(i0));

			float s0 = sampleAt(chan, i0, v);
			float s1 = sampleAt(chan, i0 + 1, v);

			return s0 * (1.0f - frac) + s1 * frac;
		}

		// A 4-term Blackman-Harris window sampled at t in [0, 1] (~ -92 dB
		// sidelobes) -- the taper for the final-resample sinc kernel.
		double blackmanHarris(double t)
		{
			const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
			const double x = 2.0 * kPi * t;

			return a0 - a1 * cos(x) + a2 * cos(2.0 * x) - a3 * cos(3.0 * x);
		}

		double sinc(double x)
		{
			if (x == 0.0)
			{
				return 1.0;
			}

			const double px = kPi * x;

			return sin(px) / px;
		}

		// --- The NW4R EnvGenerator (C4) ------------------------------------------
		//
		// A direct port of the 3DS NW4C `nw::snd::internal::EnvGenerator`, which the
		// disasm (docs/NW4C-disasm-handoff.md) proved is a behavioural 1:1 of the Wii
		// NW4R `EnvGenerator`. This REPLACES the SF2 timecent approximations in
		// Cbnk.cpp (Attack/Hold/DecayTable/ConvertTime) -- those exist for sf2cute,
		// not the engine. Provenance of every constant is documented inline.

		// DecibelSquareTable[128] (s16) and attackTable[128] (f32): read BYTE-FOR-BYTE
		// from StreetPass Mii Plaza `code.bin` at vaddr 0x328844 / 0x328944 (file
		// offset vaddr-0x100000), the exact addresses NW4C-disasm-handoff.md records.
		// Both are identical to the Wii NW4R ogws tables. The head/tail match the
		// doc's fingerprint (DecibelSquareTable -723,-722,-721,-651,...,-1,0;
		// attackTable 0.9992175...0.0).
		const int16_t DecibelSquareTable[128] =
		{
			-723, -722, -721, -651, -601, -562, -530, -503,
			-480, -460, -442, -425, -410, -396, -383, -371,
			-360, -349, -339, -330, -321, -313, -305, -297,
			-289, -282, -276, -269, -263, -257, -251, -245,
			-239, -234, -229, -224, -219, -214, -210, -205,
			-201, -196, -192, -188, -184, -180, -176, -173,
			-169, -165, -162, -158, -155, -152, -149, -145,
			-142, -139, -136, -133, -130, -127, -125, -122,
			-119, -116, -114, -111, -109, -106, -103, -101,
			-99, -96, -94, -91, -89, -87, -85, -82,
			-80, -78, -76, -74, -72, -70, -68, -66,
			-64, -62, -60, -58, -56, -54, -52, -50,
			-49, -47, -45, -43, -42, -40, -38, -36,
			-35, -33, -31, -30, -28, -27, -25, -23,
			-22, -20, -19, -17, -16, -14, -13, -11,
			-10, -8, -7, -6, -4, -3, -1, 0,
		};

		const float attackTable[128] =
		{
			0.99921751f, 0.998432577f, 0.997645199f, 0.996855319f, 0.996062875f, 0.995267928f,
			0.994470417f, 0.993670404f, 0.992867708f, 0.992062509f, 0.991254628f, 0.990444124f,
			0.989630878f, 0.988815129f, 0.987996519f, 0.987175226f, 0.986351192f, 0.985524416f,
			0.984694898f, 0.983862519f, 0.983027279f, 0.982189298f, 0.981348276f, 0.980504513f,
			0.979657829f, 0.978808105f, 0.97795552f, 0.977099895f, 0.976241291f, 0.975379705f,
			0.974515021f, 0.973647177f, 0.972776294f, 0.971902311f, 0.971025109f, 0.970144808f,
			0.969261229f, 0.968374372f, 0.967484415f, 0.966591001f, 0.965694427f, 0.964794397f,
			0.963891029f, 0.962984204f, 0.962073982f, 0.961160421f, 0.960243285f, 0.959322572f,
			0.958398402f, 0.957470596f, 0.956539214f, 0.955604196f, 0.954665482f, 0.953723073f,
			0.952776909f, 0.95182699f, 0.950873196f, 0.949915707f, 0.948954225f, 0.947988808f,
			0.947019517f, 0.946046174f, 0.945068896f, 0.944087505f, 0.943102002f, 0.942112386f,
			0.941118598f, 0.940120578f, 0.939118385f, 0.938111782f, 0.937100887f, 0.936085582f,
			0.935065925f, 0.934041679f, 0.933013082f, 0.931979775f, 0.930941999f, 0.929899514f,
			0.92885232f, 0.927800417f, 0.926743627f, 0.925682127f, 0.924615622f, 0.923544228f,
			0.922467828f, 0.921386421f, 0.920299828f, 0.919208109f, 0.918111205f, 0.917009115f,
			0.915901601f, 0.914788723f, 0.913670301f, 0.912546515f, 0.911417127f, 0.910282075f,
			0.909141421f, 0.907994926f, 0.906842709f, 0.905684471f, 0.904520392f, 0.903350174f,
			0.902173996f, 0.900991619f, 0.899802923f, 0.898608029f, 0.897406578f, 0.896198809f,
			0.894984424f, 0.890059888f, 0.882462204f, 0.875924706f, 0.869186103f, 0.863640606f,
			0.853578806f, 0.843018889f, 0.82861352f, 0.814909875f, 0.800217211f, 0.778066278f,
			0.755474985f, 0.724212527f, 0.682823896f, 0.632916927f, 0.559213519f, 0.455141097f,
			0.329876989f, 0.0f,
		};

		// CalcRelease: ported VERBATIM from the disasm (docs/NW4C-disasm-handoff.md,
		// re-confirmed by this session's own disasm at MiiPlaza 0x201D60/0x201E3C).
		// Byte 127 -> 65535/ms = the FASTEST rate (instant): this is the release-127
		// correction. One curve serves both decay and release. Rates are per-ms.
		float calcRelease(int x)
		{
			if (x == 127) return 65535.0f;
			if (x == 126) return 24.0f;
			if (x < 50)   return (x * 2 + 1) / 128.0f / 5.0f;
			return 60.0f / (126 - x) / 5.0f;
		}

		// SetHold, from the disasm at MiiPlaza 0x201D40: the hold DURATION in ms is
		// round((hold+1)^2 / 4). (0 -> 0, 1 -> 1, 4 -> 6, 127 -> 4096 ms.)
		float holdMsFromByte(int h)
		{
			int v = (h + 1) * (h + 1);
			return static_cast<float>((v + 2) / 4);  // +2 = round-to-nearest of /4
		}

		// The envelope updates once per DSP frame (160 samples / 32728 Hz = 4.889 ms),
		// matching the sequence runtime's frame clock. NW4R's Update takes an integer
		// msec per sound frame; the exact 3DS cadence is unconfirmed (a Net-B item).
		const double kMsPerFrame = 1000.0 * static_cast<double>(kFrameSamples) / static_cast<double>(kNativeRate);

		// The envelope value floor / attack start. NW4R's exact reset constant is not
		// byte-confirmed in this binary (a Net-B item); -2000 in the value domain maps
		// (see gainFromValue) to ~1e-10 amplitude -- inaudible -- so attack rises from
		// silence and release settles to silence. Decay/sustain timing is INDEPENDENT
		// of this (they use the fixed DecibelSquareTable range), so only the slow-attack
		// rise-time and the release tail-to-silence depend on it.
		const float kEnvFloor = -2000.0f;
		const float kAttackDone = -0.03125f;  // -1/32: attack completes near 0 (NW4R; flagged)
		const float kStopGain = 1.0f / 32768.0f;  // 16-bit LSB: below this the voice is silent -> Stop()

		// Convert an envelope value to a LINEAR amplitude gain: amplitude =
		// 10^(value/200). The table stores 400*log10(v/127) ("the dB of the square"),
		// and the engine applies value/10 as PLAIN dB on amplitude, so a byte v maps
		// to (v/127)^2 amplitude -- the same concave curve the SF2 exporter writes
		// (ConvertVolume) and BASSMIDI/FluidSynth apply to CC7/CC11. The /400
		// (byte-linear) reading was refuted 2026-07-15 by the SEQ_SD_BGM_RESULT
		// three-way A/B (console + BASSMIDI kick prominence ~+4 dB vs the /400
		// render's +0.7; /200 lands +5.1), and independently by the battery's decay
		// slope (console -174 dB/s vs -94 modelled: the divisor is the missing x2 --
		// the disasm-verbatim calcRelease rates were never wrong). The INFO volume
		// byte's console-confirmed LINEAR vol/127 law is a separate CPU-side f32
		// multiply, not this pipeline, and never pinned this divisor.
		float gainFromValue(float value)
		{
			if (value <= kEnvFloor) return 0.0f;
			if (value >= 0.0f) return 1.0f;
			return powf(10.0f, value / 200.0f);
		}

		// The ported EnvGenerator state machine. Phases: Attack (multiply toward full),
		// Hold (dwell at peak), Decay (rate down to Sustain level), Sustain (hold),
		// Release (rate down on note-off). Update runs once per frame.
		struct EnvGen
		{
			enum Status { Attack, Hold, Decay, Sustain, Release, Done };

			Status status = Attack;
			float value = kEnvFloor;
			float attackMul = 0.0f;
			float decayRate = 0.0f;
			float releaseRate = 0.0f;
			float holdRemain = 0.0f;
			int sustainIdx = 127;

			EnvGen(uint8_t a, uint8_t h, uint8_t d, uint8_t s, uint8_t r)
			{
				attackMul = attackTable[a & 0x7F];
				decayRate = calcRelease(d & 0x7F);
				releaseRate = calcRelease(r & 0x7F);
				holdRemain = holdMsFromByte(h & 0x7F);
				sustainIdx = s & 0x7F;
			}

			// Note-off: drop into Release from wherever the value currently sits.
			void noteOff()
			{
				if (status != Done && status != Release)
				{
					status = Release;
				}
			}

			void advance(double ms)
			{
				switch (status)
				{
					case Attack:
						// mValue *= attackMul each ms (compounded over the frame). For
						// attackMul == 0 (byte 127) this snaps to 0 in one step: instant.
						value *= powf(attackMul, static_cast<float>(ms));

						if (value > kAttackDone)
						{
							value = 0.0f;
							status = (holdRemain > 0.0f) ? Hold : Decay;
						}
						break;

					case Hold:
						holdRemain -= static_cast<float>(ms);

						if (holdRemain <= 0.0f)
						{
							status = Decay;
						}
						break;

					case Decay:
					{
						value -= decayRate * static_cast<float>(ms);
						float target = static_cast<float>(DecibelSquareTable[sustainIdx]);

						if (value <= target)
						{
							value = target;
							status = Sustain;
						}
						break;
					}

					case Sustain:
						break;

					case Release:
						value -= releaseRate * static_cast<float>(ms);

						if (value <= kEnvFloor || gainFromValue(value) <= kStopGain)
						{
							value = kEnvFloor;
							status = Done;
						}
						break;

					case Done:
						break;
				}
			}

			float gain() const { return gainFromValue(value); }
			bool done() const { return status == Done; }
		};
	}

	uint32_t voiceEndSample(const VoiceSpec& v, uint32_t startSample, uint32_t gateSamples, uint32_t capSample)
	{
		// The envelope release tail (frame-stepped, coarse). Runs the same EnvGen the
		// render uses, counting frames until Done.
		EnvGen env(v.envAttack, v.envHold, v.envDecay, v.envSustain, v.envRelease);

		uint32_t s = 0;
		uint32_t budget = (capSample > startSample) ? (capSample - startSample) : 0;

		while (s < budget)
		{
			if (s >= gateSamples)
			{
				env.noteOff();
			}

			env.advance(kMsPerFrame);
			s += kFrameSamples;

			if (env.done())
			{
				// The frame whose advance reached Done IS the final declick ramp
				// (renderVoice ramps gPrev -> gNext=0 across it), so it counts as part
				// of the voice's life. Excluding it dropped the whole sustain->0 ramp
				// for release byte 127 (instant), cutting every such note-off dead at
				// sustain gain (the 2026-07-14 first-listen diagnosis).
				break;
			}
		}

		uint32_t envEnd = (s < budget) ? s : budget;

		// One-shot sample exhaustion: a non-looped voice cannot sound past its PCM.
		uint32_t audioEnd = budget;

		if (!v.looped && v.step > 0.0)
		{
			double samples = static_cast<double>(v.sampleCount) / v.step;
			audioEnd = static_cast<uint32_t>(samples < static_cast<double>(budget) ? samples + 1.0 : static_cast<double>(budget));
		}

		uint32_t end = min(envEnd, audioEnd);

		return startSample + end;
	}

	float volumeByteToAmp(int byte)
	{
		int b = byte < 0 ? 0 : (byte > 127 ? 127 : byte);

		return gainFromValue(static_cast<float>(DecibelSquareTable[b]));
	}

	float decibelSquareAmp(float byte)
	{
		float b = byte < 0.0f ? 0.0f : (byte > 127.0f ? 127.0f : byte);

		int lo = static_cast<int>(b);
		int hi = (lo < 127) ? lo + 1 : 127;
		float frac = b - static_cast<float>(lo);

		// Interpolate in the table's stored decibel domain (NW4R CalcDecibelSquare
		// lerps adjacent table entries), then convert. Integer bytes match
		// volumeByteToAmp exactly (frac == 0).
		float value = static_cast<float>(DecibelSquareTable[lo]) * (1.0f - frac)
			+ static_cast<float>(DecibelSquareTable[hi]) * frac;

		return gainFromValue(value);
	}

	bool resolveVoice(const Cbnk& bank, uint32_t program, int key, int velocity, VoiceSpec& out)
	{
		if (program >= bank.Insts.size() || !bank.Insts[program].Exists)
		{
			return false;
		}

		const CbnkInst& inst = bank.Insts[program];

		// The key-split zone containing `key` (the same [StartNote, EndNote] test
		// Cbnk's SF2 zones carry: 0x6000 is one zone over 0..127, 0x6001 a split
		// list, 0x6002 one zone per key).
		const CbnkNote* zone = nullptr;

		for (const CbnkNote& n : inst.Notes)
		{
			if (n.Exists && key >= n.StartNote && key <= n.EndNote)
			{
				zone = &n;
				break;
			}
		}

		if (!zone || !zone->Cwav || zone->Cwav->Id >= 0xF000)
		{
			return false;
		}

		// Resolve the live wave archive by the SAME positional index Cbnk::Export
		// uses (advance Cwars.begin() by the stored Cwar index, counting null slots).
		const map<int, Cwar*>* cwars = bank.Cwars;

		if (!cwars)
		{
			return false;
		}

		size_t k = 0;
		auto it = cwars->begin();

		for (; it != cwars->end(); ++it, ++k)
		{
			if (k == zone->Cwav->Cwar)
			{
				break;
			}
		}

		if (it == cwars->end() || it->second == nullptr
			|| zone->Cwav->Id >= it->second->Cwavs.size()
			|| !it->second->Cwavs[zone->Cwav->Id]->Converted)
		{
			return false;
		}

		const Cwav& src = *it->second->Cwavs[zone->Cwav->Id];

		if (src.Channels.empty() || src.Channels[0].empty())
		{
			return false;
		}

		out.chan0 = &src.Channels[0];
		out.chan1 = (src.ChanCount >= 2 && src.Channels.size() >= 2) ? &src.Channels[1] : out.chan0;
		out.sampleCount = static_cast<uint32_t>(src.Channels[0].size());

		// Loop points come from the same smpl-chunk rule Cbnk uses: SampleMode odd
		// = looped, [LoopStart, LoopEnd). Guard against malformed points so a bad
		// loop can never spin.
		out.looped = (src.SampleMode % 2) != 0
			&& src.LoopEnd > src.LoopStart
			&& src.LoopEnd <= out.sampleCount;
		out.loopStart = src.LoopStart;
		out.loopEnd = out.looped ? src.LoopEnd : out.sampleCount;

		// Playback rate: the sample's own rate relative to the native bus, times the
		// key/root-key semitone ratio and the note's Tune (Note 0x24 f32 multiplier).
		double semis = static_cast<double>(key) - static_cast<double>(zone->RootKey);
		double pitch = pow(2.0, semis / 12.0) * static_cast<double>(zone->Tune);

		out.step = (static_cast<double>(src.SampleRate) / static_cast<double>(kNativeRate)) * pitch;

		// Velocity -> linear gain (C3), folded with the note zone's own volume
		// (the instrument's design level, the SF2 initialAttenuation analogue).
		// Native volume/pan/expression commands are C6.
		// Velocity -> gain: (vel/127)^2. Linear-squared is the NW4R velocity precedent
		// (flagged for Net-B). The note zone's design volume goes through the engine's
		// decibel-square domain, like every other volume byte.
		float velGain = static_cast<float>(velocity) / 127.0f;
		velGain *= velGain;
		float volGain = volumeByteToAmp(static_cast<int>(zone->Volume));
		float gain = velGain * volGain;

		out.gainL = gain;
		out.gainR = gain;
		out.notePan = static_cast<uint8_t>(zone->Pan > 127 ? 64 : zone->Pan);
		out.key = key;

		// The note's ADSHR envelope bytes (the same fields Cbnk parses at note+0x38);
		// the player runs the NW4R EnvGenerator over them. Per-track 0xB1/0xD0-0xD3
		// overrides are applied by the caller (they are track state, not note state).
		out.envAttack = zone->Attack;
		out.envHold = zone->Hold;
		out.envDecay = zone->Decay;
		out.envSustain = zone->Sustain;
		out.envRelease = zone->Release;

		return true;
	}

	// The persistent track LFO's contribution at absolute sample `absPos` (C9). ONE
	// LfoParam per track, retargeted by lfoTarget (0 pitch / 1 volume / 2 pan), with
	// a per-note delay (0xE0) before it engages. `targetOut` returns the live target.
	// The return is in the target's native units: pitch -> semitones; volume/pan -> a
	// control-value delta. Zero when depth/rate is zero, before the delay, or for an
	// out-of-range target (the engine applies no LFO there).
	float lfoValue(const TrackTimeline& tl, uint32_t noteOn, uint32_t absPos, int& targetOut)
	{
		int target = static_cast<int>(tl.lfoTarget.valueAt(absPos) + 0.5f);
		targetOut = target;

		float depth = tl.lfoDepth.valueAt(absPos);
		float rate = tl.lfoRate.valueAt(absPos);

		if (depth <= 0.0f || rate <= 0.0f || target > 2 || absPos < noteOn)
		{
			return 0.0f;
		}

		float delayMs = tl.lfoDelayMs.valueAt(absPos);
		float elapsedMs = 1000.0f * static_cast<float>(absPos - noteOn) / static_cast<float>(kNativeRate);

		if (elapsedMs < delayMs)
		{
			return 0.0f;
		}

		float hz = rate * kLfoRateHz;
		float phase = 2.0f * static_cast<float>(kPi) * hz * (elapsedMs - delayMs) / 1000.0f;
		float sine = sinf(phase);   // four-quadrant

		if (target == 0)
		{
			// Pitch: peak deviation cents = depth x range (memory), clamped defensively.
			float cents = depth * tl.lfoRange.valueAt(absPos);
			cents = cents > kLfoMaxCents ? kLfoMaxCents : cents;

			return sine * cents / 100.0f;   // -> semitones
		}

		// Volume (1) / pan (2): a control-value delta scaled by depth.
		return sine * depth;
	}

	// The additive semitone offset a per-note glide (sweep 0xE3 or portamento
	// 0xC9/CE/CF) contributes at absolute sample `absPos`: it starts at `fromSemis`
	// and ramps LINEARLY to 0 over `durSamples`, then stays 0. Sweep and portamento
	// are independent and additive (2026-07-11 research).
	float glideOffset(float fromSemis, uint32_t durSamples, uint32_t noteOn, uint32_t absPos)
	{
		if (fromSemis == 0.0f || durSamples == 0 || absPos < noteOn)
		{
			return 0.0f;
		}

		uint32_t elapsed = absPos - noteOn;

		if (elapsed >= durSamples)
		{
			return 0.0f;
		}

		return fromSemis * (1.0f - static_cast<float>(elapsed) / static_cast<float>(durSamples));
	}

	void renderVoice(StereoBus& bus, const VoiceSpec& v, uint32_t startSample, uint32_t gateSamples,
		uint32_t stopSample, const VoiceMod* mod)
	{
		if (!v.chan0)
		{
			return;
		}

		// The voice lives from note-on until the NW4R envelope goes silent (its
		// release tail extends past gateSamples). voiceEndSample runs the same
		// EnvGen forward to find that length; an early `stopSample` (a C5 steal /
		// mono re-trigger / render cap) cuts it shorter.
		uint32_t cap = (stopSample == UINT32_MAX) ? UINT32_MAX : stopSample;
		uint32_t endSample = voiceEndSample(v, startSample, gateSamples, cap);

		if (endSample <= startSample)
		{
			return;
		}

		// A force-stop (steal / mono re-trigger / render cap) lands while the
		// envelope is still audible. Hardware interpolates per-voice gain across the
		// next DSP frame, so a stopped voice fades over ~4.9 ms instead of stopping
		// dead -- the 8.06 s EMPTY_LANDSCAPE click (2026-07-14 diagnosis). Render
		// ONE declick frame past the cut, linearly ramped to zero. An envelope-
		// completed voice needs none (its own final frame already ramps to silence),
		// and a one-shot's PCM exhaustion steps exactly as hardware does.
		uint32_t declickStart = UINT32_MAX;

		if (cap != UINT32_MAX && endSample == cap)
		{
			declickStart = endSample;
			endSample += kFrameSamples;
		}

		uint32_t length = endSample - startSample;

		bus.ensure(static_cast<size_t>(endSample));

		// Re-run the envelope in lockstep with the sample fetch (deterministic, so it
		// tracks voiceEndSample exactly). The envelope updates once per frame; its
		// gain is linearly ramped across the 160 samples of each frame to avoid a
		// zipper. It starts at 0 (silence) and releases back to 0, so an envelope-
		// terminated voice needs no hard gate; the force-stop declick frame above
		// covers the one case that ends at audible gain.
		EnvGen env(v.envAttack, v.envHold, v.envDecay, v.envSustain, v.envRelease);

		// The per-frame modulated gain / step (constant across a frame, matching the
		// engine's per-frame parameter cadence). When `mod` is null they stay the
		// voice's static values, so the C2 single-note path is byte-for-byte as before.
		float mgL = v.gainL;
		float mgR = v.gainR;
		double effStep = v.step;

		// C10 voice LPF (0xD8/B4/B5): an RBJ biquad, coefficients recomputed per frame
		// from the live cutoff, per-channel state carried across the note. Inactive
		// (pass-through) while the cutoff is at/above the open point.
		Biquad bq{};
		bool filtOn = false;
		double lx1 = 0, lx2 = 0, ly1 = 0, ly2 = 0;
		double rx1 = 0, rx2 = 0, ry1 = 0, ry2 = 0;

		double pos = 0.0;
		float gCur = env.gain();
		float gStep = 0.0f;

		for (uint32_t s = 0; s < length; ++s)
		{
			if (s % kFrameSamples == 0)
			{
				if (s >= gateSamples)
				{
					env.noteOff();
				}

				float gPrev = env.gain();
				env.advance(kMsPerFrame);
				float gNext = env.gain();

				gCur = gPrev;
				gStep = (gNext - gPrev) / static_cast<float>(kFrameSamples);

				if (mod)
				{
					uint32_t absPos = startSample + s;

					// Volume: track volume x expression x master, all in the
					// control-value domain, converted per frame (a `_t` volume fade
					// glides the byte linearly and lands in the dB table each frame).
					float volAmp = 1.0f;
					float panPos = static_cast<float>(v.notePan);
					double semis = 0.0;

					if (mod->track)
					{
						const TrackTimeline& tl = *mod->track;

						volAmp = decibelSquareAmp(tl.volume.valueAt(absPos))
							* decibelSquareAmp(tl.expression.valueAt(absPos));

						panPos += (tl.pan.valueAt(absPos) - 64.0f) + (tl.initPan.valueAt(absPos) - 64.0f);

						semis = (tl.bend.valueAt(absPos) / 128.0) * tl.bendRange.valueAt(absPos)
							+ tl.transpose.valueAt(absPos);

						// The persistent track LFO (C9), routed to its live target. The
						// single param block persists across a retarget (the curves hold
						// their values), so a value commanded on any target survives it.
						int lfoTarget = 0;
						float lfo = lfoValue(tl, mod->noteOnSample, absPos, lfoTarget);

						if (lfo != 0.0f)
						{
							if (lfoTarget == 0)      { semis += lfo; }   // pitch vibrato (semitones)
							else if (lfoTarget == 1)                     // tremolo: volume delta in dB units
							{
								volAmp *= decibelSquareAmp(127.0f + lfo) / decibelSquareAmp(127.0f);
							}
							else if (lfoTarget == 2) { panPos += lfo; } // auto-pan: control-value delta
						}
					}

					if (mod->master)
					{
						volAmp *= decibelSquareAmp(mod->master->valueAt(absPos));
					}

					// Per-note pitch modifiers (C8): tie retune (a stepped offset), then
					// sweep + portamento glides, all additive semitones on top of the
					// track bend/transpose.
					if (mod->voicePitch)
					{
						semis += mod->voicePitch->valueAt(absPos);
					}

					semis += glideOffset(mod->sweepFromSemis, mod->sweepDurSamples, mod->noteOnSample, absPos);
					semis += glideOffset(mod->portaFromSemis, mod->portaDurSamples, mod->noteOnSample, absPos);

					panPos = panPos < 0.0f ? 0.0f : (panPos > 127.0f ? 127.0f : panPos);

					double angle = static_cast<double>(panPos) / 127.0 * (kPi / 2.0);
					mgL = v.gainL * volAmp * static_cast<float>(cos(angle));
					mgR = v.gainR * volAmp * static_cast<float>(sin(angle));

					effStep = (semis != 0.0) ? v.step * pow(2.0, semis / 12.0) : v.step;

					// The voice LPF cutoff (0xD8/B4/B5), recomputed for this frame.
					if (mod->track)
					{
						filtOn = lpfBiquad(mod->track->lpfCutoff.valueAt(absPos), bq);
					}
				}
			}

			// One-shot that ends before the envelope (guarded by voiceEndSample, but
			// keep the runtime guard so a rounding edge can never read past the PCM).
			if (!v.looped && pos >= static_cast<double>(v.sampleCount))
			{
				break;
			}

			if (v.looped && v.loopEnd > v.loopStart)
			{
				double loopLen = static_cast<double>(v.loopEnd) - static_cast<double>(v.loopStart);

				while (pos >= static_cast<double>(v.loopEnd))
				{
					pos -= loopLen;
				}
			}

			float sL = interp(*v.chan0, pos, v);
			float sR = interp(*v.chan1, pos, v);

			if (filtOn)
			{
				double yl = bq.b0 * sL + bq.b1 * lx1 + bq.b2 * lx2 - bq.a1 * ly1 - bq.a2 * ly2;
				lx2 = lx1; lx1 = sL; ly2 = ly1; ly1 = yl; sL = static_cast<float>(yl);
				double yr = bq.b0 * sR + bq.b1 * rx1 + bq.b2 * rx2 - bq.a1 * ry1 - bq.a2 * ry2;
				rx2 = rx1; rx1 = sR; ry2 = ry1; ry1 = yr; sR = static_cast<float>(yr);
			}

			float g = gCur;

			if (startSample + s >= declickStart)
			{
				g *= static_cast<float>(endSample - (startSample + s)) / static_cast<float>(kFrameSamples);
			}

			size_t idx = static_cast<size_t>(startSample) + s;
			bus.l[idx] += sL * mgL * g;
			bus.r[idx] += sR * mgR * g;

			gCur += gStep;
			pos += effStep;
		}
	}

	vector<int16_t> finalizeToPcm(const StereoBus& busNative, uint32_t outRate)
	{
		const size_t inCount = busNative.frames();

		if (inCount == 0)
		{
			return {};
		}

		// Windowed-sinc polyphase resampler. The mix bus is band-limited to the
		// native Nyquist (16.364 kHz) by construction, so for the common upsample
		// (48/96 kHz) this is provably transparent; for a downsample the cutoff
		// tracks the output Nyquist. A Blackman-Harris-tapered sinc, HALF=16 (32
		// taps), evaluated through a 2048-phase table -- deterministic, and well
		// below the accuracy ceiling (which is the per-voice linear interpolation
		// and the not-yet-native envelope).
		const int HALF = 16;
		const int TAPS = 2 * HALF;
		const int NPHASE = 2048;

		const double fc = min(1.0, static_cast<double>(outRate) / static_cast<double>(kNativeRate));

		vector<float> kernel(static_cast<size_t>(NPHASE) * TAPS);

		for (int p = 0; p < NPHASE; ++p)
		{
			double frac = static_cast<double>(p) / static_cast<double>(NPHASE);

			for (int j = 0; j < TAPS; ++j)
			{
				double x = static_cast<double>(j - (HALF - 1)) - frac;
				double win = blackmanHarris((x / static_cast<double>(HALF) + 1.0) / 2.0);

				kernel[static_cast<size_t>(p) * TAPS + j] = static_cast<float>(fc * sinc(fc * x) * win);
			}
		}

		const double ratio = static_cast<double>(kNativeRate) / static_cast<double>(outRate);
		const size_t outCount = static_cast<size_t>(static_cast<double>(inCount) / ratio);

		vector<int16_t> pcm;
		pcm.reserve(outCount * 2);

		const vector<float>& L = busNative.l;
		const vector<float>& R = busNative.r;

		for (size_t o = 0; o < outCount; ++o)
		{
			double pos = static_cast<double>(o) * ratio;
			int64_t base = static_cast<int64_t>(floor(pos));
			double frac = pos - static_cast<double>(base);

			int p = static_cast<int>(frac * static_cast<double>(NPHASE));

			if (p >= NPHASE)
			{
				p = NPHASE - 1;
			}

			const float* krow = &kernel[static_cast<size_t>(p) * TAPS];

			float accL = 0.0f;
			float accR = 0.0f;

			for (int j = 0; j < TAPS; ++j)
			{
				int64_t idx = base + (j - (HALF - 1));

				if (idx >= 0 && idx < static_cast<int64_t>(inCount))
				{
					float k = krow[j];
					accL += L[static_cast<size_t>(idx)] * k;
					accR += R[static_cast<size_t>(idx)] * k;
				}
			}

			auto toPcm = [](float sample) -> int16_t
			{
				float c = clamp(sample, -1.0f, 1.0f);

				return static_cast<int16_t>(lround(c * 32767.0f));
			};

			pcm.push_back(toPcm(accL));
			pcm.push_back(toPcm(accR));
		}

		return pcm;
	}

	bool renderSingleNote(const LoadedArchive& arch, StereoBus& bus)
	{
		if (!arch.bank)
		{
			return false;
		}

		const Cbnk& bank = *arch.bank;

		// Deterministic pick: the lowest instrument program with a zone that
		// resolves to a sample, played at that zone's own root key, velocity 100.
		for (size_t p = 0; p < bank.Insts.size(); ++p)
		{
			if (!bank.Insts[p].Exists)
			{
				continue;
			}

			for (const CbnkNote& n : bank.Insts[p].Notes)
			{
				if (!n.Exists)
				{
					continue;
				}

				int key = static_cast<int>(n.RootKey);
				VoiceSpec v;

				if (resolveVoice(bank, static_cast<uint32_t>(p), key, 100, v))
				{
					// A 1 s gate: the NW4R envelope now shapes attack/hold/decay/
					// sustain over the note and its release tail past the gate (bus
					// sized by renderVoice). Cap the release tail at 4 s so a slow
					// release cannot run unbounded in this DSP-proof fixture.
					const uint32_t gate = kNativeRate;  // 1 s note-on
					renderVoice(bus, v, 0, gate, kNativeRate * 5);

					return true;
				}
			}
		}

		return false;
	}
}
