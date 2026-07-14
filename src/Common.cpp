#include "Common.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

// Verify that a read of `bytes` bytes starting at `pos` stays inside one of the
// currently-loaded file buffers. Offsets in these archives are read from the
// file itself, so a corrupt or truncated file can point anywhere; without this
// the readers would walk off the end of the buffer and crash. Throwing here
// turns that into a clean error caught by main.
void ParseContext::CheckBounds(uint8_t* pos, size_t bytes)
{
	for (size_t i = Buffers.size(); i-- > 0; )
	{
		const Range& r = Buffers[i];

		if (pos >= r.base && pos < r.end)
		{
			if (bytes > static_cast<size_t>(r.end - pos))
			{
				ostringstream msg;
				msg << "archive damaged: a read of " << bytes << " byte(s) runs past the end of \""
					<< r.name << "\" (at offset 0x" << hex << uppercase << (pos - r.base) << ")";

				throw runtime_error(msg.str());
			}

			return;
		}
	}

	throw runtime_error("archive damaged: a file offset points outside the loaded data");
}

int32_t ParseContext::ReadFixLen(uint8_t*& pos, size_t bytes, bool littleEndian, bool isSigned)
{
	CheckBounds(pos, bytes);

	int32_t result = 0;

	for (size_t i = 0; i < bytes; ++i)
	{
		result |= *pos++ << ((littleEndian ? i : bytes - i - 1) * 8);
	}

	if (isSigned && (result >= (1 << ((bytes * 8) - 1))))
	{
		result -= 1 << (bytes * 8);
	}

	return result;
}

int32_t ParseContext::ReadVarLen(uint8_t*& pos)
{
	int32_t result = 0;

	while (true)
	{
		CheckBounds(pos, 1);

		uint8_t b = *pos++;
		result = (result << 7) | (b & 0x7F);

		if (!(b & 0x80))
		{
			break;
		}
	}

	return result;
}

void WriteFixLen(vector<uint8_t>& out, uint32_t value, size_t bytes, bool littleEndian)
{
	for (size_t i = 0; i < bytes; ++i)
	{
		out.push_back(static_cast<uint8_t>(value >> ((littleEndian ? i : bytes - i - 1) * 8)));
	}
}

void WriteVarLen(vector<uint8_t>& out, uint32_t value)
{
	// Build the 7-bit groups least-significant first (the low group carries no
	// continue bit), then emit most-significant first -- the minimum-length form
	// ReadVarLen decodes back to the same value.
	uint8_t groups[5];
	size_t n = 0;

	groups[n++] = value & 0x7F;
	value >>= 7;

	while (value > 0)
	{
		groups[n++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
		value >>= 7;
	}

	while (n-- > 0)
	{
		out.push_back(groups[n]);
	}
}

// Report something the converter could not fully handle. A bare call is
// per-item detail that only appears under -w (unchanged behaviour). When a
// noticeCategory is given, the site is also flagged as dropped/approximated
// content: it is tallied under that category and surfaced by default in the
// per-input FlushNotices summary, so a normal run tells the user what it left
// out even without -w. The verbose positional line below still needs -w.
void ParseContext::Warning(uint8_t* pos, string msg, const string& noticeCategory)
{
	if (!noticeCategory.empty())
	{
		++Notices[noticeCategory];
	}

	if (ShowWarnings)
	{
		cerr << hex << setfill('0') << uppercase << endl;
		cerr << "WARNING IN\t" << FileNames.top() << endl;
		cerr << "AT POSITION\t0x" << setw(8) << pos - Offsets.top() << endl;
		cerr << "MESSAGE\t\t" << msg << endl;
		cerr << endl;
	}
}

// Offset-taking overload (see the header). Identical to the pointer form except
// that AT POSITION is the caller-supplied value verbatim -- no pointer
// subtraction against Offsets.top(), so the position is correct even when the
// stack top is a different buffer than the one the warning belongs to (the
// heap-layout nondeterminism the pointer form suffers on deferred/borrowed
// walks). The `hex << setfill('0') << uppercase << setw(8)` formatting matches
// byte-for-byte, so a site that already resolved to its own buffer prints the
// same bytes through either overload.
void ParseContext::Warning(uint32_t position, string msg, const string& noticeCategory)
{
	if (!noticeCategory.empty())
	{
		++Notices[noticeCategory];
	}

	if (ShowWarnings)
	{
		cerr << hex << setfill('0') << uppercase << endl;
		cerr << "WARNING IN\t" << FileNames.top() << endl;
		cerr << "AT POSITION\t0x" << setw(8) << position << endl;
		cerr << "MESSAGE\t\t" << msg << endl;
		cerr << endl;
	}
}

// Print the "content skipped or approximated" summary gathered while processing
// one top-level input, then reset for the next. Shown by default (independent of
// -w) so a normal run reports what it dropped; -w adds the per-item detail. A no
// -op when nothing was dropped, so clean archives stay quiet.
void ParseContext::FlushNotices(const string& inputName)
{
	if (Notices.empty())
	{
		return;
	}

	// Warning() leaves the stream in hex/uppercase/zero-fill; restore decimal.
	cerr << dec << nouppercase << setfill(' ') << endl;
	cerr << "NOTE\t\tcontent skipped or approximated in \"" << inputName
		<< "\" (use -w for per-item detail):" << endl;

	for (const auto& notice : Notices)
	{
		cerr << "\t\t" << setw(6) << notice.second << "  " << notice.first << endl;
	}

	cerr << endl;

	Notices.clear();
}

void ParseContext::Push(string fileName, uint8_t* data, streamoff length)
{
	FileNames.push(fileName);
	Offsets.push(data);
	Buffers.push_back({ data, data + static_cast<size_t>(length), fileName });

	cout << FileNames.top() << endl;
}

void ParseContext::Pop()
{
	Offsets.pop();
	FileNames.pop();

	if (!Buffers.empty())
	{
		Buffers.pop_back();
	}
}

// Reject a failed or empty file open before its length is used to allocate a
// buffer. A missing/unreadable file yields a length of -1, which would
// otherwise become an enormous allocation and crash.
void ParseContext::RequireOpen(bool streamOk, streamoff length, const string& fileName)
{
	if (!streamOk || length <= 0)
	{
		throw runtime_error("could not open or read file (missing, empty, or unreadable): " + fileName);
	}
}

// Normalize an archive-supplied output name. First replace characters that are
// illegal in file names (the stored name is untrusted, and a path separator
// could otherwise let extraction escape the output directory). Then, if the
// name is only digits — an item the archive left unnamed, identified only by
// its id — give it a type prefix like "BANK_206" so it is recognizable and
// banks/wave-archives/etc. that share a number don't collide. Names that carry
// a real symbol from the archive are returned as-is (aside from sanitizing).
// Context-free (a pure name transform), so it stays a free function through the
// fold rather than living on the context.
string TypedName(const string& name, const string& type)
{
	string clean = name;

	for (char& c : clean)
	{
		if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
			c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 0x20)
		{
			c = '_';
		}
	}

	if (clean.empty())
	{
		return clean;
	}

	for (unsigned char c : clean)
	{
		if (!isdigit(c))
		{
			return clean;
		}
	}

	return type + "_" + clean;
}

void ParseContext::Analyse(string tag, uint32_t val)
{
	Log.push_back(FileNames.top() + "," + tag + "," + to_string(val));
}

void ParseContext::Dump(string fileName)
{
	ofstream ofs(fileName);
	ofs << "fileName,tag,val" << endl;

	for (size_t i = 0; i < Log.size(); ++i)
	{
		ofs << Log[i] << endl;
	}

	ofs.close();
}
