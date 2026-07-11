# caesar engineering history

What shipped, in full detail — the verification narratives, A/B evidence, and
investigation stories behind every completed roadmap item. This file exists so
[ROADMAP.md](ROADMAP.md) can stay a short statement of what is open: when an
item ships, its checkbox flips there and the full write-up is appended here.
Settled design for the tool suite lives in [SUITE-DESIGN.md](SUITE-DESIGN.md);
deep reverse-engineering findings (addresses, evidence chains) in
[NW4C-disasm-handoff.md](NW4C-disasm-handoff.md).

Entries are grouped by the v0.5.0 roadmap section they closed out, in the
original roadmap order, followed by fixed bugs.

---

## v0.5.0 §1 — Modern build system

A single CMake build that works on current (2026) toolchains.

- [x] Consolidate on CMake (CMake 3.21+, C++17); drop the hand-written Visual
      Studio solution/project and the dead AppVeyor config.
- [x] Build the two vendored libraries (`libsmfc`, `sf2cute`) as static libs.
- [x] Add `CMakePresets.json` for one-click Windows/MSVC builds.
- [x] Repo hygiene: slim `.gitignore`, fix `.gitattributes`, document building.
- [x] Verify the build on Linux (2026-07-10). macOS output verification remains
      open — tracked in ROADMAP.md.

      **Linux — ✅ DONE (end-to-end, output A/B).** On a Debian 13 (trixie) VM,
      GCC 14.2.0 / CMake 3.31.6, configured with a plain `cmake -S . -B build
      -DCMAKE_BUILD_TYPE=Release` (no preset — the only preset is Windows-gated;
      Ninja absent so the default Make generator is used) and built in ~17 s. The
      clone→configure→build→extract chain ran clean. Extracted three archives
      spanning the main code paths — `menu` (no groups, 331 files), `MeetSound`
      (banked instruments + release-127 notices, 1074), `ctr_dash` (MK7, embedded
      **groups** via `Cgrp`, 4595) — identical file counts to Windows. **A/B vs a
      Windows/MSVC build of the identical commit (`4c451e4`): every `.wav` (1198),
      `.mid` (1721), `.sf2` (27) and every raw sub-file dump
      (`.bcbnk`/`.bcgrp`/`.bcseq`/`.bcwar`/`.bcwav`) is byte-identical across
      platforms.** The SF2 envelope float math (`log`/`pow`/`lround`) matched to
      the bit — no glibc-vs-MSVC-CRT libm divergence, no FP-contraction or
      endianness bug. The *only* cross-platform differences were in the human-
      readable `.log` (the analysis CSV): its first column echoes the input path
      as spelled on the command line (I gave different paths per box), and it is
      written in text mode so line endings are CRLF vs LF — both expected; after
      normalising those two, the logs hash-match exactly. Note: GCC surfaced two
      `-Wmaybe-uninitialized` that MSVC hid — `Cwav.cpp` default-constructed
      `CwavChan chan;` and `Cbnk.cpp` `CbnkInst inst;` (not `{}`), copying
      uninitialised members into the channel/instrument vectors. Harmless in
      practice (the fields are overwritten before use, hence the byte-identical
      output), but real UB; **both fixed** to `chan{}` / `inst{}` in the v0.5.0
      hardening pass. **macOS/Clang (libc++) remains untested** — its libc++
      (vs libstdc++) is the most likely place a latent `std::filesystem` / float-
      formatting / `memcpy` issue would surface. Still no Linux/macOS CMake preset
      (CI can configure without one, as done here).

## v0.5.0 §2 — Robustness

A tool whose job is parsing files ripped from game images should treat malformed
or unusual input as normal, not exceptional. Previously, bad input crashed with no
explanation, and some failures corrupted output silently.

- [x] Check that every input file actually opened before using its size — a
      missing, empty, or unreadable path previously triggered a giant bogus
      allocation and crashed. (`Common::RequireOpen`, all load sites.)
- [x] Wrap each input in top-level error handling so one bad file reports
      cleanly and the rest still process (previously the first failure aborted
      the whole run), so multi-archive runs (`caesar a.bcsar b.bcsar`) work.
      (The between-inputs working-directory restore this originally added was
      later removed as unnecessary — see the composed-output-paths item below,
      which eliminates the working directory from extraction entirely.)
- [x] Add a bounds-checked reader so a truncated or corrupt file reports
      "archive damaged at offset X" instead of reading past the buffer. Every
      read through `ReadFixLen`/`ReadVarLen` is checked against whichever loaded
      buffer the position falls in. Verified behavior-preserving by diffing
      13,843 output files across 31 real archives against the pre-change build
      (byte-identical), and that truncated/corrupt inputs now fail cleanly.
- [x] Bounds-check the remaining bulk reads that bypass the readers (name/label
      string construction, the null-terminated location scan, and every raw
      sub-file dump), so a corrupt length or offset can no longer over-read.
      Verified byte-identical output on real archives.
- [x] Replace the change-working-directory extraction model with composed output
      paths (enables an `--output-dir` option and removes remaining failure-path
      and parallelism limits). Extraction no longer calls `current_path`/`chdir`
      at all: each level composes a full output path (`archiveDir / <name> / …`)
      and threads the destination through `Cwar`/`Cbnk`/`Cseq`/`Cwav`/`Cgrp` via
      each object's own full dump-file path — so, e.g., `Cbnk` now locates its
      `.wav`s from the wave-archive's stored path instead of a relative `..` hop,
      and its `cwarPath` argument is gone. Added `-o <dir>` / `--output-dir <dir>`;
      the default layout (beside the input) is unchanged. With no global
      working-directory state, one invocation can process many archives and a
      malformed one is isolated without a cwd reset, and inputs given with a path
      prefix now extract correctly (the old model mis-placed the `.log` for those).
      Display/log/embedded names are still bare basenames, so the SF2 `INAM`, the
      `.log`, and console progress are unchanged. Verified byte-identical
      old-vs-new over **24,928 output files across 6 real archives** (MK7
      `ctr_dash` incl. internal groups, MiiPlaza `MeetSound` [+0x60 layout] and
      `mgCar`, Animal `GardenSound`, eShop `TigerSound` [+0x54 layout], System
      `menu`); `-o` output matches the default byte-for-byte except the `.log`
      (which records the input path as spelled, as it always did).
- [x] Sanitize archive-supplied names before using them as file/dir names —
      illegal characters (including path separators, which could otherwise
      escape the output folder) are replaced. Done in the same `TypedName`
      choke-point; a no-op on normal names, so valid output is unchanged.

## v0.5.0 §3 — Surface what's being dropped

By default (without `-w`), the tool printed no warnings at all — so a normal run
reported success while silently omitting sound effects, whole-song loops, an
entire audio codec, and more. Users couldn't tell what they didn't get.

- [x] Promote "content was skipped / approximated" notices so they show by
      default; keep `-w` for verbose per-command detail. A normal run now prints a
      compact per-input summary of what it dropped — e.g. `41 CWSD wave-sound
      blocks skipped (sound effects not extracted)`, `267 external streams
      skipped`, `47 INFX metadata chunks skipped`, IMA-ADPCM waves left silent, and
      instrument notes pointing at a missing sample. Implemented as a second tier
      on `Common::Warning`: an optional `noticeCategory` argument tallies the site
      into `Common::Notices` (a `map<category,count>`) regardless of `-w`, while the
      existing verbose positional line still only prints under `-w`; sites with no
      category (all the per-command `Cseq` "…not implemented" spam) stay `-w`-only
      exactly as before. `Common::FlushNotices` prints and clears the tally once per
      top-level input (even if that input failed partway, so anything dropped before
      the failure is still surfaced). The five content-drop sites tagged: CWSD in
      `Cgrp`/`Csar` (merged into one count), external streams and INFX in
      `Csar`/`Cgrp`, IMA-ADPCM in `Cwav`, and the out-of-range CWAV reference in
      `Cbnk`. Verified byte-identical extraction (stderr-only change): 19,768 output
      files across 5 archives (menu, MeetSound, TigerSound, cardboard, GardenSound)
      diff clean old-vs-new; the old binary prints nothing by default while the new
      one emits the summaries, and `-w` still shows all 471 per-item warnings on
      `menu` plus the summary.
- [x] Check the MIDI-writer's return values (it silently rejected out-of-range
      events, so notes/program-changes/controllers vanished with no warning). Every
      value-bearing `smfInsert*` in `Cseq::Convert` is now checked: two file-local
      helpers `emitCtrl`/`emitProgram` surface a rejected event as a default-visible
      notice (grouped — "MIDI program changes dropped", "MIDI control/parameter
      events dropped") plus `-w` positional detail, reusing bullet 1's
      `Common::Warning` notice tier. The note site is checked inline and flags only a
      genuinely out-of-range velocity (> 127), not a velocity-0 rest (which the
      writer legitimately skips). Inserts whose value is a constant, clamped, or a
      provably in-range expression (`Int8 * 64` pitch bend, `Int8/2 + 64`
      attack/decay/release) are still emitted raw. An adversarial multi-agent audit
      (14 agents) confirmed the instrumentation and turned up the fixes below.

      **Surfacing exposed a real pre-existing bug — banked instruments were
      unreachable (fixed end-to-end).** A `.cbnk` can hold more than 128 instruments,
      but caesar numbered every SF2 preset by its raw instrument index in a single
      bank, while a MIDI program change only addresses 0-127 — so any instrument at
      index ≥ 128 could not be selected at all. The `0x81` program change passed the
      raw index to `smfInsertProgram`, which rejected it (> 127) and wrote nothing, so
      the voice was dropped and the track fell back to the wrong/default instrument —
      **6,422 program changes across the 82-archive corpus** (up to 2,038 in one).
      The complete fix makes the soundfont and the sequence agree on a bank split:
      `Cbnk` now places instrument *i* at (bank *i*/128, preset *i*%128), and `Cseq`
      selects it with a bank select + masked program (bank *i*/128, program *i*%128).
      The bank goes in the **MSB** control (CC0), because the common SF2 players —
      FluidSynth's default GS mode, and GM — take the SF2 bank from CC0 and ignore the
      LSB; a first LSB-only attempt selected bank 0 and played the wrong instrument on
      those players (only XG/MMA read the LSB). Unbanked voices (bank 0) emit exactly
      as before. The two bank-select controls are checked (`emitCtrl`) too, since a
      `Rnd`/`Var` prefix can make `Args[0]` negative.

      Verified. The surfacing half is stderr-only — **byte-identical extraction over
      40,941 files** (the pre-bullet-2 build reported nothing; the new build emits the
      summaries; `-w` still prints every per-item warning). The banked-instrument half
      changes SF2 + MIDI output but is surgically confined: over a 10-archive A/B vs
      the original, only **1,192 `.mid` (sequences that reference a banked instrument)
      and 6 `.sf2` (the only banks with > 127 instruments) differ; every `.wav`/`.log`/
      raw dump is byte-identical**, and within a changed SF2 only the `phdr`
      preset/bank numbers move (sample PCM byte-identical, file size unchanged). In
      FluidSynth's default mode the affected Mii Plaza SEs now play the intended
      instrument (they differ from the old dropped-default render). **Console-validated
      on the New 3DS (2026-07-08):** A/B of a six-effect MeetSound SE pack (staged at
      `…/3DSWii Dumps/caesar_AB_MiiPlaza`) confirmed the pre-fix renders all collapse
      onto the same wrong fallback patch while the fixed renders play the intended,
      distinct instruments and match the console. Five of six were confirmed
      positively by ear; the sixth (`SE_LEGEND_ENEMY_HEAL`) was subtle-by-design and
      inconclusive only because two of its three instrument layers already used
      in-range programs that never dropped, so the fix changes just one layer there.
      Caveat: banked voices resolve in GS/GM players; an XG/MMA-mode player reads the
      bank from the LSB and would need that convention instead (documented in
      `Cseq.cpp`). Remaining surfaced drops: ~1,020
      control/parameter events corpus-wide (8-bit values > 127, or `Int16`
      vibrato-delay/tempo) — now *visible* rather than silent; some likely want 8-bit →
      7-bit scaling instead of dropping, tracked as a follow-up fidelity item
      ("Sequence fidelity" in ROADMAP.md).

## v0.5.0 §4 — High-value fidelity & UX wins

The cheapest changes with the most audible or visible payoff.

- [x] Give numeric output names a type prefix so numbered items are
      identifiable and can't collide. Symbol-table names (e.g. `BANK_CTR_COMMON`)
      were already used where present; now items the archive leaves unnamed —
      including everything inside groups, which had no naming at all — become
      `BANK_206`, `WARC_390`, `SEQ_…` instead of bare numbers (issue #17's
      `206.sf2` → `BANK_206.sf2`). Verified content-preserving over real
      archives (only the SF2 embedded name and the `.log` change, as intended).
- [x] Deeper naming: groups keep symbol names too. A group's own file table
      carries only a numeric id for each bank/wave-archive/sequence, so
      group-resident items were extracted under a number (`BANK_206`) even though
      the CSAR INFO section already names the same file by its shared id. Fixed by
      building a `namesById` map (file id → the type-prefixed name resolved at the
      CSAR level, for every enumerated CWAR/CBNK/CSEQ) in `Csar::Extract` and
      threading it into `Cgrp` (new ctor arg + `NamesFromCsar` member); a
      `nameFor(id, type)` helper in `Cgrp::Extract` prefers the CSAR name and falls
      back to the old numeric `TypedName` only for ids the CSAR did not enumerate.
      So `BANK_206` now extracts as `BNK_SELECT_SINGLE_MULTI_G` — into the very
      directory the CSAR level had already created for it (which also holds the
      sequence that references the bank, `SEQ_MENU_SINGLE_MULTI`), instead of a
      numeric duplicate beside it. Bank/sequence are now co-located under one
      meaningful name.

      **Correction to the original framing.** This item claimed the "28 of 85 empty
      named dirs" in MK7 `ctr_dash` *were* the numerically-duplicated banks. They
      are not — they are two disjoint sets. The numeric duplicates were a separate
      **19** banks (`BANK_206`–`BANK_249`), whose data lives in the archive's
      **embedded** group; those are what this fix relocates. The **28** empty
      `BNK_*` dirs are banks whose data lives in an **external** sibling `.bcgrp`
      that caesar does not load yet — there is no data to put in them, so they stay
      empty and are untouched by this change. Filling them is the external-`.bcgrp`
      group work (still open — tracked in ROADMAP.md).

      Verified old-vs-new on `ctr_dash` (all group banks): the 19 numeric `BANK_*`
      dirs are gone, the 28 external empties are unchanged, and content is
      preserved — the `.wav` (864), `.mid` (1369), `.bcbnk` (19), `.bcseq` (1369)
      and `.bcwar` (44) content multisets are byte-identical old-vs-new, and the
      `.log` is byte-identical. The 19 `.sf2` differ **only in the embedded name**:
      parsing each pair's RIFF chunks shows the sample audio (`sdta`) and all
      preset/instrument/generator/sample data (`pdta`) byte-identical for all 19,
      with only the `INFO`/`INAM` string changed (the SF2 name comes from the bank
      file's stem, `Cbnk.cpp:482`). A group-free archive (`menu`) is fully
      byte-identical old-vs-new (330 files), confirming zero collateral effect (the
      change only runs inside `Cgrp`, which is constructed only when an archive has
      groups).
- [x] Implement whole-song loops. The sequence "jump" (`0x89`) command now ends a
      track at its loop-back and writes `loopStart`/`loopEnd` marker meta events
      spanning the looped region — honored by loop-aware players (e.g.
      foobar2000's foo_midi) and shown on the timeline by DAWs. The loop start is
      placed at the tick its target first played, so an intro before the loop is
      preserved (verified: the marker lands exactly on the sequence's own
      `..._LoopStart` label). A jump into not-yet-played code — a shared block
      several tracks reuse — is instead followed as a goto, so those tracks keep
      their notes. Verified across 5 real archives
      (~8,000 MIDIs): jump-free sequences stay byte-identical, BGM/`SEQ` music
      gains correct loop markers, sound effects don't, and nothing hangs.
- [x] Resolve note-less conditional (`[If]`) jump dispatchers. Some sequences
      build a whole track out of nothing but conditional jumps — the track body
      is a dispatcher and every note lives behind an `[If]` branch keyed on a
      runtime variable — so skipping the branches (the previous behaviour) wrote a
      silent MIDI. The variable can't be modelled statically, but when a track
      would otherwise emit no notes at all, the converter now follows the first
      branch that actually reaches a note (the default `variable == 0` section the
      engine would pick), even through a chain of nested `[If]` jumps. It only
      fires while the track is still silent, so any track that already produces
      sound is byte-for-byte unchanged, and it never removes a note. This also
      fixed a latent crash-class bug: the walk's jump/call redirect used
      `--iterator` on the first command, which is undefined behaviour when a
      target is offset 0 (exactly where these note blocks sit) — replaced with an
      explicit redirect flag. Verified old-vs-new across 8 archives (~12,300
      MIDIs): games with no dispatchers (Zelda ALBW, Kirby) stay 100%
      byte-identical, 909 previously-silent sequences gained notes (812 in
      GardenSound alone, incl. full multi-track songs), the only sequences still
      silent are genuinely note-less (`dummy_seq`, `SE_SYS_SILENT`, control-only
      `*_CTRL_*`/`*_GVAR` setters), zero notes were lost, and nothing hangs.
- [x] Fix the typo in the envelope decay-rate table in `Cbnk.cpp`, which made
      some instruments fade out ~10x too fast. It was not one typo but a
      systematic decimal-shift error on **eight** entries. The `DecayTable`
      (indexed by the decay/release parameter byte, shared by `ConvertDecay` and
      `ConvertRelease`) follows the exact curve `-1.2 / (126 - index)` over
      indices 50-126; the eight "round-number" tail entries had all been typed
      10x too large (decimal shifted one place right): indices 114, 120, 121,
      122, 123, 124, 125, 126 read `-1, -2, -2.4, -3, -4, -6, -12, -24` but sit
      on the curve at `-0.1, -0.2, -0.24, -0.3, -0.4, -0.6, -1.2, -2.4`. A rate
      10x too steep makes the decay/release time ~10x too short (≈3986 timecents
      early), so sustained/pad instruments collapse to near-silence. The single
      curve fits ~68 untouched neighbours to 5 decimals, and index 126 (at the
      curve's pole) is pinned to `-2.4` because leaving it at `-24` after fixing
      125→`-1.2` would introduce a fresh 20x discontinuity. Bug is upstream
      (traces to commit e99708a, 2019). Verified by cross-checking the curve fit
      three ways (independent derivation, adversarial refutation, external
      DS/3DS shape reference) and a clean Release rebuild. Confirmed in the
      output: regenerating MeetSound `BANK_BGM.sf2` shifts exactly 99 release
      generators by +3986 timecents (10x longer), no other bytes. Note the
      original repro, `BGM_DEN_EMPTY_LANDSCAPE`, is *not* affected by this fix —
      its four instruments use release `127` (the instant sentinel, special-cased
      before the table), so old/new render bit-identical; that track's wrongness
      was the release-127 cutoff and dropped reverb (both below), not decay.
      Console A/B still worthwhile for the many tracks that do use the table.
- [x] Emit the per-note **tune** field, previously dropped. `Cbnk.cpp`'s `Note 0x24`
      (velocity-region `+0x14`) is an `f32` pitch ratio that caesar read and discarded —
      it was not even stored in `CbnkNote`. It is `1.0` for ~99 % of notes, but real
      detunes occur (`0x3F7B9A21` = 0.9828 = −30 cents; `0x3F811D26` = 1.0087 = +15
      cents). Now stored as `CbnkNote::Tune` (raw bits `memcpy`'d to `float`) and emitted
      via a new `ConvertTune` (`cents = 1200·log2(tune)`) as SF2 `kCoarseTune` (semitone)
      + `kFineTune` (cent), split so the fine part stays within ±50 cents and the
      remainder becomes whole semitones. A note at exactly 1.0 (the >99 % case) yields
      0/0 and adds no generator; sf2cute sorts generators into spec order on write, so the
      new tune generators never disturb the byte layout of the untouched notes. A corrupt
      bank's tune ≤ 0 / NaN is guarded (non-finite cents → no detune) so `lround()` stays
      defined. The logic lives entirely in `Cbnk::Convert`, so it covers both direct-CSAR
      and group-resident banks with no second code path.

      **Verified** by A/B against the pre-change build over **19 archives (~74,500 output
      files)**: every `.wav`, `.mid`, `.log`, and raw sub-file dump (`.bcbnk`/`.bcgrp`/
      `.bcseq`/`.bcwar`/`.bcwav`) is byte-identical, and **only 24 of 1,222 `.sf2` differ**
      — each solely by inserted `kCoarseTune`/`kFineTune` generators (sample audio `sdta`
      byte-identical), 569 tune generators total. The sparsity (only detuned notes change;
      24 of 1,222 banks) independently confirms the field offset — a misread would perturb
      nearly every note. Both cited example bytes reproduce exactly in the output:
      `0x3F811D26` → MeetSound `BANK_BGM` `fineTune 15`; `0x3F7B9A21` → GardenSound
      `BANK_BGM_DJ_FAMICOM` `fineTune −30` (the roadmap's earlier "−31" was a loose
      approximation; precise value is −30.0). The shared Kirby `B_ST_FGM` bank carries the
      same ~25 detunes across all four Kirby titles tested. Remaining nuance: the `0x28`
      interpolation byte is still read-and-dropped (fine — SF2 playback is always
      interpolated). Console A/B not required: the change only *adds* pitch data the engine
      itself applies, and the two ground-truth byte patterns round-trip exactly.

      This was the *only* discarded per-note field carrying lost musical intent: the three
      4-byte fields at `Note 0x2C`/`0x30`/`0x34`, long suspected of holding LFO / graph-
      envelope / randomizer tables, were resolved (2026-07-09) to be the self-referential
      `DataRef` chain that points at the ADSHR envelope caesar already reads at `0x38`
      (`0x20` → ref; type `0`; offset `8`; `0x30 + 8 = 0x38`). They are invariant across
      1,628 notes in 57 banks and three engines, and confirmed against the voice-setup
      code at vaddr `0x192390`–`0x1923C8`. Nothing to recover there — the `Analyse` labels
      could be renamed to "ADSHR ref" to stop them looking like an open question. The
      `0x28` interpolation byte is read but never emitted; that is fine, SF2 playback is
      always interpolated.
- [x] Map the sequence FX-send commands to reverb (CC91) / chorus (CC93). The
      converter dropped `0xD9` (fx send a) and `0xDA` (fx send b) with a
      "not implemented" warning, so extracted MIDIs were bone-dry even where the
      game routed tracks through the DSP reverb/chorus — one reason
      `BGM_DEN_EMPTY_LANDSCAPE` sounded wrong vs console (525 reverb-sends dropped
      across MeetSound; 4 on that track alone), though the bigger cause on that
      track was the release-127 cutoff (below). Now `0xD9`→CC91 reverb depth and
      `0xDA`→CC93 chorus depth, passing the 7-bit send level through (observed
      0-120; clamped 0-127 so the MIDI writer can't silently drop it). Verified:
      the track's MIDI now carries 4 `CC91=60` events (was 0), and a reverb-on
      FluidSynth render differs from the old dry render by 11% RMS. Remaining
      unmapped: `0xDB` main send and the `0xD2` envelope-sustain override (no
      clean MIDI CC equivalent).
- [x] Fix envelope release `127` being treated as instant. `ConvertRelease`
      special-cased `release == 127` to `-12000` timecents (~1 ms), chopping any
      such voice dead at note-off. That is backwards: `127` is a *long*-release
      sentinel (the 0-126 table already spans fast→slow; nothing uses `127` for
      an instant stop), and on 3DS these voices ring out for seconds. This — more
      than the dropped reverb — is why `BGM_DEN_EMPTY_LANDSCAPE` (whose four
      instruments all read `A=D=S=R=127`) sounded sharply cut off. Now mapped to
      ~3.5 s (`ConvertTime(3.5)`), calibrated by A/B of patched soundfonts against
      a Citra HLE capture; the four pads went from `-12000` to `~2168` tc and the
      render matches the chosen reference to floating-point noise. **Validated
      against a real console capture** (2026-07-08, 192 kHz line-in off the CFW
      New 3DS, `EMPTY_LANDSCAPE_console.wav`): the fixed render's loudness
      distribution matches console (median ~-8 dB below peak, same ~18 dB dynamic
      range, neither ever below -30 dB — the instant-cutoff signature is gone).
      The exact release *time* is not measurable from that dense track (a voice is
      essentially always sounding, so its aggregate envelope is near-identical for
      1 ms vs 5 s release). So the value was pinned from a *second* console capture
      (2026-07-08) of `BGM_MAIN_Mii_Only_One`, which has a 1.61 s all-voices-off
      gap at ~51 s where sentinel voices ring out exposed. **A least-squares fit of
      that gap's decay against a rendered release grid (aligned at the -25 dB
      crossing, over the reliable -26…-43 dB window above the recording's -49 dB
      noise floor) minimises at 3.5 s** (RMS 4.5 dB; 4.0 s→7.0, 5.0 s→12.3).
      **BUT this is now DISPUTED — 3.5 s likely over-blends and is too long.** A
      controlled busy-section comparison of the *same* console capture (dense
      passage, only release varied between renders) shows console peak-to-trough
      articulation = 11.1 dB vs renders 0.3 s→9.4, 1.0 s→6.5, 3.5 s→5.1: console
      sits at the SHORT-release end (polyphony/voice-stealing ruled out). Since all
      34 sentinel programs are identical full-sustain/instant-decay voices, the gap
      note == the busy pads, so they share one release; if busy is short, the gap's
      long tail is **DSP reverb, not release**. Reading: the note release is short
      and the ring is reverb (already emitted as CC91) — `ConvertTime(3.5)` fakes
      reverb with note-sustain, OK for sparse pads (both the gap fit and
      EMPTY_LANDSCAPE confound reverb+release) but wrong for busy tracks.

      **RESOLVED BY DISASSEMBLY (2026-07-09): `127` = instant, NOT long — the dispute
      is settled against 3.5 s.** The `code.bin` RE is done (see
      `NW4C-disasm-handoff.md`). The NW4C rate-conversion routine (Mii Plaza `code.bin`
      @ vaddr `0x201D60` decay / `0x201E3C` release) is byte-for-byte the Wii NW4R
      `EnvGenerator::CalcRelease`, including `if (x == 127) return 65535.0f;` — 65535 is
      the *fastest* per-ms rate, i.e. an effectively instant cutoff. So byte 127 is the
      instant/fastest sentinel at the driver level (same as DS/Wii), and the seconds-long
      tail is **DSP reverb**, not note release.

      **✅ APPLIED (2026-07-09, session 3). Default is now instant; `--pad-sustain` keeps
      the old behaviour opt-in.** The listening A/B that gated the flip was finally run
      properly (instant + CC91 reverb, which had never been rendered — every prior test
      compared instant *dry*). Findings, on `BGM_MAIN_Mii_Only_One`'s 1.62 s
      all-voices-off gap at 50.90 s:
        - Instant release sounds **console-correct** to the user, both dry and with reverb.
          The long tail the 3.5 s value was inventing simply is not in the console output
          the way `ConvertTime(3.5)` renders it.
        - A GM player's reverb cannot stand in for the DSP's. At the game's own CC91=60
          send, FluidSynth's freeverb contributes ~1 dB and leaves the gap silent
          (−85 dB at +0.5 s vs the 3.5 s render's −36 dB). Boosting it (room 0.85) only
          reaches −54 dB and, per the user's ear, "sounds like a real room — the 3DS's
          reverb goes in a different direction". **Do not try to compensate by raising the
          send level or the player's reverb; the shape is wrong, not the level.**
        - **`BGM_DEN_EMPTY_LANDSCAPE` cannot settle this question and never could** — it is
          dense enough that a voice is always sounding, so instant and 3.5 s differ only
          marginally (peak −8.8 vs −8.1 dB, no near-silent frames either way). Months of
          argument used the one repro that does not discriminate. `Only_One` is the
          discriminating track because it has the only exposed tail. The user accepts that
          EMPTY_LANDSCAPE's pads now read short — they *are* short, and the console's
          sustain there is carried heavily by DSP processing that a soundfont cannot encode.
        - Implementation: `Options` struct (`src/Options.hpp`) threaded
          `Caesar` → `Csar` → `Cgrp` → `Cbnk`, replacing the loose `bool P`.
          `--pad-sustain[=SECONDS]` (bare flag defaults to 3.5) is named for the deviation,
          not for correctness, and its help text says so. A default-visible notice now
          fires on any bank containing release-127 voices, worded differently per mode
          (default: the tail is DSP reverb, use a reverb-capable player; with the flag: the
          tail is faked). Verified: default output is byte-identical to a hardcoded-instant
          build; `--pad-sustain` is byte-identical to the old 3.5 s build; across 26
          archives the flag moves **only `.sf2` bytes** (never `.mid`/`.wav`/`.log`), and on
          a sentinel-free archive it emits no notice and is a no-op. Invalid values
          (`abc`, `-1`, `0`, `3.5s`) are rejected.

      **`decay == 127` is the same branch and also instant → caesar's decay-127 handling
      was already correct** (both setters share the one `CalcRelease` curve, so
      `decay == 127` hits the same `→ 65535.0f` fastest-rate branch; no change needed).

      **REVERB LOCATION CONFIRMED, AND THE RE ENDS HERE (2026-07-09, session 2).** A
      15-agent investigation (5 recon + 9 adversarial verifiers) settled the last three
      open questions; all three claims survived 3-of-3 refutation attempts. See
      `NW4C-disasm-handoff.md` for addresses and evidence.
        - **Reverb is computed by the embedded Teak DSP firmware, not by ARM11 code.** This
          *reverses* the Wii architecture (where NW4R's `FxReverbHi` ran on the PowerPC via
          `AXFXReverbHiCallback`), so it needed checking rather than assuming. Mii Plaza's
          `code.bin` contains only the DSP *upload path* — vaddr `0x10D204` hands
          `LoadComponent` the firmware pointer `0x355100` and size `0xC288`, byte-exact the
          embedded `DSP1` blob — and contains no comb/allpass delay tables and no
          `Reverb`/`AXFX`/`AuxBus`/`nw::snd` strings at all. `0xD9` "fx send a" sets a DSP
          aux-bus mixer gain (`SourceConfiguration.gain[1]`/`[2]`), so caesar's `0xD9 → CC91`
          mapping is semantically right.
        - **The NAND dump, `otp.bin` and `movable.sed` are not needed.** No standalone
          `dspfirm.cdc` exists on 3DS NAND; every app embeds its own copy in `.code`. The
          five already-decompressed copies in `re_extract` are strictly more usable than the
          LZSS-compressed NAND ones. Extractor: `re_extract\dsp_extract.py` (self-certifying
          — the firmware's own per-segment SHA-256s all verify).
        - **RE'ing the DSP reverb cannot improve the SF2/MIDI *exporter*.** SF2 and MIDI
          carry only a reverb *send amount* (SF2 generator 16 `reverbEffectsSend`; MIDI
          CC91) — never an algorithm, IR, room size, or coefficients. caesar already emits
          both sends, so the exporter is at the format ceiling. FluidSynth applies Freeverb,
          which is not the 3DS reverb and cannot be tuned into it.
          **Scope note (superseded for the player):** this verdict holds *only* for the
          SF2/MIDI export path. The project's actual goal is a tool suite including a
          console-accurate player, which has no format between it and the speaker — there,
          reverb is required and is on the critical path. See
          [SUITE-DESIGN.md](SUITE-DESIGN.md). Teak *disassembly* still isn't needed: the
          reverb is recoverable behaviourally by running the firmware in `teakra` offline
          and capturing its impulse response.

      **ROOT CAUSE (user's ear, 2026-07-08): no single release can ever be right.**
      A per-instrument A/B ("0.3 s perfect for some notes, too short for others;
      1.0 fits nothing; 3.5 fits a couple but muds the rest") plus `variance.py`
      shows all 34 sentinel programs are ENVELOPE-IDENTICAL (attack/hold instant,
      decay 127, full sustain, release 127). The tail variation the user hears is
      NOT in the envelope — it lives in the **sample** (6 one-shot vs 28 looped;
      loop lengths 225…28k samples) and the **per-note reverb sends**. So byte 127
      is one global behaviour; caesar is being asked to fake, with one release knob,
      a variation that physically lives in the samples + reverb. Accurate-fix
      options, best→cheapest, as they stood at the time (option 1 was subsequently
      done and option 4 became the shipped default; the rest stand):
        1. **RE the ARM11 `code.bin` (`nw::snd` over `nn::snd`)** — settles the
           *envelope* half. The whole BCSEQ/BCBNK interpreter (sequence parse, note
           alloc, ADSR, LFO, pitch, bank lookup) runs in game-side ARM11 code, so the
           true meaning of release/decay `127` lives here. But note the envelope
           *curve* is already known byte-for-byte from the NW4R (Wii) decomp
           (`doldecomp/ogws` `snd_EnvGenerator.cpp` `CalcRelease`) and matches
           caesar's `DecayTable`; what is genuinely open is only whether NW4C kept
           NW4R/DS's `127 → 65535` (instant/fastest) branch or changed it. Same place:
           the 3 discarded per-note 4-byte fields (`Cbnk.cpp` Note 0x2C/0x30/0x34).
           Target `MiiPlazaEX\code.bin` (see prep note below). **Correction (prep,
           2026-07-09): the reverb is NOT in `code.bin`.** `code.bin` only writes the
           aux-send *level* (the CC91-equivalent) into the DSP voice's
           `SourceConfiguration`; the reverb *algorithm and room coefficients* live in
           the embedded DSP firmware (Teak core) and are still undocumented (Citra/
           Azahar HLE leaves the reverb block as 26 padding words, TODO). So a
           `code.bin` disassembly cannot answer the per-instrument-tail (reverb)
           question — only the envelope-127 question.
        2. **RE / model the 3DS DSP reverb** — **out of scope for the SF2/MIDI exporter;
           ON THE CRITICAL PATH for the player (2026-07-09).** Confirmed to govern the
           per-instrument tails and to live in the DSP1 Teak component embedded in each
           app's binary (not the ARM11 image). SF2/MIDI can only carry a reverb *send
           amount*, so the exporter can never use it. A player can, and must — skip it and
           the player reproduces exactly the `EMPTY_LANDSCAPE` dryness this whole thread
           began with. **Do not disassemble the Teak code, and do not crib the Wii's
           reverb**: `ogws`' `snd_FxReverbHi.cpp` is a wrapper around the Wii's PowerPC-CPU
           `AXFXReverbHi*` SDK calls, so lifting it would import Wii reverb into a 3DS
           player. Recover it behaviourally instead — run the real firmware in `teakra`
           offline, impulse the aux bus, capture the tail, fit a comb+allpass model, and
           validate against a New 3DS line-in capture. Note the firmware is **not** shared
           across all titles: three distinct images across five extracted apps (Mii Plaza
           `944b40b5…`; eShop = Photos = System Settings `8e213f3e…`; 3DS Sound
           `5c03dd63…`, the AAC-capable variant) — per-SDK-generation, not once-off.
           Addresses in `NW4C-disasm-handoff.md`.
        3. **Per-instrument isolated-note console capture** — a generated test
           `.bcseq` playing each sentinel instrument as one note→silence; measure
           each tail empirically (tooling exists). No RE; laborious.
        4. **Faithful model**: set release-127 to the true short value + rely on
           emitted CC91/93, optionally boosting reverb depth to offset weak GM
           reverb in common players; document that faithful playback needs a
           reverb-capable player.
        5. **Sample-loop-aware release** (cheap stopgap): don't force long release
           on one-shot samples; cap release relative to loop length. Heuristic,
           reduces mud without RE.
      ~~**Open follow-up:** `decay == 127` (`ConvertDecay`)~~ — **resolved 2026-07-09.**
      Both setters share the one `CalcRelease` curve, so `decay == 127` hits the same
      `→ 65535.0f` fastest-rate branch. caesar's existing instant treatment of decay-127
      is correct and needs no change.

      ~~**Remaining decision (needs the user's ear — reverses committed `b078932`).**~~
      **Done — see "✅ APPLIED" above.** One caveat from that work survives as a standing
      rule: do **not** set an SF2 `reverbEffectsSend` generator to compensate for the
      missing tail. The known send levels live in the sequence, not the bank, so it would
      only double-count the CC91 that `Cseq` already emits.

      **Disassembly prep — target selection & toolchain (2026-07-09).** Before
      committing to a target we checked whether "engine version drift across titles"
      is a real hazard. Conclusion: it's real but minor, and Mii Plaza is a sound
      target.
        - **How the engine ships.** NW4C `snd` is *statically linked into each
          title's own binary* (confirmed via leaked Sun/Moon build props linking
          `libnw_snd.a`; the OS exposes only voice-level `dsp::DSP`/`csnd:SND`, no
          sequence service). So every `code.bin` carries its own copy — there is
          genuine per-title version drift, but the ground truth for a given archive
          is that title's own binary. Since the release-127 repros
          (`EMPTY_LANDSCAPE`, `BGM_MAIN_Mii_Only_One`) are Mii Plaza archives,
          `MiiPlazaEX\code.bin` *is* the correct ground truth for them.
        - **Drift is minor and doesn't touch the envelope.** The DS→Wii→3DS→WiiU→
          Switch sequence/bank/envelope lineage is essentially invariant: the
          decay/release curve is byte-identical (up to a representation scale) across
          DS `SSEQPlayer`, NW4R `ogws`, and caesar's own table; sequence opcodes match
          between the CSEQ command list and Kinnay's BFSEQ table. Our corpus shows 5
          CSAR container versions (0x02000000 … 0x02030200), 2 BCSEQ (1.0.0.0 /
          1.1.0.0), and just **1** BCBNK (1.0.1.0) — and caesar already parses all of
          them with one code path (the only version branch is `Csar.cpp:74-80`). Mii
          Plaza is launch-era (CSAR 0x02000000, the *oldest* engine), which is exactly
          right for its own archives; if a late-era title ever needs confirming, diff
          its `code.bin` against Mii Plaza's with BinDiff rather than re-RE'ing.
        - **Mii Plaza specifics.** `MiiPlazaEX\code.bin` decompresses 1.45 MB → 2.51 MB
          and is monolithic — **no `.cro` dynamic modules** in its romfs — so the
          whole `snd` runtime is in one flat image (the easy case; nothing to chase
          into CROs). It embeds its DSP1 firmware at file offset **0x255100** (the `DSP1`
          magic sits at `+0x100` = 0x255200, after the RSA-2048 signature).
        - **Toolchain.** Decompress `.code` with GodMode9 "Extract .code" on-console
          (auto-decompresses) or `ctrtool --decompresscode` (the BLZ backwards-LZ;
          we already scripted a decompressor and produced `code.dec.bin` locally).
          Load raw at vaddr **0x00100000** (ARMv6K, LE) using the exheader's text/ro/
          data code-set info — via `kynex7510/3ds_ida` (IDA, maintained 2025) or
          `Martmists-GH/ghidra-ctr-loader` (Ghidra), or a plain raw import + manual
          segment splits. The exheader is the one missing piece: dump it with
          `ctrtool --exheader=` or GodMode9's NCCH mount. Use the NW4R `ogws`/`ss`
          decomp `snd_EnvGenerator.cpp`/`snd_MmlParser.cpp` as the structural map, and
          `SMBNext/nsmb2-headers` `symbols.ld` (real `nw::snd` addresses in a shipped
          3DS binary — note NW4C uses `nw::snd::internal`, vs NW4R's `nw4r::snd::detail`)
          as an anchor set. No public NW4C `snd` decomp exists; disassembling with the
          NW4R decomp as a crib is the only route. For the exact per-title NW4C `snd`
          revision, read the NCCH **plain region** SDK tags (e.g.
          `[SDK+NINTENDO:NW4C_3_7_5_snd]`) — a separate NCCH section our ExeFS-only
          dumps don't include, dumpable via GodMode9.
        - **Reverb caveat (repeated because it's the crux).** A `code.bin`
          disassembly answers envelope-127 but **not** the per-instrument tails —
          those are DSP-firmware reverb (option 2), a distinct `teakra` job on the
          embedded DSP1 component, which is shared across titles.

      **Disassembly prep — extraction DONE, envelope anchors located (2026-07-09).**
      The user's `E:\legoj\…\3ds firmware` holds fully-decrypted base `.cia`s
      (ctrtool: `Crypto key: None` → no keys needed). `ctrtool.exe` (top of the
      `3DSWii Dumps` folder) does the whole chain and auto-decompresses `.code` — so
      **GodMode9 was not needed**. Extracted exheader + plain region + decompressed
      `code.bin` for Mii Plaza (MeetSound), eShop (TigerSound), 3DS Camera/"Photos"
      (PNOTE_Sound), System Settings (mset), 3DS Sound (SNOTE) into
      `…\3DSWii Dumps\re_extract\<App>\`. Mii Plaza `code.bin` is byte-identical to the
      earlier scripted BLZ decompress (cross-validated).
        - **Engine confirmed present in every binary, incl. Mii Plaza** — via BCSAR
          magics and the NW4R-form envelope `DecibelSquareTable` fingerprint. (Mii
          Plaza's plain region has no per-module `NW4C_snd` tag — early CTR_SDK 5.2
          omits it — but the fingerprint proves the engine is compiled in.)
        - **Envelope anchors (Mii Plaza, load base 0x00100000):** `DecibelSquareTable`
          @ vaddr **0x328844**, `attackTable[128]` @ **0x328944**, both in `.rodata`.
          The envelope generator (`CalcRelease` + the release-127 branch) references
          these by address, so they are the entry points for the 127 investigation.
          Tables are byte-identical between Mii Plaza (SDK 5.2) and eShop
          (`NW4C_3_6_1`, SDK 11.2) — envelope invariance now confirmed *empirically*
          in the target binaries, not just inferred from the Wii decomp.
        - Binaries are **stripped** (no `nw::snd` symbols), so identification is by
          envelope-anchor xref + NW4R (`ogws`) structural match.
        - **Remaining gap: a disassembler.** No IDA/Ghidra and no python `capstone`
          on the machine. Either install Ghidra + `Martmists-GH/ghidra-ctr-loader`
          (free), or `pip install capstone` and script the xref hunt to
          0x328844/0x328944 headlessly. The NAND dump in progress is not required.
          (Since resolved: `capstone` was installed and used for the session-3
          disassembly work recorded above and in `NW4C-disasm-handoff.md`.)

## v0.5.0 §5 — Licensing

The vendored `libsmfc` shipped without a license notice, but it is loveemu's
MIT-licensed code (identical to the copy in
[loveemu-lab](https://github.com/loveemu/loveemu-lab)); the copy had simply
dropped the repo-level license. MIT is GPL-3.0-compatible, so there is no
conflict with caesar's GPL-3.0.

- [x] Add the MIT notice for `libsmfc` (`src/libsmfc/LICENSE`) and document the
      third-party licenses in the README. Binaries are now distributable.

## v0.5.0 §6 — Continuous integration & first release

- [x] GitHub Actions workflow: build on every push. Scope grew from the original
      "start with Windows/MSVC" to a **three-OS matrix** (`windows-latest`,
      `ubuntu-latest`, `macos-latest`), since the Linux path is now proven and
      macOS runners are free on public repos. `.github/workflows/build.yml`:
      push/PR-to-`master` + `workflow_dispatch`, `permissions: contents: read`,
      concurrency-cancel of superseded runs, `fail-fast: false`, the verified
      portable incantation (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` then
      `cmake --build build --config Release --parallel`), and a `shell: bash`
      usage/liveness smoke that tolerates caesar's by-design non-zero no-input exit
      while failing if the banner is absent (a crash signature). Runs on
      **GitHub-hosted runners**, not self-hosted: caesar is a public repo, so a
      self-hosted runner would let a fork PR execute code on the always-on
      server/VM — a documented RCE risk with nothing to gain (hosted is free and
      the build is ~17 s). The user's own Windows server + Debian VM stay the
      *private* A/B bench against copyrighted archives, which can't enter public CI.
- [x] Release workflow: `.github/workflows/release.yml`, triggered on `v*` tags.
      Build matrix (same three OSes) → per-OS `caesar-<os>-<arch>.zip`
      (binary + `LICENSE` + `README.md`) → `upload-artifact@v4` → a single gather
      job (`needs: build`, `permissions: contents: write`) that downloads all zips
      and publishes one GitHub Release via `softprops/action-gh-release`, **pinned
      to a commit SHA** (it holds the only write token) with
      `generate_release_notes`. `.github/dependabot.yml` keeps the actions current.
- [x] Add a CI status badge to the README (links to the `build.yml` runs).

      Validation before first push: both workflows pass `actionlint` 1.7.12 clean
      and parse as valid YAML; the build commands are the same ones verified
      end-to-end on Debian. The live run has since gone green across all three
      hosted OSes.
- [x] **v0.5.0 shipped 2026-07-10** — the `v0.5.0` tag was pushed and
      `release.yml` published the first maintained GitHub Release, with
      `caesar-windows-x64.zip`, `caesar-linux-x64.zip`, and
      `caesar-macos-arm64.zip` attached.

## Fixed bugs

Concrete defects found while surveying and evolving the code, since fixed.
Still-open defects are tracked under "Known bugs" in ROADMAP.md.

- **Group file-table desync** (`Cgrp.cpp`, the file-record loop) (*fixed
  2026-07-09*). Each file record is a fixed 16 bytes — `Id`, a presence marker,
  an offset, and a length — but the offset was read inside a short-circuiting
  `?:` (`ReadFixLen(pos, 4) == 0x1F00 ? … + ReadFixLen(pos, 4) : nullptr`), so on
  any marker other than `0x1F00` the offset `ReadFixLen` never ran and `pos`
  advanced only 12 bytes. The `Length` field then picked up the offset word and
  every following record was parsed 4 bytes early, so a group holding an external
  or absent file misparsed everything after it. The offset is now read
  unconditionally into a local and only *used* when the marker is `0x1F00`, so the
  cursor always advances a full 16-byte record. Verified with a synthetic-group
  A/B (old binary vs fixed) built from the vendored parser: with an absent record
  preceding a present file, the old parse loses the following file entirely (it
  resolves to `nullptr`) while the fixed parse finds and reports it; an all-present
  group is unchanged (both report every file), confirming the fix is
  behaviour-preserving on the common case and only repairs the desynced path.
  Note the synthetic A/B exercises the parser logic directly (via the vendored
  objects), not the shipped binary end-to-end; the source also *compiles* cleanly
  under GCC 13 on Linux. (The "Linux functionality remains untested" caveat this
  entry originally carried was closed on 2026-07-10 — see §1's end-to-end Linux
  A/B.)
- **32-bit reader invoked with an 8-byte width** (`Csar.cpp`, the `0x220C` and
  `0x220D` file-record branches) (*fixed 2026-07-09*). Both branches called
  `ReadFixLen(pos, 8)`, which accumulates into an `int32_t` via
  `result |= *pos++ << (i * 8)`; for the 5th–8th bytes that shifts a 32-bit value
  by 32–56 bits — undefined behavior — and silently folds the top four bytes back
  into the low word. It worked under MSVC (whose shift is masked mod 32) only
  because the field is a genuine 8-byte little-endian value whose low word is the
  size (`0xC`) and whose high word is reserved (`0`). Each branch now reads the
  field as two bounds-checked 32-bit halves — asserting the low word is `0xC` and
  the high word is `0` — so no shift ever exceeds 24 bits. Verified with UBSan:
  the old width-8 read reports `shift exponent 32 is too large for 32-bit type`,
  while the two-halves read is clean and consumes the same 8 bytes, leaving the
  cursor byte-identical for every real archive (high word always `0`).
- **Compiler narrowing warnings** (`C4267` / `C4244`, `size_t`/`int` to smaller
  types) (*fixed 2026-07-10, v0.5.0 hardening pass*). Harmless in practice but
  they flagged real implicit truncations. The hardening pass (`388e1bb`,
  `55e1b3c`) cleaned the warning set and the build now treats warnings as errors
  (`/W3 /WX` on MSVC, `-Wall -Wextra -Werror` on GCC/Clang; the vendored
  libraries are exempted so the caesar target's gate stays honest), so new
  warnings fail the build.
- **Non-deterministic SF2 output from an uninitialised sample key** (*fixed
  2026-07-08*). `CbnkCwav::Key` — written as each sample's shdr `byOriginalKey` —
  was only assigned when a note referenced the sample, but every sample with
  `Id < 0xF000` is emitted regardless, so a sample no instrument used wrote an
  *uninitialised* byte into the `.sf2`. Output was therefore non-deterministic:
  that byte varied (0x00 / 0x01 / 0x0D) between runs and with the output-path
  length. Value-initialising the record (`CbnkCwav cwav{}` in `Cbnk.cpp`) pins it
  to 0. Behaviour-preserving for every *defined* byte — the garbage happened to be
  0 in the runs sampled, so the 24,928-file A/B above stayed byte-identical — and
  it only ever affects the cosmetic root key of a sample that no zone plays. Found
  while validating the composed-path rewrite above, whose extra `std::filesystem`
  allocations perturbed the heap enough to expose the latent read. (The wider
  value-initialisation audit this suggested was done in the v0.5.0 hardening
  pass — see §1's `chan{}`/`inst{}` fixes.)
- **Shared sequence banks: per-entry start offsets now honoured** (*fixed
  2026-07-07*). Most `.bcseq` in these archives are *multi-entry banks*: one DATA
  blob holds many independent mini-sequences (each ends in `Fin`) plus helper
  subroutines (each ends in `Return`), and the archive maps many differently-named
  SEQ entries onto the *same* blob, each meant to begin at its own offset.
  `Cseq::Convert` used to always start at byte 0, which caused two bugs at once:
    - byte 0 is usually a `Return`-terminated helper, so the walk hit a `Return`
      with an empty call stack and the old `0xFD` handler `smfDelete`d the
      in-progress MIDI and wrote nothing — silently dropping 972/1369 sequences
      in MK7 `ctr_dash` and 115 in `GardenSound`; and
    - even the sequences that *did* convert were mostly wrong duplicates — every
      entry sharing a blob produced the same byte-0 walk.

  The fix has two parts. (1) `Csar.cpp` reads each entry's start offset from its
  INFO record and passes it to `Cseq::Convert`, which begins the walk there
  (falling back to the top, with a warning, if it does not land on a command
  boundary). The field sits `0xC` before the bank-reference sub-structure that
  `cbnkOffset` locates, so it is read at `entry + cbnkOffset + 0x10` — **not** a
  fixed `+0x54`. Two record layouts exist in the wild under the same CSAR version:
  the common one (`cbnkOffset 0x44` → field at `+0x54`, e.g. MK7, System `mset`,
  eShop `TigerSound`) and a `+0xC`-larger one (`cbnkOffset 0x50` → field at
  `+0x60`, e.g. the MiiPlaza `MeetSound`/`mgCar` archives); anchoring to
  `cbnkOffset` handles both, whereas a fixed `+0x54` silently read the constant
  `0x18` for the larger layout. (2) The `0xFD` handler now treats a stray `Return`
  as end-of-track (same path as `Fin`) rather than discarding, as a safety net.

  Verified side-by-side (old binary vs fixed) on eight archives. Original three:
  distinct MIDIs 23→1027 (`ctr_dash`), 862→2825 (`GardenSound`), 11→83 (`menu`).
  Regression set (System/MiiPlaza/eShop, accuracy-critical): distinct MIDIs
  5→40 (`mset`), 9→74 (`TigerSound`), 56→252 / 64→395 (`MeetSound` ×2),
  105→105 (`mgCar`); all boundary-fallbacks 0 after the layout fix (were 147/87);
  every start offset lands on a command boundary, and 57–76% land exactly on a
  named `..._Start` label whose name matches the entry (e.g. `SD_BGM01` →
  `SMF_SD_CAR_BGM01_Start`), which is the decisive accuracy signal. Apparent
  note-loss cases resolve to labels literally named `<nosound>`/`<dummy_bgm>`
  (intentionally silent — the old byte-0 walk played a placeholder). No hangs, no
  crashes. Sequences that emitted no notes were either those silent entries or
  dispatchers that select a section via conditional (`[If]`) jumps; the latter
  were resolved next (see the `[If]`-dispatcher item in §4 above).
