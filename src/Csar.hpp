#pragma once

#include "Cwar.hpp"
#include "Options.hpp"

#include <cstdint>
#include <ios>
#include <map>
#include <string>

struct ParseContext;

struct CsarStrg
{
	uint8_t* Offset;
	uint32_t Length;

	std::string String;
};

struct CsarFile
{
	uint8_t* Offset;
	uint32_t Length;

	std::string Location = "";
};

struct CsarCbnk
{
	uint8_t* Offset;

	uint32_t Id;
	std::string FileName;
};

struct CsarCseq
{
	uint8_t* Offset;

	std::string FileName;
};

struct CsarCgrp
{
	uint8_t* Offset;

	uint32_t Id;
	std::string FileName;
};

struct Csar
{
	ParseContext& Ctx;

	std::string FileName;
	std::streamoff Length;
	uint8_t* Data = nullptr;

	std::map<int, Cwar*> Cwars;
	Options Opts;

	Csar(const char* fileName, const Options& opts, ParseContext& ctx);
	~Csar();

	// outputDir is the base directory to extract into. When empty (the default),
	// each archive is extracted beside its own input file, into a folder named
	// after the archive (the historical behaviour). Otherwise every archive is
	// extracted into "<outputDir>/<archive-name>/".
	bool Extract(const std::string& outputDir = "");
};
