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
#include <utility>
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

	// Create the archive directory up front (before parsing), matching the
	// historical layout: even a header-invalid archive left its (empty) directory
	// behind. The parse-then-export split keeps that -- the model is built first,
	// then the whole output surface is emitted from it.
	create_directories(archiveDir);

	if (!Parse())
	{
		return false;
	}

	return Export(archiveDir.string());
}

bool Csar::Parse()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43534152, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	Version = Ctx.ReadFixLen(pos, 4);
	uint32_t declaredLength = Ctx.ReadFixLen(pos, 4);

	if (Version != 0x02000000)
	{
		if (!Ctx.Assert<uint64_t>(pos, Length, declaredLength)) { return false; }
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
	FileLength = Ctx.ReadFixLen(pos, 4);

	if (strgOffset != 0xFFFFFFFF)
	{
		pos = Data + strgOffset;

		if (!Ctx.Assert(pos, 0x53545247, Ctx.ReadFixLen(pos, 4, false))) { return false; }
		if (!Ctx.Assert<uint32_t>(pos, strgLength, Ctx.ReadFixLen(pos, 4))) { return false; }
		if (!Ctx.Assert(pos, 0x2400, Ctx.ReadFixLen(pos, 4))) { return false; }

		StrgStringsOffset = Ctx.ReadFixLen(pos, 4);

		if (!Ctx.Assert(pos, 0x2401, Ctx.ReadFixLen(pos, 4))) { return false; }

		StrgUnknownOffset = Ctx.ReadFixLen(pos, 4);
		uint32_t strgCount = Ctx.ReadFixLen(pos, 4);

		for (uint32_t i = 0; i < strgCount; ++i)
		{
			if (!Ctx.Assert(pos, 0x1F01, Ctx.ReadFixLen(pos, 4))) { return false; }

			CsarStrg strg;
			strg.Offset = strgOffset + 24 + Ctx.ReadFixLen(pos, 4);
			strg.Length = Ctx.ReadFixLen(pos, 4);

			Strgs.push_back(strg);
		}

		for (uint32_t i = 0; i < strgCount; ++i)
		{
			Ctx.CheckBounds(pos, Strgs[i].Length - 1);
			Strgs[i].String = string(reinterpret_cast<const char*>(pos), Strgs[i].Length - 1);

			pos = i != (strgCount - 1) ? Data + Strgs[i + 1].Offset : pos + Strgs[i].Length;
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
				InfoEndOffset = Ctx.ReadFixLen(pos, 4); break;

			default:
				Ctx.Error(pos - 4, "A valid chunk type", offsetId);

				return false;
		}
	}

	pos = Data + infoOffset + 8 + infoFileOffset;

	uint32_t fileCount = Ctx.ReadFixLen(pos, 4);

	vector<uint32_t> fileOffsets;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x220A, Ctx.ReadFixLen(pos, 4))) { return false; }

		fileOffsets.push_back(infoOffset + 8 + infoFileOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		pos = Data + fileOffsets[i];

		CsarFile file;
		uint32_t fileId = Ctx.ReadFixLen(pos, 4);

		switch (fileId)
		{
			case 0x220C:
			{
				// 8-byte little-endian field: low word is the size (0xC), high
				// word is reserved (0). Read it as two 32-bit halves -- ReadFixLen
				// is a 32-bit reader, so a width-8 call would shift by up to 56
				// bits (undefined behaviour) and fold the top four bytes back into
				// the low word under MSVC.
				if (!Ctx.Assert(pos, 0xC, Ctx.ReadFixLen(pos, 4))) { return false; }
				if (!Ctx.Assert(pos, 0x0, Ctx.ReadFixLen(pos, 4))) { return false; }

				// The reserved third word: read (advancing pos), retained, and
				// replayed to the .log by Export -- the read order/value are
				// byte-identical, only the Analyse call moves to the emit walk.
				file.Is220C = true;
				file.Word220C08 = Ctx.ReadFixLen(pos, 4);

				uint32_t relOffset = fileOffset + 8 + Ctx.ReadFixLen(pos, 4);
				uint32_t len = Ctx.ReadFixLen(pos, 4);

				if (((Data + relOffset) >= (Data + Length)) || (len == 0xFFFFFFFF))
				{
					file.Internal = false;
				}
				else
				{
					file.Internal = true;
					file.Offset = relOffset;
					file.Length = len;
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
				break;

			default:
				Ctx.Error(pos - 4, "A valid file type", fileId);

				return false;
		}

		Files.push_back(file);
	}

	pos = Data + infoOffset + 8 + infoCwarOffset;

	uint32_t cwarCount = Ctx.ReadFixLen(pos, 4);

	vector<uint32_t> cwarOffsets;

	for (uint32_t i = 0; i < cwarCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2207, Ctx.ReadFixLen(pos, 4))) { return false; }

		cwarOffsets.push_back(infoOffset + 8 + infoCwarOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < cwarCount; ++i)
	{
		CsarCwar cwar;
		cwar.Offset = cwarOffsets[i];

		pos = Data + cwar.Offset;

		cwar.Id = Ctx.ReadFixLen(pos, 4);
		cwar.Word04 = Ctx.ReadFixLen(pos, 4);

		uint32_t hasFileName = Ctx.ReadFixLen(pos, 4);
		cwar.HasFileName = hasFileName != 0;

		cwar.FileName = TypedName(hasFileName && (strgOffset != 0xFFFFFFFF) ? Strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cwar.Id), "WARC");

		NamesById[cwar.Id] = cwar.FileName;

		CwarRecords.push_back(cwar);
	}

	pos = Data + infoOffset + 8 + infoCbnkOffset;

	uint32_t cbnkCount = Ctx.ReadFixLen(pos, 4);

	vector<uint32_t> cbnkOffsets;

	for (uint32_t i = 0; i < cbnkCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2206, Ctx.ReadFixLen(pos, 4))) { return false; }

		cbnkOffsets.push_back(infoOffset + 8 + infoCbnkOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < cbnkCount; ++i)
	{
		CsarCbnk cbnk;
		cbnk.Offset = cbnkOffsets[i];

		pos = Data + cbnk.Offset;

		cbnk.Id = Ctx.ReadFixLen(pos, 4);
		cbnk.Word04 = Ctx.ReadFixLen(pos, 4);
		cbnk.Word08 = Ctx.ReadFixLen(pos, 4);
		cbnk.Word0C = Ctx.ReadFixLen(pos, 4);

		cbnk.FileName = TypedName(strgOffset != 0xFFFFFFFF ? Strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cbnk.Id), "BANK");

		NamesById[cbnk.Id] = cbnk.FileName;

		CbnkRecords.push_back(cbnk);
	}

	pos = Data + infoOffset + 8 + infoCseqOffset;

	uint32_t cseqCount = Ctx.ReadFixLen(pos, 4);

	vector<uint32_t> cseqOffsets;

	for (uint32_t i = 0; i < cseqCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2200, Ctx.ReadFixLen(pos, 4))) { return false; }

		cseqOffsets.push_back(infoOffset + 8 + infoCseqOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < cseqCount; ++i)
	{
		CsarCseq cseq;
		cseq.Offset = cseqOffsets[i];

		pos = Data + cseq.Offset;

		cseq.Id = Ctx.ReadFixLen(pos, 4);
		cseq.Word04 = Ctx.ReadFixLen(pos, 4);
		cseq.Word08 = Ctx.ReadFixLen(pos, 4);
		cseq.Type = Ctx.ReadFixLen(pos, 4);
		cseq.CbnkOffset = Ctx.ReadFixLen(pos, 4);
		cseq.Word14 = Ctx.ReadFixLen(pos, 4);

		cseq.FileName = TypedName(strgOffset != 0xFFFFFFFF ? Strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cseq.Id), "SEQ");

		NamesById[cseq.Id] = cseq.FileName;

		// The skip warning (external stream / CWSD) points here: the type field,
		// pos - 16 from just after the symbol lookup. Store it span-relative so
		// Export reconstructs Data + WarnOffset and hands it to the pointer-form
		// Warning, preserving this Csar site's exact AT POSITION (pinned behaviour).
		cseq.WarnOffset = static_cast<uint32_t>((pos - 16) - Data);

		switch (cseq.Type)
		{
			case 0x2201:
			case 0x2202:
				break;

			case 0x2203:
			{
				if (Files[cseq.Id].Internal)
				{
					cseq.Convertible = true;

					// The entry's start offset within the sequence data (relative to
					// its DATA+8). These archives map many entries onto one shared
					// sequence bank, each beginning at its own offset; without this
					// every entry would convert from the top of the bank.
					//
					// The field sits just before the bank-reference sub-structure that
					// CbnkOffset locates, so its position tracks CbnkOffset (which
					// grows when the record carries extra optional fields). Anchoring
					// to CbnkOffset handles both record layouts seen in the wild: the
					// common one (CbnkOffset 0x44 -> field at +0x54) and a +0xC-larger
					// one (CbnkOffset 0x50 -> field at +0x60). A fixed +0x54 would
					// silently read the wrong word for the larger layout.
					uint8_t* startPos = Data + cseq.Offset + cseq.CbnkOffset + 0x10;
					cseq.StartOffset = Ctx.ReadFixLen(startPos, 4);

					pos += cseq.CbnkOffset;
					cseq.BankIndex = Ctx.ReadFixLen(pos, 2);
				}

				break;
			}

			default:
				Ctx.Error(pos - 16, "A valid music type", cseq.Type);

				return false;
		}

		CseqRecords.push_back(cseq);
	}

	pos = Data + infoOffset + 8 + infoPlayerOffset;

	PlayerTableOffset = infoOffset + 8 + infoPlayerOffset;
	uint32_t playerCount = Ctx.ReadFixLen(pos, 4);

	for (uint32_t i = 0; i < playerCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2209, Ctx.ReadFixLen(pos, 4))) { return false; }

		PlayerEntryOffsets.push_back(infoOffset + 8 + infoPlayerOffset + Ctx.ReadFixLen(pos, 4));
	}

	pos = Data + infoOffset + 8 + infoSetOffset;

	SetTableOffset = infoOffset + 8 + infoSetOffset;
	uint32_t setCount = Ctx.ReadFixLen(pos, 4);

	for (uint32_t i = 0; i < setCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2204, Ctx.ReadFixLen(pos, 4))) { return false; }

		SetEntryOffsets.push_back(infoOffset + 8 + infoSetOffset + Ctx.ReadFixLen(pos, 4));
	}

	pos = Data + infoOffset + 8 + infoCgrpOffset;

	uint32_t cgrpCount = Ctx.ReadFixLen(pos, 4);

	vector<uint32_t> cgrpOffsets;

	for (uint32_t i = 0; i < cgrpCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x2208, Ctx.ReadFixLen(pos, 4))) { return false; }

		cgrpOffsets.push_back(infoOffset + 8 + infoCgrpOffset + Ctx.ReadFixLen(pos, 4));
	}

	for (uint32_t i = 0; i < cgrpCount; ++i)
	{
		CsarCgrp cgrp;
		cgrp.Offset = cgrpOffsets[i];

		pos = Data + cgrp.Offset;

		cgrp.Id = Ctx.ReadFixLen(pos, 4);

		if (cgrp.Id == 0xFFFFFFFF)
		{
			// A placeholder entry: nothing more is read for it. Keep it in the tree
			// (Valid == false) so the record set is complete; Export skips it.
			CgrpRecords.push_back(cgrp);

			continue;
		}

		cgrp.Valid = true;

		if (!Ctx.Assert(pos, 0x1, Ctx.ReadFixLen(pos, 4))) { return false; }

		cgrp.FileName = TypedName(strgOffset != 0xFFFFFFFF ? Strgs[Ctx.ReadFixLen(pos, 4)].String : to_string(cgrp.Id), "GROUP");

		CgrpRecords.push_back(cgrp);
	}

	return true;
}

bool Csar::Export(const string& archiveDir)
{
	path baseDir(archiveDir);

	// The .log rows this level contributed were logged as it walked; each was
	// tagged with whatever frame topped the diagnostic stack at that moment (the
	// last direct wave-archive's frame, not the archive's, once any wave archive is
	// built). Replaying them here -- in the same walk order, before the same child
	// is constructed -- reproduces those tags and that order byte-for-byte, which a
	// pure parse pass could not (it has no child frames pushed).

	// File-table words first (top is still the archive frame -- no child built yet).
	for (const CsarFile& file : Files)
	{
		if (file.Is220C)
		{
			Ctx.Analyse("0x220C 0x08", file.Word220C08);
		}
	}

	for (const CsarCwar& cwar : CwarRecords)
	{
		Ctx.Analyse("Cwar 0x04", cwar.Word04);

		if (Files[cwar.Id].Internal)
		{
			uint8_t* pos = Data + Files[cwar.Id].Offset + 12;

			uint32_t cwarLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			path warcDir = baseDir / cwar.FileName;
			create_directories(warcDir);

			Ctx.CheckBounds(pos, cwarLength);

			string warcFile = (warcDir / (cwar.FileName + ".bcwar")).string();
			ofstream ofs(warcFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cwarLength);
			ofs.close();

			// The .bcwar was just written from [pos, pos + cwarLength) into Csar's
			// own buffer; hand that span to the child instead of re-reading the
			// file. This Cwar lives in Csar::Cwars, freed before Csar::Data in
			// ~Csar, so borrowing (ownsData = false) is safe.
			Cwars[cwar.Id] = new Cwar(warcFile, pos, cwarLength, false, Ctx);

			// Parse the model, then write the sub-files and recurse into the child
			// Cwavs, invoked back to back per file so the .bcwav dumps, the stdout
			// echoes, and every parse-phase diagnostic stay byte-identical.
			if (!Cwars[cwar.Id]->Parse() || !Cwars[cwar.Id]->Export())
			{
				return false;
			}
		}
		else
		{
			Cwars[cwar.Id] = nullptr;
		}
	}

	for (const CsarCbnk& cbnk : CbnkRecords)
	{
		Ctx.Analyse("Cbnk 0x04", cbnk.Word04);
		Ctx.Analyse("Cbnk 0x08", cbnk.Word08);
		Ctx.Analyse("Cbnk 0x0C", cbnk.Word0C);

		// Create the bank directory unconditionally (even when the bank has no
		// data), matching the historical layout.
		path bankDir = baseDir / cbnk.FileName;
		create_directories(bankDir);

		if (Files[cbnk.Id].Internal)
		{
			uint8_t* pos = Data + Files[cbnk.Id].Offset + 12;

			uint32_t cbnkLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			Ctx.CheckBounds(pos, cbnkLength);

			string bankFile = (bankDir / (cbnk.FileName + ".bcbnk")).string();
			ofstream ofs(bankFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cbnkLength);
			ofs.close();

			// The .bcbnk was just written from [pos, pos + cbnkLength) into Csar's
			// own buffer; hand that span to the child instead of re-reading the
			// file. This Cbnk is a stack local, converted and destroyed here while
			// Csar::Data is alive, so borrowing is safe.
			Cbnk bank(bankFile, pos, cbnkLength, &Cwars, Opts, Ctx);

			if (!bank.Convert())
			{
				return false;
			}
		}
	}

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

	for (const CsarCseq& cseq : CseqRecords)
	{
		Ctx.Analyse("Cseq 0x04", cseq.Word04);
		Ctx.Analyse("Cseq 0x08", cseq.Word08);
		Ctx.Analyse("Cseq 0x14", cseq.Word14);

		switch (cseq.Type)
		{
			case 0x2201:
			{
				Ctx.Warning(Data + cseq.WarnOffset, "Skipping external stream", "external streams skipped (audio stored in a separate file)");

				break;
			}

			case 0x2202:
			{
				Ctx.Warning(Data + cseq.WarnOffset, "Skipping CWSD", "CWSD wave-sound blocks skipped (sound effects not extracted)");

				break;
			}

			case 0x2203:
			{
				if (cseq.Convertible)
				{
					uint8_t* pos = Data + Files[cseq.Id].Offset + 12;

					uint32_t cseqLength = Ctx.ReadFixLen(pos, 4);

					pos -= 16;

					// The sequence is written into the directory of the bank it
					// references (created in the bank loop above).
					path bankDir = baseDir / CbnkRecords[cseq.BankIndex].FileName;

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
					// The suffix is local to this path composition: cseq.FileName and
					// NamesById[id] keep the bare name, so group naming (Cgrp reads
					// NamesById) and log/display uses are unaffected. The suffixed
					// basename does surface in this entry's own per-file notices, which
					// is what tells the collision siblings apart.
					string seqName = cseq.FileName;
					string seqFile = (bankDir / (seqName + ".bcseq")).string();

					auto claimed = claimedSeqFiles.find(seqFile);

					if (claimed != claimedSeqFiles.end() && claimed->second != make_pair(cseq.Id, cseq.StartOffset))
					{
						ostringstream suffixed;
						suffixed << seqName << "_0x" << hex << cseq.StartOffset;
						seqName = suffixed.str();
						seqFile = (bankDir / (seqName + ".bcseq")).string();

						claimed = claimedSeqFiles.find(seqFile);

						if (claimed != claimedSeqFiles.end() && claimed->second != make_pair(cseq.Id, cseq.StartOffset))
						{
							seqName += "_" + to_string(cseq.Id);
							seqFile = (bankDir / (seqName + ".bcseq")).string();
						}
					}

					claimedSeqFiles[seqFile] = make_pair(cseq.Id, cseq.StartOffset);

					ofstream ofs(seqFile, ofstream::binary);
					ofs.write(reinterpret_cast<const char*>(pos), cseqLength);
					ofs.close();

					// The .bcseq was just written from [pos, pos + cseqLength) into
					// Csar's own buffer; hand that span to the child instead of
					// re-reading the file. This Cseq is a stack local, converted and
					// destroyed here while Csar::Data is alive, so borrowing is safe.
					// (StartOffset addresses inside the child's own span, so it is
					// unaffected by where the bytes came from.)
					Cseq seq(seqFile, pos, cseqLength, Ctx);

					// Parse the model, then walk it to the .mid, back to back per
					// file (mirrors the Cwar/Cwav split): the emit walk locates its
					// diagnostics via stored offsets, so the phase boundary stays
					// per-file and the -w positions are the true DATA-relative
					// offsets.
					if (!seq.Parse() || !seq.Export(cseq.StartOffset))
					{
						return false;
					}

					CseqsFromCsar[cseq.Id] = true;
				}

				break;
			}

			default:
				break;
		}
	}

	for (const CsarCgrp& cgrp : CgrpRecords)
	{
		if (!cgrp.Valid)
		{
			continue;
		}

		if (Files[cgrp.Id].Internal)
		{
			uint8_t* pos = Data + Files[cgrp.Id].Offset + 12;

			uint32_t cgrpLength = Ctx.ReadFixLen(pos, 4);

			pos -= 16;

			Ctx.CheckBounds(pos, cgrpLength);

			string grpFile = (baseDir / (cgrp.FileName + ".bcgrp")).string();
			ofstream ofs(grpFile, ofstream::binary);
			ofs.write(reinterpret_cast<const char*>(pos), cgrpLength);
			ofs.close();

			// The .bcgrp was just written from [pos, pos + cgrpLength) into Csar's
			// own buffer; hand that span to the child instead of re-reading the
			// file. This Cgrp is a stack local, extracted and destroyed here while
			// Csar::Data is alive, so borrowing is safe -- and the spans it hands
			// its own Cbnk/Cseq children are windows into this same Csar::Data.
			Cgrp group(grpFile, pos, cgrpLength, &Cwars, CseqsFromCsar, NamesById, Opts, Ctx);

			// Parse the group's model, then run its child-dump/recurse walk, back to
			// back (mirrors the other children): the two-loop deferral inside the
			// group is preserved, so group-path warnings still fire with a sibling
			// frame on top exactly as before.
			if (!group.Parse() || !group.Export())
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
			Ctx.Warning(Data + cgrp.Offset, "group " + cgrp.FileName + " lives in an external .bcgrp (not loaded)", "external group files not loaded (their banks/sequences are absent)");
		}
	}

	Ctx.Dump((baseDir / (path(FileName).stem().string() + ".log")).string());

	return true;
}
