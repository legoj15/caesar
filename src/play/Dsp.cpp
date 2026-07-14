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
	}

	bool resolveVoice(const LoadedArchive& arch, uint32_t program, int key, int velocity, VoiceSpec& out)
	{
		if (!arch.bank)
		{
			return false;
		}

		const Cbnk& bank = *arch.bank;

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
		float velGain = static_cast<float>(velocity) / 127.0f;
		float volGain = static_cast<float>(zone->Volume) / 127.0f;
		float gain = velGain * volGain;

		out.gainL = gain;
		out.gainR = gain;

		return true;
	}

	void renderVoice(StereoBus& bus, const VoiceSpec& v, uint32_t startSample, uint32_t gateSamples)
	{
		if (gateSamples == 0 || !v.chan0)
		{
			return;
		}

		bus.ensure(static_cast<size_t>(startSample) + gateSamples);

		// A short linear declick at each edge so a hard gate on a non-zero-crossing
		// sample does not click. This is NOT the envelope (C4 ports the real NW4R
		// EnvGenerator); it just keeps the trivial gate from buzzing.
		const uint32_t declick = min<uint32_t>(64, gateSamples / 2);

		double pos = 0.0;

		for (uint32_t s = 0; s < gateSamples; ++s)
		{
			// One-shot that ends before the gate: stop (leaves the rest as the tail).
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

			float env = 1.0f;

			if (declick > 0)
			{
				if (s < declick)
				{
					env *= static_cast<float>(s) / static_cast<float>(declick);
				}

				uint32_t remaining = gateSamples - s;

				if (remaining < declick)
				{
					env *= static_cast<float>(remaining) / static_cast<float>(declick);
				}
			}

			float sL = interp(*v.chan0, pos, v);
			float sR = interp(*v.chan1, pos, v);

			size_t idx = static_cast<size_t>(startSample) + s;
			bus.l[idx] += sL * v.gainL * env;
			bus.r[idx] += sR * v.gainR * env;

			pos += v.step;
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

				if (resolveVoice(arch, static_cast<uint32_t>(p), key, 100, v))
				{
					const uint32_t gate = kNativeRate;          // 1 s note
					bus.ensure(static_cast<size_t>(kNativeRate) * 2);  // 1 s note + 1 s tail
					renderVoice(bus, v, 0, gate);

					return true;
				}
			}
		}

		return false;
	}
}
