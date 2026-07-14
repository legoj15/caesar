#include "Csar.hpp"
#include "Cbnk.hpp"
#include "Cgrp.hpp"
#include "Common.hpp"
#include "Cseq.hpp"
#include "Cwar.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace filesystem;

Csar::Csar(const char* fileName, const Options& opts, ParseContext& ctx) : Ctx(ctx), FileName(fileName), Opts(opts)
{
	ifstream ifs(FileName, ios::binary | ios::ate);

	Length = ifs.tellg();
	Ctx.RequireOpen(ifs.good(), Length, FileName);
	Data = new uint8_t[Length];

	Ctx.Push(FileName, Data, Length);

	ifs.seekg(0, ios::beg);
	ifs.read(reinterpret_cast<char*>(Data), Length);
	ifs.close();
}

Csar::~Csar()
{
	for (auto& cwar : Cwars)
	{
		delete cwar.second;
	}

	Ctx.Pop();

	delete[] Data;
}

bool Csar::Extract(const string& outputDir)
{
	// Compose the archive's output directory rather than changing into it: every
	// output path below is built from archiveDir, so no global working-directory
	// state is touched (which is what previously limited failure recovery and
	// parallel/multi-file use). Default layout is unchanged -- beside the input,
	// in a folder named after the archive -- while an explicit outputDir puts
	// every archive under "<outputDir>/<name>/".
	path stem = path(FileName).stem();
	path archiveDir;

	if (outputDir.empty())
	{
		archiveDir = FileName;
		archiveDir.replace_extension();
	}
	else
	{
		archiveDir = path(outputDir) / stem;
	}

	create_directories(archiveDir);

	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43534152, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	uint32_t csarVersion = Ctx.ReadFixLen(pos, 4);
	uint32_t length = Ctx.ReadFixLen(pos, 4);

	if (csarVersion != 0x02000000)
	{
		if (!Ctx.Assert<uint64_t>(pos, Length, length)) { return false; }
	}

	if (!Ctx.Assert(pos, 0x3, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x2000, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t strgOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t strgLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x2001, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t infoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x2002, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t fileOffset = Ctx.ReadFixLen(pos, 4);
	[[maybe_unused]] uint32_t fileLength = Ctx.ReadFixLen(pos, 4);

	vector<CsarStrg> strgs;

	if (strgOffset != 0xFFFFFFFF)
	{
		pos = Data + strgOffset;

		if (!Ctx.Assert(pos, 0x53545247, Ctx.ReadFixLen(pos, 4, false))) { return false; }
		if (!Ctx.Assert<uint32_t>(pos, strgLength, Ctx.ReadFixLen(pos, 4))) { return false; }
		if (!Ctx.Assert(pos, 0x2400, Ctx.ReadFixLen(pos, 4))) { return false; }

		[[maybe_unused]] uint32_t strgStringsOffset = Ctx.ReadFixLen(pos, 4);

		if (!Ctx.Assert(pos, 0x2401, Ctx.ReadFixLen(pos, 4))) { return false; }

		[[maybe_unused]] uint32_t strgUnknownOffset = Ctx.ReadFixLen(pos, 4);
		uint32_t strgCount = Ctx.ReadFixLen(pos, 4);

		for (uint32_t i = 0; i < strgCount; ++i)
		{
			if (!Ctx.Assert(pos, 0x1F01, Ctx.ReadFixLen(pos, 4))) { return false; }

			CsarStrg strg;
			strg.Offset = Data + strgOffset + 24 + Ctx.ReadFixLen(pos, 4);
			strg.Length = Ctx.ReadFixLen(pos, 4);

			strgs.push_back(strg);
		}

		for (uint32_t i = 0; i < strgCount; ++i)
		{
			Ctx.CheckBounds(pos, strgs[i].Length - 1);
			strgs[i].String = string(reinterpret_cast<const char*>(pos), strgs[i].Length - 1);

			pos = i != (strgCount - 1) ? strgs[i + 1].Offset : pos + strgs[i].Length;
		}
	}

	pos = Data + infoOffset;

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoCseqOffset = 0;
	uint32_t infoCbnkOffset = 0;
	uint32_t infoPlayerOffset = 0;
	uint32_t infoCwarOffset = 0;
	uint32_t infoSetOffset = 0;
	uint32_t infoCgrpOffset = 0;
	uint32_t infoFileOffset = 0;
	[[maybe_unused]] uint32_t infoEndOffset = 0;

	for (uint8_t i = 0; i < 8; ++i)
	{
		uint32_t offsetId = Ctx.ReadFixLen(pos, 4);

		switch (offsetId)
		{
			case 0x2100:
				infoCseqOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2101:
				infoCbnkOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2102:
				infoPlayerOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2103:
				infoCwarOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2104:
				infoSetOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2105:
				infoCgrpOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x2106:
				infoFileOffset = Ctx.ReadFixLen(pos, 4); break;

			case 0x220B:
				infoEndOffset = Ctx.ReadFixLen(pos, 4); break;

			default:
				Ctx.Error(pos - 4, "A valid chunk type", offsetId);

				return false;
		}
	}

	pos = Data + infoOffset + 8 + infoFileOffset;

	uint32_t fileCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> fileOffsets;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x220A, Ctx.ReadFixLen(pos, 4))) { return false; }

		fileOffsets.push_back(Data + infoOffset + 8 + infoFileOffset + Ctx.ReadFixLen(pos, 4));
	}

	vector<CsarFile> files;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		pos = fileOffsets[i];

		CsarFile file;
		uint32_t fileId = Ctx.ReadFixLen(pos, 4);

		switch (fileId)
		{
			case 0x220C:
			{
				// 8-byte little-endian field: low word is the size (0xC), high
				// word is reserved (0). Read it as two 32-bit halves — ReadFixLen
				// is a 32-bit reader, so a width-8 call would shift by up to 56
				// bits (undefined behaviour) and fold the top four bytes back into
				// the low word under MSVC.
				if (!Ctx.Assert(pos, 0xC, Ctx.ReadFixLen(pos, 4))) { return false; }
				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 4))) { return false; }
				Ctx.Analyse("0x220C 0x08", Ctx.ReadFixLen(pos, 4));

				file.Offset = Data + fileOffset + 8 + Ctx.ReadFixLen(pos, 4);
				file.Length = Ctx.ReadFixLen(pos, 4);

				if ((file.Offset >= (Data + Length)) || (file.Length == 0xFFFFFFFF))
				{
					file.Offset = nullptr;
					file.Length = 0;
				}

				break;
			}

			case 0x220D:
			{
				// Same 8-byte little-endian field as 0x220C (see above): low word
				// is the size (0xC), high word is reserved (0). Read as two 32-bit
				// halves to avoid the undefined width-8 shift.
				if (!Ctx.Assert(pos, 0xC, Ctx.ReadFixLen(pos, 4))) { return false; }
				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 4))) { return false; }

				while (true)
				{
					Ctx.CheckBounds(pos, 1);

					if (*pos == 0x00)
					{
						break;
					}

					file.Location += *pos++;
				}

				break;
			}

			case 0: // Actually 0xFFFFFFFF
				file.Offset = nullptr;
				file.Length = 0;

				break;

			default:
				Ctx.Error(pos - 4, "A valid file type", fileId);

				return false;
		}

		files.push_back(file);
	}

	// File id -> the (type-prefixed) symbol name resolved at this level, for every
	// wave-archive/bank/sequence the INFO section enumerates. When a file's data
	// actually lives in a group, this level still names it (banks even get an empty
	// symbol-named directory), but the group's own file table carries only the
	// numeric id -- so this map is handed to Cgrp to carry the name across that
	// boundary and avoid a numeric-named duplicate.
	map<int, string> namesById;

	pos = Data + infoOffset + 8 + infoCwarOffset;

	uint32_t cwarCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> cwarOffsets;

	for (uint32_t i = 0; i < cwarCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2207, Ctx.ReadFixLen(pos, 4))) { return false; }

		cwarOffsets.push_back(Data + infoOffset + 8 + infoCwarOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < cwarCount; ++i)
	{
		pos = cwarOffsets[i];

		uint32_t id = Ctx.ReadFixLen(pos, 4);

		Ctx.Analyse("Cwar 0x04", Ctx.ReadFixLen(pos, 4));

		uint32_t hasFileName = Ctx.ReadFixLen(pos, 4);

		string fileName = TypedName(hasFileName  && (strgOffset != 0xFFFFFFFF) ? strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(id), "WARC");

		namesById[id] = fileName;

		if (files[id].Offset != nullptr)
		{
			pos = files[id].Offset + 12;

			uint32_t cwarLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			path warcDir = archiveDir / fileName;
			create_directories(warcDir);

			Ctx.CheckBounds(pos, cwarLength);

			string warcFile = (warcDir / (fileName + ".bcwar")).string();
			ofstream ofs(warcFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cwarLength);
			ofs.close();

			// The .bcwar was just written from [pos, pos + cwarLength) into
			// Csar's own buffer; hand that span to the child instead of
			// re-reading the file. This Cwar lives in Csar::Cwars, freed before
			// Csar::Data in ~Csar, so borrowing (ownsData = false) is safe.
			Cwars[id] = new Cwar(warcFile, pos, cwarLength, false, Ctx);

			if (!Cwars[id]->Extract())
			{
				return false;
			}
		}
		else
		{
			Cwars[id] = nullptr;
		}
	}

	pos = Data + infoOffset + 8 + infoCbnkOffset;

	uint32_t cbnkCount = Ctx.ReadFixLen(pos, 4);

	vector<CsarCbnk> cbnks;

	for (uint32_t i = 0; i < cbnkCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2206, Ctx.ReadFixLen(pos, 4))) { return false; }

		CsarCbnk cbnk;
		cbnk.Offset = Data + infoOffset + 8 + infoCbnkOffset + Ctx.ReadFixLen(pos, 4);

		cbnks.push_back(cbnk);
	}

	for (uint32_t i = 0; i < cbnkCount; ++i)
	{
		pos = cbnks[i].Offset;

		cbnks[i].Id = Ctx.ReadFixLen(pos, 4);

		Ctx.Analyse("Cbnk 0x04", Ctx.ReadFixLen(pos, 4));
		Ctx.Analyse("Cbnk 0x08", Ctx.ReadFixLen(pos, 4));
		Ctx.Analyse("Cbnk 0x0C", Ctx.ReadFixLen(pos, 4));

		cbnks[i].FileName = TypedName(strgOffset != 0xFFFFFFFF ? strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cbnks[i].Id), "BANK");

		namesById[cbnks[i].Id] = cbnks[i].FileName;

		// Create the bank directory unconditionally (even when the bank has no
		// data), matching the historical layout.
		path bankDir = archiveDir / cbnks[i].FileName;
		create_directories(bankDir);

		if (files[cbnks[i].Id].Offset != nullptr)
		{
			pos = files[cbnks[i].Id].Offset + 12;

			uint32_t cbnkLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			Ctx.CheckBounds(pos, cbnkLength);

			string bankFile = (bankDir / (cbnks[i].FileName + ".bcbnk")).string();
			ofstream ofs(bankFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cbnkLength);
			ofs.close();

			// The .bcbnk was just written from [pos, pos + cbnkLength) into Csar's
			// own buffer; hand that span to the child instead of re-reading the
			// file. This Cbnk is a stack local, converted and destroyed here while
			// Csar::Data is alive, so borrowing is safe.
			Cbnk cbnk(bankFile, pos, cbnkLength, &Cwars, Opts, Ctx);

			if (!cbnk.Convert())
			{
				return false;
			}
		}
	}

	pos = Data + infoOffset + 8 + infoCseqOffset;

	uint32_t cseqCount = Ctx.ReadFixLen(pos, 4);

	vector<CsarCseq> cseqs;
	map<int, bool> cseqsFromCsar;

	// Distinct sequence entries can share one symbol name: many INFO entries map
	// onto shared sequence banks, each beginning at its own start offset. Both the
	// .bcseq and the .mid are named from that symbol, so same-named entries would
	// compose the same output path and the later writer would silently clobber the
	// earlier entry's music (safe.bcsar maps 17 INFO entries -- 15 distinct
	// sequences -- onto one SEQ_1 name; 14 of them never reached disk). Remember
	// which seqFile path each distinct (id, startOffset) pair has claimed so a
	// later collision can be suffixed apart instead of overwriting what is
	// already on disk.
	map<string, pair<uint32_t, uint32_t>> claimedSeqFiles;

	for (uint32_t i = 0; i < cseqCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2200, Ctx.ReadFixLen(pos, 4))) { return false; }

		CsarCseq cseq;
		cseq.Offset = Data + infoOffset + 8 + infoCseqOffset + Ctx.ReadFixLen(pos, 4);

		cseqs.push_back(cseq);
	}

	for (uint32_t i = 0; i < cseqCount; ++i)
	{
		pos = cseqs[i].Offset;

		uint32_t id = Ctx.ReadFixLen(pos, 4);

		Ctx.Analyse("Cseq 0x04", Ctx.ReadFixLen(pos, 4));
		Ctx.Analyse("Cseq 0x08", Ctx.ReadFixLen(pos, 4));

		uint32_t type = Ctx.ReadFixLen(pos, 4);
		uint32_t cbnkOffset = Ctx.ReadFixLen(pos, 4);

		Ctx.Analyse("Cseq 0x14", Ctx.ReadFixLen(pos, 4));

		cseqs[i].FileName = TypedName(strgOffset != 0xFFFFFFFF ? strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(id), "SEQ");

		namesById[id] = cseqs[i].FileName;

		switch (type)
		{
			case 0x2201:
			{
				Ctx.Warning(pos - 16, "Skipping external stream", "external streams skipped (audio stored in a separate file)");

				break;
			}

			case 0x2202:
			{
				Ctx.Warning(pos - 16, "Skipping CWSD", "CWSD wave-sound blocks skipped (sound effects not extracted)");

				break;
			}

			case 0x2203:
			{
				if (files[id].Offset != nullptr)
				{
					// The entry's start offset within the sequence data (relative to
					// its DATA+8). These archives map many entries onto one shared
					// sequence bank, each beginning at its own offset; without this
					// every entry would convert from the top of the bank.
					//
					// The field sits just before the bank-reference sub-structure that
					// cbnkOffset locates, so its position tracks cbnkOffset (which
					// grows when the record carries extra optional fields). Anchoring
					// to cbnkOffset handles both record layouts seen in the wild: the
					// common one (cbnkOffset 0x44 -> field at +0x54) and a +0xC-larger
					// one (cbnkOffset 0x50 -> field at +0x60). A fixed +0x54 would
					// silently read the wrong word for the larger layout.
					uint8_t* startPos = cseqs[i].Offset + cbnkOffset + 0x10;
					uint32_t startOffset = Ctx.ReadFixLen(startPos, 4);

					pos += cbnkOffset;

					uint32_t cbnk = Ctx.ReadFixLen(pos, 2);

					pos = files[id].Offset + 12;

					uint32_t cseqLength = Ctx.ReadFixLen(pos, 4);

					pos -= 16;

					// The sequence is written into the directory of the bank it
					// references (created in the bank loop above).
					path bankDir = archiveDir / cbnks[cbnk].FileName;

					Ctx.CheckBounds(pos, cseqLength);

					// The first entry to claim a path keeps the bare symbol name, so
					// non-colliding entries (the vast majority) and each collision
					// group's first entry stay byte-identical to before. A later entry
					// landing on a path already claimed by a DIFFERENT (id, startOffset)
					// is a genuinely distinct sequence sharing the name: suffix its
					// start offset (lowercase hex) so it keeps its own .bcseq/.mid
					// rather than overwriting the earlier one. If even that collides
					// (same name and start offset but a different file id), fall back to
					// the id -- the (name, startOffset, id) triple is unique per distinct
					// entry, so this always resolves. A repeat of the same (id,
					// startOffset) is an exact-duplicate INFO entry and is left to
					// rewrite identical bytes over the same path, exactly as before.
					// The suffix is local to this path composition: cseqs[i].FileName
					// and namesById[id] keep the bare name, so group naming (Cgrp reads
					// namesById) and log/display uses are unaffected. The suffixed
					// basename does surface in this entry's own per-file notices, which
					// is what tells the collision siblings apart.
					string seqName = cseqs[i].FileName;
					string seqFile = (bankDir / (seqName + ".bcseq")).string();

					auto claimed = claimedSeqFiles.find(seqFile);

					if (claimed != claimedSeqFiles.end() && claimed->second != make_pair(id, startOffset))
					{
						ostringstream suffixed;
						suffixed << seqName << "_0x" << hex << startOffset;
						seqName = suffixed.str();
						seqFile = (bankDir / (seqName + ".bcseq")).string();

						claimed = claimedSeqFiles.find(seqFile);

						if (claimed != claimedSeqFiles.end() && claimed->second != make_pair(id, startOffset))
						{
							seqName += "_" + to_string(id);
							seqFile = (bankDir / (seqName + ".bcseq")).string();
						}
					}

					claimedSeqFiles[seqFile] = make_pair(id, startOffset);

					ofstream ofs(seqFile, ofstream::binary);
					ofs.write(reinterpret_cast<const char*>(pos), cseqLength);
					ofs.close();

					Cseq cseq(seqFile.c_str(), Ctx);

					if (!cseq.Convert(startOffset))
					{
						return false;
					}

					cseqsFromCsar[id] = true;
				}

				break;
			}

			default:
				Ctx.Error(pos - 16, "A valid music type", type);

				return false;
		}
	}

	pos = Data + infoOffset + 8 + infoPlayerOffset;

	uint32_t playerCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> playerOffsets;

	for (uint32_t i = 0; i < playerCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2209, Ctx.ReadFixLen(pos, 4))) { return false; }

		playerOffsets.push_back(Data + infoOffset + 8 + infoPlayerOffset + Ctx.ReadFixLen(pos, 4));
	}

	pos = Data + infoOffset + 8 + infoSetOffset;

	uint32_t setCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> setOffsets;

	for (uint32_t i = 0; i < setCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2204, Ctx.ReadFixLen(pos, 4))) { return false; }

		setOffsets.push_back(Data + infoOffset + 8 + infoSetOffset + Ctx.ReadFixLen(pos, 4));
	}

	pos = Data + infoOffset + 8 + infoCgrpOffset;

	uint32_t cgrpCount = Ctx.ReadFixLen(pos, 4);

	vector<CsarCgrp> cgrps;

	for (uint32_t i = 0; i < cgrpCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2208, Ctx.ReadFixLen(pos, 4))) { return false; }

		CsarCgrp cgrp;
		cgrp.Offset = Data + infoOffset + 8 + infoCgrpOffset + Ctx.ReadFixLen(pos, 4);

		cgrps.push_back(cgrp);
	}

	for (uint32_t i = 0; i < cgrpCount; ++i)
	{
		pos = cgrps[i].Offset;

		cgrps[i].Id = Ctx.ReadFixLen(pos, 4);

		if (cgrps[i].Id == 0xFFFFFFFF)
		{
			continue;
		}

		if (!Ctx.Assert(pos, 0x1, Ctx.ReadFixLen(pos, 4))) { return false; }

		cgrps[i].FileName = TypedName(strgOffset != 0xFFFFFFFF ? strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cgrps[i].Id), "GROUP");

		if (files[cgrps[i].Id].Offset != nullptr)
		{
			pos = files[cgrps[i].Id].Offset + 12;

			uint32_t cgrpLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			Ctx.CheckBounds(pos, cgrpLength);

			string grpFile = (archiveDir / (cgrps[i].FileName + ".bcgrp")).string();
			ofstream ofs(grpFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cgrpLength);
			ofs.close();

			Cgrp cgrp(grpFile.c_str(), &Cwars, cseqsFromCsar, namesById, Opts, Ctx);

			if (!cgrp.Extract())
			{
				return false;
			}
		}
		else
		{
			// The group's data is not embedded in this archive -- it lives in an
			// external sibling .bcgrp that caesar does not load yet, so the banks,
			// wave archives and sequences it references stay absent (their named
			// directories are left empty). Surface it instead of dropping silently.
			Ctx.Warning(cgrps[i].Offset, "group " + cgrps[i].FileName + " lives in an external .bcgrp (not loaded)", "external group files not loaded (their banks/sequences are absent)");
		}
	}

	Ctx.Dump((archiveDir / (stem.string() + ".log")).string());

	return true;
}
