// caesar-play — the suite stage-2 offline dry-render CLI. Renders a chosen
// sequence from a .bcsar to a .wav with no audio device. A development/suite
// tool, NOT part of the converter release.
//
//   caesar-play --list        <archive.bcsar>
//   caesar-play --render      <archive.bcsar> --seq <name-or-index> --out <file.wav>
//   caesar-play --render-note <archive.bcsar> --seq <name-or-index> --out <file.wav>
//
// --render runs the concurrent sequencer (C3) -> voices -> native mix bus ->
// one final resample -> WAV; --render-note is the C2 single-voice DSP proof;
// --list names an archive's renderable sequences.

#include "CaesarPlay.hpp"

#include "Csar.hpp"
#include "Wav.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
			"\n"
			"options:\n"
			"  --render-note       DSP proof: render the resolved bank's first note (C2)\n"
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

		cout << "index  bank  start       name\n";

		for (const play::SequenceInfo& s : seqs)
		{
			cout << "  " << s.index << "\t" << s.bankIndex << "\t0x" << hex << s.startOffset << dec
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
			<< ", " << stats.monoRetriggers << " mono re-triggers\n";

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

	if (list)
	{
		if (archivePath.empty()) { cerr << "caesar-play: --list needs an archive\n"; return 1; }
		return doList(archivePath);
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
