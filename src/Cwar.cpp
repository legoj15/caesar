#include "Cwar.hpp"
#include "Common.hpp"
#include "Cwav.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

Cwar::Cwar(const string& fileName, uint8_t* data, streamoff length, bool ownsData, ParseContext& ctx) : Ctx(ctx), FileName(fileName), OwnsData(ownsData)
{
	Length = length;

	// The parent already holds these bytes -- the span its just-written .bcwar
	// was serialised from -- so take them directly instead of re-opening the
	// file we just wrote. A zero-length span is the same degenerate condition
	// the old file-path constructor rejected when it re-read an empty file
	// (RequireOpen on length <= 0); preserve that error identically, and fire it
	// before the Push echo so the stdout stream stays byte-for-byte unchanged.
	Ctx.RequireOpen(true, Length, FileName);

	if (OwnsData)
	{
		// The parent buffer does not outlive this child: a group-resident wave
		// archive is built from the stack-local Cgrp's Data but stored in the
		// archive-lifetime shared Cwars map, so it must own a private copy.
		Data = new uint8_t[Length];
		copy(data, data + Length, Data);
	}
	else
	{
		// The parent buffer provably outlives this child (a direct Csar wave
		// archive lives in Csar's own Cwars, freed before Csar::Data); borrow it.
		Data = data;
	}

	Ctx.Push(filesystem::path(FileName).filename().string(), Data, Length);
}

Cwar::~Cwar()
{
	for (auto cwav : Cwavs)
	{
		delete cwav;
	}

	Ctx.Pop();

	// Only free Data when it is our own copy; a borrowed span belongs to the
	// parent (freed there, after this child -- see the construction sites).
	if (OwnsData)
	{
		delete[] Data;
	}
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
