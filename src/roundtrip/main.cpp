// caesar-roundtrip — a DEVELOPMENT tool, never shipped in a release.
//
// caesar_core's first external consumer. Two jobs, both for the stage-1
// byte-identical round-trip milestone (docs/HISTORY.md, "Suite stage 1"):
//
//   1. The round-trip VERIFY scaffold (default mode). Parses a .bcsar, walks
//      every embedded child (BCSEQ/BCBNK deep; BCWAR/BCWAV/BCGRP opaque) plus
//      the container itself, COPIES each source span out (so the future
//      buffer-drop honesty guard is structural — the compare is always against
//      a saved copy, never the live buffer), and — once a Serialize() lands —
//      re-serialises and compares. With zero serializers today every format is
//      SKIPPED and the run exits 2 ("nothing verifiable"): the harness can
//      never produce a false pass before the serializers exist. Exit contract
//      mirrors ab-verify: 0 all-match, 1 mismatch, 2 harness error.
//
//   2. Two one-time corpus SCANS that gate stage-1 commits 1 and 4:
//        --scan-varlen : are all VarLen-typed CSEQ args canonically encoded?
//                        (commit 1: canonical-only -> emit canonical, no model
//                        change; else CseqCmd needs a raw-length field.)
//        --scan-gaps   : are the FILE-section inter-blob / section-boundary /
//                        CGRP inter-file gap bytes always zero? (commit 4:
//                        reproduce-by-rule vs opaque gap-spans per site.)
//
// Read-only: it calls Parse() (which does no I/O and creates no directories),
// never Extract()/Export(). The archive bytes stay in memory for the whole run.

#include "Cbnk.hpp"
#include "Cgrp.hpp"
#include "Common.hpp"
#include "Cseq.hpp"
#include "Csar.hpp"
#include "Cwar.hpp"
#include "Options.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace
{
	// Exit contract, mirrored from ab-verify. 2 is the "did not verify anything"
	// value and must never be mistaken for a pass.
	constexpr int ExitAllMatch = 0;
	constexpr int ExitMismatch = 1;
	constexpr int ExitHarness  = 2;

	// Big-endian file magics (read the same way the parsers read them).
	constexpr uint32_t MagicCseq = 0x43534551; // "CSEQ"
	constexpr uint32_t MagicCbnk = 0x43424E4B; // "CBNK"
	constexpr uint32_t MagicCwar = 0x43574152; // "CWAR"
	constexpr uint32_t MagicCwav = 0x43574156; // "CWAV"
	constexpr uint32_t MagicCwsd = 0x43575344; // "CWSD"
	constexpr uint32_t MagicCgrp = 0x43475250; // "CGRP"

	// A silence guard for cout: the Csar/Cgrp/Cseq constructors echo the file
	// name (ParseContext::Push), which is noise for a read-only analyzer. Redirect
	// cout into a throwaway sink for the duration of a parse; restored on scope
	// exit even if a CheckBounds throws. A real sink (not a null rdbuf) is used
	// deliberately: a null buffer would set cout's failbit and silently swallow
	// every report line written after the guard is gone.
	struct CoutSilencer
	{
		streambuf* saved;
		ostringstream sink;
		CoutSilencer() : saved(cout.rdbuf()) { cout.rdbuf(sink.rdbuf()); }
		~CoutSilencer() { cout.rdbuf(saved); cout.clear(); }
		CoutSilencer(const CoutSilencer&) = delete;
		CoutSilencer& operator=(const CoutSilencer&) = delete;
	};

	// Harness-error carrier: any structural surprise (a bounds overrun, a broken
	// invariant in the scanner's own replay) aborts the current archive as an
	// exit-2 rather than being silently absorbed.
	struct HarnessError
	{
		string message;
	};

	// Bounds-checked little/big-endian 32-bit reads against the loaded archive.
	// A malformed/truncated blob offset must surface as a harness error, not a
	// read past the buffer.
	uint32_t readU32(const uint8_t* base, const uint8_t* end, const uint8_t* at, bool bigEndian)
	{
		if (at < base || at + 4 > end)
		{
			throw HarnessError{ "read past end of archive buffer" };
		}

		uint32_t v = 0;
		for (int i = 0; i < 4; ++i)
		{
			v |= static_cast<uint32_t>(at[i]) << ((bigEndian ? 3 - i : i) * 8);
		}
		return v;
	}

	string formatForMagic(uint32_t magic)
	{
		switch (magic)
		{
			case MagicCseq: return "BCSEQ";
			case MagicCbnk: return "BCBNK";
			case MagicCwar: return "BCWAR";
			case MagicCwav: return "BCWAV";
			case MagicCwsd: return "BCWSD";
			case MagicCgrp: return "BCGRP";
			default:        return "unknown";
		}
	}

	// One embedded child file, located as a span into the (still-live) archive
	// buffer. The gap/verify walks copy from Data; the offset is archive-relative
	// for reporting.
	struct Embedded
	{
		string   format;
		string   container; // "CSAR" or "CGRP:<id>"
		uint8_t* data;      // into Csar::Data
		uint32_t length;    // the blob's own header length (word at +0x0C)
		uint32_t archiveOffset;
	};

	// Enumerate every embedded child, de-duplicated by archive offset: the CSAR
	// FILE-section internal blobs, and (recursively) the files inside every
	// embedded CGRP. Length is the blob's self-declared header length at +0x0C —
	// exactly what Csar/Cgrp Export copy out — so a span here is byte-for-byte the
	// extracted child file.
	//
	// The dedup matters: in grouped archives (GardenSound, Mario Kart 7's
	// ctr_dash) a CGRP container blob physically holds its CBNK/CSEQ/CWAR, and
	// those contained files ALSO appear as their own top-level FILE entries whose
	// data offset points inside the container. So a nested file is reached twice —
	// once as a top-level entry, once through the group's file table — and both
	// resolve to the SAME archive offset. Keying on that offset scans each
	// physical file exactly once. (A CGRP file offset is group-relative, so its
	// archive offset is the group blob's own offset plus it.)
	vector<Embedded> collectEmbedded(Csar& csar)
	{
		vector<Embedded> out;
		set<uint32_t> seen;
		const uint8_t* base = csar.Data;
		const uint8_t* end  = csar.Data + csar.Length;

		for (const CsarFile& file : csar.Files)
		{
			if (!file.Internal || !seen.insert(file.Offset).second)
			{
				continue;
			}

			uint8_t* blob = csar.Data + file.Offset;
			uint32_t magic = readU32(base, end, blob, true);
			uint32_t len   = readU32(base, end, blob + 12, false);

			out.push_back({ formatForMagic(magic), "CSAR", blob, len, file.Offset });

			if (magic != MagicCgrp)
			{
				continue;
			}

			// Recurse into the group's own file table via caesar_core. Scoped so
			// the group's diagnostic frame is popped before the next iteration.
			try
			{
				CoutSilencer hush;
				map<int, Cwar*> noCwars;
				map<int, bool> noSeqs;
				map<int, string> noNames;
				Cgrp group("<scan-group>", blob, len, &noCwars, noSeqs, noNames, csar.Opts, csar.Ctx);

				if (!group.Parse())
				{
					continue; // a group header that fails to parse: leave its files unenumerated
				}

				for (const CgrpFile& gf : group.Files)
				{
					if (!gf.Present)
					{
						continue;
					}

					// gf.Offset is relative to the group blob; lift it to an
					// archive offset so the dedup collapses it against the same
					// file's top-level FILE entry.
					uint32_t archOff = file.Offset + gf.Offset;
					if (!seen.insert(archOff).second)
					{
						continue;
					}

					uint8_t* gblob = csar.Data + archOff;
					uint32_t gmagic = readU32(base, end, gblob, true);
					uint32_t glen   = readU32(base, end, gblob + 12, false);

					out.push_back({ formatForMagic(gmagic), "CGRP:" + to_string(gf.Id), gblob, glen, archOff });
				}
			}
			catch (const HarnessError&)
			{
				// A malformed group blob: skip it rather than abort the archive.
				// (The verify/scan of the top-level container still proceeds.)
			}
		}

		return out;
	}

	// -----------------------------------------------------------------------
	// VERIFY (default mode)
	// -----------------------------------------------------------------------

	// The serializer seam. Returns the re-serialised bytes for a format that has a
	// Serialize(), else nullopt (SKIPPED). Stage-1 commit 1 wired BCSEQ; commit 3
	// wires BCBNK, commit 4 BCSAR — each an additive edit flipping that format from
	// SKIPPED to verified. The permanently-opaque formats (BCWAR/BCWAV/BCWSD/BCGRP)
	// never re-encode (CWAV cannot round-trip its DSP-ADPCM), so they stay nullopt.
	//
	// The ctor/Parse borrow the span read-only and never mutate it (the caller has
	// already copied the source bytes out for the compare regardless); the cast
	// hands the borrowed buffer to the reader, which types Data as mutable. Parse's
	// name echo is hushed. A parse failure is a genuine SKIP: caesar itself could
	// not read the file, so there is nothing faithful to re-serialise. Cbnk::Parse
	// touches neither its Cwars map nor Opts (those are Export-only), so a scratch
	// empty map + default Options suffice for the round-trip.
	optional<vector<uint8_t>> trySerialize(const string& format, uint8_t* data, uint32_t length, ParseContext& ctx)
	{
		if (format == "BCSEQ")
		{
			CoutSilencer hush;
			Cseq seq("<roundtrip-seq>", data, length, ctx);
			if (!seq.Parse())
			{
				return nullopt;
			}
			return seq.Serialize();
		}

		if (format == "BCBNK")
		{
			CoutSilencer hush;
			map<int, Cwar*> noCwars;
			Options opts;
			Cbnk bank("<roundtrip-bnk>", data, length, &noCwars, opts, ctx);
			if (!bank.Parse())
			{
				return nullopt;
			}
			return bank.Serialize();
		}

		return nullopt;
	}

	struct FormatTally
	{
		int matched = 0;
		int mismatched = 0;
		int skipped = 0;
	};

	int runVerify(Csar& csar, const string& archivePath)
	{
		vector<Embedded> children;
		try
		{
			children = collectEmbedded(csar);
		}
		catch (const HarnessError& e)
		{
			cout << "HARNESS\t" << archivePath << "\t" << e.message << "\n";
			return ExitHarness;
		}

		map<string, FormatTally> tallies;

		// Verify one unit: copy the source span out (the structural honesty
		// guard — the compare is ALWAYS against this copy, never the live
		// buffer), then compare a re-serialisation against it.
		auto verifyOne = [&](const string& format, uint8_t* data, uint32_t length)
		{
			vector<uint8_t> saved(data, data + length);
			optional<vector<uint8_t>> produced = trySerialize(format, data, length, csar.Ctx);

			FormatTally& t = tallies[format];
			if (!produced.has_value())
			{
				++t.skipped;
			}
			else if (produced.value() == saved)
			{
				++t.matched;
			}
			else
			{
				++t.mismatched;

				// A mismatch is the one thing this tool exists to catch, so name it
				// precisely: the first differing byte offset and the two bytes (or a
				// length difference when one is a prefix of the other). The offset is
				// into the child file / container, exactly where a hexdump should be
				// opened.
				const vector<uint8_t>& got = produced.value();
				size_t n = min(got.size(), saved.size());
				size_t at = 0;
				while ((at < n) && (got[at] == saved[at])) { ++at; }

				cout << "MISMATCH\t" << archivePath << "\t" << format
					<< "\tfirstdiff=0x" << hex << at << dec
					<< "\tgotlen=" << got.size() << "\tsrclen=" << saved.size();
				if (at < n)
				{
					cout << "\tgot=0x" << hex << static_cast<int>(got[at])
						<< "\tsrc=0x" << static_cast<int>(saved[at]) << dec;
				}
				cout << "\n";
			}
		};

		// The container itself is a verify target (stage-1 commit 4).
		verifyOne("BCSAR", csar.Data, static_cast<uint32_t>(csar.Length));

		for (const Embedded& child : children)
		{
			verifyOne(child.format, child.data, child.length);
		}

		int matched = 0, mismatched = 0, skipped = 0;
		for (const auto& kv : tallies)
		{
			const FormatTally& t = kv.second;
			matched += t.matched;
			mismatched += t.mismatched;
			skipped += t.skipped;
			cout << "VERIFY\t" << archivePath << "\t" << kv.first
				<< "\tmatched=" << t.matched
				<< "\tmismatched=" << t.mismatched
				<< "\tskipped=" << t.skipped << "\n";
		}

		cout << "SUMMARY\t" << archivePath
			<< "\tmatched=" << matched
			<< "\tmismatched=" << mismatched
			<< "\tskipped=" << skipped << "\n";

		// Exit contract (per archive), tightened as serializers land:
		//   1  any child mismatched — a real regression, the loudest signal.
		//   0  every child re-serialised AND matched (skipped == 0).
		//   2  otherwise: nothing was verifiable, OR some matched but some are still
		//      SKIPPED (a partial verify is NOT a pass). This is the never-a-false-
		//      pass floor: with only BCSEQ serialising, the BCSAR container and the
		//      opaque BCWAR/BCWAV children are always SKIPPED, so every archive
		//      exits 2 today — the BCSEQ matched-count is the commit-1 proof, not
		//      the exit code. Exit 0 becomes reachable once commits 3-4 land the
		//      BCBNK + BCSAR serializers and the opaque formats are the only skips.
		if (mismatched > 0)
		{
			return ExitMismatch;
		}
		if ((matched > 0) && (skipped == 0))
		{
			return ExitAllMatch;
		}
		if (matched > 0)
		{
			cout << "HARNESS\t" << archivePath << "\tpartial: " << matched
				<< " matched but " << skipped << " still SKIPPED (not a full verify)\n";
			return ExitHarness;
		}

		// Nothing was re-serialised at all: not a pass.
		cout << "HARNESS\t" << archivePath << "\tno format is verifiable here; nothing was proven\n";
		return ExitHarness;
	}

	// -----------------------------------------------------------------------
	// SELF-TEST: the byte-flip proof (scheduled by commit 0)
	// -----------------------------------------------------------------------

	// Prove, on the first serialisable child of EACH deep format present (BCSEQ,
	// BCBNK), BOTH halves of the round-trip contract: (a) Serialize() reproduces the
	// source span byte-for-byte, and (b) a single planted byte-flip in the output is
	// DETECTED by the compare — a harness that cannot catch a planted diff must never
	// be trusted with a clean verdict. Prints one SELFTEST line per proven format and
	// returns 0 only when every one holds both halves. If the archive carries no
	// serialisable deep child, returns 2 so the wrapper moves on.
	int runSelfTest(Csar& csar, const string& archivePath)
	{
		vector<Embedded> children;
		try
		{
			children = collectEmbedded(csar);
		}
		catch (const HarnessError& e)
		{
			cout << "HARNESS\t" << archivePath << "\t" << e.message << "\n";
			return ExitHarness;
		}

		const char* deepFormats[] = { "BCSEQ", "BCBNK" };
		int proven = 0;
		bool allOk = true;

		for (const char* fmt : deepFormats)
		{
			for (const Embedded& child : children)
			{
				if (child.format != fmt)
				{
					continue;
				}

				vector<uint8_t> saved(child.data, child.data + child.length);
				optional<vector<uint8_t>> produced = trySerialize(child.format, child.data, child.length, csar.Ctx);
				if (!produced.has_value())
				{
					continue; // a child that would not parse: try the next of this format
				}

				bool roundtrip = (produced.value() == saved);

				// Plant one flipped byte and confirm the compare catches it. Mid-buffer
				// so it lands in the body, XOR so it is provably a new value.
				vector<uint8_t> tampered = produced.value();
				bool caught = false;
				if (!tampered.empty())
				{
					size_t at = tampered.size() / 2;
					tampered[at] ^= 0xFF;
					caught = (tampered != saved);
				}

				cout << "SELFTEST\t" << archivePath << "\t" << child.format
					<< "\tlen=" << saved.size()
					<< "\troundtrip=" << (roundtrip ? 1 : 0)
					<< "\tbyteflip_caught=" << (caught ? 1 : 0) << "\n";

				++proven;
				if (!(roundtrip && caught))
				{
					allOk = false;
				}

				break; // only the first serialisable child of this format
			}
		}

		if (proven == 0)
		{
			cout << "SELFTEST\t" << archivePath << "\tno-serialisable-child\n";
			return ExitHarness;
		}

		return allOk ? ExitAllMatch : ExitMismatch;
	}

	// -----------------------------------------------------------------------
	// SCAN: VarLen canonicality
	// -----------------------------------------------------------------------

	// Canonical (minimum) byte count for a value under the CSEQ VarLen encoding
	// (7 bits/byte, high bit = continue).
	int canonicalVarLenBytes(uint32_t value)
	{
		int n = 1;
		while (value >= 0x80)
		{
			value >>= 7;
			++n;
		}
		return n;
	}

	// Decode a VarLen starting at p; returns bytes consumed, sets value and
	// whether the first byte was 0x80 (a leading zero-payload continuation — the
	// signature of a non-canonical encoding).
	int decodeVarLen(const uint8_t* p, const uint8_t* end, uint32_t& value, bool& firstByte80)
	{
		firstByte80 = (p < end) && (*p == 0x80);

		uint32_t result = 0;
		int nbytes = 0;
		while (p < end)
		{
			uint8_t b = *p++;
			result = (result << 7) | (b & 0x7Fu);
			++nbytes;
			if (!(b & 0x80))
			{
				break;
			}
		}

		value = result;
		return nbytes;
	}

	struct VarLenStats
	{
		long long totalArgs = 0;
		long long nonCanonical = 0;
		int seqsScanned = 0;
	};

	// Scan one .bcseq span for non-canonical VarLen encodings. Every VarLen slot
	// is Arg1 (note gate time for status<0x80, 0x80 wait, 0x81 program) and only
	// when no Rnd/Var prefix retyped it — so cmd.Arg1 == VarLen uniquely marks
	// them. The VarLen bytes are located by replaying the command's own prefix +
	// status (+ velocity for a note) layout; the decoded value is cross-checked
	// against Args.back(), which throws if the replay drifted (a scanner bug, not
	// data).
	void scanSeqVarLen(Cseq& seq, const string& seqName, const string& archivePath, VarLenStats& stats)
	{
		const uint8_t* base = seq.Data;
		const uint8_t* end  = seq.Data + seq.Length;
		const uint32_t cmdBase = seq.DataOffset + 8;

		for (const auto& kv : seq.Commands)
		{
			const CseqCmd& cmd = kv.second;

			if (cmd.Arg1 != ArgType::VarLen)
			{
				continue;
			}

			// Defensive: VarLen is only ever assigned to a plain (non-extended)
			// note / 0x80 / 0x81 (see Cseq::Parse). If that ever changes, stop
			// rather than mismeasure.
			if (cmd.Extended || !(cmd.Cmd < 0x80 || cmd.Cmd == 0x80 || cmd.Cmd == 0x81))
			{
				throw HarnessError{ "unexpected VarLen host command " + to_string(static_cast<unsigned>(cmd.Cmd)) };
			}

			// Replay the byte layout to reach the VarLen slot.
			uint32_t off = cmdBase + cmd.Offset;
			if (cmd.Suffix3 == SuffixType::If)   { ++off; } // 0xA2
			if (cmd.Suffix2 != SuffixType::None) { ++off; } // 0xA3 / 0xA4 / 0xA5
			if (cmd.Suffix1 != SuffixType::None) { ++off; } // 0xA0 / 0xA1
			++off;                                          // status byte
			if (cmd.Cmd < 0x80)                  { ++off; } // note velocity (fixed u8)

			const uint8_t* p = base + off;
			if (p < base || p >= end)
			{
				throw HarnessError{ "VarLen replay ran off the sequence buffer" };
			}

			uint32_t value = 0;
			bool firstByte80 = false;
			int measured = decodeVarLen(p, end, value, firstByte80);

			// Self-check: the value we just re-decoded must equal what Parse
			// stored (the last Arg in all three VarLen hosts). A mismatch means
			// the replay offset is wrong — a scanner defect.
			if (cmd.Args.empty() || static_cast<uint32_t>(cmd.Args.back()) != value)
			{
				throw HarnessError{ "VarLen self-check failed (replay offset wrong) at seq " + seqName };
			}

			int canonical = canonicalVarLenBytes(value);
			bool nonCanonicalByLen = measured > canonical;

			// The two definitions of non-canonical must agree (a leading 0x80 is
			// exactly a removable leading zero group).
			if (nonCanonicalByLen != firstByte80)
			{
				throw HarnessError{ "VarLen canonicality definitions disagree at seq " + seqName };
			}

			++stats.totalArgs;
			if (nonCanonicalByLen)
			{
				++stats.nonCanonical;
				cout << "VARLEN-NC\t" << archivePath << "\t" << seqName
					<< "\toff=0x" << hex << off << dec
					<< "\tvalue=" << value
					<< "\tbytes=" << measured
					<< "\tcanon=" << canonical << "\n";
			}
		}
	}

	int runScanVarLen(Csar& csar, const string& archivePath)
	{
		vector<Embedded> children;
		VarLenStats stats;
		try
		{
			children = collectEmbedded(csar);

			for (const Embedded& child : children)
			{
				if (child.format != "BCSEQ")
				{
					continue;
				}

				CoutSilencer hush;
				Cseq seq("<scan-seq>", child.data, child.length, csar.Ctx);
				if (!seq.Parse())
				{
					continue; // a .bcseq that fails to parse: caesar itself would error out too
				}

				++stats.seqsScanned;
				scanSeqVarLen(seq, child.container, archivePath, stats);
			}
		}
		catch (const HarnessError& e)
		{
			cout << "HARNESS\t" << archivePath << "\t" << e.message << "\n";
			return ExitHarness;
		}

		cout << "VARLEN\t" << archivePath
			<< "\tseqs=" << stats.seqsScanned
			<< "\ttotal=" << stats.totalArgs
			<< "\tnoncanonical=" << stats.nonCanonical << "\n";
		return ExitAllMatch;
	}

	// -----------------------------------------------------------------------
	// SCAN: FILE-section gaps
	// -----------------------------------------------------------------------

	struct GapSite
	{
		long long gaps = 0;        // gaps examined (a nonzero-length run of padding)
		long long nonZero = 0;     // of those, ones with a non-zero byte
		long long overlaps = 0;    // blobs whose spans overlap (a layout anomaly)
	};

	// Check [from, to) for any non-zero byte; report the first offender. Returns
	// true if the range was all zero (or empty).
	bool checkZeroRange(const uint8_t* base, const uint8_t* end, uint32_t from, uint32_t to,
		const char* site, const string& archivePath, GapSite& tally)
	{
		if (to <= from)
		{
			return true;
		}
		const uint8_t* p = base + from;
		const uint8_t* q = base + to;
		if (p < base || q > end)
		{
			throw HarnessError{ string("gap range past buffer at ") + site };
		}

		++tally.gaps;
		for (const uint8_t* it = p; it < q; ++it)
		{
			if (*it != 0)
			{
				++tally.nonZero;
				cout << "GAP-NZ\t" << archivePath << "\t" << site
					<< "\toff=0x" << hex << from << "\tlen=0x" << (to - from)
					<< "\tfirstnz=0x" << static_cast<int>(*it) << dec << "\n";
				return false;
			}
		}
		return true;
	}

	// A blob's physical extent within the archive: start offset + declared length.
	struct Extent
	{
		uint32_t start;
		uint32_t len;
	};

	// Split extents into the MAXIMAL (top-level) set, counting the ones nested
	// inside a container. A grouped archive lists a CGRP container AND every file
	// it holds as sibling FILE entries, the latter's data lying inside the
	// former's span — so a naive "gap between consecutive entries" is meaningless
	// (entries nest). Sorting by start ascending / length descending, a blob whose
	// end does not exceed the running max-end of everything before it is contained
	// in some earlier (>= start, >= end) blob; the rest are the true top-level
	// tiling whose gaps the serializer must reproduce.
	void splitMaximal(vector<Extent>& ex, vector<Extent>& top, long long& nested)
	{
		sort(ex.begin(), ex.end(), [](const Extent& a, const Extent& b)
			{ return a.start != b.start ? a.start < b.start : a.len > b.len; });

		uint64_t runMax = 0;
		for (const Extent& e : ex)
		{
			uint64_t endHere = static_cast<uint64_t>(e.start) + e.len;
			if (endHere <= runMax)
			{
				++nested;
			}
			else
			{
				top.push_back(e);
			}
			runMax = max(runMax, endHere);
		}

		sort(top.begin(), top.end(), [](const Extent& a, const Extent& b) { return a.start < b.start; });
	}

	// Inter-blob gaps between the (already maximal, start-sorted) top-level blobs.
	void scanTopGaps(const uint8_t* base, const uint8_t* end, const vector<Extent>& top,
		const char* site, const string& archivePath, GapSite& tally)
	{
		for (size_t i = 1; i < top.size(); ++i)
		{
			uint64_t prevEnd = static_cast<uint64_t>(top[i - 1].start) + top[i - 1].len;
			uint32_t nextStart = top[i].start;
			if (nextStart < prevEnd)
			{
				// Two maximal blobs partially overlapping (neither contains the
				// other) — a genuine layout anomaly, distinct from clean nesting.
				++tally.overlaps;
				cout << "GAP-OVL\t" << archivePath << "\t" << site
					<< "\tprevEnd=0x" << hex << prevEnd << "\tnextStart=0x" << nextStart << dec << "\n";
				continue;
			}
			checkZeroRange(base, end, static_cast<uint32_t>(prevEnd), nextStart, site, archivePath, tally);
		}
	}

	int runScanGaps(Csar& csar, const string& archivePath)
	{
		GapSite interBlob;   // CSAR: between consecutive top-level FILE-section blobs
		GapSite sectionPad;  // CSAR: FILE-section leading + trailing padding
		GapSite cgrpInter;   // CGRP: between consecutive embedded files (per group)
		long long csarNested = 0;
		long long cgrpNested = 0;

		const uint8_t* base = csar.Data;
		const uint8_t* end  = csar.Data + csar.Length;

		try
		{
			// The FILE section start is the 0x2002 offset word — a fixed header
			// slot (byte 0x30) the parse model does not retain; read it directly.
			// The section spans [fileOffset, fileOffset + FileLength); child blobs
			// begin at fileOffset + 8 (after the 'FILE' magic + length words). Extents
			// use the INFO-declared allocation length (Files[i].Length), the archive's
			// own statement of the region a blob occupies.
			uint32_t fileOffset = readU32(base, end, base + 0x30, false);
			uint64_t sectionEnd = static_cast<uint64_t>(fileOffset) + csar.FileLength;

			// --- CSAR inter-blob + section boundaries (over top-level blobs) ---
			vector<Extent> csarEx;
			for (const CsarFile& file : csar.Files)
			{
				if (file.Internal)
				{
					csarEx.push_back({ file.Offset, file.Length });
				}
			}

			vector<Extent> csarTop;
			splitMaximal(csarEx, csarTop, csarNested);
			scanTopGaps(base, end, csarTop, "CSAR-interblob", archivePath, interBlob);

			if (!csarTop.empty())
			{
				uint32_t firstStart = csarTop.front().start;
				uint64_t lastEnd = 0;
				for (const Extent& e : csarTop)
				{
					lastEnd = max(lastEnd, static_cast<uint64_t>(e.start) + e.len);
				}

				checkZeroRange(base, end, fileOffset + 8, firstStart, "CSAR-section-lead", archivePath, sectionPad);

				// Clamp the trailing bound to the physical archive: an archive with
				// external content (Mario Kart 7's ctr_dash) declares a FILE-section
				// length that runs past its own end, so only the in-file remainder
				// [lastEnd, min(sectionEnd, Length)) is real padding to check.
				uint64_t trailEnd = min(sectionEnd, static_cast<uint64_t>(csar.Length));
				if (trailEnd >= lastEnd)
				{
					checkZeroRange(base, end, static_cast<uint32_t>(lastEnd), static_cast<uint32_t>(trailEnd), "CSAR-section-trail", archivePath, sectionPad);
				}
			}

			// --- CGRP inter-file (per embedded group; group-relative offsets
			//     lifted to archive-relative so one base suffices) ---
			for (const CsarFile& file : csar.Files)
			{
				if (!file.Internal)
				{
					continue;
				}
				uint8_t* blob = csar.Data + file.Offset;
				if (readU32(base, end, blob, true) != MagicCgrp)
				{
					continue;
				}

				vector<Extent> grpEx;
				try
				{
					uint32_t grpLen = readU32(base, end, blob + 12, false);
					CoutSilencer hush;
					map<int, Cwar*> noCwars;
					map<int, bool> noSeqs;
					map<int, string> noNames;
					Cgrp group("<scan-group>", blob, grpLen, &noCwars, noSeqs, noNames, csar.Opts, csar.Ctx);
					if (!group.Parse())
					{
						continue;
					}
					for (const CgrpFile& gf : group.Files)
					{
						if (gf.Present)
						{
							grpEx.push_back({ file.Offset + gf.Offset, gf.Length });
						}
					}
				}
				catch (const HarnessError&)
				{
					continue; // one malformed group does not sink the archive's scan
				}

				vector<Extent> grpTop;
				splitMaximal(grpEx, grpTop, cgrpNested);
				scanTopGaps(base, end, grpTop, "CGRP-interfile", archivePath, cgrpInter);
			}
		}
		catch (const HarnessError& e)
		{
			cout << "HARNESS\t" << archivePath << "\t" << e.message << "\n";
			return ExitHarness;
		}

		cout << "GAPS\t" << archivePath
			<< "\tcsar_nested=" << csarNested
			<< "\tinterblob_gaps=" << interBlob.gaps << "\tinterblob_nz=" << interBlob.nonZero << "\tinterblob_ovl=" << interBlob.overlaps
			<< "\tsectionpad_gaps=" << sectionPad.gaps << "\tsectionpad_nz=" << sectionPad.nonZero
			<< "\tcgrp_nested=" << cgrpNested
			<< "\tcgrp_gaps=" << cgrpInter.gaps << "\tcgrp_nz=" << cgrpInter.nonZero << "\tcgrp_ovl=" << cgrpInter.overlaps
			<< "\n";
		return ExitAllMatch;
	}
}

int main(int argc, char** argv)
{
	// Usage: caesar-roundtrip [--verify|--scan-varlen|--scan-gaps] <archive.bcsar>
	string mode = "--verify";
	string archivePath;

	for (int i = 1; i < argc; ++i)
	{
		string arg = argv[i];
		if (arg == "--verify" || arg == "--scan-varlen" || arg == "--scan-gaps" || arg == "--selftest")
		{
			mode = arg;
		}
		else if (arg == "-h" || arg == "--help")
		{
			cout << "usage: caesar-roundtrip [--verify|--selftest|--scan-varlen|--scan-gaps] <archive.bcsar>\n";
			return ExitAllMatch;
		}
		else if (archivePath.empty())
		{
			archivePath = arg;
		}
		else
		{
			cerr << "caesar-roundtrip: unexpected extra argument: " << arg << "\n";
			return ExitHarness;
		}
	}

	if (archivePath.empty())
	{
		cerr << "caesar-roundtrip: no archive given\n";
		cerr << "usage: caesar-roundtrip [--verify|--selftest|--scan-varlen|--scan-gaps] <archive.bcsar>\n";
		return ExitHarness;
	}

	ParseContext ctx;
	Options opts;

	// Parse the archive once (no I/O, no directories). A parse failure or a
	// malformed-input throw is a harness error: nothing could be verified.
	try
	{
		CoutSilencer hush;
		Csar csar(archivePath.c_str(), opts, ctx);
		if (!csar.Parse())
		{
			cout.rdbuf(hush.saved); // restore before reporting
			cout << "HARNESS\t" << archivePath << "\tarchive failed to parse\n";
			return ExitHarness;
		}
		// restore cout for the mode's own (intended) output
		cout.rdbuf(hush.saved);

		if (mode == "--scan-varlen")
		{
			return runScanVarLen(csar, archivePath);
		}
		if (mode == "--scan-gaps")
		{
			return runScanGaps(csar, archivePath);
		}
		if (mode == "--selftest")
		{
			return runSelfTest(csar, archivePath);
		}
		return runVerify(csar, archivePath);
	}
	catch (const HarnessError& e)
	{
		cout << "HARNESS\t" << archivePath << "\t" << e.message << "\n";
		return ExitHarness;
	}
	catch (const exception& e)
	{
		cout << "HARNESS\t" << archivePath << "\t" << e.what() << "\n";
		return ExitHarness;
	}
}
