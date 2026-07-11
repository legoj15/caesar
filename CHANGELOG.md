# Changelog

All notable, user-facing changes to caesar (behavior, output, CLI, build) are
recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); release tags are
`vX.Y.Z` on GitHub, release zips bundle this file, and the release workflow
publishes the released version's section as the GitHub Release notes (with the
auto-generated commit list appended).

House rules:

- Every user-facing change adds a line under **[Unreleased]** in the same
  commit, grouped as Added / Changed / Fixed (plus Removed / Security when
  needed).
- Each line states its **output impact** — `output-identical` (extraction
  bytes unchanged; stderr/UX/build only) or which output types change (e.g.
  `changes .mid only`). This mirrors the project's byte-identical A/B
  verification discipline.
- Cutting a release: rename `[Unreleased]` to `[X.Y.Z] — YYYY-MM-DD`, start a
  fresh empty `[Unreleased]` above it, update the compare links at the bottom,
  then push the tag.

## [Unreleased]

### Added

- This changelog, bundled into release zips and published as the release notes
  by `release.yml`. (output-identical)
- `docs/HISTORY.md` (completed-work narratives) and `docs/SUITE-DESIGN.md`
  (tool-suite design); `docs/ROADMAP.md` slimmed to goals + open work, with a
  v0.5.1 MIDI-fidelity patch scoped. (docs only — output-identical)

### Fixed

- Out-of-bounds guard on the sequence `OpenTrack` (`0x88`) handler: the track
  index is a full byte but the format has only 16 tracks (the `0xFE` enable mask
  is 16-bit), and an index ≥ 16 wrote past the 16-entry `trackOffsets` stack
  array. No real archive uses such an index (verified across the corpus), so this
  only hardens against malformed input and surfaces it as a skipped-content
  notice. (output-identical)
- GM percussion-channel collision: the CSEQ track index was used directly as the
  MIDI channel, so a sequence's 10th track (index 9) landed on channel 9 — the
  channel GM/GS players reserve for drums — and its melodic notes rendered as a
  drum kit or (with no drum-bank preset in the SF2) as silence. The MIDI channel
  is now decoupled from the SMF track number: track 9 is relocated to a free
  channel, and only when all 16 tracks are in use does it stay on channel 9,
  with a Roland GS "Use for Rhythm Part: OFF" SysEx for part 10 emitted so
  GS-aware players (e.g. FluidSynth) treat it melodically. (changes `.mid` only;
  sequences that never open a 10th track are byte-identical)

## [0.5.0] — 2026-07-10

The first maintained release, continuing
[kr3nshaw/caesar](https://github.com/kr3nshaw/caesar) (unmaintained since
2021). Changes are relative to the last upstream commit.

### Added

- Modern CMake build (3.21+, C++17) with a Windows/MSVC preset; the two
  vendored libraries (`libsmfc`, `sf2cute`) build as static libs. Output
  verified byte-identical between Windows/MSVC and Linux/GCC; three-OS GitHub
  Actions CI plus this tag-driven release pipeline.
- `-o <dir>` / `--output-dir <dir>` — extraction composes full output paths
  (the working-directory dance is gone), so multi-archive runs are isolated
  and output can be redirected.
- `-v` / `--version`.
- `--pad-sustain[=SECONDS]` — opt-in long release tail for release-127 voices,
  for players without usable reverb. The accurate default is instant; the
  console's audible tail is DSP reverb, which SF2/MIDI can only reference by
  send level.
- Default-visible notices summarizing skipped or approximated content (CWSD
  sound-effect blocks, external streams, INFX chunks, IMA-ADPCM waves left
  silent, notes pointing at missing samples, dropped MIDI events, unloadable
  external groups); `-w` still prints the per-item detail.
- Whole-song loops: the sequence jump command now produces
  `loopStart`/`loopEnd` markers honored by loop-aware players (foobar2000's
  foo_midi) and DAW timelines.
- Per-note tune field emitted as SF2 coarse/fine tune (was read and
  discarded; 24 of 1,222 corpus banks carry real detunes).
- Sequence FX sends mapped to MIDI: reverb → CC91, chorus → CC93 (were
  dropped, so extracted MIDIs rendered bone-dry).

### Changed

- Unnamed numeric outputs gain type prefixes (`206.sf2` → `BANK_206.sf2`), and
  group-resident items inherit the archive's symbol names instead of numbers,
  landing in the directories the archive level already named.

### Fixed

- Shared multi-entry `.bcseq` banks: every sequence entry now converts from
  its own start offset (previously every entry walked byte 0 — thousands of
  MIDIs were missing or duplicated; Mario Kart 7 alone went from 23 to 1,027
  distinct MIDIs).
- Banked instruments (index ≥ 128) were unreachable from MIDI: an SF2 bank
  split plus CC0 bank select recovers 6,422 program changes corpus-wide
  (console-validated on a New 3DS).
- Note-less conditional-jump dispatcher tracks converted to silent MIDIs: 909
  sequences gained their notes, with zero notes lost elsewhere.
- Envelope decay-table decimal-shift typos (eight entries ~10× too fast,
  upstream since 2019) — sustained instruments no longer collapse to
  near-silence.
- Envelope byte 127 settled by disassembly of the NW4C driver as the
  instant/fastest sentinel for both decay and release (see `--pad-sustain`
  for the compensating option).
- Robustness and hardening: bounds-checked reads ("archive damaged at offset
  X" instead of crashes or over-reads), per-input failure isolation,
  missing/empty-input checks, archive-supplied names sanitized (the
  path-separator escape is closed), the group file-table desync on
  absent/external entries, an 8-byte-read undefined behavior, uninitialized-
  memory copies (including a non-deterministic SF2 byte), and a warning-clean
  build enforced with warnings-as-errors.

### Licensing

- The vendored `libsmfc`'s MIT notice restored, and all third-party licenses
  documented and bundled into the release zips — binaries are distributable.

[Unreleased]: https://github.com/legoj15/caesar/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/legoj15/caesar/releases/tag/v0.5.0
