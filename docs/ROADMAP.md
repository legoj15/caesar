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

- [x] **Fixed the GM percussion-channel collision** — track 9 now relocates off
      GM channel 9 to a free channel (or, when all 16 tracks are used, stays put
      with a GS "rhythm part off" SysEx for part 10); the SMF track layout is
      unchanged, so sequences without a 10th track are byte-identical. Full
      narrative and A/B in [HISTORY.md](HISTORY.md#fixed-bugs).
- [x] **Triaged the ~1,020 surfaced controller/parameter drops** — plain
      out-of-range volume/pan/master-volume/expression values now clamp to 127
      with an approximation notice (unevaluated `Rnd`/`Var` stand-ins keep
      dropping with a notice until suite stage 4), and a caller-side `bpm > 0`
      guard closes the vendored `libsmfcx.c` zero-BPM division UB. Corpus A/B
      byte-identical (82 archives, 257,097 files) — full narrative in
      [HISTORY.md](HISTORY.md#fixed-bugs).
- [x] **Fixed the two mis-wired vibrato/pitch controls** — `0xE3` sweep pitch
      no longer masquerades as CC78 (it drops with an honest notice until the
      stage-2 player can render pitch-bend ramps), and `0xE0` mod delay — s16
      in **5 ms units** per the NW4R decomp, correcting the triage's
      "milliseconds" — scales into CC78's relative upper half (64 = no delay,
      saturating at 1 s). A/B: 1,772 `.mid` diffs, every one CC78-only by
      independent SMF parse; out-of-range drops fell 1,020 → 230. Narrative in
      [HISTORY.md](HISTORY.md#fixed-bugs).
- [ ] **Pass finite loop repeat counts through.** `0xD4`/`0xFC` loop pairs are
      emitted as EMIDI CC116/CC117 but always with value 0 — which means
      *infinite* in that convention, so an EMIDI-aware player loops a
      "play 4×" section forever. One line: emit `0xD4`'s count argument as the
      CC116 value. (Fully unrolling repeats in the timeline shares the
      flattening machinery with suite stages 2/5; not required here.)
- [ ] **Fix the damper-pedal threshold bug (`0xDF`).** The argument is a bool
      (0/1) but the raw value goes to CC64, and 1 reads as *pedal off* on
      every GM/GS synth (the on/off threshold is 64) — so the pedal never
      engages and notes that should ring out are cut. Mirror the `0xCE`
      normalization (`? 127 : 0`); one line. Found + adversarially verified
      in the 2026-07-11 dropped-parameter triage
      ([HISTORY.md](HISTORY.md#investigations)).
- [ ] **Implement the two dropped commands with clean MIDI targets**:
      `0xDC` init_pan → CC10 (exact mapping; 8,438 drops across 46 archives)
      and `0xD8` lpf_cutoff → CC74 brightness (near-identity 0–127; 4,672
      drops). One line each, mirroring the existing pan / FX-send handlers.
- [ ] **Gate the vibrato CCs on mod type (`0xCC`).** The track LFO targets
      pitch, volume, or pan; caesar emits the pitch-vibrato CCs
      (CC1/76/77/78) unconditionally, so tremolo/auto-pan tracks render as
      pitch wobble on every GM synth. Track mod_type per track (default
      pitch) and suppress those CCs for types 1/2. 8,006 occurrences in 64
      archives.
- [ ] **Warning-hygiene pass over the drop sites** (census-ranked in
      [HISTORY.md](HISTORY.md#investigations)): demote `span` (55k
      occurrences, the #1 warning — the front/rear surround axis; now
      console-confirmed audible under the System-Settings *Surround* mode, but
      MIDI has no surround axis regardless — see the HISTORY addendum),
      `priority`,
      and `front bypass` to benign "no MIDI equivalent" notices; give the remaining drops default-visible
      notice categories (bare warnings are `-w`-only today); add a final
      `else` unknown-opcode notice (`0xDE` FxSendC currently vanishes
      silently); fix the extended-command warning chain being dead code
      (`cmd.Cmd` is never set past `0xF0`, so every `setvar`/`cmp`/mod2-4
      drop is silent — 353k `setvar` alone) and its scrambled mod4 labels;
      emit honest notices for `Rnd`/`Var`/`[If]`-prefixed commands the
      converter currently mangles silently.
- [ ] **Close the Time-suffix desync hazard.** A `_t` ramp command carries a
      trailing s16 duration that the parser only consumes for `0xB0–0xDF`;
      Time-suffixed tempo/sweep/notes/extended commands leave those 2 bytes
      unread and desync the rest of the track. Zero corpus occurrences (all
      ~473k observed `_t` commands sit in the safe range), but it is the one
      genuine wrong-arg-count hazard left. The ramps themselves flatten to
      instant jumps (375k volume fades) — full interpolation is stage-2/5
      flattening territory; a notice suffices for now.
- [ ] **Use the `Rnd` midpoint instead of the minimum.** Random-valued
      commands currently collapse to the range *minimum*, silently biasing
      196k volumes, 177k pitch bends, and 94k rest durations (timing!) low.
      Midpoint is the honest deterministic stand-in until real randomness
      lands with the VM. Output-changing for those sequences.
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

- **Sequence (CSEQ → MIDI) fidelity: the convert-time variable VM.** The
  2026-07-11 triage ([HISTORY.md](HISTORY.md#investigations)) quantified the
  gap: ~2.4M sequence events convert wrong or vanish silently corpus-wide,
  dominated by the un-evaluated variable machinery — 353k `setvar` / 210k
  `cmp` ops ignored, `[If]`-prefixed non-jump commands executing
  unconditionally (including 33k conditional `Return`s and 8.5k conditional
  `Fin`s that can truncate tracks in GardenSound/Alice/Jack/ctr_dash), and
  `Var`-valued parameters emitting the variable *index* as the value. The
  settled plan is a small deterministic VM in the converter: three variable
  scopes initialised to 0 (power-on hardware state), the 12 arithmetic ops,
  the 6 comparisons setting a per-track flag, `[If]` gating *every* command
  type, the existing revisit guard for backward jumps, and a fixed documented
  `randvar` value. That resolves sequence-internal `[If]`s bit-exactly,
  defaults game-driven globals to the same "default section" the current
  heuristic aims for, strictly supersedes the two-reachability heuristic, and
  is a direct down-payment on suite stage 4. (Semantics are pinned:
  Gota7/GotaSequenceLib `CtrCafe.cs` is the authoritative CTR byte map, plus
  the NW4R decomps — see the triage entry for sources.)
- **Mid-sequence bank switching (`0xB6`).** 8,778 bank selects across 41
  archives (WarioWare Gold's `SoundData1` alone has 4,585); dropping them
  plays the wrong instrument wherever a track switches banks. Not a local
  Cseq fix: the emitted CC0 must be co-designed with Cbnk's SF2 bank layout
  (currently derived from the flat `0x81` program index), or it fights the
  existing bank/program split.
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
      Same oracle method now also covers the **Surround-mode virtualization**
      (the DSP folds a quad per-voice gain matrix down to stereo using
      `surround_depth`/`rear_ratio`/speaker-position + two biquads,
      headphone-aware; Citra/Azahar have never implemented it — `span` and
      `front_bypass` feed this path; see the HISTORY 2026-07-11 addendum).
- [ ] **Stage 4 — exact variables/conditionals/random** + the NW4C
      `mod2/3/4` LFO curves (the one genuine engine unknown left).
- [ ] **Stage 5 — tracker export**: `.it` writer (`.mptm` one-flag upgrade).
- [ ] **Stage 6 — editor (write-back)**: smallest size-preserving edit first,
      proven on the New 3DS via LayeredFS.

Hardware-RE queue (New 3DS + CFW, feeds stages 2–3):

- [x] **Surround-mode A/B probe** (`tools/surround-probe/`) — **CONFIRMED on
      console 2026-07-11 (v2)**: the 3DS Surround output mode performs real
      front/back virtualization on the headphone jack (Stereo FL≡BL null at
      D=0.23 dB with a dead −85 dBFS opposite channel; Surround reshapes the
      FL-vs-BL spectrum by ~6 dB and lifts +46 dB of cross-channel energy the
      rig cannot produce; WIDE position raises it further; Mono collapses to
      L=R). Therefore `span` (0xD7) IS audible in Surround mode. Full write-up +
      metrics table in the HISTORY 2026-07-11 addendum.
- [ ] **Surround Part B — tie the opcode to the register** (hardware-RE,
      follow-up now that Part A is confirmed). Dump the live
      `SourceConfiguration.gain[3][4]` while a span-sweeping `.bcseq` plays via
      LayeredFS, binding `span`/`front_bypass` to the rear gain lanes at the
      source. Feeds suite stage 3's Surround virtualization model.

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

The remaining MIDI-converter discrepancies found in the 2026-07-10 post-release
audit (mis-wired `0xE3`/`0xE0`, discarded loop counts) and the 2026-07-11
dropped-parameter triage (damper threshold, init_pan/lpf, mod-type gating,
silent `Rnd`/`Var`/`[If]`/ramp handling) are scoped as work items under
**v0.5.1** above rather than listed here twice. (The GM drum-channel
collision, the controller-range drops, and the tempo-zero UB from those audits
are fixed.)

- **Unknown-opcode bytes are swallowed instead of failing fast.** `0x90`/`0x96`
  (2-byte `Analyse` probes guessed by the original author) and `0xB7–0xBC`
  (1-byte catch-all) are not real CTR opcodes — the map jumps `0xB6`→`0xBD` —
  so their presence would mean the parser already desynced upstream; consuming
  a guessed length silently perpetuates the desync. Zero corpus occurrences;
  should be `Common::Error` like other unknown bytes. Latent, not observed.
- **The `(v/2)+64` transform on CC72/73/75/76/77** (attack/decay/release,
  vibrato rate/depth) compresses the unsigned 0–127 args into 64–127 — caesar
  can never express "faster/shorter than default". Root cause: the parse phase
  types `0xD0/D1/D3` as *signed* under a mistaken model, so a fix must touch
  both phases together. Low priority: FluidSynth-class players ignore CC72–79
  entirely, so this is byte-level rather than audible wrongness.

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
- **`0xC9` portamento may under-serve the engine semantics** (unverified). On
  hardware the command sets the portamento start key *and* turns portamento
  on; caesar emits only CC84 (portamento control), never CC65. Whether that
  loses the glide depends on the synth's CC84 interpretation (Roland's
  one-shot portamento-control works without CC65; others may gate on it).
  Surfaced by the 2026-07-11 `0xE3` research (sweep pitch and portamento are
  independent, additive mechanisms). Verify CC84 semantics across target
  players before changing anything.
