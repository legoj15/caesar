# caesar roadmap

This fork continues [kr3nshaw/caesar](https://github.com/kr3nshaw/caesar), which
has been unmaintained since 2021. caesar is the de-facto standard tool for
converting Nintendo 3DS **BCSAR** sound archives into SoundFont and MIDI, and no
maintained alternative exists — so the goal here is simply to be the maintained,
correct, well-packaged version.

The tool already works for the common case (its DSP-ADPCM audio decoding, the
codec nearly every 3DS game uses, is a faithful implementation). The work below
is about making it **robust**, **more faithful**, and **properly distributed**.

This is a living document. Items are ordered by priority; checkboxes track
progress. The road to the first release ends with continuous integration and
published binaries.

---

## Road to first release (v0.5.0)

### 1. Modern build system — ✅ done
A single CMake build that works on current (2026) toolchains.

- [x] Consolidate on CMake (CMake 3.21+, C++17); drop the hand-written Visual
      Studio solution/project and the dead AppVeyor config.
- [x] Build the two vendored libraries (`libsmfc`, `sf2cute`) as static libs.
- [x] Add `CMakePresets.json` for one-click Windows/MSVC builds.
- [x] Repo hygiene: slim `.gitignore`, fix `.gitattributes`, document building.
- [ ] Verify the build on Linux and macOS (the CMake is written to be portable;
      only Windows/MSVC is tested so far).

### 2. Robustness — never crash on real-world input
A tool whose job is parsing files ripped from game images should treat malformed
or unusual input as normal, not exceptional. Today, bad input crashes with no
explanation, and some failures corrupt output silently.

- [x] Check that every input file actually opened before using its size — a
      missing, empty, or unreadable path previously triggered a giant bogus
      allocation and crashed. (`Common::RequireOpen`, all load sites.)
- [x] Wrap each input in top-level error handling so one bad file reports
      cleanly and the rest still process (previously the first failure aborted
      the whole run), and restore the working directory between inputs so
      multi-archive runs (`caesar a.bcsar b.bcsar`) work.
- [x] Add a bounds-checked reader so a truncated or corrupt file reports
      "archive damaged at offset X" instead of reading past the buffer. Every
      read through `ReadFixLen`/`ReadVarLen` is checked against whichever loaded
      buffer the position falls in. Verified behavior-preserving by diffing
      13,843 output files across 31 real archives against the pre-change build
      (byte-identical), and that truncated/corrupt inputs now fail cleanly.
- [ ] Bounds-check the remaining bulk reads that bypass the readers (string
      construction from a file-supplied length, raw sub-file writes), which can
      still over-read on a corrupt length field.
- [ ] Replace the change-working-directory extraction model with composed output
      paths (enables an `--output-dir` option and removes remaining failure-path
      and parallelism limits).
- [ ] Sanitize archive-supplied names before using them as file/dir names.

### 3. Surface what's being dropped
By default (without `-w`), the tool prints no warnings at all — so a normal run
reports success while silently omitting sound effects, whole-song loops, an
entire audio codec, and more. Users can't tell what they didn't get.

- [ ] Promote "content was skipped / approximated" notices so they show by
      default; keep `-w` for verbose per-command detail.
- [ ] Check the MIDI-writer's return values (it silently rejects out-of-range
      events today, so some notes/program-changes vanish with no warning).

### 4. High-value fidelity & UX wins
The cheapest changes with the most audible or visible payoff.

- [ ] Name output files/folders from the archive's symbol table instead of bare
      numbers (`206.sf2`) — the single most-requested usability fix upstream.
- [ ] Implement whole-song loops (the sequence "jump" command is currently
      unimplemented, so every looping track converts as play-once).
- [ ] Verify and fix the suspected typo in the envelope decay-rate table in
      `Cbnk.cpp`, which makes some instruments fade out ~10x too fast.
- [ ] Map the sequence FX-send commands to reverb (CC91) / chorus (CC93) — the
      MIDI library already supports them; the converter just never emits them.

### 5. Licensing — ✅ resolved
The vendored `libsmfc` shipped without a license notice, but it is loveemu's
MIT-licensed code (identical to the copy in
[loveemu-lab](https://github.com/loveemu/loveemu-lab)); the copy had simply
dropped the repo-level license. MIT is GPL-3.0-compatible, so there is no
conflict with caesar's GPL-3.0.

- [x] Add the MIT notice for `libsmfc` (`src/libsmfc/LICENSE`) and document the
      third-party licenses in the README. Binaries are now distributable.

### 6. Continuous integration & first release — final step
- [ ] GitHub Actions workflow: build on every push (start with Windows/MSVC).
- [ ] Release workflow: attach built binaries to a GitHub Release when a version
      tag is pushed.
- [ ] Add a CI status badge to the README.
- [ ] Cut **v0.5.0** — the first maintained release.

---

## After the first release (not blocking v0.5.0)

Larger efforts that expand what caesar can do. Rough priority order:

- **Sequence (CSEQ → MIDI) fidelity.** Implement the variable/conditional/random
  command machinery (currently parsed but ignored, so conditional passages play
  unconditionally and random/variable values convert wrong), tie mode, and
  finite loop repeat counts.
- **Missing audio coverage.** Implement IMA-ADPCM (codec 3), which currently
  produces silent output reported as success; extract CWSD wave-sound data
  (most sound effects), currently skipped entirely.
- **External dependencies (Mario Kart 7 class).** Resolve archives whose data
  lives in sibling `.bcgrp` group files — a known gap no maintained tool handles.
- **Architecture modernization.** Retire the global mutable parser state (blocks
  reentrancy and multi-file/parallel use), pass decoded samples in memory
  instead of round-tripping through `.wav` files on disk, and adopt RAII/smart
  pointers over the manual `new`/`delete`.
- **Format-family expansion.** Wii U / Switch **BFSAR** archives share this
  structure; supporting them would make caesar the only maintained cross-console
  converter. (Note: the sequence command stream is big-endian even where the
  container is little-endian — endianness can't be a single global switch.)
