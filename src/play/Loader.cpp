#include "CaesarPlay.hpp"

#include "Cbnk.hpp"
#include "Common.hpp"
#include "Cseq.hpp"
#include "Csar.hpp"
#include "Cwar.hpp"
#include "Cwav.hpp"
#include "Options.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace play
{
	namespace
	{
		// The Csar/Cwar/Cwav/Cbnk/Cseq constructors echo the file name they parse
		// (ParseContext::Push writes it to cout). For the player that is noise, so
		// redirect cout into a throwaway sink for the duration of a load. A real
		// ostringstream sink (not a null rdbuf) is used deliberately: a null buffer
		// would set cout's failbit and swallow every later line. Restored on scope
		// exit even if a bounds check throws (mirrors caesar-roundtrip's guard).
		struct CoutSilencer
		{
			streambuf* saved;
			ostringstream sink;
			CoutSilencer() : saved(cout.rdbuf()) { cout.rdbuf(sink.rdbuf()); }
			~CoutSilencer() { cout.rdbuf(saved); cout.clear(); }
			CoutSilencer(const CoutSilencer&) = delete;
			CoutSilencer& operator=(const CoutSilencer&) = delete;
		};

		// Locate an internal child blob the way Csar::Export does: the file record's
		// data span begins 16 bytes before its length word, which sits at
		// Files[id].Offset + 12. Reads (and bounds-checks) that length off the live
		// archive buffer, then returns the borrowed [data, data+length) span.
		bool locateChild(Csar& csar, uint32_t fileId, uint8_t*& outData, uint32_t& outLen)
		{
			if (fileId >= csar.Files.size() || !csar.Files[fileId].Internal)
			{
				return false;
			}

			uint8_t* pos = csar.Data + csar.Files[fileId].Offset + 12;
			uint32_t length = static_cast<uint32_t>(csar.Ctx.ReadFixLen(pos, 4));
			pos -= 16;

			csar.Ctx.CheckBounds(pos, length);

			outData = pos;
			outLen = length;

			return true;
		}
	}

	LoadedArchive::LoadedArchive() = default;

	// Destruction order (reverse member order): seq -> bank -> csar -> ctx. This
	// pops the ParseContext frames in the order that keeps the stack balanced:
	// the sequence and bank frames (pushed last, during resolveSequence) release
	// first, then ~Csar deletes the wave archives it built (each popping its own
	// frame) before its own, and the ParseContext -- referenced by every object
	// above -- is torn down last of all.
	LoadedArchive::~LoadedArchive() = default;

	unique_ptr<LoadedArchive> loadArchive(const string& path)
	{
		auto arch = make_unique<LoadedArchive>();
		arch->ctx = make_unique<ParseContext>();

		try
		{
			CoutSilencer hush;

			Options opts;
			arch->csar = make_unique<Csar>(path.c_str(), opts, *arch->ctx);

			if (!arch->csar->Parse())
			{
				cerr << "caesar-play: failed to parse archive: " << path << "\n";
				return nullptr;
			}

			Csar& csar = *arch->csar;

			// Construct + parse every wave archive into Csar::Cwars, decoding its
			// samples in memory. This mirrors Csar::Export's Cwar loop exactly
			// minus the disk writes: an absent (external) entry is still recorded as
			// a null slot so the positional Cwar index the banks use stays correct
			// (Cbnk resolves a sample's wave archive by advancing Cwars.begin() by
			// the stored index, counting the null slots). The child Cwavs are then
			// constructed and Parse()d -- Parse decodes the PCM into Cwav::Channels
			// and sets Converted -- but never ExportWav()'d.
			for (const CsarCwar& cwar : csar.CwarRecords)
			{
				uint8_t* data = nullptr;
				uint32_t length = 0;

				if (!locateChild(csar, cwar.Id, data, length))
				{
					csar.Cwars[cwar.Id] = nullptr;
					continue;
				}

				Cwar* war = new Cwar(cwar.FileName + ".bcwar", data, length, false, *arch->ctx);
				csar.Cwars[cwar.Id] = war;

				if (!war->Parse())
				{
					cerr << "caesar-play: failed to parse wave archive " << cwar.FileName << "\n";
					return nullptr;
				}

				// Decode each sub-file the way Cwar::Export does, without writing a
				// .bcwav: the record's span into the war's own buffer, bounds-checked,
				// handed to a borrowed Cwav that decodes in Parse().
				for (size_t j = 0; j < war->CwavRecords.size(); ++j)
				{
					uint8_t* cwavData = war->Data + war->CwavRecords[j].Offset;
					uint32_t cwavLen = war->CwavRecords[j].Length;

					arch->ctx->CheckBounds(cwavData, cwavLen);

					string cwavName = cwar.FileName + "/" + to_string(j) + ".bcwav";
					Cwav* wav = new Cwav(cwavName, cwavData, cwavLen, *arch->ctx);
					war->Cwavs.push_back(wav);

					if (!wav->Parse())
					{
						cerr << "caesar-play: failed to decode sample " << j << " of " << cwar.FileName << "\n";
						return nullptr;
					}
				}
			}
		}
		catch (const exception& e)
		{
			cerr << "caesar-play: error loading " << path << ": " << e.what() << "\n";
			return nullptr;
		}

		return arch;
	}

	vector<SequenceInfo> listSequences(const Csar& csar)
	{
		vector<SequenceInfo> out;

		for (size_t r = 0; r < csar.CseqRecords.size(); ++r)
		{
			const CsarCseq& rec = csar.CseqRecords[r];

			if (rec.Type != 0x2203 || !rec.Convertible)
			{
				continue;
			}

			SequenceInfo info;
			info.index = static_cast<uint32_t>(out.size());
			info.name = rec.FileName;
			info.bankIndex = rec.BankIndex;
			info.startOffset = rec.StartOffset;
			info.recordIndex = static_cast<uint32_t>(r);

			out.push_back(info);
		}

		return out;
	}

	bool resolveSequence(LoadedArchive& arch, const SequenceInfo& info)
	{
		Csar& csar = *arch.csar;

		if (info.bankIndex >= csar.CbnkRecords.size())
		{
			cerr << "caesar-play: sequence '" << info.name << "' references bank index "
				<< info.bankIndex << " which does not exist\n";
			return false;
		}

		try
		{
			CoutSilencer hush;

			// The bank the sequence binds to. Built once here (Csar::Export builds a
			// fresh stack-local Cbnk per bank); its samples resolve live against the
			// already-decoded Cwars, so no .wav is re-read from disk.
			const CsarCbnk& bankRec = csar.CbnkRecords[info.bankIndex];
			uint8_t* bankData = nullptr;
			uint32_t bankLen = 0;

			if (!locateChild(csar, bankRec.Id, bankData, bankLen))
			{
				cerr << "caesar-play: bank '" << bankRec.FileName << "' has no in-archive data\n";
				return false;
			}

			arch.bank = make_unique<Cbnk>(bankRec.FileName + ".bcbnk", bankData, bankLen, &csar.Cwars, csar.Opts, *arch.ctx);

			if (!arch.bank->Parse())
			{
				cerr << "caesar-play: failed to parse bank '" << bankRec.FileName << "'\n";
				return false;
			}

			// The sequence's own .bcseq span.
			const CsarCseq& seqRec = csar.CseqRecords[info.recordIndex];
			uint8_t* seqData = nullptr;
			uint32_t seqLen = 0;

			if (!locateChild(csar, seqRec.Id, seqData, seqLen))
			{
				cerr << "caesar-play: sequence '" << info.name << "' has no in-archive data\n";
				return false;
			}

			arch.seq = make_unique<Cseq>(seqRec.FileName + ".bcseq", seqData, seqLen, *arch.ctx);

			if (!arch.seq->Parse())
			{
				cerr << "caesar-play: failed to parse sequence '" << info.name << "'\n";
				return false;
			}

			arch.startOffset = info.startOffset;
		}
		catch (const exception& e)
		{
			cerr << "caesar-play: error resolving sequence '" << info.name << "': " << e.what() << "\n";
			return false;
		}

		return true;
	}
}
