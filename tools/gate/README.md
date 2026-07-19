# gate — the player-change regression gate

One command that runs caesar's five offline verification nets, honours their
shared exit contract, verifies the goldens manifest first, and prints one summary
table. **Run it before committing a player-affecting change.**

```powershell
tools\gate\gate.ps1                     # full gate (all five nets)
tools\gate\gate.ps1 -Nets play,console  # just the two player nets
tools\gate\gate.ps1 -ListNets           # print the net table and exit
```

## What it orchestrates (cheapest first)

| key | net | binary | guards |
|---|---|---|---|
| `diag`      | `tools/diag-goldens`      | `caesar.exe`           | converter diagnostic surfaces (corrupt-input stderr, CLI errors, `-w`, `.log`) |
| `play`      | `tools/play-goldens`      | `caesar-play.exe`      | player render byte-goldens (the stage-2 dry player) |
| `console`   | `tools/console-tolerance` | `caesar-play.exe`      | player vs New 3DS line-in captures (within tolerance, reverb excepted) |
| `roundtrip` | `tools/roundtrip-verify`  | `caesar-roundtrip.exe` | stage-1 serializer byte-identical round-trip |
| `ab`        | `tools/ab-verify`         | `caesar.exe`           | converter corpus A/B (SF2/MIDI/.log/stderr byte-identity) |

Each net runs as its own `pwsh` child process (they call `exit`, so they must be
isolated) and streams its full output live. The gate adds a summary table and a
single verdict line.

## Exit codes (the gate itself)

| code | meaning |
|---|---|
| 0 | **FULL PASS** — every selected net ran and passed; manifest fully verified |
| 1 | **FAILED** — a net reported a regression (diff / out-of-tolerance / mismatch) |
| 2 | **ERROR** — a net could not verify (harness error, or a missing binary) — never a pass |
| 3 | **REDUCED** — nets were skipped or degraded; not a full pass |

A **full pass** means all five nets ran and passed with the goldens manifest fully
verified. A **reduced pass** means the gate ran but at less than full coverage
(`-SkipCorpus`, a net subset that leaves gaps, or a DEGRADED net) — a skipped or
degraded net never counts as a pass.

Per-net exit codes map in as `0→PASS 1→FAIL 2→HARNESS ERROR`. `ab-verify` has a
**fourth** code the roadmap's "shared 0/1/2 contract" omits — `3` = output
byte-identical but console/stderr changed — which the gate treats as a FAIL
(a converter console change during a player-change gate is a real signal).

## The goldens manifest

`goldens-manifest.json` (checked in) records **sha256 + provenance only, never
content** for the goldens the nets consume — they are gitignored copyrighted-corpus
derivatives scattered across three trees (the corpus `BANK_BGM`,
`tools/console-capture/captures/`, and the per-machine `%LOCALAPPDATA%` caches).

Before running any net, the gate verifies it: every gate-critical golden
(`enforce: "sha"`, `gates: true`) must be present with a matching hash, else the
consuming net is marked **DEGRADED** and the gate cannot claim a full pass. This
kills the *"a missing console WAV silently shrinks `console-tolerance` to a smaller
PASS"* failure mode — that net skips absent captures silently, so its two present
console WAVs are the gate-critical rows. (`play-goldens`/`diag-goldens` ERROR on a
missing golden, so theirs are catalogued for provenance, not enforced.)

Four of the six console-capture rows — the **SE captures** — are
`obsolete-superseded`, **not a TODO**. The isolated-note plan was abandoned
2026-07-14 because these SEs only fire inside a music sequence, so a standalone
isolated-SE WAV is not physically capturable (confirmed 2026-07-19: the analog rig
is live, but the SEs cannot be isolated). Their calibration targets are already
exercised by the present, sha-enforced **battery-v2 track A/B** rows, which each
obsolete row names via `subsumedBy`. They are kept on record so the abandoned plan
stays visible — do not resurrect isolated-SE capture.

## Flags

- `-Nets diag,play,console,roundtrip,ab` — run a subset (default: all).
- `-SkipCorpus` — for a machine without the private corpus: every corpus-dependent
  net (all of them) is reported **SKIPPED** and the gate runs **REDUCED**. A
  plumbing/no-corpus safety valve, not a gating mode.
- `-AllowStaleExe` — bypass each net's "exe older than `src/`" freshness guard.
  **Only correct when the change cannot affect that net's binary.** A pure
  `src/play` edit is the standing example: `caesar.exe` and `caesar-roundtrip.exe`
  do not link `src/play`, so they are behaviourally current for the converter and
  round-trip even when "stale" by mtime — but the nets' freshness guard watches
  *all* of `src/` and trips anyway. Either rebuild all three exes, or pass this
  after confirming the change is player-only.
- `-AbBaseline <ref>` — passed to `ab-verify` as `-Baseline`. Note: on a **clean**
  tree with the default `HEAD`, `ab-verify` refuses as a vacuous comparison (exit
  2) — run the gate on the dirty tree *before* committing, or pass `-AbBaseline
  HEAD~1` after.
- `-BuildDir <dir>` — where the exes live (default `build\Release`).
- `-CorpusRoot <dir>` — the private dump tree (default the standard path).

## Notes

- The gate **never builds**. A missing binary FAILS FAST for that net with the
  exact VS 18 (2026) `vcvars64` build command (see the `caesar-build-env` memory).
- Nothing corpus-derived is committed: only this script, the manifest, and this
  README are in the repo. The manifest holds hashes, not WAVs.
