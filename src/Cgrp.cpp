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

Cgrp::Cgrp(const char* fileName, map<int, Cwar*>* cwars, const map<int, bool>& cseqsFromCsar, const Options& opts) : FileName(fileName), Cwars(cwars), CseqsFromCsar(cseqsFromCsar), Opts(opts)
{
	ifstream ifs(FileName, ios::binary | ios::ate);

	Length = ifs.tellg();
	Common::RequireOpen(ifs.good(), Length, FileName);
	Data = new uint8_t[Length];

	Common::Push(filesystem::path(FileName).filename().string(), Data, Length);

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

	Common::Pop();

	delete[] Data;
}

bool Cgrp::Extract()
{
	// A group shares the archive's output directory (the folder its own dump was
	// written into); its wave-archives/banks get sub-folders there and its
	// sequences sit directly in it. Compose everything from that base instead of
	// changing the working directory.
	path baseDir = path(FileName).parent_path();

	uint8_t* pos = Data;

	if (!Common::Assert(pos, 0x43475250, ReadFixLen(pos, 4, false))) { return false; }
	if (!Common::Assert(pos, 0xFEFF, ReadFixLen(pos, 2))) { return false; }
	if (!Common::Assert(pos, 0x40, ReadFixLen(pos, 2))) { return false; }

	uint32_t cgrpVersion = ReadFixLen(pos, 4);

	if (!Common::Assert<uint64_t>(pos, Length, ReadFixLen(pos, 4))) { return false; }

	uint32_t chunkCount = ReadFixLen(pos, 4);

	uint32_t infoOffset = 0;
	uint32_t infoLength = 0;

	uint32_t fileOffset = 0;
	uint32_t fileLength = 0;

	uint32_t infxOffset = 0;
	uint32_t infxLength = 0;

	for (uint32_t i = 0; i < chunkCount; ++i)
	{
		uint32_t chunkId = ReadFixLen(pos, 4);

		switch (chunkId)
		{
			case 0x7800:
				infoOffset = ReadFixLen(pos, 4);
				infoLength = ReadFixLen(pos, 4);

				break;

			case 0x7801:
				fileOffset = ReadFixLen(pos, 4);
				fileLength = ReadFixLen(pos, 4);

				break;

			case 0x7802:
				infxOffset = ReadFixLen(pos, 4);
				infxLength = ReadFixLen(pos, 4);

				break;

			default:
			{
				Common::Error(pos - 4, "A valid chunk type", chunkId);

				return false;
			}
		}
	}

	pos = Data + infoOffset;

	if (!Common::Assert(pos, 0x494E464F, ReadFixLen(pos, 4, false))) { return false; }
	if (!Common::Assert<uint32_t>(pos, infoLength, ReadFixLen(pos, 4))) { return false; }

	uint32_t fileCount = ReadFixLen(pos, 4);

	vector<uint8_t*> fileOffsets;

	for (uint32_t i = 0; i < fileCount; ++i)
	{
		if (!Common::Assert(pos, 0x7900, ReadFixLen(pos, 4))) { return false; }

		fileOffsets.push_back(Data + infoOffset + 8 + ReadFixLen(pos, 4));
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
		file.Id = ReadFixLen(pos, 4);
		uint32_t marker = ReadFixLen(pos, 4);
		uint32_t offset = ReadFixLen(pos, 4);
		file.Offset = marker == 0x1F00 ? Data + fileOffset + 8 + offset : nullptr;
		file.Length = ReadFixLen(pos, 4);

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

		uint32_t fileId = ReadFixLen(pos, 4, false);

		switch (fileId)
		{
			case 0x43574152:
			{
				pos += 8;

				uint32_t cwarLength = ReadFixLen(pos, 4);

				pos -= 16;

				string name = Common::TypedName(to_string(files[i].Id), "WARC");

				path warcDir = baseDir / name;
				create_directories(warcDir);

				Common::CheckBounds(pos, cwarLength);

				string warcFile = (warcDir / (name + ".bcwar")).string();
				ofstream ofs(warcFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cwarLength);
				ofs.close();

				(*Cwars)[files[i].Id] = new Cwar(warcFile.c_str());

				if (!(*Cwars)[files[i].Id]->Extract())
				{
					return false;
				}

				break;
			}

			case 0x43424E4B:
			{
				pos += 8;

				uint32_t cbnkLength = ReadFixLen(pos, 4);

				pos -= 16;

				string name = Common::TypedName(to_string(files[i].Id), "BANK");

				path bankDir = baseDir / name;
				create_directories(bankDir);

				Common::CheckBounds(pos, cbnkLength);

				string bankFile = (bankDir / (name + ".bcbnk")).string();
				ofstream ofs(bankFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cbnkLength);
				ofs.close();

				Cbnks.push_back(new Cbnk(bankFile.c_str(), Cwars, Opts));

				break;
			}

			case 0x43534551:
			{
				pos += 8;

				uint32_t cseqLength = ReadFixLen(pos, 4);

				pos -= 16;

				string name = Common::TypedName(to_string(files[i].Id), "SEQ");

				Common::CheckBounds(pos, cseqLength);

				string seqFile = (baseDir / (name + ".bcseq")).string();
				ofstream ofs(seqFile, ofstream::binary);
				ofs.write(reinterpret_cast<const char*>(pos), cseqLength);
				ofs.close();

				Cseqs.push_back(new Cseq(seqFile.c_str()));

				break;
			}

			case 0x43575344:
			{
				Common::Warning(pos - 4, "Skipping CWSD", "CWSD wave-sound blocks skipped (sound effects not extracted)");

				break;
			}

			default:
			{
				Common::Error(pos - 4, "A valid file type", fileId);

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

		Common::Warning(pos, "Skipping INFX chunk", "INFX metadata chunks skipped");
	}

	return true;
}
