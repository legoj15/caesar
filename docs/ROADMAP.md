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
  narratives, A/B evidence, investigation stories, fixed bugs, and the
  per-release records (v0.5.0's six workstreams, the v0.5.1 fidelity patch).
- [SUITE-DESIGN.md](SUITE-DESIGN.md) — the settled design for the tool suite:
  pipeline-rate decision, HLE-vs-oracle ruling, refactor plan, staged-plan
  rationale, risk register.
- [NW4C-disasm-handoff.md](NW4C-disasm-handoff.md) — deep reverse-engineering
  findings on the 3DS NW4C audio engine (addresses, evidence chains).
- [CHANGELOG.md](../CHANGELOG.md) — the user-facing change record, per release.

## Overarching goals

The plan is big; each goal is its own large step.

| # | Goal | Status |
|---|------|--------|
| 1 | Get the caesar MIDI/SF2 converter debugged to the best effort — fix the known bugs, apply the obvious UX fixes, ship it | ✅ Shipped: v0.5.0 (2026-07-10) and the v0.5.1 fidelity patch (2026-07-12). Everything still open converter-side is the next two sections |
| 2 | Rebrand the repo as **"caesar salad"**, a play on the mix of things this repo strives to bring | Not started |
| 3 | Make an honest, **accurate-to-console player** | Design settled ([SUITE-DESIGN.md](SUITE-DESIGN.md)); suite stages 0–4 below |
| 4 | Make said player play at a **higher fidelity than console** *(ultimate goal)* | After goal 3 — a genuinely second pipeline clocked at the output rate (see the Nyquist correction in the design doc) |
| 5 | Add **PC-compatible tracker export** (best format, or several) | Format chosen: `.it`, with `.mptm` as a one-flag upgrade; suite stage 5 |
| 6 | **Editor** *(low priority)* | Suite stage 6 — write-back, gated on the round-trip milestone |

## Remaining converter scope

The complete known backlog for the extraction/conversion tool (BCSAR →
MIDI/SF2/WAV). This list plus the [Known bugs](#known-bugs-open) below is
everything left on the converter; none of it is a prerequisite for starting
the suite — the biggest item is itself shared groundwork (ramp handling
overlaps the stage-2/5 timeline flattener), and the architecture bullet is
subsumed by suite stage 0. Rough priority order within each group.

### Sequence (CSEQ → MIDI) fidelity

- [x] **The convert-time variable VM** — shipped 2026-07-13: variables,
  comparisons and `[If]` execute with disasm-verified CTR semantics; full
  narrative (incl. the NW4C≠NW4R `[If] Fin` divergence, the init-0 two-init
  A/B, and the re-roll-loop escape rule) in [HISTORY.md](HISTORY.md#the-convert-time-variable-vm-2026-07-13).
- **Opt-in trigger-seed "preview" mode** (concept, from the VM verification):
  ~138 GardenSound-class game-triggered SEs are now honestly silent at rest —
  they poll a never-written variable the game seeds at runtime (each named by
  its read-before-write notice). If audible "what does this SE sound like
  when fired" previews are wanted, the correct vehicle is an explicit opt-in
  flag that seeds each read-before-write trigger variable to the first
  note-unlocking value per entry — never a change to the default VM
  semantics. Belongs naturally to the future player/editor (which will seed
  real game state), but could ship converter-side earlier.
- **Ramp synthesis — the `_t` family, `0xE3` sweep pitch, tie
  single-envelope.** The largest remaining fidelity mass (~462k
  flattened events: 375k volume fades, 76k pan sweeps, 10.7k pitch-bend
  ramps, plus 871 sweep-pitch commands and the tie re-attack approximation).
  The converter currently only *surfaces* these with notices; nothing is
  synthesized. MIDI *can* carry all of them as stepped CC / pitch-bend event
  streams, so the open decision is converter-side stepped emission vs leaving
  them to the stage-2/5 timeline flattener (sweep-pitch caveats: pitch bend
  is channel-global and collides with real `0xC4` bends, and bend-range
  interplay must be handled).
- **Convert-time-expressible drops, never implemented:** `0xB3` velocity
  range — the engine scales each note-on velocity by the latched range,
  directly expressible by scaling emitted velocities (zero corpus
  occurrences per the 2026-07-11 census, so latent-correctness only);
  `0xDD` track mute — judged fully doable in the one-pass walk (512
  occurrences, 2 archives); `0xFB` envelope reset — could re-emit CC72/73/75
  to their defaults (inert on FluidSynth-class players; low value); `0xDB`
  main (dry) send — decide whether it is genuinely "no MIDI equivalent"
  (likely: CC7 would clobber `0xC1` volume) and reword its "not implemented"
  notice accordingly.
- **Mid-sequence bank switching (`0xB6`).** 8,778 bank selects across 41
  archives (WarioWare Gold's `SoundData1` alone has 4,585); dropping them
  plays the wrong instrument wherever a track switches banks. Not a local
  Cseq fix: the emitted CC0 must be co-designed with Cbnk's SF2 bank layout
  (currently derived from the flat `0x81` program index), or it fights the
  existing bank/program split.

### Audio coverage

- **IMA-ADPCM (codec 3)** decodes to silence — surfaced by a default-visible
  notice, but the run still succeeds and writes silent `.wav`s; **CWSD
  wave-sound data** (most sound effects) is skipped entirely.
- **External dependencies (Mario Kart 7 class).** Resolve archives whose data
  lives in sibling `.bcgrp` group files — a known gap no maintained tool
  handles (the 28 empty `BNK_*` dirs in `ctr_dash`; surfaced by a
  default-visible notice).
- **External stream entries** (267 corpus-wide — the streamed BGM) point at
  sibling stream files and are skipped with a notice. Open decision: locate
  and convert them, or settle on "defer to vgmstream" (mature tools already
  handle standalone 3DS streams) and document that in the README.
- **INFX metadata chunks** (47 corpus-wide) are skipped with a notice; decide
  whether they carry anything worth extracting or record a settled skip.

### Packaging, docs & portability

- **Playback ergonomics (docs + optional `.rmi` export).** The per-bank output
  layout already auto-pairs in foobar2000's foo_midi with zero caesar changes —
  its directory-named-soundfont rule matches `BANK_X/BANK_X.sf2`, and it honors
  the emitted `loopStart`/`loopEnd` markers and EMIDI CC116/117 (marker-only) —
  so document that as the recommended listening setup (README "how to listen"
  section; FluidSynth engine = the project's reference synth). Falcosoft SFMP
  (the best per-file inspection GUI; honors *finite* CC116/117 counts) only
  auto-loads a literal `folder.sf2`, so consider an opt-in self-contained RMIDI
  (`.rmi`, embedded-SF2) export for sharing single songs outside the extraction
  tree — both players support embedded-SF2 RMI. (Surveyed 2026-07-12.)
- **macOS output verification.** CI proves macOS/Clang *builds*; a
  byte-identical output A/B on a real Mac has never been run (libc++ is the
  likeliest place a latent `std::filesystem`/float-formatting issue would
  surface). Needs a Mac with test archives.
- **Linux/macOS CMake presets.** Only a Windows/MSVC preset exists; CI and
  the Linux verification configure with the raw two-command incantation. A
  preset per OS makes local builds one command.
- **Synthetic corrupted-input fixtures for public CI** (idea, not started).
  CI is build-only on all three OSes today because the verification corpus is
  copyrighted and private. `tools/diag-goldens/` pins the failure-family
  behaviour, but its fixtures are mutated from corpus archives and stay local.
  A small set of **synthetic**, from-scratch (non-copyrighted) malformed
  `.bcsar` files — one per mechanism (bad magic/BOM/length, enum-default,
  bounds overrun/outside, empty file) — committed to the repo would let public
  CI assert the exit codes and family markers on every push, giving the
  diagnostics a behavioural smoke net without any corpus dependency.

### Longer horizon

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

- [x] **Stage 0 — library-core refactor: COMPLETE (2026-07-14).** All four
      steps shipped, every one output-identical: the `.wav` in-memory handoff,
      the `ParseContext` fold (globals → a reference-threaded context, plus
      per-input scoping), the per-class parser/exporter model split across all
      six classes, and the `caesar_core` static-library split — `caesar.cpp` is
      now just the CLI entry point linking `caesar_core`, and the suite's
      player/tracker/editor will link that same library. Flag parity across the
      target split was verified by `compile_commands.json` diff; the final split
      passed the full gate (warning-clean clean build, 18/18 diagnostics
      goldens, 257,125-file corpus A/B byte-identical). Full write-up in
      [HISTORY.md](HISTORY.md#suite-stage-0-complete--the-caesar_core-library-split-step-4-2026-07-14).
- [x] **Stage 1 — byte-identical round-trip of BCSEQ/BCBNK/BCSAR (2026-07-14).**
      All three deep formats re-serialise from the retained model byte-identically
      corpus-wide — **BCSEQ 20,791/20,791, BCBNK 11,136/11,136, BCSAR 82/82 whole
      archives** — so `caesar-roundtrip --verify` reaches exit 0 (opaque
      BCWAR/BCWAV/BCWSD/BCGRP children skip informationally). Six commits
      (scans/scaffold → BCSEQ → Cbnk split → BCBNK → the BCSAR container); `caesar`
      output-identical throughout. Container write-up (nesting re-point, the STRG/
      INFO/FILE layout facts, retention extensions) in
      [HISTORY.md](HISTORY.md#suite-stage-1-commit-4--csarserialize-the-bcsar-container-round-trip-serializer-2026-07-14).
    - [ ] **Optional capstone — deep child re-embed (commit 5):** re-lay-out
      deep-serialized CBNK/CSEQ children so the container consumes *computed* child
      lengths (the edit-safe property stage 6's write-back needs). Not required for
      the round-trip (which copies child blobs through verbatim at their original
      offsets); `Cbnk` would need to expose a relocation path, since it reconstructs
      bodies positionally from retained offsets.
    - Permanently opaque (settled, not a gap): BCWAR/BCWAV/BCWSD/BCGRP never
      re-encode — CWAV's DSP-ADPCM cannot round-trip — so they stay copy-through
      spans in the container and SKIPPED (informational) in the verifier.
- [ ] **Stage 2 — dry player**: native-rate voices, console interpolation,
      solved envelopes, priority voice stealing (RE'd and confirmed), tempo
      clock, single final upsample. Blueprinted 2026-07-14 (11 commits in four
      phases; first audible `.wav` at C3; `caesar_play` lib + `caesar-play`
      offline-render exe; golden-render net from the first `.wav`): full plan in
      [HISTORY.md](HISTORY.md#suite-stage-2--the-dry-player-blueprint-2026-07-14-survey).
      **Console-capture dependency for the final tolerance proof (Phase IV):
      the old music captures are gone — fresh New 3DS captures needed
      (`BGM_MAIN_Mii_Only_One`, per-instrument isolated notes, the MeetSound
      SEs); ask the user when Phase III nears completion.** The one flagged
      unknown: the console interpolation filter (ship linear; recover the
      truth via the offline teakra oracle later).
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

- [x] **Surround-mode A/B probe** (`tools/surround-probe/`) — console-confirmed
      2026-07-11: `span` (0xD7) IS audible in Surround mode; write-up in the
      HISTORY 2026-07-11 addendum.
- [ ] **Surround Part B — tie the opcode to the register** (hardware-RE,
      follow-up now that Part A is confirmed). Dump the live
      `SourceConfiguration.gain[3][4]` while a span-sweeping `.bcseq` plays via
      LayeredFS, binding `span`/`front_bypass` to the rear gain lanes at the
      source. Feeds suite stage 3's Surround virtualization model.
- [ ] **Decay-table console spot-check.** The corrected decay values
      (2026-07-08 fix) have never been exercised against a console capture —
      the original repro track was release-127-sentinel and untouched by the
      fix. One A/B on a decay-table-using track closes the loop; also
      grounds stage 2's envelope solver.

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
[HISTORY.md](HISTORY.md#fixed-bugs). The one large-mass fidelity gap left
(ramp flattening) is tracked under
[Remaining converter scope](#remaining-converter-scope) above, not here — this
list is the known defect tail.

- **Bank/WARC/GROUP naming shares the symbol-collision hazard structurally.**
  The sequence-side collision fix (2026-07-12) covers `.bcseq`/`.mid` only;
  bank, wave-archive and group outputs are still named from the symbol alone
  with no id disambiguation. Banks have no per-entry offset, so it takes two
  INFO entries sharing one symbol — unmeasured in the corpus. Census before
  extending the suffix scheme. (2026-07-13: the in-memory handoff removed the
  worst downstream symptom — bank SF2s no longer read cross-contaminated
  samples when two WARCs collide on one directory; the on-disk `.wav`
  overwrite itself remains.)
- **The `(v/2)+64` transform on CC72/73/75/76/77** (attack/decay/release,
  vibrato rate/depth) compresses the unsigned 0–127 args into 64–127 — caesar
  can never express "faster/shorter than default". Root cause: the parse phase
  types `0xD0/D1/D3` as *signed* under a mistaken model, so a fix must touch
  both phases together. Low priority: FluidSynth-class players ignore CC72–79
  entirely, so this is byte-level rather than audible wrongness. (NW4R note
  for whoever recalibrates these: the engine's audible vibrato width is the
  *product* of `0xCA` depth and `0xCD` range — range, default 1, is a raw
  multiplier — so the width term largely lives in the CC77 caesar writes to
  an inert controller, while CC1 alone drives what players actually render.)
  (Audit refinements 2026-07-12: the vibrato pair `0xCB`/`0xCD` already
  parses unsigned, so its fix is emit-only; `0xD2` sustain level shares the
  signed mis-typing and should be corrected in the same pass; and removing
  the transform needs a calibration decision — GM2's CC72/73/75 are
  relative-to-64 while the engine args are absolute rates, so raw
  pass-through is "less wrong", not right.)

- **`0xFE` track-enable mask is parsed but never enforced.** Tracks opened by
  `0x88` convert regardless of the allocation mask; if the engine gates
  OpenTrack on allocation (unverified), caesar renders tracks the console
  never plays. Zero observed impact on the corpus.
- **A malformed `0x88` OpenTrack offset off a command boundary silently ends
  the whole track walk** — the lookup misses, the loop exits, and every
  remaining track is skipped with no notice (the start-offset and jump-target
  fallbacks are noticed; this path is not). Malformed-input edge only.
- **A borrowed span-child's out-of-bounds read can fall through to the parent
  range.** Since the embedded-child span construction (Cwar/Cwav, then
  Cbnk/Cseq/Cgrp — all borrows; 2026-07-13), a borrowed child's `CheckBounds`
  range is a sub-range of the parent's still-registered range; a read landing
  entirely past the child's declared length — reachable only on a
  malformed/truncated embedded file — used to throw "points outside the loaded
  data" and now silently succeeds if it stays inside the parent's buffer.
  Corpus-invisible (a well-formed child never addresses past its own length; the
  A/B would have caught any flip). Fix, if it ever matters: give `CheckBounds`
  per-child range scoping, or copy that path.
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
- **`-w` positional warning output is heap-layout nondeterministic — `Cwav`/
  `Cbnk`/`Cgrp`/`Csar` sites (the Cseq slice is fixed).** caesar computes a
  warning's `AT POSITION` as `pos - Offsets.top()`; when `pos` and the
  top-of-stack buffer base come from *different* heap allocations (any archive
  that warns during wave/bank decode, or a group's `Skipping INFX`/`CWSD` and
  external-group warnings — empirically **broader** than the group-resident
  conversions first suspected), the subtraction is garbage that shifts run-to-run
  with the heap layout. Affects only the `-w` per-item `AT POSITION` line; the
  default-visible notice summary and every output file are unaffected, so it is
  byte-level rather than audible. **The Cseq slice is fixed** (2026-07-14,
  model/exporter commit 4): the emit walk now locates each warning from a stored
  command offset via the new `ParseContext::Warning(uint32_t, …)` overload, so a
  group-resident sequence would print the true in-file offset. That fix is
  **latent on this corpus** — a whole-corpus scan found every sequence converts
  direct off the `Csar` (none via an embedded group), where the stored offset
  already equals the subtraction, so the corpus `-w` bytes are unchanged. The
  real fix for the remaining sites is the same shape (carry each warning's offset
  relative to its own buffer; the overload is ready to adopt) — a stage-1
  drop-the-buffer prerequisite. A **separate residue this did NOT touch:** the
  `WARNING IN <file>` line also reads the shared stack top (`FileNames.top()`),
  so a deferred/group-path warning still names the wrong file — but
  *deterministically* wrong, not heap-nondeterministic. Pinned empirically by
  `tools/diag-goldens` (its `-w` golden set is chosen by a twice-run
  byte-identity filter; `w-dlplay` now covers the direct-path Cseq `-w` surface).
  *(Was filed alongside the multi-input `.log` bleed, which was a lifetime bug
  and is now fixed — 2026-07-13, HISTORY.)*
- **Group WARC id collisions leak the overwritten object.** `Cgrp` inserts its
  wave archives into the shared map by plain assignment; an id already present
  (a direct WARC's or another group's) is overwritten without being freed.
  Pre-existing and unobserved on the corpus; noted 2026-07-13 because retained
  decoded PCM now makes each leaked object materially heavier.
