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

- **Init pan (`0xDC`) and LPF cutoff (`0xD8`) are now converted** instead of
  being dropped (12,110 previously discarded commands across 46 and 35
  archives). Neither is the raw mapping the roadmap assumed. Init pan is an
  *additive* offset the engine sums with the `0xC0` pan — writing it straight to
  CC10 would have clobbered the pan on the 76 corpus tracks that set both — so
  caesar tracks both terms and emits the combined stereo position (the note's own
  pan stays in the SF2 `kPan` generator, which SoundFont players already sum with
  CC10, exactly as hardware does). LPF cutoff maps to CC74 brightness but is
  *darken-only*: the engine clamps its cutoff scale at 64, so values above it are
  clamped rather than passed through, which would have told synths to brighten
  past the sample's own tone. Both semantics were confirmed against the CSEQ
  command dispatcher in a real 3DS binary. (changes `.mid` only — 2,963 files
  across 54 archives; every one of the 11,865 differing events is a CC10 (7,383,
  in 1,000 files) or CC74 (4,482, in 2,002 files), verified by independent SMF
  parse of every changed pair — no `.wav`, `.sf2` or raw dump differs, and no
  file is added or removed)

- This changelog, bundled into release zips and published as the release notes
  by `release.yml`. (output-identical)
- `docs/HISTORY.md` (completed-work narratives) and `docs/SUITE-DESIGN.md`
  (tool-suite design); `docs/ROADMAP.md` slimmed to goals + open work, with a
  v0.5.1 MIDI-fidelity patch scoped. (docs only — output-identical)
- **Surround-mode `span` (0xD7) confirmed audible on real hardware.** The
  `tools/surround-probe/` v2 console capture shows the 3DS Surround output mode
  performs genuine front/back virtualization on the headphone jack (front-vs-rear
  spectrum reshapes ~6 dB and +46 dB of cross-channel energy appears, only in
  Surround; both null controls pass) — upgrading the HISTORY finding from
  inference-grade to console-confirmed. (docs/tooling only — output-identical)

### Fixed

- `0xB2` mono/poly no longer emits CC126/CC127. Those are Channel *Mode*
  messages — the MIDI 1.0 spec hangs an implicit All Notes Off on CC124–127,
  so each of the 56 corpus firings that land mid-track (35 with notes already
  sounding) chopped every ringing note on its channel — and FluidSynth-class
  players treat the pair as a basic-channel poly/mono *reconfiguration* that
  can disable every other channel outright (49 of the 249 corpus firings sit
  on channel 0). The engine's per-track voice-allocation flag has no MIDI
  equivalent, so the command now drops with a default-visible notice.
  (changes `.mid` only — 53 files across 11 archives; all 249 removed events
  are CC126/127 by independent SMF parse of every changed pair, with no other
  event, note, or timing change; no `.sf2`/`.wav`/`.log`/raw dump differs and
  no file is added or removed)

- `0xDF` damper pedal now applies the sound engine's own threshold when writing
  CC64 (`arg >= 64` → pedal down), instead of passing the raw argument to a
  control that the MIDI writer will drop if it exceeds 127. The argument's
  domain is `Uint8` 0–255 and hardware reads *anything* ≥ 64 as pedal-down, so
  a value above 127 used to lose its pedal event entirely. (Note: the v0.5.1
  plan called this a *bool* bug, on the claim that "damper on" was emitted as a
  1 and read as pedal-*off*; that premise was refuted — the engine thresholds at
  64 exactly as MIDI does, and every value in the 82-archive corpus is already
  0 or 127. The prescribed `? 127 : 0` normalization would have inverted the
  1–63 range.) (output-identical on the whole corpus; changes `.mid` only for
  inputs carrying a damper argument > 127)
- Finite loop repeat counts now reach the loop markers. `0xD4` (loop start)
  emits its count as EMIDI **CC116** (was a hardcoded 0, which means *infinite*
  in that convention — so EMIDI-aware players replayed a finite "play N×"
  section forever), and `0xFC` (loop end) emits the spec-fixed **CC117 = 127**
  (was 0). The CTR and EMIDI conventions match exactly — both total-plays with
  0 = infinite — so the count passes through 1:1; counts above the 7-bit CC
  range clamp to 127 with a default-visible approximation notice, and
  unevaluated `Rnd`/`Var` counts keep the 0 (= infinite) stand-in. (changes
  `.mid` only, in sequences that use `0xD4`/`0xFC` loops)
- `0xE3` "sweep pitch" — an intra-note pitch ramp — is no longer mis-emitted
  as CC78 vibrato delay. It has no static MIDI equivalent (the faithful form
  is a pitch-bend ramp — future-player territory), so it now drops with a
  default-visible notice. Removes the 86 bogus CC78 events corpus-wide and
  surfaces all 871 sweep-pitch commands honestly instead of as generic
  out-of-range drops. (changes `.mid` only in sequences using sweep pitch)
- `0xE0` mod delay (vibrato onset delay; signed-16 in 5 ms units per the NW4R
  decomp) now scales into CC78's relative upper half — 64 = no delay,
  saturating at 1 s, above the corpus's largest real value — replacing the
  `(x/2)+64` transform that treated the time as a signed ±64 parameter and
  pushed delays ≥ 640 ms out of MIDI range. (changes `.mid` only in sequences
  using mod delays ≥ 10 ms)
- Plain (un-prefixed) out-of-range values for pan, volume, master volume, and
  expression (`Uint8` 128–255) now clamp to 127 and emit — surfaced by a
  default-visible "clamped" approximation notice — instead of being dropped;
  values from unevaluated `Rnd`/`Var` prefixes still drop with a notice.
  (output-identical on the whole 82-archive corpus, where no such plain values
  occur; changes `.mid` only for inputs that carry them)
- A sequence tempo of 0 BPM no longer reaches the vendored `libsmfcx.c`
  division whose infinite result has an undefined-behavior `int` cast; a
  caller-side `bpm > 0` guard drops it with the standard out-of-range notice.
  (output-identical)
- `tools/surround-probe/tools/split_run.py` mis-segmented the real broadband
  AUTO capture: the app's post-run idle tail became a phantom 11th "body", and
  naming keyed on pip counts (which the multitone's envelope dips and transition
  ticks corrupt). It now rejects over-long bodies by duration, names segments by
  deterministic schedule order (pip counts demoted to a cross-check), and cuts
  `noise_floor.wav` from a raw-envelope onset so it never clips the first pip.
  (tooling only — does not affect caesar output)
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
