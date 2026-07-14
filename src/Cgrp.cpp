#include "Cgrp.hpp"
#include "Cbnk.hpp"
#include "Common.hpp"
#include "Cseq.hpp"
#include "Cwar.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace std;
using namespace filesystem;

Cgrp::Cgrp(const char* fileName, map<int, Cwar*>* cwars, const map<int, bool>& cseqsFromCsar, const map<int, string>& namesFromCsar, const Options& opts, ParseContext& ctx) : Ctx(ctx), FileName(fileName), Cwars(cwars), CseqsFromCsar(cseqsFromCsar), NamesFromCsar(namesFromCsar), Opts(opts)
{
	ifstream ifs(FileName, ios::binary | ios::ate);

	Length = ifs.tellg();
	Ctx.RequireOpen(ifs.good(), Length, FileName);
	Data = new uint8_t[Length];

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);

	ifs.seekg(0, ios::beg);
	ifs.read(reinterpret_cast<char*>(Data), Length);
	ifs.close();
}

Cgrp::~Cgrp()
{
	for (auto cseq : Cseqs)
	{
		delete cseq;
	}

	for (auto cbnk : Cbnks)
	{
		delete cbnk;
	}

	Ctx.Pop();

	delete[] Data;
}

bool Cgrp::Extract()
{
	// A group shares the archive's output directory (the folder its own dump was
	// written into); its wave-archives/banks get sub-folders there and its
	// sequences sit directly in it. Compose everything from that base instead of
	// changing the working directory.
	path baseDir = path(FileName).parent_path();

	// Resolve a group-resident file's output name. The group's own file table
	// only carries a numeric id, but the CSAR INFO section already named the same
	// file (by its shared id) -- and for banks it left an empty symbol-named
	// directory at this level. Prefer that CSAR name so the extraction fills the
	// empty directory instead of creating a numeric duplicate beside it; fall back
	// to the numeric type-prefixed name for anything the CSAR did not enumerate.
	auto nameFor = [&](uint32_t id, const string& type) -> string
	{
		auto it = NamesFromCsar.find(id);

		if (it != NamesFromCsar.end() && !it->second.empty())
		{
			return it->second;
		}

		return TypedName(to_string(id), type);
	};

	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43475250, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cgrpVersion = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t chunkCount = Ctx.ReadFixLen(pos, 4);

	uint32_t infoOffset = 0;
	uint32_t infoLength = 0;

	uint32_t fileOffset = 0;
	[[maybe_unused]] uint32_t fileLength = 0;

	uint32_t infxOffset = 0;
	[[maybe_unused]] uint32_t infxLength = 0;

	for (uint32_t i = 0; i < chunkCount; ++i)
	{
		uint32_t chunkId = Ctx.ReadFixLen(pos, 4);

		switch (chunkId)
		{
			case 0x7800:
				infoOffset = Ctx.ReadFixLen(pos, 4);
				infoLength = Ctx.ReadFixLen(pos, 4);

				break;

			case 0x7801:
				fileOffset = Ctx.ReadFixLen(pos, 4);
				fileLength = Ctx.ReadFixLen(pos, 4);

				break;

			case 0x7802:
				infxOffset = Ctx.ReadFixLen(pos, 4);
				infxLength = Ctx.ReadFixLen(pos, 4);

				break;

			default:
			{
				Ctx.Error(pos - 4, "A valid chunk type", chunkId);

				return false;
			}
		}
	}

	pos = Data + infoOffset;

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t fileCount = Ctx.ReadFixLen(pos, 4);

	vector<uint8_t*> fileOffsets;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x7900, Ctx.ReadFixLen(pos, 4))) { return false; }

		fileOffsets.push_back(Data + infoOffset + 8 + Ctx.ReadFixLen(pos, 4));
	}

	vector<CgrpFile> files;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		// Each record is a fixed 16 bytes: Id, a presence marker, an offset, and a
		// length. The offset field must be consumed unconditionally -- reading it
		// only when the marker is 0x1F00 (as a short-circuiting ?: did) left pos 4
		// bytes short on any other marker, so Length picked up the offset field and
		// every following record was parsed from the wrong position. A group holding
		// an external or absent file (marker != 0x1F00) misparsed everything after it.
		CgrpFile file{};
		file.Id = Ctx.ReadFixLen(pos, 4);
		uint32_t marker = Ctx.ReadFixLen(pos, 4);
		uint32_t offset = Ctx.ReadFixLen(pos, 4);
		file.Offset = marker == 0x1F00 ? Data + fileOffset + 8 + offset : nullptr;
		file.Length = Ctx.ReadFixLen(pos, 4);

		files.push_back(file);
	}

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		if (files[i].Offset == nullptr)
		{
			continue;
		}

		if (CseqsFromCsar[files[i].Id] == true)
		{
			continue;
		}

		pos = files[i].Offset;

		uint32_t fileId = Ctx.ReadFixLen(pos, 4, false);

		switch (fileId)
		{
			case 0x43574152:
			{
				pos += 8;

				uint32_t cwarLength = Ctx.ReadFixLen(pos, 4);

				pos -= 16;

				string name = nameFor(files[i].Id, "WARC");

				path warcDir = baseDir / name;
				create_directories(warcDir);

				Ctx.CheckBounds(pos, cwarLength);

				string warcFile = (warcDir / (name + ".bcwar")).string();
				ofstream ofs(warcFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cwarLength);
				ofs.close();

				(*Cwars)[files[i].Id] = new Cwar(warcFile.c_str(), Ctx);

				if (!(*Cwars)[files[i].Id]->Extract())
				{
					return false;
				}

				break;
			}

			case 0x43424E4B:
			{
				pos += 8;

				uint32_t cbnkLength = Ctx.ReadFixLen(pos, 4);

				pos -= 16;

				string name = nameFor(files[i].Id, "BANK");

				path bankDir = baseDir / name;
				create_directories(bankDir);

				Ctx.CheckBounds(pos, cbnkLength);

				string bankFile = (bankDir / (name + ".bcbnk")).string();
				ofstream ofs(bankFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cbnkLength);
				ofs.close();

				Cbnks.push_back(new Cbnk(bankFile.c_str(), Cwars, Opts, Ctx));

				break;
			}

			case 0x43534551:
			{
				pos += 8;

				uint32_t cseqLength = Ctx.ReadFixLen(pos, 4);

				pos -= 16;

				string name = nameFor(files[i].Id, "SEQ");

				Ctx.CheckBounds(pos, cseqLength);

				string seqFile = (baseDir / (name + ".bcseq")).string();
				ofstream ofs(seqFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cseqLength);
				ofs.close();

				Cseqs.push_back(new Cseq(seqFile.c_str(), Ctx));

				break;
			}

			case 0x43575344:
			{
				Ctx.Warning(pos - 4, "Skipping CWSD", "CWSD wave-sound blocks skipped (sound effects not extracted)");

				break;
			}

			default:
			{
				Ctx.Error(pos - 4, "A valid file type", fileId);

				return false;
			}
		}
	}

	for (uint32_t i = 0; i < Cbnks.size(); ++i)
	{
		if (!Cbnks[i]->Convert())
		{
			return false;
		}
	}

	for (uint32_t i = 0; i < Cseqs.size(); ++i)
	{
		if (!Cseqs[i]->Convert())
		{
			return false;
		}
	}

	if (infxOffset)
	{
		pos = Data + infxOffset;

		Ctx.Warning(pos, "Skipping INFX chunk", "INFX metadata chunks skipped");
	}

	return true;
}
