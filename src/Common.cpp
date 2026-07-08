#include "Common.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

bool Common::ShowWarnings = false;
stack<string> Common::FileNames;
stack<uint8_t*> Common::Offsets;
vector<Range> Common::Buffers;
vector<string> Common::Log;

// Verify that a read of `bytes` bytes starting at `pos` stays inside one of the
// currently-loaded file buffers. Offsets in these archives are read from the
// file itself, so a corrupt or truncated file can point anywhere; without this
// the readers would walk off the end of the buffer and crash. Throwing here
// turns that into a clean error caught by main.
void Common::CheckBounds(uint8_t* pos, size_t bytes)
{
	for (size_t i = Common::Buffers.size(); i-- > 0; )
	{
		const Range& r = Common::Buffers[i];

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

int32_t ReadFixLen(uint8_t*& pos, size_t bytes, bool littleEndian, bool isSigned)
{
	Common::CheckBounds(pos, bytes);

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

int32_t ReadVarLen(uint8_t*& pos)
{
	int32_t result = 0;

	while (true)
	{
		Common::CheckBounds(pos, 1);

		uint8_t b = *pos++;
		result = (result << 7) | (b & 0x7F);

		if (!(b & 0x80))
		{
			break;
		}
	}

	return result;
}

void Common::Warning(uint8_t* pos, string msg)
{
	if (ShowWarnings)
	{
		cerr << hex << setfill('0') << uppercase << endl;
		cerr << "WARNING IN\t" << Common::FileNames.top() << endl;
		cerr << "AT POSITION\t0x" << setw(8) << pos - Common::Offsets.top() << endl;
		cerr << "MESSAGE\t\t" << msg << endl;
		cerr << endl;
	}
}

void Common::Push(string fileName, uint8_t* data, streamoff length)
{
	FileNames.push(fileName);
	Offsets.push(data);
	Buffers.push_back({ data, data + static_cast<size_t>(length), fileName });

	cout << FileNames.top() << endl;
}

void Common::Pop()
{
	Offsets.pop();
	FileNames.pop();

	if (!Buffers.empty())
	{
		Buffers.pop_back();
	}
}

// Clear all parsing context. Used to recover to a clean slate after an input
// fails partway through, so a later input is not blamed on a stale filename.
void Common::Reset()
{
	while (!FileNames.empty()) { FileNames.pop(); }
	while (!Offsets.empty()) { Offsets.pop(); }
	Buffers.clear();
	Log.clear();
}

// Reject a failed or empty file open before its length is used to allocate a
// buffer. A missing/unreadable file yields a length of -1, which would
// otherwise become an enormous allocation and crash.
void Common::RequireOpen(bool streamOk, streamoff length, const string& fileName)
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
string Common::TypedName(const string& name, const string& type)
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

void Common::Analyse(string tag, uint32_t val)
{
	Common::Log.push_back(FileNames.top() + "," + tag + "," + to_string(val));
}

void Common::Dump(string fileName)
{
	ofstream ofs(fileName);
	ofs << "fileName,tag,val" << endl;

	for (size_t i = 0; i < Common::Log.size(); ++i)
	{
		ofs << Common::Log[i] << endl;
	}

	ofs.close();
}
