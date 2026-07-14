#pragma once

#include "Cwav.hpp"

#include <cstdint>
#include <ios>
#include <string>
#include <vector>

struct ParseContext;

struct CwarCwav
{
	uint8_t* Offset;
	uint32_t Length;
};

struct Cwar
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	std::vector<Cwav*> Cwavs;

	Cwar(const char* fileName, ParseContext& ctx);
	~Cwar();
	bool Extract();
};
