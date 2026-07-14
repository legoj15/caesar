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

- **The convert-time variable VM — sequence variables, comparisons, and `[If]`
  conditions now execute during conversion.** The extended command space that
  previously dropped with notices (353k `setvar`-family ops, 210k comparisons
  corpus-wide) runs in a deterministic VM: all 12 variable ops and 6
  comparisons with byte-verified 3DS semantics (each op was disassembled in a
  real CTR binary — including the ÷0-guarded div/mod, `notvar` complementing
  the *operand*, and the s16 wrap), 48 variable slots in the engine's three
  scopes, and the `[If]` prefix gating **every** command as the CTR engine
  does — including `Fin` and `Return`, where the 3DS port diverges from the
  Wii engine (conditional truncation is real hardware behavior; disasm-cited
  in the code). Consequences: `Var`-valued arguments now carry the live
  variable value instead of the variable *index*; conditional jumps are
  evaluated exactly, retiring the two-reachability dispatcher heuristic;
  counted conditional loops unroll until their comparison clears (budgeted);
  self-contained RNG re-roll loops — which always eventually play on hardware
  but whose exit a PRNG-free converter can never roll open — take their gated
  exit once instead of ending the track silent; and a per-track execution
  budget closes a latent infinite-loop hazard on `Call` cycles. Converter
  policies, each surfaced by a notice: variables initialise to 0 (the
  game-at-rest "default section"; a two-init corpus A/B against the hardware's
  −1 power-on value showed −1 rescues nothing and silences 1,294 more files),
  `randvar` and `Rnd` arguments stand in their range midpoint, and reads of
  never-written (game-seeded) variables are flagged per execution. Also
  retires the latched-stand-in hazard at note-wait/portamento/timebase (a
  `Var`-prefixed flag used to latch the variable index as persistent state).
  (changes `.mid` only — 3,356 files across 55 archives; net +37k note-ons as
  conditional content becomes reachable; an independent SMF parse of every
  changed pair ran clean; 332 files render honestly silent where the old
  converter fabricated notes by ignoring conditions — each is a
  mechanism-diagnosed game-triggered spin-wait or minority-probability random
  selector, named by its read-before-write notice; no `.sf2`/`.wav`/raw dump
  differs and no file is added or removed)

### Fixed

- **macOS build fixed** — the in-memory sample handoff used an unqualified
  `move(...)` call, which Apple Clang rejects under `-Werror`
  (`-Wunqualified-std-cast-call`); now qualified as `std::move`. Windows/Linux
  builds were unaffected. (output-identical — build only)
- **A failed or truncated `.wav` write is now reported instead of silently
  succeeding** — `Cwav::Convert` never checked its output stream, so a full disk
  or I/O error left a short/empty `.wav` behind while the run reported success.
  The stream is now checked after close and a write failure throws (caught by the
  per-input handler). This rides in on the in-memory sample handoff: `Cwav` now
  retains its decoded PCM, channel count, sample rate, and raw loop points, and
  `Cbnk` reads samples from the live wave object rather than re-opening and
  re-parsing the `.wav` it just wrote — which also retires a latent stale-frame /
  bounds-check leak on the old read-back's error paths. The `.wav` files are
  still written exactly as before, and malformed banks that reference an absent
  or out-of-range wave archive now fail with the same clean per-input error as a
  missing sample instead of crashing. (output-identical — byte-identical and
  stderr-identical across the corpus on healthy inputs; the new throw fires only
  on real I/O failure, which the corpus does not exercise. Runtime tradeoff:
  extraction is modestly faster, and peak memory roughly doubles on the largest
  archives because decoded samples now live in memory for the archive's
  lifetime — measured 0.46 GB → 1.0 GB on the corpus's worst case, a 151 MB
  archive)
- **The `0x8A`/`0xFD` call stack no longer leaks across track boundaries** —
  `sp`, the Call/Return stack, was a single `stack<uint32_t>` shared by all 16
  tracks of a sequence, and `advanceToNextTrack` reset every other per-track
  field (`absTime`, `noteWait`, pan, mod type, tie state, `offsetTime`, …) but
  not it. A track that ended (`Fin`, a whole-song loop-back, or a stray Return)
  while a Call frame was still on the stack left that frame behind, so the next
  track's first unbalanced `0xFD` Return took the non-empty branch and jumped
  into the **previous track's code**, replaying it under the new track's index
  and channel — silently, since the "Return with empty call stack" notice only
  fires when the stack *is* empty. The engine keeps the call stack per-track
  (NW4R `callStack[]`/`callStackDepth`), so caesar now clears `sp` alongside the
  other resets. The same commit hardens the `0x8A` Call handler: a call target
  that is not a command boundary is now reported (`call target out of range;
  call ignored`) and skipped, instead of silently ending the whole track walk
  and dropping every remaining track; and a Call that is the last command in the
  bank no longer reads one past the end of the command map for its return
  address (undefined behaviour — the same family as the already-fixed
  `--begin()` bug) but jumps without pushing, leaving a later Return to take the
  honest empty-stack end-of-track path. (output-identical — byte-identical and
  stderr-identical across the 82-archive / 257,097-file corpus, where no leaked
  frame is ever consumed by a subsequent track's Return and every call target is
  valid; the fix removes latent cross-track corruption and two undefined-
  behaviour / silent-track-drop hazards that this corpus does not exercise)
- **Distinct sequence entries sharing a symbol name no longer overwrite each
  other's output** — a bank can be referenced by many INFO entries, each with
  its own start offset but the same symbol name, and both the extracted
  `.bcseq` and the converted `.mid` were named from that symbol alone. Every
  such entry composed the same output path, so the last writer won and the
  earlier entries' music silently never reached disk. The output path now keeps
  the bare name for the first entry to claim it and suffixes later collisions
  with the entry's start offset (`SEQ_1_0x1b40`, and the file id as a further
  tiebreak), so every distinct entry keeps its own `.bcseq`/`.mid`; an exact-
  duplicate INFO entry (same id and start offset) still reuses the one path.
  (changes `.mid`/`.bcseq` only — across the 82-archive / 257,097-file corpus
  this adds 28 files in one archive, `safe.bcsar` (14 `.bcseq` + 14 `.mid`, the
  relocated collision siblings of a 17-entry `SEQ_1` group), and changes 1 bare
  `.mid` to carry the first entry's output instead of the last's; no `.sf2`,
  `.wav`, raw-dump, stderr, or exit-code output changes)
- **The plain-value clamp is now universal, and the two comment-only sequence
  approximations emit a run-time notice.** `clampPlainCtrl` — a plain
  (un-prefixed) out-of-range `Uint8` clamps to 127 with an approximation notice,
  while unevaluated `Rnd`/`Var` stand-ins keep dropping — was applied at pan,
  volume, master volume, expression and init pan, but not at bend range (`0xC5`),
  portamento control (`0xC9`), modulation depth (`0xCA`) or portamento time
  (`0xCF`), which dropped a plain out-of-range write instead. All four now clamp;
  `0xCA` clamps once into the value it both latches into the mod shadow and
  emits, so the `0xCC` restore path can never replay an unclamped value.
  Separately, the `0xD8` LPF-cutoff → CC74 curve (which reads ~20% shallower than
  hardware) and `0xE0` mod delays above 1 s (which flatten to CC78 = 127) — both
  previously documented only in code comments — now fire the standard
  default-visible approximation notice. (changes `.mid` only, and only from the
  clamp: the corpus carries 228 plain portamento-time (`0xCF`) values above 127 —
  160 in `JokerSound.bcsar`, 68 in `QueenSound.bcsar`, refuting the v0.5.1
  triage's assumption that the remaining out-of-range drops were all `Rnd`/`Var` —
  which previously dropped their glide-time write and now clamp to CC5 = 127,
  changing 47 `.mid` across those two archives, every differing event a
  newly-emitted CC5 = 127 by independent SMF parse; `0xC5`/`0xC9`/`0xCA` never
  fire on the corpus. The two notices change no emitted value — stderr only:
  +4,121 LPF notices across 35 archives and +2 mod-delay notices, matching the
  documented p99 = 500 ms / max = 1,150 ms delay distribution. No `.sf2`/`.wav`/
  raw dump differs and no file is added or removed.)

## [0.5.1] — 2026-07-12

### Added

- **Tie mode (`0xC8`) is now converted** — tied notes previously re-attacked
  as independent short notes. On hardware a tie region is one continuous
  voice: the first note attacks, each later note only updates the sounding
  voice's pitch/velocity, note lengths are ignored for audio (the voice
  sustains through gates and rests), and the voice releases at the next tie
  command, `Fin`, or track end (NW4R-confirmed, cross-checked against
  GotaSequenceLib's executing player). caesar now flattens each region to
  gap-free back-to-back segments — one MIDI note per commanded
  pitch/velocity, spanning rests, the last extended to the region's end;
  identical re-commands merge into one sustained note. The remaining
  approximation (a re-attack at each pitch change instead of one continuous
  envelope — MIDI has no "retune the sounding note") is surfaced by a
  default-visible per-region notice. (changes `.mid` only, in sequences
  using tie)

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

### Changed

- `Rnd`-valued (random-range) arguments now convert as the range **midpoint**
  instead of the first-stored bound (nominally the minimum; the corpus
  carries both orders, which the symmetric midpoint makes moot). The engine
  rolls a fresh value between the bounds per execution; a deterministic
  converter must pick one stand-in, and the old choice silently biased 196k
  volumes, 177k pitch bends, and 94k rest durations (timing!) toward one end
  corpus-wide. Midpoint is the honest deterministic choice until real
  randomness lands with the convert-time VM. (changes `.mid` only — 5,135
  files across 60 archives; an independent SMF parse of every changed pair
  found only the predicted value changes, tick shifts, and note-duration
  changes, with zero note key/velocity changes and zero events added or
  removed)

- Warning-hygiene pass over the sequence converter's drop sites: everything the
  converter drops or approximates is now a default-visible notice with an
  honest category (previously most were `-w`-only, and several were fully
  silent). Notably: the extended (`0xF0`) command space — 353k `setvar`, 210k
  comparison ops corpus-wide — warned through a dead code path that could never
  fire (the parser never recorded the extended opcode) and is now surfaced;
  `Rnd`/`Var`-valued arguments (which convert as a deterministic stand-in —
  the range midpoint / the variable *index*) and `[If]`-prefixed non-jump
  commands (which execute unconditionally) get per-execution notices; `span`, `priority`, and
  `front bypass` are demoted to benign "no MIDI equivalent" notices (span is
  the console's front/rear surround axis — real on hardware under Surround
  mode, but MIDI has no surround axis in any mode); `0xDE` FX-send-C and any
  future unhandled opcode hit a final catch-all notice instead of vanishing;
  and the mod4 LFO warning labels (`0xAC`–`0xB1`), scrambled against the CTR
  byte map, are corrected. (output-identical — byte-identical across the
  82-archive corpus; stderr notices only)

### Fixed

- **Tracks now start with note-wait ON, matching the engine** — caesar
  initialised it OFF, compressing the timing of every track that plays notes
  before an explicit `0xC7` note-wait command (~112k notes across 67
  archives, overwhelmingly sound effects: steps lost their note-length
  waits, and the no-rest tied sweeps like `SE_Map_WarpstarUp*` collapsed
  onto a single tick). Three lines of evidence: the NW4R track constructor
  initialises `noteWaitFlag = true`; 92% of the corpus's explicit `0xC7`
  commands are *disables* (44,349 off vs 3,654 on — an authoring tool
  escaping an on-default); and the tie sweeps only produce their console
  sound with waits. (changes `.mid` only, in sequences with notes before
  any `0xC7`)

- A mod type (`0xCC`, or the extended mod2–4 types) above 2 no longer aborts
  the whole sequence with a parse error. The hardware stores the LFO target
  unvalidated and simply applies no LFO to an out-of-range value — the
  console plays such a file with the LFO silent — so caesar now emits a
  default-visible notice and converts the sequence, suppressing the
  pitch-vibrato CCs exactly as it does for any non-pitch target.
  (output-identical on the whole corpus, where no such value occurs; inputs
  carrying one now convert instead of failing)

- The sequence parser's two latent wrong-arg-count desync hazards are closed.
  A `_t` (ramp) suffix's trailing duration was only consumed for commands in
  `0xB0`–`0xDF` — on a note, tempo, sweep, or extended command those bytes
  were left unread, misframing every later command in the track — and the
  fixed-1-byte command group (`0xB2`/`0xBF`/`0xC7`/`0xC8`/`0xC9`/`0xCE`/
  `0xDF`, plus `0xCC` and the extended mod-types) read its argument with a
  bare 1-byte read that ignored a `Rnd` prefix's 4-byte range form. Both now
  go through the prefix-aware argument reader, and the trailing duration is
  consumed for every command form. Alongside: the non-opcodes `0x90`/`0x96`
  and `0xB7`–`0xBC` (not in the CTR command map; the original author's guessed
  probes) now fail fast as unknown commands instead of swallowing a guessed
  length that would perpetuate an upstream desync, and every `_t` ramp
  surfaces a per-execution "flattened to an instant jump" notice (375k
  volume fades corpus-wide were flattening silently). (output-identical —
  byte-identical across the 82-archive corpus, where none of the hazard
  patterns occur; stderr notices only)

- Vibrato CCs are now gated on the track LFO target (`0xCC` mod type). The
  3DS track LFO is one retargetable oscillator — pitch, volume (tremolo), or
  pan (auto-pan) — and caesar emitted the pitch-vibrato CCs (CC1/76/77/78)
  unconditionally, so tremolo and auto-pan spans (3,020 of the corpus's 8,006
  mod-type commands, at nearly double the depth of typical pitch spans) played
  as pitch wobble on every GM/SF2 player. Those CCs are now suppressed while
  the LFO targets volume or pan (default-visible notice), a live CC1 is zeroed
  when the LFO leaves pitch — the SF2 default mod-wheel modulator otherwise
  keeps wobbling pitch — and persisted values are restored when it returns
  (NW4R keeps the LFO parameters across a retarget). (changes `.mid` only —
  2,146 files across 58 archives; every differing event is a removed
  CC1/76/77/78 or an inserted CC1=0 by independent SMF parse of every changed
  pair, with no note or timing change; no `.sf2`/`.wav`/raw dump differs and
  no file is added or removed)

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

[Unreleased]: https://github.com/legoj15/caesar/compare/v0.5.1...HEAD
[0.5.1]: https://github.com/legoj15/caesar/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/legoj15/caesar/releases/tag/v0.5.0
