# roundtrip-verify

Corpus fan-out for the `caesar-roundtrip` dev tool: the stage-1 byte-identical
round-trip net. It runs `caesar-roundtrip --verify` over every `.bcsar` in the
corpus, aggregates the per-format match/mismatch/skip tallies, and enforces the
same never-a-false-pass exit contract as `tools/ab-verify`.

This is the companion to ab-verify, not a replacement:

- **ab-verify** proves the *export* path (BCSAR -> MIDI/SF2/WAV) is byte-identical
  old-vs-new. It is the shipped tool's regression net.
- **roundtrip-verify** proves the *serializer* (model -> bytes) reproduces the
  original archive bytes. It is the stage-1 milestone's proof net.

## Status (stage-1 complete)

All three deep formats serialise byte-identically over the whole corpus: **BCSEQ
20,791 / 20,791, BCBNK 11,136 / 11,136, BCSAR 82 / 82** (the container). The
permanently-opaque children (BCWAR / BCWAV / BCWSD / BCGRP) can never re-encode -
CWAV's DSP-ADPCM does not round-trip - so they report SKIPPED **informationally**,
and their skips do not hold an archive short of a pass. The whole corpus now
aggregates to **exit 0** - the stage-1 byte-identical round-trip milestone.

## Usage

```powershell
# Build the dev exe first (from a VS dev prompt):
cmake --build build --config Release        # produces build/Release/caesar-roundtrip.exe

# Verify the whole corpus:
tools\roundtrip-verify\roundtrip-verify.ps1

# Prove the harness plumbing before trusting a verdict:
tools\roundtrip-verify\roundtrip-verify.ps1 -SelfTest

# One game:
tools\roundtrip-verify\roundtrip-verify.ps1 -Filter '*Fates*'
```

Key parameters: `-Exe` (default `build\Release\caesar-roundtrip.exe`),
`-CorpusRoot`, `-Jobs`, `-Filter`, `-OutDir` (report + `mismatches.tsv`),
`-AllowStaleExe` (skip the "exe older than `src/`" guard - named for what it lets
through).

## Exit codes

| code | meaning |
|------|---------|
| 0 | every DEEP format present (BCSEQ/BCBNK/BCSAR) re-serialised byte-identically; opaque skips do not block it |
| 1 | at least one child mismatched (see `mismatches.tsv`) |
| 2 | a deep format failed to re-serialise, or nothing was verifiable - **never read as a pass** |

## What the exe does per archive

`caesar-roundtrip --verify <archive>` parses the archive (read-only: no output
files, no directories), enumerates every embedded child - BCSEQ / BCBNK (deep
targets), BCWAR / BCWAV / BCWSD / BCGRP (opaque), plus the BCSAR container itself,
recursing into embedded groups and de-duplicating by archive offset. For each it
**copies the source span out** and compares a re-serialisation against that copy
(never the live buffer - the buffer-drop honesty guard is structural). An opaque
format reports SKIPPED. Its own exit is `0` (every deep format matched, opaque
skips informational) / `1` mismatch / `2` a deep format failed or nothing
verifiable.

The exe also carries the two one-time stage-1 gate scans, run per archive:

```powershell
build\Release\caesar-roundtrip.exe --scan-varlen <archive>   # VarLen canonicality
build\Release\caesar-roundtrip.exe --scan-gaps   <archive>   # FILE-section gap zero-fill
```

whose corpus verdicts are recorded in `docs/HISTORY.md` (stage-1 commit 0).

## Discipline (mirrored from ab-verify)

- A single `Stop-RtHarness` funnel for every fatal path, and a `trap` so no
  unhandled error is ever mistaken for "no mismatches".
- An AST-derived shadowed-helper guard (the `man`-alias trap).
- A stale-exe guard: any `src/` file (or the CMake files) newer than the binary
  stops the run - a stale exe is a false result waiting to happen.
- A zero-children guard: if the corpus walked nothing, the enumeration is broken
  and a "0 mismatches" verdict is meaningless.
- `-SelfTest`: checks that a real archive exits 0 (deep formats matched, opaque
  skips do not block it), that a missing archive is a harness error, and that the
  verdict aggregation maps 0/1/2 (and a vacuous "0 walked" run -> 2) exactly. It
  also carries the **byte-flip proof**: the exe's `--selftest` re-serialises the
  BCSAR container plus the first serialisable BCSEQ and BCBNK child, asserts each
  reproduces the source span, then flips one output byte and requires the compare
  to catch it - a harness that has never caught a planted diff must not be trusted
  with a clean verdict.
