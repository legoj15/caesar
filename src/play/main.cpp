// caesar-play — the suite stage-2 offline dry-render CLI. Renders a chosen
// sequence from a .bcsar to a .wav with no audio device. A development/suite
// tool, NOT part of the converter release.
//
//   caesar-play --list        <archive.bcsar>
//   caesar-play --render      <archive.bcsar> --seq <name-or-index> --out <file.wav>
//   caesar-play --render-note <archive.bcsar> --seq <name-or-index> --out <file.wav>
//   caesar-play --zones       <archive.bcsar> [--wave <warFileId>:<waveIndex>]
//   caesar-play --trace       <archive.bcsar> [--seq <name-or-index>] [--wave <warFileId>:<waveIndex>]
//
// --render runs the concurrent sequencer (C3) -> voices -> native mix bus ->
// one final resample -> WAV; --render-note is the C2 single-voice DSP proof;
// --list names an archive's renderable sequences.
//
// --zones and --trace are read-only diagnostics that answer "which bank zones and
// which sequence notes play this wave, and at what pitch, level and envelope".
// Both print TSV to stdout (a header row, then rows; '#'-prefixed lines are
// summaries) and write no audio.
//   --zones  one row per existing zone of every in-archive bank: where the zone
//            sits (bank, program, key range), the wave it references (raw index,
//            wave-archive position resolved to a file id the way the player does,
//            wave index, rate, frames, loop), its pitch/level/envelope fields and
//            the unnamed words in hex, and a status: "ok" or why the zone cannot
//            sound (";substituted-index" flags a raw index past the bank's sample
//            table, which the parser repoints at sample 0).
//   --trace  runs the same sequence VM as --render (every renderable sequence, or
//            one chosen exactly as --render chooses it) up to the point where note
//            events resolve to voices, then stops: one row per note event with the
//            bank/zone/wave it resolved to, key - RootKey + Tune as semitones and
//            playback speed, the envelope used, sweep/portamento, and the track's
//            mix state at note-on (volume, expression, pan, transpose, bend, fx/main
//            sends). Status is "fired" (resolved to a voice; the 24-voice pool is
//            NOT run, so this is not the --render "fired" count, which counts every
//            scheduled note) or "dropped:<reason>". Random note lengths stand in
//            their midpoint, as in --render, and are marked len_src=rnd.
//            semitones/speed are the static zone ratio (the track's live transpose,
//            bend, sweep and portamento, printed beside them, ride on top); the
//            track values are the timeline's value at the note's start.
//            gate_s is the gate as the sequence wrote it: a damper (0xDF) held
//            past the note-off makes the voice sound longer in --render, and a
//            written length of 0 prints as 0 (hardware plays such a note's wave
//            to its end; see docs/ROADMAP.md, known bugs).
//   --wave   keeps only the zones / notes whose zone resolves to that wave-archive
//            file id and wave index.

#include "CaesarPlay.hpp"

#include "Cbnk.hpp"
#include "Csar.hpp"
#include "Cwar.hpp"
#include "Cwav.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std;

namespace
{
	constexpr uint32_t kDefaultRate = 48000;
	constexpr uint32_t kDefaultMaxSeconds = 300;

	void printUsage()
	{
		cerr <<
			"caesar-play — offline dry renderer for BCSAR sequences (suite stage 2)\n"
			"\n"
			"usage:\n"
			"  caesar-play --list        <archive.bcsar>\n"
			"  caesar-play --render      <archive.bcsar> --seq <name-or-index> --out <file.wav>\n"
			"              [--rate <hz>] [--max-seconds <n>]\n"
			"  caesar-play --render-note <archive.bcsar> --seq <name-or-index> --out <file.wav>\n"
			"              [--rate <hz>]\n"
			"  caesar-play --zones       <archive.bcsar> [--wave <warFileId>:<waveIndex>]\n"
			"  caesar-play --trace       <archive.bcsar> [--seq <name-or-index>]\n"
			"              [--wave <warFileId>:<waveIndex>] [--max-seconds <n>]\n"
			"\n"
			"options:\n"
			"  --render-note       DSP proof: render the resolved bank's first note (C2)\n"
			"  --zones             TSV of every bank zone: its wave, pitch/level/envelope, status\n"
			"  --trace             TSV of every note event of every sequence (or the --seq one):\n"
			"                      the zone/wave it resolves to, pitch, envelope, track mix state,\n"
			"                      fired or dropped:<reason>; no audio\n"
			"  --wave <war>:<idx>  with --zones/--trace, keep only rows resolving to that wave\n"
			"                      (wave-archive FILE id : wave index inside it)\n"
			"  --rate <hz>         output sample rate (default 48000)\n"
			"  --max-seconds <n>   safety cap on render length (default 300)\n"
			"  --version           print version and exit\n";
	}

	// Resolve --seq: an exact name match first, else a numeric index into the
	// renderable-sequence list. Returns nullptr when neither resolves.
	const play::SequenceInfo* resolveChoice(const vector<play::SequenceInfo>& seqs, const string& choice)
	{
		for (const play::SequenceInfo& s : seqs)
		{
			if (s.name == choice)
			{
				return &s;
			}
		}

		// Numeric index fallback (all-digits).
		if (!choice.empty() && choice.find_first_not_of("0123456789") == string::npos)
		{
			uint32_t idx = static_cast<uint32_t>(strtoul(choice.c_str(), nullptr, 10));

			for (const play::SequenceInfo& s : seqs)
			{
				if (s.index == idx)
				{
					return &s;
				}
			}
		}

		return nullptr;
	}

	int doList(const string& archivePath)
	{
		auto arch = play::loadArchive(archivePath);

		if (!arch)
		{
			return 1;
		}

		vector<play::SequenceInfo> seqs = play::listSequences(*arch->csar);

		cout << "index  bank  start       vol   name\n";

		for (const play::SequenceInfo& s : seqs)
		{
			cout << "  " << s.index << "\t" << s.bankIndex << "\t0x" << hex << s.startOffset << dec
				<< "\t" << static_cast<int>(s.volume)
				<< "\t" << (s.name.empty() ? "(unnamed)" : s.name) << "\n";
		}

		cout << seqs.size() << " renderable sequence(s)\n";

		return 0;
	}

	// Name a safe-skipped opcode for the render summary.
	string opName(uint32_t op)
	{
		char buf[32];

		if (op & 0x100u)
		{
			snprintf(buf, sizeof(buf), "0xF0/0x%02X", op & 0xFF);
		}
		else
		{
			snprintf(buf, sizeof(buf), "0x%02X", op & 0xFF);
		}

		return buf;
	}

	int doRender(const string& archivePath, const string& choice, const string& outPath,
		uint32_t rate, uint32_t maxSeconds)
	{
		auto arch = play::loadArchive(archivePath);

		if (!arch)
		{
			return 1;
		}

		vector<play::SequenceInfo> seqs = play::listSequences(*arch->csar);

		if (seqs.empty())
		{
			cerr << "caesar-play: archive has no renderable sequences\n";
			return 1;
		}

		const play::SequenceInfo* chosen = resolveChoice(seqs, choice);

		if (!chosen)
		{
			cerr << "caesar-play: no sequence named or indexed '" << choice << "' (try --list)\n";
			return 1;
		}

		if (!play::resolveSequence(*arch, *chosen))
		{
			return 1;
		}

		play::StereoBus bus;
		play::RenderStats stats;

		if (!play::renderSequence(*arch, bus, maxSeconds, stats))
		{
			cerr << "caesar-play: failed to render '" << chosen->name << "'\n";
			return 1;
		}

		vector<int16_t> pcm = play::finalizeToPcm(bus, rate);

		if (!play::writeWavPcm(outPath, pcm, 2, rate))
		{
			cerr << "caesar-play: failed to write " << outPath << "\n";
			return 1;
		}

		double seconds = (bus.frames() > 0) ? static_cast<double>(bus.frames()) / play::kNativeRate : 0.0;

		cout << "rendered '" << chosen->name << "' -> " << outPath << "\n"
			<< "  " << (pcm.size() / 2) << " frames @ " << rate << " Hz (" << seconds << " s)\n"
			<< "  notes: " << stats.notesFired << " fired, " << stats.notesDropped << " dropped"
			<< "; tracks: " << stats.tracksOpened
			<< "; tempo " << stats.tempoBpm << " BPM, timebase " << stats.timebase
			<< "; " << stats.tickLength << " ticks\n"
			<< "  voices: peak " << stats.maxConcurrent << "/24"
			<< "; " << stats.notesRefused << " refused (pool full)"
			<< ", " << stats.voicesStolen << " stolen (" << stats.voicesStolenHeld << " still held)"
			<< ", " << stats.monoRetriggers << " mono re-triggers\n"
			<< "  params: " << stats.rampsApplied << " _t ramps (longest " << stats.longestRampTicks
			<< " ticks), " << stats.instantSets << " instant sets\n";

		if (stats.tieVoices > 0)
		{
			cout << "  tie: " << stats.tieVoices << " continuous voice(s), "
				<< stats.tieRetunes << " no-attack retunes\n";
		}

		if (stats.lfoCommands > 0)
		{
			cout << "  lfo: " << stats.lfoCommands << " command(s) (depth/rate/target/range/delay)\n";
		}

		if (stats.bankSwitches > 0)
		{
			cout << "  bank: " << stats.bankSwitches << " mid-sequence switch(es) (0xB6)\n";
		}

		if (stats.loopDetected)
		{
			cout << "  whole-song loop detected: body rendered once (a track's backward jump ended it)\n";
		}

		if (stats.cappedByMaxSeconds)
		{
			cout << "  render hit the --max-seconds " << maxSeconds << " cap (sequence still running)\n";
		}

		if (!stats.skippedOps.empty())
		{
			cout << "  safe-skipped (no audio yet): ";

			for (size_t i = 0; i < stats.skippedOps.size(); ++i)
			{
				cout << (i ? ", " : "") << opName(stats.skippedOps[i]);
			}

			cout << "\n";
		}

		return 0;
	}

	int doRenderNote(const string& archivePath, const string& choice, const string& outPath, uint32_t rate)
	{
		auto arch = play::loadArchive(archivePath);

		if (!arch)
		{
			return 1;
		}

		vector<play::SequenceInfo> seqs = play::listSequences(*arch->csar);

		if (seqs.empty())
		{
			cerr << "caesar-play: archive has no renderable sequences (needed to bind a bank)\n";
			return 1;
		}

		const play::SequenceInfo* chosen = resolveChoice(seqs, choice);

		if (!chosen)
		{
			cerr << "caesar-play: no sequence named or indexed '" << choice << "' (try --list)\n";
			return 1;
		}

		if (!play::resolveSequence(*arch, *chosen))
		{
			return 1;
		}

		play::StereoBus bus;

		if (!play::renderSingleNote(*arch, bus))
		{
			cerr << "caesar-play: bank bound to '" << chosen->name << "' has no playable note\n";
			return 1;
		}

		vector<int16_t> pcm = play::finalizeToPcm(bus, rate);

		if (!play::writeWavPcm(outPath, pcm, 2, rate))
		{
			cerr << "caesar-play: failed to write " << outPath << "\n";
			return 1;
		}

		cout << "rendered one note from '" << chosen->name << "'s bank -> " << outPath
			<< " (" << (pcm.size() / 2) << " frames @ " << rate << " Hz)\n";

		return 0;
	}

	// --- Diagnostics: --zones / --trace --------------------------------------

	// The --wave filter: a wave-archive FILE id and the wave's index inside it.
	struct WaveFilter
	{
		bool active = false;
		int32_t warId = 0;
		uint32_t waveIndex = 0;

		bool matches(const play::VoiceTrace& z) const
		{
			return !active || (z.haveZone && z.warFileId == warId && z.waveIndex == waveIndex);
		}
	};

	bool parseWaveFilter(const string& text, WaveFilter& out)
	{
		size_t colon = text.find(':');

		if (colon == string::npos || colon == 0 || colon + 1 >= text.size()
			|| text.find_first_not_of("0123456789") != colon
			|| text.find_first_not_of("0123456789", colon + 1) != string::npos)
		{
			return false;
		}

		out.active = true;
		out.warId = static_cast<int32_t>(strtol(text.c_str(), nullptr, 10));
		out.waveIndex = static_cast<uint32_t>(strtoul(text.c_str() + colon + 1, nullptr, 10));

		return true;
	}

	string fmtNum(const char* format, double value)
	{
		char buf[64];

		snprintf(buf, sizeof(buf), format, value);

		return buf;
	}

	string fmtHex(uint32_t value)
	{
		char buf[16];

		snprintf(buf, sizeof(buf), "0x%X", value);

		return buf;
	}

	// A count that is blank when the value is the "never set / unknown" -1.
	string fmtOpt(int value)
	{
		return value < 0 ? string() : to_string(value);
	}

	string joinTabs(const vector<string>& fields)
	{
		string line;

		for (size_t i = 0; i < fields.size(); ++i)
		{
			if (i)
			{
				line += '\t';
			}

			line += fields[i];
		}

		return line;
	}

	// TSV on Windows would otherwise come out CRLF (stdout text mode).
	void useLfOutput()
	{
#ifdef _WIN32
		_setmode(_fileno(stdout), _O_BINARY);
#endif
	}

	int doZones(const string& archivePath, const WaveFilter& wave)
	{
		auto arch = play::loadArchive(archivePath);

		if (!arch)
		{
			return 1;
		}

		useLfOutput();

		const Csar& csar = *arch->csar;

		cout << "bank_pos\tbank_id\tbank_name\tprogram\tinst_type\tzone\tkey_lo\tkey_hi"
			"\tcwav_index_raw\twar_pos\twar_id\twave\twave_rate\twave_frames\tlooped"
			"\troot_key\ttune\tvolume\tpan\tinterp\tattack\tdecay\tsustain\thold\trelease"
			"\tid\tword08\tword0c\tflags\tword28\tw6001_10\tw6001_14\tw6001_18\tw6001_1c\tstatus\n";

		size_t rows = 0;
		size_t silent = 0;
		size_t banksBuilt = 0;
		vector<string> skipped;

		for (size_t b = 0; b < csar.CbnkRecords.size(); ++b)
		{
			const CsarCbnk& rec = csar.CbnkRecords[b];
			play::DropReason why = play::DropReason::None;
			const Cbnk* bank = play::getBank(*arch, static_cast<uint32_t>(b), &why);

			if (!bank)
			{
				skipped.push_back("# bank " + to_string(b) + " (file id " + to_string(rec.Id) + ", " + rec.FileName
					+ ") skipped: " + play::dropReasonName(why));
				continue;
			}

			++banksBuilt;

			for (size_t p = 0; p < bank->Insts.size(); ++p)
			{
				const CbnkInst& inst = bank->Insts[p];

				if (!inst.Exists)
				{
					continue;
				}

				for (size_t z = 0; z < inst.Notes.size(); ++z)
				{
					const CbnkNote& n = inst.Notes[z];

					if (!n.Exists)
					{
						continue;
					}

					// The same zone -> wave-archive -> sample walk resolveVoice runs, so the
					// file id and status here are exactly what a note on this zone would get.
					play::VoiceTrace vt;
					play::DropReason reason = play::DropReason::None;
					const Cwav* sample = play::resolveZoneWave(*bank, n, &vt, reason);

					if (!wave.matches(vt))
					{
						continue;
					}

					string status = play::dropReasonName(reason);

					// Cbnk.cpp repoints a raw sample index past the bank's table at sample 0.
					if (n.CwavIndex >= bank->Cwavs.size())
					{
						status += ";substituted-index";
					}

					// A split zone whose end key sits below its start key covers no key.
					if (n.StartNote > n.EndNote)
					{
						status += ";empty-key-range";
					}

					++rows;

					if (reason != play::DropReason::None)
					{
						++silent;
					}

					bool layered = (n.Id == 0x6001);

					vector<string> f;
					f.push_back(to_string(b));
					f.push_back(to_string(rec.Id));
					f.push_back(rec.FileName);
					f.push_back(to_string(p));
					f.push_back(fmtHex(inst.Type));
					f.push_back(to_string(z));
					f.push_back(to_string(static_cast<unsigned>(n.StartNote)));
					f.push_back(to_string(static_cast<unsigned>(n.EndNote)));
					f.push_back(to_string(n.CwavIndex));
					f.push_back(n.Cwav ? to_string(n.Cwav->Cwar) : string());
					f.push_back(fmtOpt(vt.warFileId));
					f.push_back(n.Cwav ? to_string(n.Cwav->Id) : string());
					f.push_back(vt.haveWave ? to_string(vt.sampleRate) : string());
					f.push_back(sample ? to_string(sample->Channels[0].size()) : string());
					f.push_back(sample ? (play::waveIsLooped(*sample) ? "1" : "0") : string());
					f.push_back(to_string(n.RootKey));
					f.push_back(fmtNum("%.7g", static_cast<double>(n.Tune)));
					f.push_back(to_string(n.Volume));
					f.push_back(to_string(n.Pan));
					f.push_back(to_string(static_cast<unsigned>(n.Interpolation)));
					f.push_back(to_string(static_cast<unsigned>(n.Attack)));
					f.push_back(to_string(static_cast<unsigned>(n.Decay)));
					f.push_back(to_string(static_cast<unsigned>(n.Sustain)));
					f.push_back(to_string(static_cast<unsigned>(n.Hold)));
					f.push_back(to_string(static_cast<unsigned>(n.Release)));
					f.push_back(fmtHex(n.Id));
					f.push_back(fmtHex(n.Word08));
					f.push_back(fmtHex(n.Word0C));
					f.push_back(fmtHex(n.Flags));
					f.push_back(fmtHex(n.Word28));
					f.push_back(layered ? fmtHex(n.Word6001_10) : string());
					f.push_back(layered ? fmtHex(n.Word6001_14) : string());
					f.push_back(layered ? fmtHex(n.Word6001_18) : string());
					f.push_back(layered ? fmtHex(n.Word6001_1C) : string());
					f.push_back(status);

					cout << joinTabs(f) << "\n";
				}
			}
		}

		for (const string& line : skipped)
		{
			cout << line << "\n";
		}

		// The positional index a bank stores resolves through the map, which orders its
		// wave archives by file id; report whether that order is also the order of the
		// archive's own wave-archive records (it is the player's existing assumption).
		size_t pos = 0;
		size_t external = 0;
		vector<string> differing;

		for (auto it = csar.Cwars.begin(); it != csar.Cwars.end(); ++it, ++pos)
		{
			if (!it->second)
			{
				++external;
			}

			if (pos >= csar.CwarRecords.size() || static_cast<uint32_t>(it->first) != csar.CwarRecords[pos].Id)
			{
				differing.push_back(to_string(pos) + "->" + to_string(it->first));
			}
		}

		cout << "# " << rows << " zone row(s) from " << banksBuilt << " of " << csar.CbnkRecords.size()
			<< " bank(s); " << silent << " of them cannot sound\n";
		cout << "# wave-archive map: " << csar.Cwars.size() << " archive(s), " << external << " external; "
			<< csar.CwarRecords.size() << " wave-archive record(s); position order "
			<< (differing.empty() ? "matches the record order" : "DIFFERS from the record order at");

		for (const string& d : differing)
		{
			cout << " " << d;
		}

		cout << "\n";

		return 0;
	}

	int doTrace(const string& archivePath, const string& choice, const WaveFilter& wave, uint32_t maxSeconds)
	{
		auto arch = play::loadArchive(archivePath);

		if (!arch)
		{
			return 1;
		}

		vector<play::SequenceInfo> seqs = play::listSequences(*arch->csar);

		if (seqs.empty())
		{
			cerr << "caesar-play: archive has no renderable sequences\n";
			return 1;
		}

		vector<const play::SequenceInfo*> picked;

		if (choice.empty())
		{
			for (const play::SequenceInfo& s : seqs)
			{
				picked.push_back(&s);
			}
		}
		else
		{
			const play::SequenceInfo* chosen = resolveChoice(seqs, choice);

			if (!chosen)
			{
				cerr << "caesar-play: no sequence named or indexed '" << choice << "' (try --list)\n";
				return 1;
			}

			picked.push_back(chosen);
		}

		useLfOutput();

		const Csar& csar = *arch->csar;

		cout << "seq\tseq_name\trecord\tstart_off\tbank_pos\tbank_id\ttrack\ttick\tt_s\tgate_s"
			"\tprogram\tkey\tvel\tstatus\twar_id\twave\troot_key\ttune\tzone_vol\tzone_pan"
			"\tsemitones\tspeed\twave_rate\tatk\thold\tdec\tsus\trel"
			"\tsweep_semis\tsweep_s\tporta_semis\tporta_s\tseq_vol"
			"\ttrk_vol\ttrk_expr\ttrk_pan\ttrk_initpan\ttrk_transpose\ttrk_bend\ttrk_bend_range"
			"\tfx_a\tfx_b\tmain_send\tcmd_off\tlen_ticks\tlen_src\tzone\ttie\tprio\tmono\n";

		struct SeqSummary
		{
			uint32_t index = 0;
			string name;
			bool traced = false;
			size_t notes = 0;
			size_t fired = 0;
			size_t hits = 0;        // notes whose zone resolves to the --wave target (any status)
			size_t hitsFired = 0;
			uint32_t tieRetunes = 0;
			uint64_t ticks = 0;
			bool capped = false;
		};

		vector<SeqSummary> summaries;
		map<string, size_t> reasons;   // every dropped note of every traced sequence
		size_t totalNotes = 0;
		size_t totalDropped = 0;
		set<uint32_t> hitSeqs;
		set<uint32_t> hitFiredSeqs;
		size_t failed = 0;

		const double nativeRate = static_cast<double>(play::kNativeRate);

		for (const play::SequenceInfo* s : picked)
		{
			SeqSummary sum;
			sum.index = s->index;
			sum.name = s->name;

			// Release the previous sequence (and its switch banks) before binding the next.
			play::releaseSequence(*arch);

			if (!play::resolveSequence(*arch, *s))
			{
				++failed;
				summaries.push_back(sum);

				if (picked.size() == 1)
				{
					return 1;
				}

				continue;
			}

			play::StereoBus bus;
			play::RenderStats stats;
			play::SequenceTrace trace;

			if (!play::renderSequence(*arch, bus, maxSeconds, stats, &trace))
			{
				cerr << "caesar-play: failed to trace '" << s->name << "'\n";
				++failed;
				summaries.push_back(sum);

				if (picked.size() == 1)
				{
					return 1;
				}

				continue;
			}

			sum.traced = true;
			sum.tieRetunes = stats.tieRetunes;
			sum.ticks = stats.tickLength;
			sum.capped = stats.cappedByMaxSeconds;

			for (const play::NoteTraceRow& r : trace.notes)
			{
				const play::VoiceTrace& z = r.voice;

				++sum.notes;
				++totalNotes;

				if (r.resolved)
				{
					++sum.fired;
				}
				else
				{
					++totalDropped;
					++reasons[play::dropReasonName(z.reason)];
				}

				if (!wave.matches(z))
				{
					continue;
				}

				if (wave.active)
				{
					++sum.hits;
					hitSeqs.insert(s->index);

					if (r.resolved)
					{
						++sum.hitsFired;
						hitFiredSeqs.insert(s->index);
					}
				}

				const play::TrackSnapshot& tk = r.origin.track;
				string bankId = (r.bankIndex < csar.CbnkRecords.size()) ? to_string(csar.CbnkRecords[r.bankIndex].Id) : string();
				string semis;
				string speed;

				if (z.haveZone)
				{
					double base = static_cast<double>(r.key) - static_cast<double>(z.rootKey);

					// semitones = key - RootKey + 12*log2(Tune); speed = 2^(semitones/12), the
					// playback rate relative to the wave's own sample rate (Dsp.cpp resolveVoice).
					if (z.tune > 0.0f)
					{
						semis = fmtNum("%.4f", base + 12.0 * log2(static_cast<double>(z.tune)));
						speed = fmtNum("%.6f", pow(2.0, base / 12.0) * static_cast<double>(z.tune));
					}
					else
					{
						semis = "n/a";
						speed = "0";
					}
				}

				const char* lenSource = (r.origin.lenSource == play::ArgSource::Rnd) ? "rnd"
					: (r.origin.lenSource == play::ArgSource::Var) ? "var" : "plain";

				vector<string> f;
				f.push_back(to_string(s->index));
				f.push_back(s->name);
				f.push_back(to_string(s->recordIndex));
				f.push_back(fmtHex(s->startOffset));
				f.push_back(to_string(r.bankIndex));
				f.push_back(bankId);
				f.push_back(to_string(r.track));
				f.push_back(to_string(r.origin.tick));
				f.push_back(fmtNum("%.4f", static_cast<double>(r.startSample) / nativeRate));
				f.push_back(fmtNum("%.4f", static_cast<double>(r.gateSamples) / nativeRate));
				f.push_back(to_string(r.program));
				f.push_back(to_string(r.key));
				f.push_back(to_string(r.velocity));
				f.push_back(r.resolved ? string("fired") : string("dropped:") + play::dropReasonName(z.reason));
				f.push_back(fmtOpt(z.haveZone ? z.warFileId : -1));
				f.push_back(z.haveZone ? to_string(z.waveIndex) : string());
				f.push_back(z.haveZone ? to_string(z.rootKey) : string());
				f.push_back(z.haveZone ? fmtNum("%.7g", static_cast<double>(z.tune)) : string());
				f.push_back(z.haveZone ? to_string(z.volume) : string());
				f.push_back(z.haveZone ? to_string(z.pan) : string());
				f.push_back(semis);
				f.push_back(speed);
				f.push_back(z.haveWave ? to_string(z.sampleRate) : string());

				for (int e = 0; e < 5; ++e)
				{
					f.push_back(r.resolved ? to_string(static_cast<unsigned>(r.env[e])) : string());
				}

				f.push_back(fmtNum("%.4f", static_cast<double>(r.sweepFromSemis)));
				f.push_back(fmtNum("%.4f", static_cast<double>(r.sweepDurSamples) / nativeRate));
				f.push_back(fmtNum("%.4f", static_cast<double>(r.portaFromSemis)));
				f.push_back(fmtNum("%.4f", static_cast<double>(r.portaDurSamples) / nativeRate));
				f.push_back(to_string(static_cast<unsigned>(s->volume)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.volume)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.expression)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.pan)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.initPan)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.transpose)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.bend)));
				f.push_back(fmtNum("%.6g", static_cast<double>(tk.bendRange)));
				f.push_back(fmtOpt(tk.fxSendA));
				f.push_back(fmtOpt(tk.fxSendB));
				f.push_back(fmtOpt(tk.mainSend));
				f.push_back(fmtHex(r.origin.cmdOffset));
				f.push_back(to_string(r.origin.lenTicks));
				f.push_back(lenSource);
				f.push_back(z.haveZone ? to_string(z.zoneIndex) : string());
				f.push_back(r.tieRegion >= 0 ? to_string(r.tieRegion) : string());
				f.push_back(to_string(r.priority));
				f.push_back(r.mono ? "1" : "0");

				cout << joinTabs(f) << "\n";
			}

			summaries.push_back(sum);
		}

		// '#' summaries: per sequence, then the archive-wide drop-reason histogram. A
		// trace's "fired" means resolved to a voice (the 24-voice pool is not run).
		for (const SeqSummary& sum : summaries)
		{
			cout << "# seq " << sum.index << " " << sum.name << ": ";

			if (!sum.traced)
			{
				cout << "not traced (could not be resolved)\n";
				continue;
			}

			cout << "notes=" << sum.notes << " fired=" << sum.fired << " dropped=" << (sum.notes - sum.fired)
				<< " tie-retunes=" << sum.tieRetunes << " ticks=" << sum.ticks;

			if (sum.capped)
			{
				cout << " (hit the --max-seconds " << maxSeconds << " cap)";
			}

			if (wave.active)
			{
				cout << " wave-hits=" << sum.hits << " (fired " << sum.hitsFired << ")";
			}

			cout << "\n";
		}

		vector<pair<string, size_t>> histogram(reasons.begin(), reasons.end());

		stable_sort(histogram.begin(), histogram.end(),
			[](const pair<string, size_t>& a, const pair<string, size_t>& b) { return a.second > b.second; });

		cout << "# drop-reason histogram (over all notes of the " << (summaries.size() - failed)
			<< " sequence(s) traced, whatever --wave keeps: " << totalNotes << " note(s), "
			<< totalDropped << " dropped):\n";

		for (const pair<string, size_t>& h : histogram)
		{
			cout << "#   " << h.first << "\t" << h.second << "\n";
		}

		if (failed)
		{
			cout << "# " << failed << " sequence(s) could not be resolved (see stderr)\n";
		}

		if (wave.active)
		{
			auto list = [](const set<uint32_t>& ids)
			{
				string out;

				for (uint32_t id : ids)
				{
					out += (out.empty() ? "" : ",") + to_string(id);
				}

				return out.empty() ? string("(none)") : out;
			};

			cout << "# wave " << wave.warId << ":" << wave.waveIndex << " matched by sequence list indices: "
				<< list(hitSeqs) << "\n";
			cout << "# wave " << wave.warId << ":" << wave.waveIndex << " of which with a fired note: "
				<< list(hitFiredSeqs) << "\n";
		}

		return 0;
	}
}

int main(int argc, char** argv)
{
	string archivePath;
	string choice;
	string outPath;
	uint32_t rate = kDefaultRate;
	uint32_t maxSeconds = kDefaultMaxSeconds;
	bool list = false;
	bool render = false;
	bool renderNote = false;
	bool zones = false;
	bool trace = false;
	WaveFilter wave;

	for (int a = 1; a < argc; ++a)
	{
		string arg = argv[a];

		if (arg == "--version")
		{
#ifdef CAESAR_VERSION
			cout << "caesar-play " << CAESAR_VERSION << "\n";
#else
			cout << "caesar-play (unknown version)\n";
#endif
			return 0;
		}
		else if (arg == "--list")
		{
			list = true;

			if (a + 1 < argc && argv[a + 1][0] != '-')
			{
				archivePath = argv[++a];
			}
		}
		else if (arg == "--render")
		{
			render = true;

			if (a + 1 < argc && argv[a + 1][0] != '-')
			{
				archivePath = argv[++a];
			}
		}
		else if (arg == "--render-note")
		{
			renderNote = true;

			if (a + 1 < argc && argv[a + 1][0] != '-')
			{
				archivePath = argv[++a];
			}
		}
		else if (arg == "--zones")
		{
			zones = true;

			if (a + 1 < argc && argv[a + 1][0] != '-')
			{
				archivePath = argv[++a];
			}
		}
		else if (arg == "--trace")
		{
			trace = true;

			if (a + 1 < argc && argv[a + 1][0] != '-')
			{
				archivePath = argv[++a];
			}
		}
		else if (arg == "--wave")
		{
			if (a + 1 >= argc) { cerr << "caesar-play: --wave needs a value\n"; return 1; }

			if (!parseWaveFilter(argv[++a], wave))
			{
				cerr << "caesar-play: --wave wants <warFileId>:<waveIndex> (two decimal numbers), got '" << argv[a] << "'\n";
				return 1;
			}
		}
		else if (arg == "--seq")
		{
			if (a + 1 >= argc) { cerr << "caesar-play: --seq needs a value\n"; return 1; }
			choice = argv[++a];
		}
		else if (arg == "--out")
		{
			if (a + 1 >= argc) { cerr << "caesar-play: --out needs a value\n"; return 1; }
			outPath = argv[++a];
		}
		else if (arg == "--rate")
		{
			if (a + 1 >= argc) { cerr << "caesar-play: --rate needs a value\n"; return 1; }
			rate = static_cast<uint32_t>(strtoul(argv[++a], nullptr, 10));
		}
		else if (arg == "--max-seconds")
		{
			if (a + 1 >= argc) { cerr << "caesar-play: --max-seconds needs a value\n"; return 1; }
			maxSeconds = static_cast<uint32_t>(strtoul(argv[++a], nullptr, 10));
		}
		else if (arg == "--help" || arg == "-h")
		{
			printUsage();
			return 0;
		}
		else if (archivePath.empty())
		{
			archivePath = arg;
		}
		else
		{
			cerr << "caesar-play: unexpected argument '" << arg << "'\n";
			return 1;
		}
	}

	if (rate < 8000 || rate > 384000)
	{
		cerr << "caesar-play: --rate " << rate << " out of the supported 8000..384000 range\n";
		return 1;
	}

	if (wave.active && !zones && !trace)
	{
		cerr << "caesar-play: --wave only applies to --zones and --trace\n";
		return 1;
	}

	if (list)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --list needs an archive\n"; return 1; }
		return doList(archivePath);
	}

	if (zones)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --zones needs an archive\n"; return 1; }
		return doZones(archivePath, wave);
	}

	if (trace)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --trace needs an archive\n"; return 1; }
		return doTrace(archivePath, choice, wave, maxSeconds);
	}

	if (render)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --render needs an archive\n"; return 1; }
		if (choice.empty()) { cerr << "caesar-play: --render needs --seq <name-or-index>\n"; return 1; }
		if (outPath.empty()) { cerr << "caesar-play: --render needs --out <file.wav>\n"; return 1; }
		return doRender(archivePath, choice, outPath, rate, maxSeconds);
	}

	if (renderNote)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --render-note needs an archive\n"; return 1; }
		if (choice.empty()) { cerr << "caesar-play: --render-note needs --seq <name-or-index>\n"; return 1; }
		if (outPath.empty()) { cerr << "caesar-play: --render-note needs --out <file.wav>\n"; return 1; }
		return doRenderNote(archivePath, choice, outPath, rate);
	}

	printUsage();
	return 1;
}
