# caesar roadmap

This fork continues [kr3nshaw/caesar](https://github.com/kr3nshaw/caesar), which
has been unmaintained since 2021. caesar is the de-facto standard tool for
converting Nintendo 3DS **BCSAR** sound archives into SoundFont and MIDI, and no
maintained alternative exists. The near goal is to be the maintained, correct,
well-packaged version; the long goal is for caesar to be the foundation of a
full BCSEQ tool suite (player, tracker export, editor).

This is a living document tracking **what is open and what comes next**. It
stays short by design — completed items keep one line here; everything else
lives in the companion docs:

- [HISTORY.md](HISTORY.md) — completed work in full detail: verification
  narratives, A/B evidence, investigation stories, fixed bugs.
- [SUITE-DESIGN.md](SUITE-DESIGN.md) — the settled design for the tool suite:
  pipeline-rate decision, HLE-vs-oracle ruling, refactor plan, staged-plan
  rationale, risk register.
- [NW4C-disasm-handoff.md](NW4C-disasm-handoff.md) — deep reverse-engineering
  findings on the 3DS NW4C audio engine (addresses, evidence chains).

## Overarching goals

The plan is big; each goal is its own large step.

| # | Goal | Status |
|---|------|--------|
| 1 | Get the caesar MIDI/SF2 debugged to the best effort — fix the known bugs, apply the obvious UX fixes, ship as **v0.5.0** | ✅ Shipped 2026-07-10; **v0.5.1 fidelity patch scoped ← we are here** |
| 2 | Rebrand the repo as **"caesar salad"**, a play on the mix of things this repo strives to bring | Not started |
| 3 | Make an honest, **accurate-to-console player** | Design settled ([SUITE-DESIGN.md](SUITE-DESIGN.md)); suite stages 0–4 below |
| 4 | Make said player play at a **higher fidelity than console** *(ultimate goal)* | After goal 3 — a genuinely second pipeline clocked at the output rate (see the Nyquist correction in the design doc) |
| 5 | Add **PC-compatible tracker export** (best format, or several) | Format chosen: `.it`, with `.mptm` as a one-flag upgrade; suite stage 5 |
| 6 | **Editor** *(low priority)* | Suite stage 6 — write-back, gated on the round-trip milestone |

## Road to first release (v0.5.0)

All six workstreams are done — full narratives in [HISTORY.md](HISTORY.md):

1. **Modern build system** — ✅ CMake 3.21+/C++17, vendored libs as static
   libs, presets, repo hygiene; Linux verified end-to-end byte-identical.
2. **Robustness** — ✅ open-checked inputs, bounds-checked readers, per-input
   failure isolation, composed output paths (`-o/--output-dir`), sanitized
   archive-supplied names.
3. **Surface what's being dropped** — ✅ default-visible skip/approximation
   notices; MIDI-writer return values checked; fixed the banked-instrument
   (index ≥ 128) unreachability it exposed, console-validated.
4. **High-value fidelity & UX wins** — ✅ type-prefixed + group symbol naming,
   whole-song loop markers, note-less `[If]`-dispatcher resolution, decay-table
   typo fix, per-note tune field, FX sends → CC91/93, release-127 settled as
   instant by disassembly (`--pad-sustain` keeps the faked tail opt-in).
5. **Licensing** — ✅ libsmfc MIT notice restored; binaries distributable.
6. **CI & release automation** — ✅ three-OS build workflow (green) +
   tag-triggered release workflow + badge + dependabot.

Remaining:

- [x] **Cut v0.5.0** — shipped 2026-07-10: tag pushed, `release.yml` published
      the GitHub Release with all three OS zips.
- [ ] **macOS output verification** — CI proves macOS/Clang *builds*; a
      byte-identical output A/B on a real Mac has never been run (libc++ is the
      likeliest place a latent `std::filesystem`/float-formatting issue would
      surface). Not release-blocking; needs a Mac with test archives.

## v0.5.1 — MIDI-fidelity patch (scoped 2026-07-10)

A post-release opcode-by-opcode audit of the MIDI converter against the
12,308-file corpus found the discrepancies below. All are exporter-side,
bounded, and **touch `.mid` output only** — the standard byte-identical A/B
should show every `.sf2`/`.wav`/`.log`/raw dump unchanged and `.mid` diffs
confined to the affected sequences, which is itself the verification signal.
Ranked by priority:

- [ ] **Fix the GM percussion-channel collision.** *(Highest priority — a
      long-standing, user-confirmed pain: affected songs could previously only
      be salvaged by working around the drum channel in FluidSynth.)* The CSEQ
      track index is used directly as the MIDI channel on every emitted event,
      and channel 9 (1-based "channel 10") is reserved for drums by GM/GS
      players — so the 10th track of any large sequence renders as a drum kit,
      or as silence, since caesar SF2s carry no bank-128 drum preset. A corpus
      scan found **418 of 12,308 MIDIs with melodic note-ons on channel 9**
      (697 sequences use ≥ 10 channels), including Animal Crossing's museum and
      Kapp'n BGM and all of 3DS Photos' music. Fix: remap tracks to skip
      channel 9 whenever ≤ 15 channels are in use; for sequences using all 16,
      additionally emit the GS "rhythm part off" SysEx for part 10 (honored by
      FluidSynth) since no free channel exists. Ear repro:
      `GardenSound\BANK_BGM_IND_MUSEUM\SEQ_BGM_IND_MUSEUM.mid`.
- [ ] **Triage the ~1,020 surfaced controller/parameter drops.** Clamp plain
      out-of-range volume/pan/expression values (`Uint8` 128-255) to 127
      instead of dropping them; keep dropping-with-notice values that are
      garbage from unevaluated `Rnd`/`Var` prefixes until suite stage 4 models
      variables for real. Add a `bpm > 0` guard before `smfInsertTempoBPM` —
      this also closes the vendored `libsmfcx.c` `bpm == 0` division UB from
      the caller side (a `0xE1` tempo decodes as signed 16-bit; `bpm == 0`
      makes `60000000 / bpm` infinite and the int cast is UB — benign today
      only because the garbage fails a later range check), keeping the vendored
      copy pristine.
- [ ] **Fix the two mis-wired vibrato/pitch controls.** `0xE3` (sweep pitch, a
      signed-16 pitch-sweep amount) is emitted as CC78 "vibrato delay" — a
      mis-targeted control whose values also routinely exceed 127 and get
      dropped as part of the ~1,020. Stop emitting the wrong CC (the faithful
      treatment is a pitch-bend ramp — player/stage-2 territory; until then,
      drop with an honest notice). Sibling `0xE0` (mod delay, s16
      **milliseconds**) goes through the Int8-style `(x/2)+64` transform,
      which is meaningless for millisecond values; scale it sensibly.
- [ ] **Pass finite loop repeat counts through.** `0xD4`/`0xFC` loop pairs are
      emitted as EMIDI CC116/CC117 but always with value 0 — which means
      *infinite* in that convention, so an EMIDI-aware player loops a
      "play 4×" section forever. One line: emit `0xD4`'s count argument as the
      CC116 value. (Fully unrolling repeats in the timeline shares the
      flattening machinery with suite stages 2/5; not required here.)
- [ ] *(stretch — may slip to a later release)* **Tie mode (`0xC8`).** Tied
      notes currently re-attack instead of merging into one sustained note.
      Bounded but the largest item in this patch; overlaps with the stage-2
      sequence front-end, so slipping it costs nothing.
- [x] **Adopt a changelog** (`CHANGELOG.md`, Keep a Changelog format; release
      zips bundle it and `release.yml` publishes the version's section as the
      release notes). Convention: every user-facing change adds a line under
      `[Unreleased]` in the same commit, stating its output impact
      (output-identical vs which output types change).

## After the first release (not blocking v0.5.0)

Larger efforts that expand what caesar can do. Rough priority order:

- **Sequence (CSEQ → MIDI) fidelity.** Implement the full variable/conditional/
  random command machinery. Note-less `[If]` dispatchers are now resolved
  heuristically (a fully-silent track follows the default branch), but the
  underlying variables are still not evaluated, so: `[If]`-prefixed *non-jump*
  commands (e.g. `[If] Program`, `[If] Return`) currently execute
  unconditionally; the extended `setvar`/`cmp`/mod commands are parsed but
  dropped (their walker branch is dead code — `cmd.Cmd` is never set to the
  extended opcode); and random/variable values convert wrong. (The true engine
  semantics are now documented — see "RE priorities" in
  [SUITE-DESIGN.md](SUITE-DESIGN.md); bit-exact reproduction is possible.)
  The bounded exporter-side pieces — controller-range triage of the ~1,020
  surfaced drops, the mis-wired `0xE3`/`0xE0` sends, finite loop repeat
  counts, tie mode — are scoped into **v0.5.1** above; what remains here is
  the real variable evaluation those triage rules will eventually hand off to.
- **Missing audio coverage.** Implement IMA-ADPCM (codec 3), which currently
  produces silent output reported as success; extract CWSD wave-sound data
  (most sound effects), currently skipped entirely.
- **External dependencies (Mario Kart 7 class).** Resolve archives whose data
  lives in sibling `.bcgrp` group files — a known gap no maintained tool
  handles (the 28 empty `BNK_*` dirs in `ctr_dash`; now surfaced by a
  default-visible notice).
- **Architecture modernization.** Retire the global mutable parser state,
  pass decoded samples in memory instead of round-tripping through `.wav`
  files on disk, and adopt RAII/smart pointers over the manual `new`/`delete`.
  (Largely subsumed by suite stage 0 — see the refactor plan in
  [SUITE-DESIGN.md](SUITE-DESIGN.md).)
- **Format-family expansion.** Wii U / Switch **BFSAR** archives share this
  structure; supporting them would make caesar the only maintained cross-console
  converter. (Note: the sequence command stream is big-endian even where the
  container is little-endian — endianness can't be a single global switch.)

## The BCSEQ tool suite (goals 3–6)

caesar becomes the foundation of a suite: an accurate-to-console player, tracker
export, and eventually an editor. The design — including the two decisions that
determine everything else (mix at the native 32,728 Hz rate, resample exactly
once at the end; build our own HLE engine and use `teakra` + extracted firmware
strictly as an offline oracle) — is settled and recorded in
[SUITE-DESIGN.md](SUITE-DESIGN.md), with effort estimates and proof criteria
per stage. Status:

- [ ] **Stage 0 — library-core refactor**: `ParseContext` over globals, kill
      the disk round-trip, split parser/exporter, `caesar_core` library.
- [ ] **Stage 1 — byte-identical round-trip** of BCSEQ/BCBNK/BCSAR from a
      raw-backed model (**the next milestone** — the cheapest complete proof
      the format is understood, and the serializer everything else sits on).
- [ ] **Stage 2 — dry player**: native-rate voices, console interpolation,
      solved envelopes, priority voice stealing (RE'd and confirmed), tempo
      clock, single final upsample.
- [ ] **Stage 3 — reverb + delay**: offline `teakra` impulse capture →
      comb/allpass fit → New 3DS hardware validation. The long pole.
- [ ] **Stage 4 — exact variables/conditionals/random** + the NW4C
      `mod2/3/4` LFO curves (the one genuine engine unknown left).
- [ ] **Stage 5 — tracker export**: `.it` writer (`.mptm` one-flag upgrade).
- [ ] **Stage 6 — editor (write-back)**: smallest size-preserving edit first,
      proven on the New 3DS via LayeredFS.

## Settled decisions & standing rules

Hard-won conclusions — do not re-litigate without new evidence. Full stories in
[HISTORY.md](HISTORY.md) and [SUITE-DESIGN.md](SUITE-DESIGN.md).

- **Envelope byte 127 = instant** (fastest rate), for decay *and* release —
  settled by disassembly of the NW4C envelope code. The seconds-long tails on
  console are **DSP reverb**, not the note envelope. `--pad-sustain` exists as
  a labelled fake for reverb-less players; the future player must *not*
  inherit it.
- **Do not compensate for the missing reverb tail** with an SF2
  `reverbEffectsSend` generator (it would double-count the CC91 the sequence
  already carries) or by boosting a GM player's reverb (the 3DS reverb's
  *shape* is wrong in GM players, not its level).
- **`BGM_DEN_EMPTY_LANDSCAPE` cannot discriminate release times** — it is too
  dense for any voice to ring out exposed. `BGM_MAIN_Mii_Only_One` (1.6 s
  all-voices-off gap at ~51 s) is the discriminating console capture.
- **Reverb lives in the embedded Teak DSP firmware**, not `code.bin` — the
  reverse of the Wii architecture. Recover it behaviourally (offline `teakra`
  impulse capture); do not disassemble Teak code, and do not crib the Wii's
  `FxReverbHi` (that would import Wii reverb into a 3DS player).
- **Do not port Citra/Azahar's HLE audio** — it is the known-inaccurate path,
  and it is only the DSP *sink*; the game-side ARM11 sound driver it consumes
  from is the real project.
- **Stay in C++** — no language rewrite. The parser proven byte-identical
  across 40,000+ files is the project's one irreplaceable asset.
- **Verification is byte-identical old-vs-new A/B** over the private archive
  corpus (never in public CI — the archives are copyrighted). That net covers
  only the export path; the player needs its own golden-render +
  console-tolerance net from its first `.wav`.
- **SF2 bank selects go in CC0 (MSB)** — FluidSynth-GS and GM players ignore
  the LSB; only XG/MMA read it.

## Known bugs (open)

Fixed bugs and their verification stories are in
[HISTORY.md](HISTORY.md#fixed-bugs).

The MIDI-converter discrepancies found in the 2026-07-10 post-release audit
(GM drum-channel collision, controller-range drops, mis-wired `0xE3`/`0xE0`,
discarded loop counts, tempo-zero UB) are scoped as work items under
**v0.5.1** above rather than listed here twice.

- **Bank note fields are read at hardcoded offsets** (`Cbnk.cpp`, the
  note-parse loop). The format actually locates the ADSHR envelope through a
  `DataRef` chain (`note+0x10 + *(note+0x2C)`, then `+8`), and which optional
  parameters are present is gated by the flags word at `note+0x14`. Every one
  of 1,628 notes sampled across 57 banks and three engines carries flags
  `0x21F`, so the fixed layout is empirically safe for the games tested — and
  since the hardening pass a default-visible notice fires on any note whose
  flags differ (never yet observed). A differing bank would still *misparse*
  into plausible garbage rather than adapt; following the reference instead of
  hardcoding `+0x38` would remove the hazard. Latent, not observed.
- **Non-ASCII file names.** Input paths and archive-internal names pass through
  narrow `char*` / `std::string` into `std::filesystem`, so non-ASCII names
  (common for Japanese titles) can be mangled or throw. Illegal-character
  sanitizing is done; character encoding is not.
