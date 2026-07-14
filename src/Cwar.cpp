#include "Cwar.hpp"
#include "Common.hpp"
#include "Cwav.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

Cwar::Cwar(const char* fileName, ParseContext& ctx) : Ctx(ctx), FileName(fileName)
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

Cwar::~Cwar()
{
	for (auto cwav : Cwavs)
	{
		delete cwav;
	}

	Ctx.Pop();

	delete[] Data;
}

bool Cwar::Extract()
{
	uint8_t* pos = Data;

	if (!Ctx.Assert(pos, 0x43574152, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert(pos, 0xFEFF, Ctx.ReadFixLen(pos, 2))) { return false; }
	if (!Ctx.Assert(pos, 0x40, Ctx.ReadFixLen(pos, 2))) { return false; }

	[[maybe_unused]] uint32_t cwarVersion = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert<uint64_t>(pos, Length, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x2, Ctx.ReadFixLen(pos, 4))) { return false; }
	if (!Ctx.Assert(pos, 0x6800, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t infoOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t infoLength = Ctx.ReadFixLen(pos, 4);

	if (!Ctx.Assert(pos, 0x6801, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t fileOffset = Ctx.ReadFixLen(pos, 4);
	uint32_t fileLength = Ctx.ReadFixLen(pos, 4);

	pos = Data + infoOffset;

	if (!Ctx.Assert(pos, 0x494E464F, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, infoLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	uint32_t cwavCount = Ctx.ReadFixLen(pos, 4);

	vector<CwarCwav> cwavs;

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		if (!Ctx.Assert(pos, 0x1F00, Ctx.ReadFixLen(pos, 4))) { return false; }

		CwarCwav cwav{};
		cwav.Offset = Data + fileOffset + 8 + Ctx.ReadFixLen(pos, 4);
		cwav.Length = Ctx.ReadFixLen(pos, 4);

		cwavs.push_back(cwav);
	}

	pos = Data + fileOffset;

	if (!Ctx.Assert(pos, 0x46494C45, Ctx.ReadFixLen(pos, 4, false))) { return false; }
	if (!Ctx.Assert<uint32_t>(pos, fileLength, Ctx.ReadFixLen(pos, 4))) { return false; }

	// Write each sub-file into this wave-archive's own directory (the folder its
	// dump was written into), composed from the full path rather than relying on
	// the working directory.
	filesystem::path dir = filesystem::path(FileName).parent_path();

	for (uint32_t i = 0; i < cwavCount; ++i)
	{
		Ctx.CheckBounds(cwavs[i].Offset, cwavs[i].Length);

		string cwavFile = (dir / (to_string(i) + ".bcwav")).string();
		ofstream ofs(cwavFile, ofstream::binary);
		ofs.write(reinterpret_cast<const char*>(cwavs[i].Offset), cwavs[i].Length);
		ofs.close();

		Cwavs.push_back(new Cwav(cwavFile.c_str(), Ctx));

		if (!Cwavs[i]->Convert())
		{
			return false;
		}
	}

	return true;
}
