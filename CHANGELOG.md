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

- **`tools/console-capture` live-shakedown hardening (console-in-the-loop
  automation shipped).** The capture pipeline is now live-proven end-to-end on
  the New 3DS (console-tolerance PASS): a closed-loop `RosalinaMenuDriver`
  pins the console volume override (rig constant 65%) verified against the
  Rosalina text readout; deploy is verify-first (hash the on-console cartridge
  read-only; push only when bytes differ — the boot-time file service is
  write-confined to `/luma/staging/`); the plaza navigation path is the
  live-verified route (HOME-tile walk, Y/switch-icons, touch-select). Offline
  tests 26 → 32. Tooling only; output-identical.

- **`tools/dsp-oracle` — the stage-3 DSP oracle (offline measuring
  instrument).** Vendors the teakra Teak-DSP interpreter (MIT, pinned commit,
  no patches) and adds a standalone `dsp_oracle` that boots the console's real
  DSP1 firmware (extracted locally from the user's own dumps via the included
  self-certifying `extract_dspfirm.py`; the firmware is copyrighted and never
  committed) through the LLE boot/pipe protocol to the audio callback, capturing
  the final stereo mix as WAV. Its own standalone CMake tree — never part of the
  main build or CI. Output-identical: no converter or player code touched.
- **`dsp_oracle --click` (stage-3 commit 2)** renders one dry PCM16 source
  through the real DSP firmware and captures it at the final mix, and answers
  route-a: the firmware **does** write the final mix back to the ARM11-visible
  `final_samples` region (word 0x8540) — region peak equals DAC peak at every
  amplitude. New `dsp_oracle_config_smoke` target. Offline measuring instrument
  only, out of caesar_core and CI; output-identical.

- **`tools/console-capture/` — hands-free New-3DS audio capture harness.**
  Independently-callable stages (preflight / deploy / navigate / record /
  verdict / perturbation-A-B) reusing `n3ds-mcp` by import; 26 offline tests
  against the simulators + a fake ffmpeg/ftp. Offline dev tooling only,
  output-identical (no caesar output type changes).
- **`tools/dsp-tap/` — Luma 3GX DSP shared-memory tap (scaffold + design).**
  Route-b console-capture side of the analog-free program: a barebones `.3gx`
  (not CTRPluginFramework — that is not GPL-compatible with this repo) that
  streams per-frame DSP config to an SD ring; versioned dump-format contract in
  `DSP-TAP-DESIGN.md`. Uncompiled scaffold, optional devkitARM, never in
  caesar_core/CI; output-identical.
- **Battery-v2 console-capture analysis scripts** (`tools/capture-cartridge/`):
  `analyze_capture_v2.py` (pilot-normalized pan L/R split + law/direction,
  LPF corner, velocity, steal tie-order), `analyze_capture_v2_trackB.py`
  (release/decay slopes, LFO FM-demod, portamento rate law, reverb residual),
  and `verify_lfo_porta.py` (independent blind re-measure). Offline analysis
  tooling; output-identical.
- **`tools/surround-probe/partb/` — Surround Part B stimulus + register reader
  (scaffold).** A span-sweep (`0xD7`)/`front_bypass` stimulus cartridge
  (`build_partb_cartridge.py`, byte-identical stage-1 round-trip gated) and a
  one-shot Rosalina GDB-RSP client (`gdb_read.py`, reads the current-bank
  `SourceConfiguration.gain[3][4]` IEEE-754 floats; 24/24 offline tests) to
  bind `span` to the DSP rear gain lanes. Live run pending a physical GDB-stub
  enable. Offline dev tooling; output-identical.
- **`caesar-play --list` now prints each entry's INFO volume byte.** The list
  already loaded the per-sound volume byte but never showed it; `doList` gains
  a `vol` column between `start` and `name`. Output-identical everywhere else
  (extraction and `.wav` renders unchanged; only `caesar-play --list` stdout
  gains a column).

### Fixed

- **`tools/capture-cartridge/check_prediction_v2.py` pan gate un-staled.** The
  track-A pan check predated the console-confirmed pan-direction inversion
  (player fix `9769a96`, byte 0 → RIGHT) and failed correct renders; it now
  asserts the monotone-increasing L−R split. Verification tooling only;
  output-identical.
- **`caesar-play` — the portamento byte→rate law is now `1/byte²` (console
  battery v2).** The `aafb2f3` provisional law made the glide rate `∝ 1/byte`
  (anchored byte 48 = 2.841 st/s). Battery v2 (2026-07-15, both agents,
  blind-cross-verified) pinned the law at **three** points — byte 24/48/96 =
  **11.49/2.84/0.713 st/s** — which is rate **∝ 1/byte²** (byte²·rate ≈ 6578,
  constant to ±0.6%); the `1/byte` form predicts 5.68/1.42 st/s at byte 24/96
  (2× wrong). The law is now `rate = 2.841·(48/byte)²` st/s (time-per-semitone
  ∝ byte²). The distance law (duration ∝ pitch distance) and the byte-48 = 2.841
  st/s anchor were already confirmed — only the byte→rate exponent changes. Byte
  0 (portamento off) stays instant. Changes portamento `caesar-play` `.wav`
  renders only; converter outputs identical (play-goldens byte-identical — no
  pinned sequence uses portamento; both New 3DS console-tolerance captures PASS).
- **`caesar-play` — the LFO rate constant is ~5.11× faster (console battery
  v2).** `kLfoRateHz` was a flagged `5/64` (0.078 Hz/unit) NW4R-precedent guess.
  Battery v2 (2026-07-15, two independent agents, FM-sideband spacing,
  blind-cross-verified) measured vibrato at **19.17 Hz at rate byte 48 and
  38.31 Hz at byte 96** — rate ∝ byte (exact 2× doubling) but 5.11× faster than
  the model. The constant is now the exact closed form `kNativeRate ÷ (512 ×
  kFrameSamples)` = 32728/81920 = **0.3995 Hz/unit** (the LFO advances byte/512
  of a cycle per 160-sample DSP frame at 32728 Hz; predicts 19.176/38.35 Hz at
  byte 48/96, ≤0.04% error). The rate ∝ byte law is unchanged; only the
  constant. Changes vibrato/LFO-modulated `caesar-play` `.wav` renders (CC1/
  pitch/etc. gated on the `0xCC` LFO target); converter outputs identical
  (play-goldens re-pinned; both New 3DS console-tolerance captures PASS).
- **`caesar-play` — the `0xD8` low-pass byte-48 corner is re-anchored to
  ~5.15 kHz (battery v2).** The shipped byte-48 anchor of 4.1 kHz came from
  battery v1, which undershot. Battery v2 (2026-07-15 stereo) reads the byte-48
  −3 dB corner at **≈5.1 kHz** (5.06–5.22 kHz across passes; the more defensible
  −3 dB crossing ≈5150 Hz, so `kLpfByte48Hz` moves 4100 → 5150). The
  187.5-cents/unit slope and the 1-pole order are confirmed — re-anchoring to
  5150 also fits byte-24 ≈400 Hz and byte-40 ≈2.1 kHz. Makes the `0xD8`-filtered
  voices brighter; changes LPF'd `caesar-play` `.wav` renders only; converter
  outputs identical (play-goldens re-pinned; both New 3DS console-tolerance
  captures PASS).
- **`caesar-play` — the pan law is now constant-power sqrt with the console's
  L/R direction.** The stereo pan used an equal-power `cos`/`sin` split; the
  console battery v2 (2026-07-15 stereo, both passes ≤0.01 dB apart) measured a
  **constant-power sqrt** split — byte 32 = −4.83 dB, byte 96 = +4.82 dB L/R,
  fit by sqrt to **0.10 dB** where `cos`/`sin` misses by 2.91 dB — and that the
  **direction is inverted**: byte 0 → RIGHT, byte 127 → LEFT (byte 64 → both
  √0.5). The sqrt law is corroborated by the disasm (the vsqrt pan routine at
  `0x14AFC8`); the L/R direction rests on the console split sign plus the rig's
  ch0 = 3DS-left labeling (flagged for a one-probe re-confirm). Changes every
  non-center-panned stereo `caesar-play` `.wav` render: the L/R spread narrows
  ~2.9 dB at the extremes **and mirrors**; loudness is preserved (both laws are
  constant-power). Converter outputs identical (play-goldens re-pinned; both
  New 3DS console-tolerance captures PASS).
- **`caesar-play` — the voice low-pass filter is retuned and softened to
  1-pole.** The `0xD8` LPF was an RBJ 2nd-order biquad placing byte 48 at
  2,890 Hz with a 12 dB/oct slope. The console battery (2026-07-14) measured
  byte 48 at **≈4.1 kHz with a ~6–7 dB/oct (1-pole) slope** — half an octave
  too dark and twice too steep. The byte→corner curve is re-anchored on the
  measured point (byte 48 = 4.1 kHz, the 187.5-cents/unit slope kept and
  flagged for battery v2) and the topology is now a bilinear-transform 1-pole
  low-pass (reusing the biquad state with `b2 = a2 = 0`). This lifts the
  LPF'd `SE_NEW_SLIDE_MAP` **+5.7 dB RMS (+4.8 dB peak)** while the unfiltered
  `SE_NEW_DRUM01` is unchanged (0.0 dB), closing most of the battery-A finding
  that the slide rendered quiet relative to the drums. Changes `caesar-play` `.wav` renders only; converter
  outputs identical (play-goldens re-pinned; both New 3DS console-tolerance
  captures PASS).
- **`caesar-play` — portamento is now a constant-rate, distance-proportional
  glide.** The `0xCF` glide was a fixed full-distance glide over
  `portaTime × samplesPerTick` (distance-independent, ~17× too fast on the
  measured interval). The console battery (2026-07-14, KEY_FLY) measured a
  constant-rate, linear-in-cents glide of **2.841 st/s at time byte 48**
  (0.352 s/semitone, R²=0.99998), so the duration is now `|distance| / rate`.
  The `portaTime → rate` law is a **provisional single-point fit**
  (seconds-per-semitone linear in the time byte, anchored so byte 48 = 2.841
  st/s; monotonic and tempo-independent), pending battery v2's second capture
  point. Byte 0 (portamento off) stays instant. Changes `caesar-play` `.wav`
  renders only; converter outputs identical (play-goldens re-pinned; both New
  3DS console-tolerance captures PASS).
- **`caesar-play` — the decibel-domain divisor: volume/expression/sustain bytes
  now map to `(v/127)²` amplitude and decay/release run at their true dB rate.**
  `gainFromValue` applied the NW4C `DecibelSquareTable` as `10^(value/400)`
  (byte-linear amplitude); the engine applies `value/10` as plain dB —
  `10^(value/200)`. Diagnosed from a user report that `SEQ_SD_BGM_RESULT`
  (Mii Plaza DLC `mgExp`) buried its kick drum: the accompaniment tracks ride
  expression bytes 74/83/59 and each played 3.7–6.7 dB too loud. The fix lands
  kick prominence within ~1 dB of the console capture (was 3.4 dB under), makes
  the player's volume law agree with the SF2 exporter's `ConvertVolume` and the
  console-confirmed vel² law, and closes the capture battery's "decay dynamics
  ×2" finding (−188 dB/s modelled vs −174 measured) without touching the
  disasm-verbatim `calcRelease` rates. Changes `caesar-play` renders only;
  converter outputs identical (play-goldens re-pinned; both New 3DS
  console-tolerance captures PASS).

### Added

- **`tools/capture-cartridge` — the console calibration cartridge builder.**
  Surgically patches a retail `MeetSound.bcsar` (same-size, in-place, by-name
  lookup; three byte regions) so StreetPass Mii Plaza's `BGM_MAIN_Mii_Only_One`
  plaza tune becomes a hands-free 43.5 s looping calibration battery played by
  the console's own engine via Luma3DS LayeredFS — replacing the impractical
  per-SE in-game trigger hunt in `docs/CAPTURE-REQUEST.md`. The driver gates
  the build on converter parse, byte-identical round-trip, a `caesar-play`
  prediction render, and a numerical schedule check. Tooling only:
  output-identical (no `src/` change; all converter and player outputs
  unchanged).
- **`caesar-play` — the suite stage-2 offline dry player (Phase I: first audible
  `.wav`).** A new `caesar_play` static library and `caesar-play` CLI, built
  alongside the converter but entirely separate from it: they re-interpret
  `Cseq`'s emit walk as a concurrent real-time sequencer feeding an in-memory
  voice DSP that mixes at the DSP's native 32,728 Hz and resamples once
  (Blackman-Harris windowed-sinc) to the output rate. `caesar-play --list` names
  an archive's renderable sequences; `--render <archive> --seq <name-or-index>
  --out <file.wav> [--rate <hz>] [--max-seconds <n>]` renders a chosen sequence to
  a 16-bit stereo WAV with no audio device; `--render-note` is the single-voice
  DSP proof. Loads read-only via `Csar::Parse` and an in-memory construct+parse of
  the wave archives, banks and sequence (no extraction, no `.wav`/`.sf2`/`.mid`
  written). A render golden net (`tools/play-goldens`) byte-pins the rendered WAV
  hashes with a determinism guard and self-test. Commands wired: notes, rests,
  program change, tempo (0xE1), timebase (0xB0), note-wait (0xC7), OpenTrack/
  Jump/Call/Return/Fin, and the `[If]`/variable/comparison VM; volume/pan/pitch/
  LFO/fx-send commands are safe-skipped (no timing desync) pending later commits.
  **`output-identical` for the converter** — `caesar` is a byte-for-byte unchanged
  parallel target (ab-verify + diag-goldens both exit 0); this adds a new build
  artifact only.
- **`caesar-play` Phase II — the "mostly right" milestone (C4–C6): native
  envelope, voice pool, and gain/pan/pitch.** The dry player now shapes every note
  with a direct port of the NW4R/NW4C `EnvGenerator` (attack/hold/decay/sustain/
  release) instead of a declick gate — its `DecibelSquareTable`/`attackTable` read
  byte-for-byte from the console binary, `CalcRelease` verbatim, so a `release`/
  `decay` byte of 127 stops the voice in one step (the instant-release correction,
  no fake 3.5 s tail); Cbnk's SF2 timecent approximations are ignored. Voices now
  contend for the single 24-voice priority pool (reuse-free / steal-lowest /
  refuse-if-front-outranks, verbatim from the disasm), with `0xC6` track priority
  and `0xB2` mono/poly honoured (both of which the converter can only drop). Track
  volume (`0xC1`), master volume (`0xC2`), expression (`0xD5`), additive pan
  (`0xC0`/`0xDC` + the note's own pan, equal-power), velocity (linear-squared), and
  pitch bend + range + coarse-tune (`0xC4`/`0xC5`/`0xC3`) fold natively into
  per-voice gain / pan / step — resolving Phase I's dry-sum clipping (BGM_DEN_RESULT
  peak-clip 0.64 % of samples → 11 samples, RMS 0.29 → 0.12, with a real stereo
  field). **`output-identical` for the converter** — `caesar` stays byte-for-byte
  unchanged (ab-verify + diag-goldens exit 0); only `caesar-play`'s audio changes.
- **`caesar-play` Phase IV — the console-tolerance net (`tools/console-tolerance/`),
  built and self-validated, and run against the real captures.** A numpy-only
  analyzer (`console_tolerance.py`) verdicts a `caesar-play --render` against a New
  3DS line-in capture of the same sequence to the stage-2 criterion — match within
  tolerance EXCEPT the reverb tail — over four metrics (envelope fit, onset-timing
  tempo/clock, per-channel Welch-PSD distance, loudness distribution), reports the
  reverb-attributable residual separately (never a failure; it is stage 3's target),
  and emits a targeted measurement for each flagged constant (output level, pan,
  vibrato rate, tail-decay time, interpolation HF). A PowerShell driver
  (`console-tolerance.ps1`) renders each captured sequence at the capture's own rate
  and verdicts it (default `-CapturesDir` is the extracted MeetSound bank directory,
  handling **192 kHz / 16-bit** captures), with the ab-verify/play-goldens discipline
  (build-freshness gate, Stop-funnel, exit 0/1/2). A mandatory `--self-validate`
  synthesizes a realistic capture-chain fixture (capture-rate resample, gain, −85 dBFS
  noise, lead-in, ±50 ppm drift, exponential reverb tail) that must PASS while three
  negative controls (wrong tempo, padded release, different sequence) each FAIL their
  named metric. First real run: `BGM_MAIN_Mii_Only_One` and `BGM_DEN_EMPTY_LANDSCAPE`
  both PASS all four metrics against their console captures. **`output-identical`** —
  no C++ changed; this is dev tooling only (`caesar` byte-for-byte unchanged).
- **`caesar-play` Phase III — the fidelity mass (C7–C10): live `_t` ramps, tie,
  sweep, portamento, the LFO, and the remaining track features.** Per-track
  volume/pan/pitch/expression stop latching at note-on: a reusable
  timeline-flattener makes them **live curves** a sounding voice follows every
  frame, and a `_t`-suffixed command glides the parameter to its target over the
  trailing duration (the ~462k-event ramp mass). Tie (`0xC8`) plays **one
  continuous voice** across its notes, retuning it with no re-attack (the tied
  WarpstarUp sweeps go from 249 stolen voices to 3 continuous ones); sweep
  (`0xE3`) and portamento (`0xC9`/`0xCE`/`0xCF`) add independent intra-note pitch
  glides. The persistent, retargetable track **LFO** (`0xCA`–`0xCD` + `0xE0`)
  renders vibrato / tremolo / auto-pan (pitch cents = depth × range; rate linear in
  the commanded value). Mid-sequence **bank switch** (`0xB6`) re-points a track's
  instrument lookup to another in-archive bank (resolving notes that otherwise
  drop); velocity range (`0xB3`), track mute (`0xDD`), damper (`0xDF`), and an RBJ
  low-pass biquad for the LPF cutoff (`0xD8`/`0xB4`/`0xB5`) round it out. Four new
  render goldens pin the repros (a ramp fade, a tie sweep, a vibrato tune, a
  bank-switcher). LFO rate/sine, portamento timing and the biquad topology are not
  in the disasm and are flagged for the Phase IV console capture. **`output-
  identical` for the converter** — `caesar` stays byte-for-byte unchanged (ab-verify
  257,125 files + diag-goldens 18/18 exit 0); only `caesar-play`'s audio changes.
- **BCSAR container round-trip serialization (`Csar::Serialize()` in `caesar_core`)
  — the stage-1 milestone reached: byte-identical BCSAR + BCSEQ + BCBNK round-trip,
  proven corpus-wide.** The inverse of `Csar::Parse`, reading only model state
  (child blobs, opaque tails, the STRG lookup tree and the `0x220B` block are the
  only reads of the source, and those are pure copy-throughs). It rebuilds the CSAR
  header, the STRG string table, the INFO section (8-entry reference block, each
  sub-table's count + entry-offset array, and the FILE table's `0x220C` data
  offsets) and the FILE section, recomputing every section and sub-table offset/
  length from the laid-out content. Child blobs are copied through as spans (the
  deep BCSEQ/BCBNK re-embed is an optional later capstone); the FILE section lays
  out only the **maximal (top-level)** blobs and **re-points** a nested FILE entry
  (a bank/sequence/wave-archive that lives inside a CGRP container blob) into that
  copied container rather than emitting it twice. Layout facts pinned by a
  standalone parser+serializer over all 82 corpus archives first: `declaredLength`
  (the header file-length word) is larger than the physical file for archives that
  reference external content and is retained separately; a few archives carry no
  STRG symbol table (`StrgOffset == 0xFFFFFFFF`); the `0x220B` "end" reference is a
  real trailing metadata block, not padding; FILE blobs are `0x20`-aligned with
  all-zero gaps. The `caesar-roundtrip` dev tool's `--verify` now re-serialises the
  container itself: **82 / 82 archives byte-identical, 0 mismatched** (BCSEQ stays
  20,791 / 20,791, BCBNK 11,136 / 11,136). With all three deep formats serialising,
  the per-archive and corpus exit contract reaches its final form — **exit 0 = every
  deep format matched**, the permanently-opaque BCWAR/BCWAV/BCWSD/BCGRP children
  reported SKIPPED informationally (they never block a pass). The exe's `--selftest`
  and the wrapper's `-SelfTest` byte-flip proof now cover BCSAR as well as BCSEQ and
  BCBNK. (`output-identical` for the shipped `caesar` — the added `Csar::Parse`
  retention produces no output and `Serialize` is dev-tool-only; 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0;
  18-surface diagnostics goldens byte-identical.)
- **BCBNK round-trip serialization (`Cbnk::Serialize()` in `caesar_core`) — the
  stage-1 milestone's second serializer, proven byte-identical over the whole
  corpus.** The inverse of `Cbnk::Parse`, reading only model state and never the
  source buffer. The header, INFO header, CWAV table and instrument table are
  recomputed at their fixed positions; the instrument and note bodies are written
  **positionally** — each at its retained file offset into a zero-filled buffer —
  because the offset tables place bodies out of index order with gaps between them,
  so a linear re-emit is impossible. The one Cbnk-internal trap (found by a
  standalone parser+serializer over all 11,136 corpus banks before the C++): the
  read fields tile each bank almost perfectly, but a handful of note-body tails
  and inter-body pads are never read — corpus-wide only ~1.5 KB across 5 banks —
  so `Parse` now captures those unread runs (only the non-zero ones; the rest are
  reproduced by the zero fill) and `Serialize` overlays them. The `caesar-roundtrip`
  dev tool's `--verify` now re-serialises every embedded `.bcbnk` and compares
  against a saved source span: **11,136 / 11,136 banks across 82 archives
  byte-identical, 0 mismatched** (BCSEQ stays 20,791 / 20,791; the BCSAR container
  and the opaque BCWAR/BCWAV/BCWSD/BCGRP children remain SKIPPED, so the run stays
  exit 2 = PARTIAL by contract until the commit-4 container serializer). The exe's
  `--selftest` and the wrapper's `-SelfTest` byte-flip proof now cover BCBNK as
  well as BCSEQ. (`output-identical` for the shipped `caesar` — the `Parse`
  gap-capture produces no output and `Serialize` is dev-tool-only; 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0;
  18-surface diagnostics goldens byte-identical.)
- **BCSEQ round-trip serialization (`Cseq::Serialize()` in `caesar_core`) — the
  stage-1 milestone's first serializer, proven byte-identical over the whole
  corpus.** The exact inverse of `Cseq::Parse`: it walks the parsed command map
  and re-emits every command's prefix/status/argument bytes (canonical VarLen,
  big-endian fixed-width command args, `Rnd` bounds verbatim) plus the CSEQ
  header and the LABL symbol section, reading only model state and never the
  source buffer. Three findings drove the design and are now handled exactly:
  the DATA section's `0x20` zero-padding is parsed as phantom note commands that
  spill up to two bytes into LABL (reproduced by re-emitting the command stream
  and truncating to the retained section length); the LABL entry table carries
  duplicate-target labels the parse's pointer-keyed map collapses (a full label
  table is now retained on the model, separate from the per-command label Export
  uses); and each LABL record is null-terminated and 4-byte aligned with the
  section `0x20`-padded. The `caesar-roundtrip` dev tool's `--verify` now
  re-serialises every embedded `.bcseq` and compares against a saved copy of the
  source span: **20,791 / 20,791 sequences across 82 archives byte-identical, 0
  mismatched.** A new `--selftest` mode and the wrapper's upgraded `-SelfTest`
  carry the byte-flip proof (round-trip a real BCSEQ, then confirm a planted
  one-byte diff is caught). Shared little/big-endian + canonical-VarLen emit
  primitives (`WriteFixLen`/`WriteVarLen`, the inverses of the `ReadFixLen`/
  `ReadVarLen` parse helpers) live in `Common` for the BCBNK/BCSAR serializers
  to reuse. (`output-identical` for the shipped `caesar` — additive `caesar_core`
  API + dev-tool proof only; 82-archive / 257,125-file corpus A/B byte-identical
  with identical stdout/stderr, exit 0; 18-surface diagnostics goldens
  byte-identical.)
- **`caesar-roundtrip` — a development tool (not shipped) that scaffolds the
  stage-1 byte-identical round-trip verifier and carries the two one-time corpus
  scans that gate the serializer commits.** It is `caesar_core`'s first external
  consumer, which is why `caesar_core` now carries an explicit
  `target_include_directories(... PUBLIC src)` instead of borrowing the header
  path from libsmfc. `--verify` parses an archive read-only, enumerates every
  embedded child plus the container, copies each source span out, and (once a
  `Serialize()` lands) compares a re-serialisation against that copy; with no
  serializers yet it reports every format SKIPPED and exits 2 ("nothing
  verifiable"), so the harness can never print a false pass. `--scan-varlen` and
  `--scan-gaps` produced the commit-0 gate verdicts. A `tools/roundtrip-verify`
  corpus fan-out wrapper (ab-verify-grade guards + a self-test) drives it.
  (`output-identical` for the shipped `caesar` — new dev binary only; public CI
  builds it but the release zip does not package it; 82-archive / 257,125-file
  corpus A/B byte-identical with identical stdout/stderr, exit 0; 18-surface
  diagnostics goldens byte-identical.)
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

### Changed

- **`Cbnk` now parses into a retained model, then builds the SF2 from it in a
  separate step** (stage-1 commit 2 — the split the other five classes already
  had). The former one-pass `Convert` is split into `Parse` (the CBNK/INFO walk:
  the CWAV reference table, every instrument and note body, and all of the
  parse-phase `Analyse`/`Warning`/`Assert` output) and `Export` (the live-`Cwav`
  sample resolution, the `<id>.wav` echoes, the release-127 warning, and the SF2
  write); Csar and Cgrp call them back to back per bank, so the output stream is
  unchanged. The model is promoted off `Convert`'s function locals onto the
  object and the last two raw-pointer model fields (`CbnkInst::Offset`,
  `CbnkNote::Offset`) become span-relative `uint32_t` body offsets; the model now
  also retains the discarded `cbnkVersion`, the instrument-type discriminator,
  the note `id` gating the layered-note quartet, the raw CWAV index, and the raw
  instrument/note offset-table words — the lossless retention the BCBNK
  round-trip serializer will consume. (`output-identical` — 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0;
  18-surface diagnostics goldens byte-identical.)
- **`Cwav` now parses into a retained model, then writes the `.wav` from that
  model in a separate step** (stage-0 model/exporter split, commit 1 of 5). The
  former one-pass `Convert` is split into `Parse` (INFO walk + PCM decode into
  the object; no file output) and `ExportWav` (RIFF/`smpl` writer, reading only
  model fields); the wave archive calls them back to back. The object now retains
  the codec, per-channel DSP context/coefficients as span-relative offsets, the
  raw DATA-section span (offset+length, no copy), and the previously discarded
  header words — groundwork for the round-trip serializer. (`output-identical` —
  82-archive / 257,125-file corpus A/B byte-identical with identical
  stdout/stderr, exit 0; 17-surface diagnostics goldens byte-identical.)
- **`Cwar` and `Cbnk` likewise parse into a retained model, then export from
  it** (stage-0 model/exporter split, commits 2–3 of 5). `Cwar::Extract` splits
  into `Parse` (the cwav offset+length table, the FILE span, and `cwarVersion`)
  and `Export` (dump each `.bcwav`, then construct + `Parse`/`ExportWav` the
  child `Cwav`); `Cbnk`'s `CbnkCwav` shrinks to the raw sample reference, with
  live-`Cwav` resolution (decoded PCM, sample rate, loop points, the per-sample
  `<id>.wav` echo) moved out of the parse walk and into the SF2-emit phase, and
  the note words it previously logged-then-dropped now retained on the model.
  (`output-identical` — 82-archive / 257,125-file corpus A/B byte-identical with
  identical stdout/stderr, exit 0; 17-surface diagnostics goldens byte-identical.)
- **Embedded wave archives and waves are parsed from the parent's buffer
  instead of re-reading the file they were just written to** (stage-0 per-file
  split, first tranche: `Cwar`, then `Cwav`). `Csar`/`Cgrp` still write each
  extracted `.bcwar`, and `Cwar` still writes each `.bcwav` (and `Cwav` its
  `.wav`) — the user output is unchanged — but the child parser now receives the
  exact bytes as a pointer + length into the parent's already-loaded buffer
  rather than opening, seeking, and re-reading the file. The direct-`Csar` wave
  archive and every wave borrow the parent span (their parent provably outlives
  them); a group-resident wave archive, stored in the archive-lifetime shared
  map but built from the stack-local `Cgrp` buffer, takes an owned copy. The old
  file-path constructors are removed. (`output-identical` — 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0;
  17-surface diagnostics goldens byte-identical; extraction is modestly faster
  from dropping the per-child re-read.)
- **Embedded banks, sequences and group files are likewise parsed from the
  parent's buffer — the disk round-trip for embedded children is now fully
  gone** (stage-0 per-file split, second tranche: `Cbnk`, `Cseq`, `Cgrp`). With
  `Cwar`/`Cwav` already migrated, every embedded child now receives its bytes as
  a pointer + length into the parent's already-loaded buffer instead of
  re-opening the file it was just written to; only the root `Csar` still reads
  from disk, because it opens the actual CLI input rather than a child it wrote.
  `Csar`/`Cgrp` still write each extracted `.bcbnk`/`.bcseq`/`.bcgrp` as user
  output. All five new sites borrow the parent span (each parent provably
  outlives its child — a `Cbnk`/`Cseq` never enters the archive-lifetime shared
  map, and a group borrows a window into `Csar`'s buffer that outlives it), so
  the only owned copy in the whole split stays the group-resident `Cwar`. The
  old file-path constructors are removed. (`output-identical` — 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0
  each commit; 17-surface diagnostics goldens byte-identical.)
- **The global mutable parser state is now a `ParseContext` passed by
  reference** (stage-0 library-core fold). The six process-globals behind the
  old `Common::` facade (`ShowWarnings`, `FileNames`, `Offsets`, `Buffers`,
  `Log`, `Notices`) and the parse helpers that operate on them (`Assert`,
  `Error`, `Warning`, `FlushNotices`, `Push`/`Pop`/`Reset`, `RequireOpen`,
  `CheckBounds`, `Analyse`, `Dump`, `ReadFixLen`/`ReadVarLen`) now live on a
  `ParseContext` object created in `main` and threaded through every reader
  (`Csar` → `Cgrp` → `Cbnk`/`Cwar`/`Cwav`/`Cseq`), mirroring the existing
  `Options` injection; the `Common` struct and the free read functions are
  gone (`TypedName`, being context-free, stays a free function). The context's
  lifetime still spans the whole run, so cross-input behaviour is unchanged;
  this is the internal-architecture prerequisite for reentrancy and later
  per-input scoping. (`output-identical` — 82-archive / 257,125-file corpus
  A/B byte-identical with identical stdout/stderr, exit 0; 17-surface
  diagnostics goldens byte-identical.)
- **`Rnd` argument bounds now survive parse; the midpoint stand-in is computed
  at emit** (`Cseq.cpp`/`Cseq.hpp`). `ReadArgs` used to collapse a random
  range's two raw `s16` bounds to their `(lo + hi) / 2` midpoint at parse time,
  welding an exporter policy into the parsed command model. The model now
  retains the raw pair as read (`CseqCmd::Arg1Rnd`/`Arg2Rnd`, kept UNSORTED —
  the hardware stores them unsorted and both orders occur), and the emit walk
  (`resolveArg`) computes the identical midpoint at the same point every
  consumer already saw it. Groundwork for the stage-1 lossless round-trip
  serializer. (`output-identical` — 82-archive / 257,125-file corpus A/B
  byte-identical with identical stdout/stderr, exit 0.)
- **`Cseq` now parses into a retained command model, then emits the `.mid` from
  that model in a separate step** (stage-0 model/exporter split, commit 4 of 5 —
  the last per-class split before `Csar`/`Cgrp`). The former one-pass `Convert`
  splits into `Parse` (CSEQ/LABL/DATA headers → the command map; no file output,
  every `Assert`/`Error` fires here) and `Export` (the convert-time VM +
  control-flow interpreter + MIDI writer, reading only model state); `Csar`/`Cgrp`
  call them back to back per sequence, so the per-file phase boundary — and the
  `-w` warning ordering — is unchanged. The emit walk **no longer reads the
  source buffer at all**: each command now carries its own DATA-relative source
  offset, and `DataOffset`/`Version` (the `cseqVersion` word) are retained on the
  object, so the walk locates every command and every diagnostic from a stored
  offset (`DataOffset + 8 + offset`) rather than a raw pointer into the live
  bytes. Groundwork for the stage-1 lossless round-trip serializer.
  (`output-identical` — 82-archive / 257,125-file corpus A/B byte-identical with
  identical stdout/stderr, exit 0; diagnostics goldens byte-identical, including
  the `-w` surfaces corpus-wide.)
- **`Csar` and `Cgrp` now parse the whole archive into a persistent record tree,
  then export from it — the model/exporter split is complete across all six
  classes** (stage-0 model/exporter split, commit 5 of 5). Each container's
  former one-pass `Extract` splits into `Parse` (headers/STRG/INFO/FILE →
  retained `Strgs`/`Files`/`Cwar`/`Cbnk`/`Cseq`/`Cgrp` record vectors, the
  discarded header words, and the never-parsed player/set/INFX regions as opaque
  spans) and `Export` (the child-dump/recurse walk: directory creation, blob
  writes, child echoes, conversions, and skip/external warnings, ending in the
  `.log` dump). `Csar::Extract` stays the public entry `main` calls, now a thin
  Parse-then-Export driver. Every record offset is span-relative (no new
  raw-pointer state), the internal/external discriminator on file records is
  explicit, and the previously logged-then-dropped INFO words are retained as
  typed fields. (`output-identical` — 82-archive / 257,125-file corpus A/B
  byte-identical with identical stdout/stderr, exit 0; 18-surface diagnostics
  goldens byte-identical, including the multi-input and `-w` surfaces.)
- **A `caesar_core` static library is split out from the CLI** (stage-0 library
  split — the final step; **stage 0 is now complete**). The six BCSAR format
  classes, their shared `Common`/`ParseContext`, and the header-only `Options`
  now build as a `caesar_core` static library (which links the vendored
  `sf2cute` + `libsmfc`); the `caesar` executable is just `src/caesar.cpp`
  linking it. This is the library the suite's player/tracker/editor will link
  directly — the CLI is merely its first consumer. (`output-identical` — a
  build-only restructure with no code changes; every first-party translation
  unit's compile flags verified byte-identical before/after via a
  `compile_commands.json` diff, 82-archive / 257,125-file corpus A/B
  byte-identical with identical stdout/stderr, exit 0, 18-surface diagnostics
  goldens byte-identical.)

### Fixed

- **`caesar-play`: the per-sound INFO volume byte now reaches the mix.** Every
  CSAR sound entry carries a designer-set volume (low byte of the retained
  `Word08`); the player never applied it, so the bus ran hot and the final
  clamp flat-topped loud passages — the diagnosed eShop-menu bass distortion
  (`SEQ_TIGER_TOP_EF` ships at 101 = −2.0 dB; retail archives almost never use
  127). The byte is now a typed `CsarCseq::Volume` field applied per voice as
  a plain linear ratio `vol/127` (bytes above 127 exist in retail archives, so
  the law is linear-with-boost, not a dB-table lookup). Measured: eShop
  clipped samples collapse 2,367 → 166 and the render's RMS shifts −1.98 dB
  against the byte's predicted −1.99 dB. **Changes `caesar-play` rendered
  audio only** — the converter never consumed the byte and its outputs are
  byte-identical (ab-verify + diag-goldens + round-trip all exit 0).

- **`caesar-play`: a force-stopped voice now declicks over one DSP frame instead
  of stopping dead.** A voice cut by a pool steal, a mono re-trigger or the
  render cap ended at its instantaneous amplitude with no ramp — an audible
  click landing exactly on a 160-sample frame boundary (the diagnosed
  `BGM_DEN_EMPTY_LANDSCAPE` 8.06 s click: two still-audible releasing pads
  stolen at native sample 263,840). The same off-by-one-frame truncation in
  `voiceEndSample` dropped the envelope's final sustain→0 ramp frame, cutting
  every release-127 (instant-release) note-off dead at sustain gain. Both paths
  now render that one final frame linearly faded to zero (~4.9 ms), matching
  the hardware DSP's per-frame gain interpolation. **Changes `caesar-play`
  rendered audio only** (every render: +160 native samples of final-ramp tail;
  the converter is untouched — no A/B surface changes).

- **Sequence `-w` warning positions now derive from a stored offset instead of a
  pointer subtraction against the shared stack top** — the Cseq slice of the
  heap-layout `-w` nondeterminism Known bug. A new
  `ParseContext::Warning(uint32_t position, …)` overload takes the `AT POSITION`
  value directly (same output format); the ~40 Cseq emit-walk warning sites pass
  `DataOffset + 8 + <command offset>` rather than the old `pos - Offsets.top()`,
  which is only correct when this sequence's own frame tops the stack. **Latent
  on this corpus:** every corpus sequence converts direct off the `Csar` (a
  whole-corpus scan found none routed through an embedded group), where the
  stored offset equals what the subtraction already produced — so the `-w` bytes
  are unchanged corpus-wide. A *group-resident* sequence, which this corpus
  lacks, would previously have printed run-to-run-varying garbage
  (`0xFFFFFFFFFF…`-class) and now prints the correct in-file offset. The Known
  bug stays **open** for the `Cwav`/`Cbnk`/`Cgrp`/`Csar` warning sites (the ones
  that do manifest run-to-run on the corpus), which keep the pointer form; the
  new overload is theirs to adopt next. (`output-identical` — corpus-wide,
  default *and* `-w`; a new `w-dlplay` diagnostics golden pins the direct-path
  Cseq `-w` surface the corpus A/B never sees.)
- **macOS build fixed (again)** — the Cbnk model/exporter split introduced
  three more unqualified `move(...)` calls, rejected by Apple Clang under
  `-Werror` (`-Wunqualified-std-cast-call`); qualified as `std::move`.
  Windows/Linux builds were unaffected. (output-identical — build only)
- **Multi-input runs no longer bleed the analysis `.log` across archives** — in
  `caesar a.bcsar b.bcsar` the single process reused one parser context, whose
  `Log` was cleared only on the exception path, so `b`'s `.log` was `a`'s
  analysis rows *followed by* `b`'s own (the per-input dropped/approximated
  notice summary already flushed per input and never bled). Each top-level input
  now gets a fresh `ParseContext`, so every input behaves exactly as if it were
  the only argument on the command line: a multi-input run is now N independent
  single-input runs. The now-structurally-guaranteed cross-input isolation also
  means an earlier input's soft-fail or a partial parse can no longer influence a
  later input's diagnostics or attribution (the old exception-path `Reset()`,
  which existed only for that cleanup, is removed). Unchanged across inputs, by
  design: the leaked `cerr` format flags (they live on the stream, not the
  context) and the shared process exit code. `-w` remains positional — it
  applies only to inputs after it on the command line. (changes `.log` content in
  **multi-input** runs only; single-input output is byte-identical — 82-archive /
  257,125-file corpus A/B byte-identical with identical stdout/stderr, exit 0,
  and the 17-surface diagnostics goldens show the sole change is the multi-input
  `.log`, recaptured)
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
