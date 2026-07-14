# diag-goldens — diagnostic-surface goldens (stage-0 fold guard)

A byte-for-byte guard for caesar's **diagnostic** output, built to protect the
suite stage-0 **`ParseContext` fold** (the ~600-site change that turns
`struct Common`'s process-global parser state into a threaded context). That
fold lands almost entirely on surfaces the corpus A/B never exercises, so it
needs its own net.

Run it as a pair with the fold:

```powershell
# 1. Before the fold, from the pre-fold build:
tools\diag-goldens\diag-goldens.ps1 -Capture

# 2. After each fold commit rebuild:
tools\diag-goldens\diag-goldens.ps1          # exit 0 = byte-identical

# Prove the harness itself can catch a diff:
tools\diag-goldens\diag-goldens.ps1 -SelfTest
```

Exit codes match `ab-verify`: **0** identical, **1** one or more surfaces
differ, **2** harness error (nothing was verified — never a pass).

## What it pins (and why ab-verify can't)

`tools/ab-verify` already pins stdout/stderr/exit **and every output file
(including `.log`)** for *healthy, single-input, default-args* runs across the
whole corpus. This harness covers exactly the diagnostic surfaces it can't see —
the ones the fold most endangers, because every one of them is produced by the
`Common::` helpers whose state the fold rewrites:

| Surface | Pinned by a fixture / invocation | Why it matters to the fold |
|---|---|---|
| **Assert** blocks (magic, BOM, stored length) | `assert-magic`, `assert-bom`, `assert-length` | `pos - Offsets.top()` position, `hex/uppercase/setfill` formatting, exit 1 |
| **Error** enum-default | `error-enum` (bad INFO chunk-id) | the `EXPECTED <string>` / `INSTEAD GOT 0x…` form |
| **CheckBounds** throws | `bounds-overrun`, `bounds-outside` | the `archive damaged: …` text + main's `ERROR IN / MESSAGE` catch block |
| **RequireOpen** | `require-open` (zero-byte file) | the missing/empty-file error; **stdout is empty** here (the `Push` echo never runs — pinned) |
| **CLI errors** | `cli-noargs`, `cli-version`, `cli-padsustain`, `cli-missing-odir`, `cli-noinput` | usage text, version string, argument-error text, exit codes |
| **`-w` per-item detail** | `w-queenstream`, `w-pksnd` | the positional `WARNING IN / AT POSITION` blocks + the run's `.log` bytes |
| **Multi-input `.log` bleed** | `multi-bleed` (`caravel` then `pksnd`, one process) | `b.log` currently = `a`'s rows **followed by** `b`'s (`Common::Log` is cleared only on the exception path). This golden **pins the bleed as-is**; fixing it is a later, deliberate output-changing commit — the fold must *reproduce* it. |
| **`-w` on a failure path** | `fix-assert-magic-w`, `fix-bounds-overrun-w` | pins that `-w` adds *nothing* before an early failure (no warning has fired yet) |

Every corrupted fixture is also **self-validated**: its stderr must still
contain its family marker (`INSTEAD GOT`, `EXPECTED\tA valid`,
`archive damaged`, `could not open or read file`) and its expected exit code,
at capture **and** compare time. A fixture that stops firing its mechanism is a
harness error (exit 2), never a pass — otherwise a golden could silently pin a
*healthy* run.

## What it deliberately does NOT cover

- **Healthy full-file output** (`.mid`/`.sf2`/`.wav`/raw dumps, and healthy
  `.log`s) — that is `ab-verify`'s territory, over the whole corpus. This
  harness touches a handful of tiny archives only.
- **`-w` positional output that isn't deterministic.** caesar computes a
  warning's position as `pos - Offsets.top()`. When `pos` and the top-of-stack
  buffer base come from **different heap allocations**, the subtraction is
  garbage that shifts run-to-run with the heap layout. This is **not** limited
  to group-resident conversions (the stage-0 survey's wording): it fires on any
  archive that warns during wave/bank decode — e.g. `caravel`'s `0.bcwav`
  warnings print `0xFFFFFFFFFFFE75A8`-class values, and `pika`'s
  `WARC_0.bcwar` warnings print *plausible-looking* values (`0x0000D860` vs
  `0x0000E590`, a constant per-run offset) that a "looks-like-garbage" filter
  would wrongly accept. So the `-w` golden set is chosen **empirically**: every
  `-w` success archive is run **twice** and must be byte-identical both times,
  in capture *and* compare (a run-to-run mismatch is exit 2). Only `pksnd`
  (metadata-only, 42 warning blocks) and `queenstream` (one Csar-level
  external-stream warning) survive that filter among the small archives; the
  multi-input and corrupted goldens run **without `-w`** so they stay
  deterministic.

## How it works

Two modes, both driven off one fixed list of fixture recipes + invocations:

- **`-Capture`** materialises the fixtures (deterministic byte-mutations of a
  few corpus archives), runs the current exe over every invocation, normalises
  the captured text, and writes one golden per surface (`stderr.txt`,
  `stdout.txt`, `exit.txt`, `logs.txt`) plus a `manifest.json` (recipe + source
  archive shas + golden shas). Refuses to overwrite an existing set without
  `-Force`.
- **default (compare)** re-verifies the source archive shas against the
  manifest, regenerates the fixtures (and checks *their* shas), runs the current
  exe, and byte-compares every surface against the goldens. Refuses a **stale
  exe** (older than `src/` or `CMakeLists.txt`) unless `-AllowStaleExe`, the
  same false-PASS guard `ab-verify` has.

**Normalisation is minimal by design** (same rule as
`ab-verify`'s `ConvertTo-AbNormalisedStream`): CRLF→LF, then the work-dir root
and exe path are folded to `<ROOT>` (fixtures' echoed input paths and the `.log`
rows' embedded paths both live under the work dir). **Nothing else** —
positions, hex, tabs, and blank lines are the payload being pinned.

### Fixtures & source archives

Fixtures are mutated from three small corpus archives, chosen for exact
properties:

- **`caravel`** (`F-Zero\caravel.bcsar`, version `0x02000000`) — the smallest
  Csar **direct-path** archive with real banks/seqs/wave-archives; base for the
  header/section mutations and the multi-input `a`. (Its own `-w` is
  nondeterministic, so it is never run with `-w`.)
- **`pksnd`** (`pokemon red\snd\PKSnd.bcsar`, version `0x02030100`) — the
  stored-length `Assert` is **guarded out for `0x02000000`**, so the
  `assert-length` fixture needs a non-`0x02000000` archive; `pksnd` is also a
  clean, deterministic `-w` archive and the multi-input `b`.
- **`queenstream`** (`Ocarina\sound\QueenStream.bcsar`) — a tiny stream-only
  archive; a second deterministic `-w` archive.

One mutation per **mechanism**, not per call site (the header is a fixed layout;
`error-enum`/`bounds-overrun` parse the archive's own header at generation time
to locate the INFO / STRG fields):

| Fixture | Recipe | Fires |
|---|---|---|
| `assert-magic` | flip byte 0 | `Csar.cpp` magic Assert |
| `assert-bom` | flip byte 4 (BOM) | BOM Assert |
| `assert-length` | truncate last byte of `pksnd` | stored-length Assert |
| `error-enum` | first INFO offset-id → `0x9999` | `Common::Error` enum-default |
| `bounds-overrun` | first STRG string length → `0xFFFFFF00` | `CheckBounds` overrun |
| `bounds-outside` | header `infoOffset` → `0x10000000` | `CheckBounds` outside |
| `require-open` | zero-byte file | `RequireOpen` |

## Privacy — nothing corpus-derived is committed

Like `ab-verify`'s cache, the **fixtures and goldens embed corpus archive names
and analysis rows**, so they stay local: everything lives under
`%LOCALAPPDATA%\caesar-diag\` (`fixtures\`, `goldens\`, `reports\`). Only this
script and this README are in the repo. The default `-CorpusRoot` hardcodes this
machine's dump path; pass `-CorpusRoot` to point elsewhere.

> A future idea (filed on the roadmap): ship a small set of **synthetic**,
> from-scratch (non-copyrighted) corrupted `.bcsar` fixtures *in the repo*, so
> the three-OS public CI — build-only today — gets behavioural smoke coverage of
> these same failure families without any corpus dependency.
