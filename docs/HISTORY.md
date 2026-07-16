# caesar engineering history

What shipped, in full detail — the verification narratives, A/B evidence, and
investigation stories behind every completed roadmap item. This file exists so
[ROADMAP.md](ROADMAP.md) can stay a short statement of what is open: when an
item ships, its checkbox flips there and the full write-up is appended here.
Settled design for the tool suite lives in [SUITE-DESIGN.md](SUITE-DESIGN.md);
deep reverse-engineering findings (addresses, evidence chains) in
[NW4C-disasm-handoff.md](NW4C-disasm-handoff.md).

Entries are grouped by the v0.5.0 roadmap section they closed out, in the
original roadmap order, followed by the v0.5.1 release record, post-v0.5.1
feature narratives, fixed bugs, and investigations.

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

## v0.5.1 — MIDI-fidelity patch (shipped 2026-07-12)

The item-level release record, moved here from the roadmap after release. A
post-release opcode-by-opcode audit of the MIDI converter against the
12,308-file corpus (scoped 2026-07-10) found the discrepancies below. All were
exporter-side, bounded, and touched `.mid` output only; every item shipped in
v0.5.1 (released 2026-07-12), each verified by corpus A/B and — where output
changed — an independent SMF parse of every changed pair. Full narratives are
in [Fixed bugs](#fixed-bugs) below; the user-facing summary is the changelog's
`[0.5.1]` section. Two engine-level discoveries landed alongside: the
note-wait default is ON (a long-standing timing bug affecting ~112k notes) and
tie mode's true semantics — both also below.

- **Fixed the GM percussion-channel collision** — track 9 now relocates off
  GM channel 9 to a free channel (or, when all 16 tracks are used, stays put
  with a GS "rhythm part off" SysEx for part 10); the SMF track layout is
  unchanged, so sequences without a 10th track are byte-identical.
- **Triaged the ~1,020 surfaced controller/parameter drops** — plain
  out-of-range volume/pan/master-volume/expression values now clamp to 127
  with an approximation notice (unevaluated `Rnd`/`Var` stand-ins keep
  dropping with a notice until suite stage 4), and a caller-side `bpm > 0`
  guard closes the vendored `libsmfcx.c` zero-BPM division UB. Corpus A/B
  byte-identical (82 archives, 257,097 files).
- **Fixed the two mis-wired vibrato/pitch controls** — `0xE3` sweep pitch
  no longer masquerades as CC78 (it drops with an honest notice until the
  stage-2 player can render pitch-bend ramps), and `0xE0` mod delay — s16
  in **5 ms units** per the NW4R decomp, correcting the triage's
  "milliseconds" — scales into CC78's relative upper half (64 = no delay,
  saturating at 1 s). A/B: 1,772 `.mid` diffs, every one CC78-only by
  independent SMF parse; out-of-range drops fell 1,020 → 230.
- **Passed finite loop repeat counts through.** `0xD4`'s count now drives
  EMIDI CC116 and `0xFC` emits the spec-fixed CC117 = 127, so EMIDI-aware
  players honour a finite "play N×" instead of looping forever (0 stays
  infinite in both conventions; counts ≥ 128 clamp to 127 with a notice).
  (Fully unrolling repeats in the timeline stays stage-2/5 flattening work.)
- **Damper pedal (`0xDF`) — the reported bug was a phantom; hardened
  anyway.** The triage's "argument is a bool" premise was *refuted* (NW4R
  thresholds it at `>= 64`, exactly as MIDI CC64 does, and every corpus
  value is already 0 or 127), so the prescribed `? 127 : 0` would have
  inverted args 1–63; caesar now applies the engine's own threshold, which
  is byte-identical on the corpus and fixes a latent >127 drop.
- **Implemented the two dropped commands — but neither was the prescribed
  one-liner.** The engine *sums* `0xDC` init_pan with `0xC0` pan, so a raw
  CC10 write clobbers the pan (76 corpus tracks set both); caesar now tracks
  both terms and emits the combined position. And `0xD8` lpf_cutoff is
  darken-only — the engine clamps its scale at 64, so a raw pass-through
  would have told synths to brighten past the sample's own tone (261
  commands). Both semantics confirmed in the 3DS binary itself (the CSEQ
  dispatcher, found at last). A/B: `.mid`-only, 2,385 files, every diff
  CC10/CC74 by independent SMF parse.
- **Stopped `0xB2` mono/poly from silencing the channel** — the census
  confirmed mid-track firings (56 of 249 corpus executions, 35 after notes
  were already sounding), so the CC126/CC127 emission is demoted to a
  default-visible "no MIDI equivalent" notice.
- **Gated the vibrato CCs on mod type (`0xCC`)** — tracks whose LFO
  targets volume (tremolo) or pan (auto-pan) no longer render as pitch
  wobble: CC1/76/77/78 are suppressed on those spans, a live CC1 is
  zeroed when the LFO leaves pitch, and persisted values are restored on
  return (the engine keeps them across a retarget — NW4R-confirmed). A/B:
  2,146 `.mid`-only diffs across 58 archives, every event a removed
  CC1/76/77/78 or an inserted CC1=0 by independent SMF parse.
- **Warning-hygiene pass over the drop sites** — every drop/approximation
  is now a default-visible categorized notice: span/priority/front-bypass
  demoted to benign "no MIDI equivalent", the dead extended-command warning
  chain revived (parse now records the extended opcode) with its mod4
  labels corrected against CtrCafe, per-execution notices for the silent
  `Rnd`/`Var`/`[If]` manglings, and final unknown-opcode catch-alls
  (`0xDE` FxSendC included). Byte-identical corpus A/B.
- **Closed the two wrong-arg-count desync hazards** — the `_t` trailing
  duration is now consumed for every command form (not just `0xB0–0xDF`),
  and the fixed-1-byte group (plus `0xCC` and the extended mod-types) reads
  its argument through the prefix-aware path; the `0x90`/`0x96`/`0xB7–0xBC`
  non-opcodes now fail fast instead of swallowing guessed lengths, and `_t`
  ramps surface a per-execution flatten notice. Byte-identical corpus A/B.
- **`Rnd` values now convert as the range midpoint** instead of the
  minimum (which silently biased 196k volumes, 177k pitch bends, and 94k
  rest durations low) — the honest deterministic stand-in until the
  convert-time VM brings real randomness. `.mid`-only A/B.
- **Tie mode (`0xC8`) implemented** — tie regions flatten to gap-free
  back-to-back segments (one note per commanded pitch/velocity, sustaining
  through gates and rests, released at the next tie command/`Fin`/track
  end; NW4R-confirmed semantics), replacing the per-note re-attacks. The
  re-attack-at-pitch-change approximation is surfaced per region.
  `.mid`-only A/B.
- **Adopted a changelog** (`CHANGELOG.md`, Keep a Changelog format; release
  zips bundle it and `release.yml` publishes the version's section as the
  release notes). Convention: every user-facing change adds a line under
  `[Unreleased]` in the same commit, stating its output impact
  (output-identical vs which output types change).

## The convert-time variable VM (2026-07-13)

The largest single fidelity item since the fork began: the sequence variable
machinery — 353k `setvar`-family ops, 210k comparisons, the `[If]` prefix,
and `Var`-valued arguments — now *executes* during conversion instead of
dropping with notices. This entry is the full record: semantics sourcing,
converter policies, the verification net, and the two design rules the
verification itself forced.

**Semantics sourcing — three independent layers, and a real engine
divergence.** (1) The NW4R decomp (kiwi515/ogws `snd_MmlParser.cpp` /
`snd_SeqPlayer.cpp` / `snd_SeqTrack.cpp`) supplied the op formulas; (2)
GotaSequenceLib supplied the CTR byte map but was found wrong in **five**
places (garbled `notvar`, exclusive-max `Rnd` roll, unguarded ÷0, unchecked
variable indices, global-var default 0) — every one resolved in NW4R's
favour; (3) a fresh capstone disassembly of MiiPlaza `code.bin` byte-verified
all 18 ops in the shipped 3DS engine (addresses in the disasm handoff,
Session 4). The disassembly also settled scoping — 16 player locals (idx
0–15), 16 **process-global** vars at an absolute address (16–31), **16**
track vars (32–47, not 8), NULL no-op at ≥48 — and caught a genuine
NW4C≠NW4R divergence found nowhere else: **the CTR port gates `Fin` and
`Return` behind `[If]`** (`[If] Fin` with a false flag keeps the track
playing), where the Wii engine returns FINISH outside the gate. The Wii
reading had already been written into the implementation spec; the binary
overruled it mid-flight. Only the inert `0xFE` alloc-track escapes the gate.

**Converter policies (each surfaced by a notice).** Variables initialise to
**0** — a deliberate policy, not the hardware's −1 power-on value: the
roadmap-mandated two-init corpus A/B showed init −1 rescues *nothing* (0
files) while silencing **1,294 more** files and ~309k note-ons, because
authored comparisons overwhelmingly key on 0 as the default section; the
per-group mechanism diagnoses independently confirmed init choice was
irrelevant to every contested file (their variables are sequence-written
before read). `randvar` and `Rnd` arguments stand in their range midpoint
(the VM is deliberately PRNG-free — one input, one reproducible file). Reads
of never-written variables fire a per-execution notice naming the variable —
the honest marker for game-seeded state. `cmpFlag` inits true per track;
skipped commands consume arguments and no time; an `[If]`-skipped comparison
cannot write `cmpFlag` (all binary-confirmed).

**Control flow.** The two-reachability dispatcher heuristic is deleted —
conditional jumps evaluate exactly. Backward conditional takes follow the
spin-wait/counted-loop rule (allowed while VM state changed since the target
last ran, under a 1024-per-site revisit budget): a counted loop unrolls until
its comparison clears; a spin-wait's body plays once. A per-track execution
budget (1M commands, added after the adversarial review) is the terminal
backstop and incidentally closes a latent pre-existing hang on `Call` cycles.
No loop markers ever come from conditional jumps.

**The re-roll-loop escape rule — forced by verification.** The first corpus
census found 482 sound→silent files. Mechanism diagnosis (per-group, against
the raw `.bcseq` streams, with a validated Python re-implementation of both
walks) split them three ways: (a) **50 unique Pokémon `niji_sound` files
(403 notes)** are self-contained RNG re-roll loops — `randvar`, compare,
`[If]`-exit to the play block, unconditional jump back — which on hardware
ALWAYS eventually play (every threshold satisfiable), but whose exit a
PRNG-free midpoint can never roll open; the unconditional back-jump then
read as a "whole-song loop" and the track ended silent. The fix: when the
whole-song-loop rule fires, if the loop span contains an `[If]`-jump the
gate turned off **whose comparison read only sequence-written state**, take
that exit once — it is the loop's guaranteed hardware escape. (b) **138
GardenSound files** are game-triggered spin-wait dispatchers polling
never-written variables (`cmp_ne var33 != var17`-style) — the old converter's
notes were *fabricated* by the heuristic taking an arbitrary branch; honest
silence-at-rest is console-accurate, so these are deliberately NOT escaped
(the never-written tag excludes them), and each names its trigger variable
in a notice. An opt-in trigger-seed preview mode is filed on the roadmap.
(c) **17 niji selector chirps + 6 GreenCube selectors** are random one-shots
whose midpoint outcome is the (majority) "no fire" branch — honest silence.
After the rule landed, the census settled at exactly the diagnosed
honest-silence population (332 files incl. archive duplicates) and the 50
re-roll files recovered all 403 notes; 11 additional ambience/engine SEs
(bakery cafés, Zelda waves, kart wind, WarioWare engines) gained notes the
old converter never reached.

**Verification.** Corpus A/B (82 archives, 257,125 files/side): every diff
`.mid`-only — 3,356 files across 55 archives, none added or removed, no
`.sf2`/`.wav`/raw-dump byte moved; 19 machinery-free archives byte-identical
including stderr. Independent SMF parse of all 3,356 changed pairs: zero
parse errors; net **+37,486 note-ons** (+55,175 gained as conditional content
became reachable, −17,689 lost to honest gating). Every sound→silent
transition mechanism-diagnosed and justified (above); the 909-file
dispatcher regression watch resolved as: the majority still sound via real
evaluation, 5 GardenSound files *gained* sound (incl. `SE_ESC_INSECT_HORNET`
0→1, `SE_ESCAPE_ENV_SHAPER` 0→5), and the 138 silenced ones are proven
fabrications. A three-lens adversarial review (engine-semantics fidelity /
byte-identity threat model / loop termination) confirmed **zero defects**;
its informational findings produced the execution-budget backstop and two
comment corrections. Two byte-diff oddities were run to ground as correct
improvements: `SEQ_BGM_PROLOGUE_12` swaps a same-tick loopStart/note-on pair
(the old converter executed an `[If]` tie-off unconditionally, finalizing the
tie mid-stream; the VM correctly gates it), and `SE_DJ_CTRL_WAIT_TO_FIN`
loses a bogus 7 BPM tempo + 8-tick rest that were the old variable-*index*
stand-ins (var7/var8 resolve to 0; the 0-BPM guard drops the tempo).

**Residue, documented.** Six GardenSound-class files whose trigger is seeded
still select a velocity-0 variant under the randvar midpoint (the known
midpoint approximation, not a silencing bug). GotaSequenceLib was dropped as
a cross-oracle (its execution layer failed five semantic checks); the
validated per-file Python walk simulators used for the diagnoses served that
role instead. The New 3DS in-game spot checks for default-section ground
truth remain available on request but were superseded by the two-init A/B's
unambiguous result plus the per-mechanism diagnoses.

## Fixed bugs

Concrete defects found while surveying and evolving the code, since fixed.
Still-open defects are tracked under "Known bugs" in ROADMAP.md.

- **The note-wait default fixed: tracks start with note-wait ON, as the
  engine does** (`Cseq.cpp`) (*2026-07-12*). A new bug found (and fixed)
  during the tie-mode work — caesar initialised `noteWait = false` (track
  time advanced only via explicit rests until an explicit `0xC7`), so every
  track that plays notes before its first `0xC7` was time-compressed:
  ~112k notes across 67 archives (instrumented census), overwhelmingly
  sound effects, lost their note-length waits — `SE_CTR_COMMON_WAIT`'s
  steps played on a 9-tick grid instead of the engine's 13 (gate 4 +
  rest 9), and the no-rest tied sweeps (`SE_Map_WarpstarUp*`, 300+ tied
  notes, zero rests) collapsed onto a single tick, which the tie-mode
  verification surfaced as 3,366 suddenly-silent files.

  **The evidence chain (three independent lines).** (1) The NW4R decomp's
  track constructor initialises `noteWaitFlag = true` (ogws
  `snd_MmlSeqTrack.cpp`). (2) An instrumented corpus census of explicit
  `0xC7` arguments: **44,349 executions disable note-wait vs 3,654 that
  enable it** — 92% disables is an authoring tool escaping an ON default
  (an OFF default would make 44k explicit "off" commands pointless). (3)
  The tied sweeps are dispositive: under OFF they are inaudible nonsense
  (a 327-step swell in one tick), under ON they are exactly the rising
  sweep the console plays. An earlier same-day reading of the rest-timed
  UI blips ("OFF matches; ON would drag them") mistook a genuinely
  ambiguous case for evidence — the sweeps and the `0xC7` census are not
  ambiguous. (A LayeredFS timing probe remains the definitive console
  check if ever doubted.)

  **Verification.** A/B over the 82-archive corpus: 257,097 files per
  side, none added or removed; only `.mid` changed — 12,562 files across
  66 archives (the 67th census archive's default-dependent notes shift
  nothing) — and stdout/stderr are byte-identical (no notice depends on
  the clock). Independent SMF parse of every changed pair (old side
  re-extracted and SHA-matched against the A/B manifest): **0 violations,
  0 unresolved**. Zero value changes on any event, zero control events
  added or dropped, zero events moving earlier; 112,987 events shift
  monotonically later (max 97,408 ticks). Non-tie notes keep key,
  velocity, and duration by construction (60,930 shifted intact); all
  3,255 duration changes are tie-span recomputations; 31,782 notes
  *reappear* inside tie regions as clean monophonic segments (99.96%
  non-overlapping, none creating new polyphony) — the silent sweeps
  restored. Same-channel note overlaps corpus-wide drop 39,080 → 1,995
  (collapsed chord-stacks becoming sequential audio), and end-of-track
  times only grow or stay. The 156 strict-monotonicity heuristic trips
  were each run to ground as benign loop-marker anchoring or tie-cluster
  restructuring with zero absolute earlier-moves.

- **Tie mode (`0xC8`) implemented — tied notes no longer re-attack as
  independent short notes** (`Cseq.cpp`) (*2026-07-12*). Closes the v0.5.1
  stretch item, completing the section's work items.

  **Ground truth (NW4R decomp, cross-checked against GotaSequenceLib's
  executing player and the NitroStudio2 spec — three independent sources
  agreeing line-for-line).** A tie region is ONE continuous monophonic
  voice: `MML_SET_TIE` (both edges!) releases and frees the track's
  channels, then latches the flag; with tie on, `SeqTrack::NoteOn` reuses
  the sounding channel and merely updates its key and velocity — no
  re-attack — and a fresh allocation stores length −1, which
  `UpdateChannelLength` never counts down, so the note-length argument is
  ignored for audio and the voice sustains through gates and rests until
  the next tie command, `Fin`, or track end. Note-wait still advances track
  time by the length argument, so tie affects audio duration only, never
  event timing. (For `0xC8` specifically Gota's player *executes* the
  command, so its bool arg-typing is validated — unlike the damper trap.)

  **The flattening.** MIDI has no "retune the sounding note" event a GM/SF2
  player honours, so each region flattens to gap-free back-to-back
  segments: a note command with a new (key, velocity) closes the current
  segment at its tick and opens the next; identical re-commands merge into
  one sustained note; the last segment extends to the region's end;
  zero-length segments (same-tick updates) are skipped. Segments close at
  every region exit — tie command, `Fin`, stray Return, whole-song
  loop-back, and the no-`Fin` end-of-bank walk exit. The remaining
  approximation — a re-attack at each pitch change instead of one
  continuous envelope — is surfaced by a per-region default-visible notice
  (the faithful single-envelope form is a pitch-bend ramp, stage-2
  territory).

  **Verification.** A/B over the 82-archive corpus: 257,097 files per
  side, none added or removed; every non-`.mid` file byte-identical; 6,566
  `.mid` changed across exactly the census's 33 tie archives, the only
  stderr change on each being the tie notice swap. An independent SMF
  parse of **every** changed pair: 0 violations, 0 unresolved. The note
  accounting reconciles exactly — 51,465 old notes = 15,288 surviving
  (14,161 duration-unchanged, 336 extended across gates/rests, 791
  shortened where an old gate overran the region's end) + 36,177 removed,
  which decompose fully into 152 same-pitch merges (containment verified),
  2,481 notes subsumed inside a sustained segment, 33,532 zero-length
  skips, and 12 same-tick tie-off collapses; zero notes added, zero
  note-on ticks moved, zero velocity changes, zero non-note events
  touched, and all 8,301 end-of-track shrinks explained by old gates
  overrunning the (unchanged) track end. 14 files differ only in
  equal-tick event order (a tie segment's note-on, emitted at finalize
  time, now sorts after a same-tick CC it used to precede — no tick or
  value changes).

  **The verification's headline side-finding: 3,366 of the changed files
  now emit zero notes.** Their tie regions sit on tracks that never advance
  time — 300+ tied notes at one tick, no rests — so every segment is
  zero-length. Under tie's (correct) model that is what the commanded
  time base says; the real defect is the time base itself: those are
  exactly the tracks that play notes before any `0xC7`, where caesar's
  note-wait default (OFF) contradicts the engine's (ON). The tie work thus
  *exposed* the pre-existing default bug — see the note-wait entry above
  for the evidence chain and fix. With the default corrected, these
  regions become the rising swept notes the console actually plays
  (`SE_Map_WarpstarUp*` and kin).

- **Mod types above 2 no longer abort the sequence** (`Cseq.cpp`)
  (*2026-07-12*). Closes the "parse hard-errors on mod types above 2" known
  bug. NW4R stores the LFO target unvalidated and its routing if-chain
  applies no LFO to an out-of-range value, so the console plays such a file
  (LFO silent) where caesar's `Common::Error` refused it outright — a
  converter stricter than the hardware it models. The `0xCC` check is now a
  default-visible notice and the sequence converts, with the emit phase
  suppressing the pitch-vibrato CCs for the out-of-range target (the same
  wire behaviour as tremolo/auto-pan, and exactly the audible result of "no
  LFO"); a live CC1 is still zeroed on the retarget, with an out-of-range
  wording. The extended mod2–4 type checks are dropped entirely — that
  family is wholly unimplemented and already notices per execution. Zero
  corpus occurrences: the A/B is byte-identical with byte-identical stderr
  (exit 0).

- **`Rnd` values converted as the range midpoint instead of the first bound —
  the silent bias on 500k+ random-valued events** (`Cseq.cpp`)
  (*2026-07-12*). Closes the v0.5.1 Rnd-midpoint item. The `Rnd` prefix
  (`0xA0`) encodes two s16 bounds the engine rolls between per execution; a
  deterministic converter must pick one stand-in, and `ReadArgs` returned
  the raw pair with every consumer taking `Args[0]` — the *first bound*
  (the triage called it the minimum; the verification below found the pair
  is stored in file order and both orders occur, which the symmetric
  midpoint makes irrelevant). Census scale (2026-07-11 triage): 196k
  volumes, 177k pitch bends, 94k rest durations (timing!), 71k transposes
  silently biased toward one end of their ranges. The fix collapses the
  pair to `(a + b) / 2` (C++ truncation toward zero) at the single read
  site, so every consumer — including note durations and rests, which move
  absolute time — inherits the midpoint; the emit-side drop decisions keyed
  on `Suffix1` (lpf, init pan, mod type, loop count) are unaffected and
  keep dropping their unevaluated stand-ins.

  **Verification.** A/B over the 82-archive corpus: 257,097 files per side,
  none added or removed, every non-`.mid` file byte-identical; 5,135 `.mid`
  changed across 60 archives (stderr diffs are exactly the notice wording
  swap on 61 — the one extra, `safe.bcsar`, carries Rnd commands whose
  stand-ins land unchanged or on drop paths). An independent SMF parse of
  **every** changed pair (fresh stdlib parser, OLD side re-extracted with
  the cached baseline exe): **0 violations, 0 unresolved**. Note
  key/velocity changes: 0; note insertions/deletions: 0. The 18,413
  value-changed events sit entirely in the predicted set — 9,608 pitch
  bends, 2,983 CC7 volume, 1,907 CC11 expression, 1,884 CC6 RPN data
  (transpose/bend-range), 1,530 CC10 pan, 317 program changes, 136
  CC72/73/75 envelope, 29 CC1/76/78 mod, 12 GM master-volume SysEx, 5
  tempi, 2 CC5 — plus 16,410 tick-shifted events and 2,769 note-duration
  changes from `Rnd` rests/durations (file split: 3,607 value-only, 274
  timing-only, 1,254 mixed). The drop-path controls (CC74, init-pan
  combines, mod-type, loop counts) correctly appear in no diff, and zero
  events crossed the MIDI-validity boundary (no insertions/deletions
  anywhere). The 321 value *decreases* were all inverted-transform-verified
  as unsorted-pair cases (first bound > second), not anomalies.

- **The two wrong-arg-count desync hazards closed; guessed non-opcodes now
  fail fast** (`Cseq.cpp`) (*2026-07-12*). Closes the v0.5.1 desync item and
  the "unknown-opcode bytes are swallowed" known bug in one parse-safety pass.
  Every hazard here has **zero corpus occurrences** — these are latent
  misframe bombs, not observed breakage — which is why the whole change is
  verifiably byte-identical.

  **(a) The `_t` trailing duration.** The Time suffix (`0xA3`/`0xA4`/`0xA5`)
  appends an s16 ramp duration (or its Rnd/Var form) after the command's own
  arguments, for *any* command the prefix byte can precede — but the parser
  consumed it only inside the `0xB0–0xDF` branch. A `_t` on a note, tempo,
  sweep, or extended command left those bytes unread, and every later command
  in the track misframed. The consumption is hoisted to run after the whole
  command dispatch (error paths still return first), so it now applies to
  every command form; stream position is unchanged for the ~473k corpus `_t`
  commands, which all sit in the safe range.

  **(b) The bare 1-byte reads.** `0xB2`/`0xBF`/`0xC7`/`0xC8`/`0xC9`/`0xCE`/
  `0xDF` — plus `0xCC` in its own branch, `0xD6` (which always read
  `ArgType::Var`), and the extended mod-types `0xA4`/`0xAA`/`0xB0` — read
  their argument with a raw 1-byte read that ignored `cmd.Arg1`, so a
  `Rnd`-prefixed command in that set consumed 1 byte where the stream carries
  a 4-byte range: the same misframe. All of them now route through the
  prefix-aware `ReadArgs`, with defaults preserved (`Uint8`, `Var` for
  `0xD6`), so literal arguments read the identical byte. The mod-type `> 2`
  validation applies only to literal arguments now — an unevaluated Rnd/Var
  stand-in is not a target byte, and the emit phase already notices it.

  **(c) Fail fast on bytes that are not commands.** `0x90`/`0x96` (the
  original author's 2-byte `Analyse` probes) and `0xB7–0xBC` (a 1-byte
  catch-all) are not CTR opcodes — the plain command map jumps `0xB6 → 0xBD`,
  re-verified against `CtrCafe.cs` for this change. If one ever appears, the
  parser has already desynced upstream, and consuming a guessed length only
  perpetuates the misframe with plausible-looking garbage; they now hit
  `Common::Error` like any other unknown byte.

  **(d) The ramps themselves surface.** A `_t` ramp commands a glide from the
  parameter's current value to the target over the duration; caesar emits the
  target at the command tick (375,316 volume fades, 76,362 pan sweeps, 10,725
  pitch-bend ramps corpus-wide — the single largest fidelity gap, previously
  completely silent). Each execution now emits a "ramped (_t) change flattened
  to an instant jump" notice; real interpolation stays stage-2/5 flattening
  work.

  **Verification.** A/B over the 82-archive corpus: 257,097 files per side,
  none added or removed, every one byte-identical; stderr gained only the new
  ramp-flatten notices. The hazard fixes themselves are exercised by no corpus
  file (as expected), so their proof is the unchanged stream position for
  every literal argument plus inspection.

- **Warning-hygiene pass over the drop sites — the converter no longer drops
  anything silently** (`Cseq.cpp`) (*2026-07-12*). Closes the v0.5.1
  warning-hygiene item, implementing the 2026-07-11 triage's census-ranked
  demote/surface calls.

  **The dead extended-command chain.** The walk's entire `Extended` branch —
  42 "not implemented" warnings covering `setvar`/`cmp`/`mod2–4`/`userproc` —
  was dead code: parse recorded `Cmd = 0xF0` and never the extended opcode
  behind it, so no branch could match and 353k `setvar` + 210k `cmp_eq` +
  64k `addvar` (and the rest) vanished with no trace. Parse now records the
  extended opcode (safe because every consumer — `ReachableNotes`,
  `collectEntryTracks`, both emit chains — branches on `Extended` first),
  and the 42-branch chain is replaced by one name table in CtrCafe byte
  order — which also fixes the mod4 labels (`0xAC`–`0xB1`), scrambled
  (rotated by one, `range` first instead of last) against the authoritative
  map. The corrected order, the `0xDE` = FxSendC identity, and the
  0x90/0x96/0xB7–0xBC "not real plain opcodes" claim were each re-verified
  directly against `Gota7/GotaSequenceLib CtrCafe.cs` for this change.

  **Demotions (census verdicts).** `span` (55,291 occurrences — the #1
  warning corpus-wide), `priority` (3,161) and `front bypass` (7,129) are
  demoted to benign "no MIDI equivalent" notices: span is the front/rear
  axis of the DSP's quad voice-gain matrix — console-confirmed audible under
  the Surround output mode, but MIDI has no surround axis in any mode —
  priority is voice-steal scheduling state, and front bypass is
  Surround-path routing.

  **Default-visible categories for every remaining drop.** Every bare
  (`-w`-only) warning at a drop site gained a notice category: sustain
  level, tie mode, bank select, biquad type/value, envelope hold, mute,
  velocity range, mod phase/curve/period, print var, main send, envelope
  reset, conditional-jump skips, out-of-range jump targets.

  **The silent manglings now surface per execution.** `Rnd`-valued
  arguments (converted as the range minimum), `Var`-valued arguments
  (converted as the variable *index* — garbage), and `[If]`-prefixed
  non-jump commands (executed unconditionally — including the 33k
  conditional `Return`s that can truncate tracks) each get a notice at
  every execution, so the biggest known fidelity gaps are visible in every
  affected extraction instead of only in the triage document.

  **The catch-alls.** `0xDE` FxSendC (a real CTR command — third aux send,
  no GM equivalent) gets an explicit notice, and both the plain and extended
  chains end in a final `else` that names any parsed-but-unwired opcode, so
  a future gap can never be silent again.

  **Verification.** A/B over the 82-archive corpus: 257,097 files per side,
  none added or removed, every one byte-identical
  (`.mid`/`.sf2`/`.wav`/`.log`/raw dumps); stderr notice summaries changed
  on 66 archives — the intended (and only) effect.

- **Vibrato CCs gated on the `0xCC` LFO target — tremolo and auto-pan no
  longer render as pitch wobble** (`Cseq.cpp`) (*2026-07-12*). Closes the
  v0.5.1 "gate the vibrato CCs on mod type" item. The track LFO is one
  retargetable oscillator: `0xCC` routes it to pitch (0, the engine default),
  volume (1, tremolo) or pan (2, auto-pan). caesar emitted the pitch-vibrato
  CCs (CC1 mod depth, CC76 rate, CC77 range, CC78 delay) unconditionally, so
  a tremolo or auto-pan span played as pitch vibrato — CC1 drives an audible
  ±up-to-50-cent wobble through the SF2 2.01 §8.4 default mod-wheel modulator,
  which FluidSynth implements — i.e. qualitatively the wrong effect, not a
  miscalibrated right one.

  **Ground truth (NW4R decomp, ogws — the established NW4C lineage).**
  `LfoTarget { PITCH, VOLUME, PAN }` = 0/1/2; `MML_SET_LFOTARGET` stores the
  raw byte into a `lfoTarget` field *separate from* the `LfoParam` block
  (depth/speed/range/delay), so parameters persist untouched across a
  retarget — which is what makes restoring them on a return to pitch faithful
  rather than invented. Track defaults are target=pitch, depth=0 (no LFO
  until a `0xCA`), range=1, speed=6.25 Hz, delay=0; param changes propagate
  to sounding voices every frame, and phase/delay restart per note-on. The
  routing applies depth × range in *cents* to pitch, dB-domain amplitude to
  volume, and an additive pan sweep — so types 1/2 are genuinely different
  effects with no static-CC MIDI equivalent (CC92 "tremolo depth" is inert in
  FluidSynth-class players, same as CC76–78; a real auto-pan needs a
  synthesized CC10 stream, which is stage-2 player territory).

  **The census (an instrumented build over all 82 archives).** 8,006 literal
  `0xCC` executions in 64 archives — an exact match to the triage figure,
  confirming that number counted mod-type *commands*, not affected CCs. The
  split: 4,986 set pitch (overwhelmingly redundant init-time re-assertions of
  the default), 1,828 tremolo, 1,192 auto-pan — 38% select a non-pitch
  target. Zero are `Rnd`/`Var`-prefixed; 98% fire before the track's first
  note (501 after, only 156 of them value-changing), so the target is in
  practice a static per-track patch attribute. The content being mis-rendered
  is *strong*: mean commanded depth inside tremolo/auto-pan spans is 41/127,
  versus 24/127 on pitch spans. Two numbers drove the design: **70% of
  switches away from pitch (1,961 of 2,804) happen with a nonzero CC1 already
  on the wire** — suppression alone leaves that stale CC1 wobbling forever,
  so the CC1=0 write is load-bearing, not a safety net — and exactly **one**
  track in the whole corpus ever returns to pitch, so the restore logic is
  nearly theoretical but cheap.

  **The fix.** A per-track `trackModType` (default 0 = the engine default)
  tracked in walk order exactly like the existing `trackPan` state, reset at
  every track boundary; wire/shadow value pairs per CC. While the target is
  volume/pan: the four commands update their shadows but emit nothing, each
  surfacing a default-visible "pitch-vibrato CCs suppressed (track LFO
  targets volume/pan)" notice. Leaving pitch with a live nonzero CC1 writes
  CC1=0 (plus a "tremolo/auto-pan LFO dropped (no MIDI equivalent)" notice);
  returning to pitch re-emits any shadow that differs from the wire. An
  unevaluated `Rnd`/`Var` mod type never latches (its parse also shares the
  fixed-1-byte desync hazard now folded into the ROADMAP's wrong-arg-count
  item). Only literal args update shadows, so stand-ins are never persisted.

  **Verification.** A/B over the 82-archive corpus, 257,097 files per side,
  none added or removed; every `.sf2`, `.wav`, `.log` and raw dump
  byte-identical. 2,146 `.mid` differ across 58 archives (census predicted
  2,141 files, 58 archives). An independent SMF parse of **every** differing
  pair classifies every event-level change with zero unexpected diffs:
  removed CCs — 1,242 CC1, 1,509 CC76, 822 CC77, 685 CC78 — plus 1,971
  inserted CC1=0, and no note, timing, or other-event change anywhere. Each
  removal count reconciles with the census's 4,260 suppressions: the corpus's
  single return-to-pitch case (`safe.bcsar` `BANK_4/SEQ_1.mid`, an init-time
  `pitch → auto-pan → pitch` sequence entirely at tick 0) restores its CC1
  and CC76 byte-identically at the same tick, cancelling one CA and one CB
  removal — the engine-faithful restore converging back to the old bytes on
  its own is exactly the behaviour the persistence model predicts. The
  CC1-zero count exceeds the census's 1,961 because the census heuristic
  counted only literal prior depths, while the implementation zeroes based on
  what is actually on the wire (including emitted `Rnd`/`Var` stand-ins) —
  the wire-accurate superset.

- **`0xB2` mono/poly demoted from CC126/CC127 to an honest notice — the
  Channel-Mode note-killer** (`Cseq.cpp`) (*2026-07-12*). Closes the v0.5.1
  "stop `0xB2` from silencing the channel" item. The engine's command is a
  per-track *voice-allocation* flag — mono means a new note steals the track's
  previous voice, and toggling it does nothing to already-sounding voices. The
  CCs caesar emitted are not that: CC126/CC127 are Channel **Mode** messages,
  and the MIDI 1.0 spec mandates an implicit All Notes Off on every one of
  CC124–127, so each mid-track toggle chops the channel's ringing notes.

  **The census (the roadmap's decision criterion).** An instrumented build
  logged every `0xB2` execution across the 82-archive corpus: **249
  executions — 56 mid-track, 35 of them after the track had already sounded
  notes**, so the "demote rather than emit a note-killer" branch is the one
  the roadmap's own rule selects. The argument is a 0/127 two-state like the
  damper's (values observed: 127 "mono on" ×160, 0 "poly off" ×89; never
  Rnd/Var-prefixed). The users are concentrated: Mii Plaza's `mgCar.bcsar`
  racing minigame (189 executions), `cplay.bcsar` (20 per dump),
  `MeetSound.bcsar` (4 per dump), Kirby Team Clash and the Pokémon Sun family
  (1 each).

  **The player-side finding that killed the "fix the value" alternative.** The
  fallback plan — keep the emission for track-header inits, correcting the
  data byte (the old code sent CC126 value 0, the "as many channels as
  available" legacy form, where a single mono channel should send 1) — fails
  against how real players implement these messages. FluidSynth (the
  project's reference player class) honours CC126/127 **only on a basic
  channel** (default: channel 0), where they do not act on that channel alone
  — they reconfigure the poly/mono layout of the whole channel *group*, and
  channels left outside any group are **disabled outright**. 49 of the 249
  corpus executions sit on track 0 = channel 0, 25 of them the mono form: on
  FluidSynth 2.x each of those could silence every other channel in the file,
  even when "safely" emitted at tick 0. Players that don't implement basic
  channels ignore CC126/127 entirely. So the choice was between a message
  that is ignored, kills notes, or mutes the rest of the file — and a notice.

  **An aside for the record.** The Wii-era NW4R MML has no mono/poly command
  at all — the ogws decomp's `MmlParser` dispatch and command enum skip
  straight past `0xB2` — so it is a CTR/Cafe addition, consistent with
  `CtrCafe.cs` (not the NW4R decomps) being the byte-map authority for this
  command. Its engine-side semantics stay on the stage-2 player's plate,
  where the flag is read from the sequence itself; nothing is lost for the
  suite by dropping the CC.

  **Verification.** A/B over the 82-archive corpus, 257,097 files per side,
  none added or removed. Every `.sf2`, `.wav`, `.log` and raw dump
  **byte-identical**; 53 `.mid` differ, spread over the same 11 archives the
  census flagged. An independent SMF parse of every differing pair confirms
  the only change is the removal of CC126/CC127 events — 249 removed (160
  CC126, 89 CC127), a one-to-one match with the census's 249 executions — no
  note, timing, or other-event change anywhere, and no SMF track appears or
  disappears. The drop is surfaced by a new default-visible "mono/poly
  dropped (no MIDI equivalent)" notice.

- **Init pan (`0xDC`) and LPF cutoff (`0xD8`) implemented — and *both* of the
  triage's prescriptions were wrong** (`Cseq.cpp`) (*2026-07-12*). Closes the
  v0.5.1 "two dropped commands with clean MIDI targets" item. The roadmap called
  for "one line each, mirroring the existing pan / FX-send handlers": `0xDC` →
  CC10 raw ("exact mapping"), `0xD8` → CC74 raw ("near-identity 0–127"). Neither
  survived contact with the engine. As with the `0xDF` damper phantom, the
  argument semantics had been *assumed*, not read.

  **The `0xDC` clobber.** The triage's own table said "engine sums init_pan+pan"
  and, in the same sentence, called a raw CC10 write "exact" — an internal
  contradiction nobody had cashed out. MIDI has exactly one CC10 register per
  channel and caesar already writes `0xC0` pan to it, so a second independent
  raw CC10 stream is *last-writer-wins*: whichever command comes second wins and
  the other is silently lost. The engine instead treats the two as **additive
  offsets from centre** — NW4R's MML parser reads them with the *identical*
  conversion (`pan = arg - 64`, `initPan = arg - 64`) and sums both into the
  voice's pan, along with a third term, the bank note's own pan. So `pan=32`
  with `init_pan=32` is hard left on hardware (−32/63 + −32/63 → clamped −1.0),
  where a raw emit gives half-left.

  **Why the pan sum is exact, not an approximation.** caesar already exports the
  note term as the SF2 `kPan` generator (`Cbnk.cpp`: `(pan - 64) * (500/63)` —
  literally the engine's `(instInfo.pan - 64) / 63` rescaled), and a SoundFont
  player *sums* `kPan` with CC10 at the generator summing node. That is the
  engine's own additive structure, so CC10 must carry exactly the other two
  terms and must not re-fold in the note pan (which would double-count it).
  `Cbnk` needed no change. The new `combinePan` emits
  `clamp(pan + initPan - 64, 0, 127)`; because `initPan` defaults to 64, this
  collapses to `pan` on every track that never sends `0xDC`, which is what makes
  the change byte-identical everywhere else and gives the A/B a falsifiable
  prediction.

  **The `0xD8` invented brightening.** "Neutral 64" was right (`InitParam` starts
  `lpfFreq` at 64), but "near-identity 0–127" was not: the value scales the
  voice's cutoff by `value / 64`, and `Voice::SetLpfFreq` **clamps that scale to
  [0,1]**. Every byte ≥ 64 is therefore bit-identical to 64 — the command can
  only *darken*, never brighten. CC74 above 64, by contrast, tells a synth to
  brighten past the sample's own tone, so a raw pass-through would have
  manufactured treble the console never produced, on the 261 corpus commands
  that sit above 64. caesar clamps to the engine's own ceiling instead. The
  residual approximation is the curve, not the direction: hardware steps 187.5
  cents per unit (an exponential 31.25 Hz – 32 kHz sweep) against GM2's ~150, so
  a cut reads about 20% shallow — a relative control can carry the direction,
  the neutral point and both end stops, and it does.

  **The CSEQ command dispatcher, found at last.** The evidence chain ran on the
  actual 3DS binary, not only the Wii decomps: `MmlParser::CommandProc` lives at
  `0x2E32D4` in MiiPlaza's `code.bin` (load base `0x00100000`), pre-computing
  `arg - 0x40` at `0x2E3304`. The `0xDC` case (`0x2E35F0`) is a bare
  `strb r1, [r4, #0x6b]` → SeqTrack+0x87; the `0xC0` case (`0x2E35D0`) writes a
  *different* field (+0x72); `Channel::Update` (`0x149FC0`) adds them with
  `vadd.f32 s16, s2, s3`. The `0xD8` case (`0x2E3768`) multiplies by
  `0x3C800000` = 1/64. This is the sequence runtime that the disasm handoff's
  Session 3 listed as the next dig and never reached — worth carrying into
  `NW4C-disasm-handoff.md` and into suite stage 2.

  Two incidental findings: `0xDC`'s CTR handler is a plain `strb` that ignores
  any `_t` ramp length, so a time-suffixed init_pan is an instant jump **on
  hardware too** — caesar's instant emit is exact there, not a flattening. And
  an unevaluated `Rnd`/`Var`-prefixed `0xDC` now drops with a notice rather than
  emitting: unlike other commands, baking in a range-minimum or variable index
  would poison the *combined* pan for every later note on the track, not just
  its own event.

  **Verification.** A/B over the 82-archive corpus, 216,485 files per side, none
  added or removed. Every `.sf2` (6,059), `.wav` (64,580), `.log` and raw dump
  **byte-identical**; 2,385 `.mid` differ and 31,266 do not. An independent SMF
  parse of all 2,385 confirms **every difference is a CC10 or CC74 event** — no
  note, timing, or other-event change anywhere (3,448 CC74 added, 4,664 CC10
  added, 99 CC10 values changed by the pan combination). The differing-file count
  reconciles exactly with a control-flow-scoped corpus census: 787 init_pan files
  + 1,917 lpf files − 319 overlap = 2,385.

  **The census also refuted the "common case" claim** the roadmap leaned on.
  "Set-once-before-notes" is not the common case: init_pan fires **mid-track,
  after notes have already sounded, in 6,209 of 8,438 uses (74%)**. It is used as
  a live pan move, and its values cluster on real stereo positions (64 centre,
  then 32/48/80/96) rather than sitting at centre. The one place the export stays
  approximate is a consequence of MIDI, not of this fix: hardware *latches*
  init_pan at note-on and never moves a sounding note, whereas CC10 moves the
  whole channel. MIDI has no per-note pan, so the tick is the honest place to put
  it.

- **Damper pedal (`0xDF`): the reported bug did not exist — the *premise* was
  the bug** (`Cseq.cpp`, the `0xDF` handler) (*investigated and hardened
  2026-07-12*). Closes the v0.5.1 damper work item, but not the way that item
  described. **The claim under test** (from the 2026-07-11 triage, since
  retracted): the argument is a Bool (0/1) written raw to CC64, and since CC64
  is a threshold control (< 64 = pedal up), "damper on" emitted as `1` reads as
  pedal *up* — the pedal never engages and ringing notes get cut. The
  prescribed fix was a one-liner mirroring the sibling bool `0xCE`:
  `Args[0] ? 127 : 0`. **The premise was refuted on two independent axes before
  the line was touched.** *Engine side*: the "Bool" typing traces to
  GotaSequenceLib's `SequenceCommand.cs` argument table — but its player never
  executes Damper (it sits in the `//Not implemented.` fallthrough), so the
  typing is an untested modelling guess. The Wii RSEQ command map is
  byte-identical to CtrCafe's over `0xC0–0xE3`, making the NW4R decomps
  directly authoritative, and two *matching* decomps (doldecomp/ogws and
  zeldaret/ss, both of which recompile to Nintendo's shipped bytes) read the
  command as `rTrackParam.damperFlag = static_cast<u8>(arg1) >= 64` — a full
  `u8` argument thresholded at 64, **exactly the MIDI CC64 rule**, not a bool.
  (`snd_SeqTrack.cpp` confirms the semantics are a true CC64 hold: a channel
  whose note length has expired is not released while `damperFlag` is set.)
  *Corpus side*: the baseline extraction wrote `Args[0]` straight to CC64, so
  every CC64 value in a baseline `.mid` **is** the raw argument. Across all
  41,235 corpus `.mid`, 20 files carry damper at all, for **1,548 events whose
  value is only ever 0 (786×) or 127 (762×)** — never 1, and **zero occurrences
  in the disputed 1–63 range**. Nintendo's tooling emits MIDI-style pedal
  values. So the raw pass-through was *already correct* (the engine's ≥ 64 rule
  and a GM synth's ≥ 64 rule are the same rule), the pedal did engage, and the
  prescribed `? 127 : 0` would have **inverted** args 1–63 — emitting pedal-down
  where hardware leaves it up. **What shipped instead** is the engine's own
  threshold, `(Args[0] >= 64) ? 127 : 0`, which is exact across the argument's
  entire `Uint8` 0–255 domain and closes a real (if unobserved) latent hole: an
  argument above 127 is pedal-**down** on hardware, but the libsmfc writer
  silently drops any control value outside 7-bit range, so that pedal event
  vanished. The normalized value is 7-bit by construction, so the `emitCtrl`
  drop-guard is no longer needed. **Verification, in two parts.** (1) The
  full-corpus byte-identical A/B (82 archives, 257,097 compared files per tree):
  **zero differing files** — no `.mid`, no `.sf2`/`.wav`/raw dump, none added or
  removed, and all 82 console logs identical down to the notice counts. That is
  the expected result, and it doubles as proof that no corpus damper argument
  lies outside {0, 127}. (2) Because the corpus therefore never *exercises* the
  new branch, the untested bands were driven directly: the three damper-on
  arguments inside `Torte.bcsar`'s `SE_BossMb_Tornade` sequence were patched
  in-place (the blob was located by matching caesar's own raw `.bcseq` dump back
  into the archive, so the offsets are exact) and both binaries re-run. With the
  real value 127, old and new agree exactly (`CC64 = {0:3, 127:3}`). Patched to
  **30** (hardware: pedal *up*), old emits `CC64 = 30` and new emits `CC64 = 0` —
  same pedal-up result, and the *prescribed* `? 127 : 0` would have emitted 127,
  **pedal down, inverted**. Patched to **200** (hardware: pedal *down*), the old
  build **dropped the events entirely** ("2 MIDI control/parameter events dropped
  (value out of range)") and the pedal silently vanished, while the new build
  emits `CC64 = 127`. Note-on counts were identical (23,728) across all six runs,
  confirming the patch landed on real damper arguments and desynced nothing. The
  companion audit of the same bug *class* — a sequence argument whose domain
  does not match its MIDI controller's domain — found **no siblings**: of the
  MIDI switch controllers (CC64–69) caesar only ever emits CC64 and CC65, and
  CC65 (`0xCE` portamento) is a genuine bool, correctly normalized. It did
  surface an adjacent defect of a *different* class (`0xB2` mono/poly emits
  Channel Mode messages that carry a mandated All Notes Off), now filed in
  ROADMAP.md.
- **Finite loop repeat counts discarded — `0xD4`/`0xFC` loops always emitted as
  infinite** (`Cseq.cpp`, the `0xD4`/`0xFC` handlers) (*fixed 2026-07-11*).
  Implements the finite-loop-count v0.5.1 work item. `0xD4` (loop start)
  hardcoded EMIDI **CC116 = 0** and `0xFC` (loop end) emitted **CC117 = 0**. In
  the EMIDI convention CC116 = 0 means *loop forever*, so an EMIDI-aware player
  replays a finite "play N×" section endlessly; CC117's value should be the
  spec's fixed 127, not 0. **Semantics were pinned from primary sources on both
  sides before touching the line.** CTR side — GotaSequenceLib
  `Playback/Player.cs` (whose `CtrCafe.cs` is the authoritative CTR byte map)
  and Kermalis VGMusicStudio use byte-identical logic: `LoopStart` stores the
  U8 count on the call stack; `LoopEnd` does
  `if (count != 0) { count--; if (count == 0) { pop; break; } } <jump back>`,
  so the count is **total-plays** (1 → play once, 2 → twice) and **0 is the
  infinite sentinel** (it is never decremented) — no off-by-one. EMIDI side —
  the Apogee Expanded MIDI v1.1 spec defines CC116 as "0 = infinite, 1 = loop
  once, 2 = loop twice, x = loop x times" (total-plays) and CC117 as a
  value-less marker fixed at 127; libADLMIDI / BW_Midi_Sequencer, the reference
  EMIDI engine, honours the count with `infinity = (value == 0)` and decrements
  to N plays. The two conventions line up 1:1, so `0xD4`'s count now passes
  straight through to CC116 (0 → 0 = infinite → infinite) and `0xFC` emits
  CC117 = 127. **Honest scope limit:** only EMIDI/XMI-aware players (the
  libADLMIDI family, ZDoom) *honour* the finite count — precisely the
  "EMIDI-aware player" the bug describes; marker-only players such as
  foobar2000's foo_midi read the loop points but ignore the value, so they
  looped regardless before and are unchanged now. (The future stage-2 player
  reads the count natively, not through MIDI.) The count is a U8 (0–255) while
  a MIDI CC is 7-bit and the libsmfc writer silently drops an event whose value
  exceeds 127 — which would lose the loop-start marker outright — so counts
  ≥ 128 clamp to 127 (the maximum finite "many times", closer to intent than
  flipping back to 0 = infinite) with a new default-visible "loop repeat counts
  clamped to 127" approximation notice, mirroring the plain-controller clamp; a
  `Rnd`/`Var`-prefixed count is not yet evaluable, so it keeps the old 0
  (= forever) stand-in. The EMIDI/CTR semantics were cross-checked by a research
  fan-out (Apogee spec + libADLMIDI/foo_midi source) and two independent
  adversarial verifiers (both reading the RE'd player sources), returning
  "supported" on the pass-through and "0 = infinite" claims. Verified with the
  full-corpus byte-identical A/B (82 archives, 257,261 files per tree, all runs
  exit 0): **exactly 3,415 `.mid` files differ and nothing else** — zero
  `.sf2`/`.wav`/`.log`/raw-dump changes, zero files added or removed. An
  independent SMF byte parser proved every difference sits at a CC116 or CC117
  value slot (old `0x00` → new count ≤ 127 for CC116 / exactly `0x7F` for
  CC117): **2,812 CC116 count-byte changes and 6,863 CC117 → 127 changes, with
  zero file-size changes and zero off-target bytes**. Emitted CC116 counts are
  realistically small (modes at 2, 5 and 10). The ≥ 128 clamp fired 23 times
  across five archives (Animal Crossing's `GardenSound` plus four Kirby titles —
  Team Clash, Fighters Deluxe, Planet Robobot, Triple Deluxe), surfaced by the
  new notice; a separate rebuild-and-re-run confirmed that adding the notice
  leaves all 41,235 corpus `.mid` byte-identical (the notice is stderr-only).
  Fully unrolling repeats into the timeline is deferred to the stage-2/5
  flattening machinery.
- **The two mis-wired vibrato/pitch controls: `0xE3` sweep pitch and `0xE0`
  mod delay** (`Cseq.cpp`, the `0xE0`/`0xE3` handlers) (*fixed 2026-07-11*).
  Implements the second v0.5.1 work item; both commands were emitted as CC78
  "vibrato delay". `0xE3` is SweepPitch — a signed-16 intra-note pitch ramp in
  1/64-semitone units that glides from the offset to the note's nominal pitch
  (confirmed against GotaSequenceLib's sweep implementation), independent of
  and *additive with* the portamento commands — so CC78 was doubly wrong:
  mis-targeted, and sweeps of two semitones or more (|value| ≥ 128) also fell
  out of MIDI range, forming the bulk of the census's 1,020 out-of-range
  drops. No static CC expresses a per-note pitch ramp (the faithful form is a
  pitch-bend ramp — stage-2 flattening territory; a one-shot bend at note-on
  was considered and rejected: channel-global, needs resets, collides with
  real `0xC4` bends), so it now drops with a default-visible "sweep pitch
  dropped (no MIDI equivalent)" notice. `0xE0` is ModDelay — the per-note
  delay before the track LFO engages. The research pass *corrected the
  triage's unit claim*: the argument is in **5 ms units**, not milliseconds
  (NW4R decomp `snd_MmlParser.cpp`: `lfoParam.delay = arg * 5`, accumulated
  against real milliseconds in `snd_Lfo.cpp`; NW4C is its documented port, so
  the exact ×5 is Wii-confirmed, 3DS-presumed — the mapping's shape survives
  either constant). An instrumented corpus census (4,780 plain events over 40
  archives) found median arg = 1 (5 ms), p90 = 25 (125 ms), p99 = 100
  (500 ms), max = 230 (1,150 ms). The old `(x/2)+64` treated the time value
  as a signed ±64 parameter — meaningless — and pushed delays ≥ 640 ms out of
  MIDI range entirely. New mapping: `CC78 = 64 + min(ms·63/1000, 63)` with
  `ms = max(arg, 0)·5`. CC78 is a GM2/XG *relative* control (64 = patch
  default), and caesar's SF2s program no LFO delay, so 64 is the honest 0 ms
  baseline; the delay scales into the upper half, saturating at 1 s (above
  the corpus p99); delays ≤ 15 ms — including the corpus median — collapse to
  the neutral 64, far below onset-perception thresholds. Rnd/Var-prefixed
  `0xE0` keeps the old path bit-identically (3 corpus events, values 0–1) so
  unevaluated stand-ins still drop when out of range rather than being scaled
  into plausible-looking delays. A 13-agent adversarial review (3 lenses,
  every finding independently verified) returned zero confirmed findings.
  Verified with the full-corpus A/B: 257,097 files per tree, all 164 runs
  exit 0; exactly 1,772 `.mid` files differ and nothing else; an independent
  SMF event parser proved every difference is CC78-only — 86 events removed
  (the bogus `0xE3` emissions), 2,233 rescaled, 5 added (the ≥ 640 ms delays
  the old transform dropped) — matching the census prediction
  event-for-event, with all note streams untouched. The notice ledger
  reconciles exactly: "out of range" drops fell 1,020 → 230 (the remainder is
  unevaluated `Rnd`/`Var` garbage, correct until the convert-time VM), and
  all 871 sweep-pitch commands now surface under the honest new category.
  Side finding filed under Known bugs: `0xC9` portamento may under-serve the
  engine's "also turns portamento on" semantics, pending CC84-interpretation
  checks.
- **Out-of-range controller values dropped instead of clamped, and a zero-BPM
  tempo UB** (`Cseq.cpp`, the plain-controller emit sites and the `0xE1`
  handler) (*fixed 2026-07-11*). Implements the first v0.5.1 triage work item.
  A plain (un-prefixed) `Uint8` argument to pan (`0xC0`), volume (`0xC1`),
  master volume (`0xC2`), or expression (`0xD5`) is genuine 0–255 sequence
  data, but the libsmfc writer rejects anything above 127, so such values
  vanished with a drop notice. They now clamp to 127 and emit, surfaced by a
  default-visible "clamped" approximation notice; `Rnd`/`Var`-prefixed
  arguments (whose value is an unevaluated stand-in — the range minimum or the
  variable index) keep the honest drop-with-notice path until the convert-time
  VM lands. `Suffix1 == None` is the discriminator, verified sound: only the
  `0xA0`/`0xA1` prefixes set it, the Time and `[If]` prefixes live in other
  fields, and for Time-suffixed ramps `Args[0]` is still the value (the
  duration is appended after). The `0xE1` tempo handler gained a `bpm > 0`
  guard: the argument decodes as signed 16-bit, and `bpm == 0` made the
  vendored `libsmfcx.c` compute `60000000 / 0.0` = infinity, whose `int` cast
  is UB (benign in practice — on MSVC/x86-64 it lands on `INT_MIN` and fails
  the writer's later range check — but UB nonetheless); the guard closes it
  caller-side, keeping the vendored copy pristine, and routes the drop through
  the same notice. A 15-agent adversarial review (4 lenses, every finding
  independently verified) confirmed the design — clamp-direction is
  hardware-correct (masking `& 0x7F` would flip loud values quiet, and a
  0–255→0–127 rescale would fabricate a scale these 7-bit commands don't
  have), the four sites are exactly the sanctioned ones, and the guard is
  output-preserving for every representable input — and produced one applied
  cosmetic fix (the notice says "control/parameter" to match the `emitCtrl`
  house style, since master volume is a SysEx, not a control change).
  Verified with a byte-identical old-vs-new A/B over all 82 corpus archives:
  257,097 files per tree, **zero** diffs, all 164 runs exit 0, all stderr
  notice summaries identical. No real archive carries a plain out-of-range
  value for these four commands (corroborating the census: the ~1,020
  out-of-range drops are all `0xE3`/`Rnd`/`Var`-sourced), so the change is
  pure hardening today and only alters output for hypothetical future inputs.
- **GM percussion-channel collision: melodic 10th tracks rendered as drums**
  (`Cseq.cpp`, the sequence-to-MIDI walk) (*fixed 2026-07-10*). Every emitted
  MIDI event used the CSEQ track index directly as the MIDI channel — the channel
  and SMF-track arguments of every `smfInsert*` were passed the same `track`.
  Channel 9 is the GM/GS percussion channel, so a sequence's 10th track (index 9)
  played as a drum kit or, since caesar SF2s carry no bank-128 drum preset, as
  silence. The channel is now decoupled from the SMF track number (which stays
  equal to the index, so the file's track layout is unchanged): `channelOf[]` is
  the identity map except track 9, which relocates to the lowest channel the
  entry leaves free. Only when the entry genuinely uses all 16 tracks — no free
  channel — does track 9 stay on channel 9, and then a Roland GS "Use for Rhythm
  Part: OFF" SysEx for part 10 (`F0 41 10 42 12 40 10 15 00 1B F7`) is emitted
  lazily, just before that track's first note, so GS-aware players (FluidSynth)
  treat channel 9 melodically. The tracks an entry uses are gathered by
  control-flow reachability from the entry's own start offset
  (`collectEntryTracks`), **not** a scan of the shared bank's whole command map:
  the first attempt used the bank-wide scan and the full-corpus A/B caught the
  regression it caused — a one-track sound-effect entry sharing a bank whose
  siblings collectively open all 16 tracks was told "16 tracks used," emitting a
  spurious rhythm-off SysEx that materialised nine empty SMF tracks (a 1-track
  MIDI became 10). An independent 13-agent adversarial code review reached the
  same root cause in parallel and confirmed the rest of the change (identity map,
  channel kept in 0–15 so the port stays 0, master-volume/tempo/meta/marker/
  end-timing calls left on the SMF-track argument) was sound. Verified with a
  byte-identical old-vs-new A/B over all 81 corpus archives (241,893 files):
  every `.sf2`/`.wav`/`.log`/raw dump is byte-identical, no file is added or
  removed, and of 37,864 MIDIs the 1,004 that changed are each identical to the
  old output except that channel 9's events moved to a free channel — verified
  event-by-event by an independent SMF parser (only channel 9 ever remaps,
  note-on totals preserved). 85,111 melodic note-ons were rescued off the drum
  channel and 87 all-16-track sequences took the GS-SysEx path. Ear repro:
  `GardenSound\BANK_BGM_IND_MUSEUM\SEQ_BGM_IND_MUSEUM.mid`, whose 65 channel-9
  notes now play on channel 13.
- **`OpenTrack` index out-of-bounds write** (`Cseq.cpp`, the `0x88` handler)
  (*fixed 2026-07-11*). The handler stored a sibling track's start offset as
  `trackOffsets[Args[0]]`, but `Args[0]` is a full byte while the array holds 16
  entries; an index ≥ 16 wrote past it onto the stack. Found while answering
  "does any sequence use more than 16 tracks?" during the channel-collision work:
  an instrumented pass over all 81 archives found **zero** `OpenTrack` opcodes
  with an index ≥ 16, confirming the format's 16-track cap (the `0xFE` enable
  mask is 16-bit) and that the write is safe on every real archive — but a
  malformed one could corrupt the stack. Now guarded like the other bounds
  checks from the v0.5.0 hardening pass, emitting a default-visible
  "OpenTrack index out of range" notice instead. Output-identical on the corpus
  (the guarded branch is never taken).
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
- **The `0x8A`/`0xFD` call stack was shared across all 16 tracks; now cleared
  per track** (`Cseq.cpp`) (*2026-07-12*). Found by the 2026-07-12 pre-VM audit.
  `sp`, the Call/Return stack in `Cseq::Convert`, was one `stack<uint32_t>` for
  the whole sequence, and `advanceToNextTrack` — which resets `absTime`,
  `noteWait`, `trackHasNote`, pan, mod type, tie state, `modShadow`/`modWire`
  and `offsetTime` at each track boundary — never touched it. A track that ended
  (`Fin`, a whole-song loop-back, or a stray `Return`) while a `0x8A` Call frame
  was still on the stack left that frame behind for the next track. The next
  track's first *unbalanced* `0xFD` Return then took the `!sp.empty()` branch and
  jumped to the leaked return address — inside the **previous** track's code —
  replaying it under the new track's index and channel, silently (the "Return
  with empty call stack; ending track" notice only fires when the stack *is*
  empty). The 3DS engine keeps the call stack per-track (NW4R
  `callStack[]`/`callStackDepth`), so such a frame can never cross a track
  boundary on hardware.

  **The fix (one commit).** (1) `advanceToNextTrack` now clears `sp` (`sp = {};`)
  alongside the other per-track resets. (2) The `0x8A` Call handler is hardened
  the way the `0x89` jump handler already was: it resolves the call target
  first, and if the target is not a command boundary it emits a default-visible
  notice (`call target out of range; call ignored`) and falls through, instead
  of the old `commands.find(...)` with no `end()` check — which, on a bad
  target, left the walk's iterator at `end()` and silently ended the entire
  multi-track walk, dropping every remaining track. (3) The same handler no
  longer evaluates `next(i, 1)->first` unconditionally: when the Call is the last
  command in the bank, `next(i, 1)` is `end()` and dereferencing it is undefined
  behaviour (the same family as the already-fixed `--begin()` bug). It now pushes
  the return address only when one exists; when it does not, it jumps without
  pushing, so a later Return honestly takes the empty-stack end-of-track path.

  **Verification (A/B + independent SMF check).** Corpus A/B
  (`tools/ab-verify/ab-verify.ps1 -KeepTrees`, buggy `f344e19` baseline vs fixed
  working build): **byte-identical AND stderr-identical across all 82 archives /
  257,097 output files — 0 changed, 0 added, 0 removed, 0 stream diffs** (exit
  0). The bug is therefore *latent* on this corpus: no sequence leaves a call
  frame on the stack that a later track's unbalanced Return then consumes. The
  stderr-identical result is the dispositive proof — had any leaked frame been
  consumed, the fixed build would have emitted an extra "Return with empty call
  stack" notice exactly where the old build silently followed the leaked frame,
  and none appeared. Likewise both `0x8A` guard branches never fire: every call
  target in the corpus is a valid command boundary and no Call is a bank's last
  command (the guards are output-neutral, like the `[If]`/`Rnd`/mono-poly
  hardening above). Because zero `.mid` files changed, the planned per-pair SMF
  classification (the only permissible change being trailing events removed from
  an affected track, never an added event or a changed value) had no pairs to
  examine and no anomalies to flag — and the harness's own per-file SHA-256
  comparison over all 257,097 files is byte-level, strictly stronger than an
  event-level SMF diff would have been. The fix is a latent-correctness
  hardening: it eliminates cross-track state corruption and two
  undefined-behaviour / silent-track-drop hazards without moving a single output
  byte on the available corpus.

- **Distinct sequence entries sharing a symbol name no longer overwrite each
  other's `.bcseq`/`.mid`** (`Csar.cpp`) (*2026-07-12*). A `.bcsar` maps many
  sequence INFO entries onto shared sequence banks; each entry carries a symbol
  name and its own start offset within the bank. The `case 0x2203:` extraction
  composed both the raw `.bcseq` path and (via `Cseq::Convert`) the `.mid` path
  from the symbol name alone (`bankDir / (FileName + ".bcseq")`), so two entries
  with the same name wrote the same two paths. Last writer won; every earlier
  entry's converted music silently never reached disk, and when the colliding
  entries referenced different file ids even the raw `.bcseq` bytes were
  clobbered.

  **The fix (name composition only).** A per-archive record maps each claimed
  `seqFile` path to the `(id, startOffset)` that claimed it. The first entry to
  claim a path keeps the bare name unchanged — so every non-colliding entry and
  each collision group's first entry stay byte-identical to before. A later
  entry landing on a path already held by a *different* `(id, startOffset)` is a
  genuinely distinct sequence sharing the name, and gets its start offset as a
  lowercase-hex suffix (`SEQ_1_0x1b40`); if even that collides (same name and
  offset, different id) the id is appended as a further tiebreak, so the
  `(name, startOffset, id)` triple — unique per distinct entry — always
  resolves. A repeat of the same `(id, startOffset)` is an exact-duplicate INFO
  entry and is left to rewrite identical bytes over the one path, as before. The
  suffix is local to the path: `cseqs[i].FileName` and `namesById[id]` keep the
  bare name, so group naming (Cgrp reads `namesById`) and log/display uses are
  untouched — only the entry's own per-file console line shows the suffixed
  basename, which is what distinguishes the siblings.

  **Verification (corpus A/B, output-changing by design).** Baseline HEAD
  `91517e7`, 82-archive / 257,097-file corpus; exit 1 as expected. New side
  257,125 files: **29 output diffs — 1 changed, 28 added, 0 removed**, all in
  `3DSNand/sound/safe.bcsar`, `BANK_4/SEQ_1`. The 28 additions are 14 `.bcseq`
  + 14 `.mid` (the relocated collision siblings); the single change is the bare
  `SEQ_1.mid`. Console echoes 17 `SEQ_1` write lines on both sides: the group is
  **17 INFO entries → 15 distinct `(id, startOffset)`** (1 bare + 14 suffixed)
  **plus 2 exact duplicates** (a second entry each at `0x12e` and `0x402`, same
  id and offset) that reuse their path and add nothing — the exact-duplicate
  passthrough, exercised on real data. All 15 entries reference the same bank id,
  so every `.bcseq` is byte-identical (`D997D71…`, 1760 B) and the bare `.bcseq`
  therefore does not change; only the bare `.mid` moves, from the last writer
  (271 B `D0A653…`, offset `0x41e`) to the first writer (36 B `818A61…`). **No
  music lost, only relocated:** the old bare `.mid` content reappears
  byte-identical as `SEQ_1_0x41e.mid`, and the old bare `.bcseq` content is
  present across the whole group. Every other invariant held exactly: no file
  removed; every addition matches `<base>_0x<hex>[_id]` with the bare base
  present in the same directory; every change is a collision-group bare
  `.bcseq`/`.mid`; **no `.sf2`, `.wav`, or raw-dump byte changed anywhere**;
  stderr byte-identical (0 diffs, so no warning category added or vanished) and
  exit codes unchanged, the one stdout diff being the suffixed basenames on
  `safe.bcsar`'s per-file write lines (same 17-line count both sides).

  **Reconciling the roadmap's "104 of 7,990".** Scanning the new trees:
  **41,249** `.bcseq` reach disk across **28,375** distinct symbol names, of
  which **9,334 recur across more than one directory** — but those are benign
  (each archive/bank extracts into its own directory, so cross-directory
  same-names never share a path). Genuine *within-directory* path collisions,
  the only case that overwrote, number exactly **one** here (the `SEQ_1` group).
  The roadmap headline counted recurring output names corpus-wide (dominated by
  that benign cross-directory reuse) and drew on a wider survey than the
  harness's default corpus; the fix mechanism is collision-count-agnostic, and
  this corpus simply contains a single true collision site. MSVC Release build
  warning-clean.

- **Made the plain-value clamp universal and gave the two comment-only
  approximations a run-time notice** (`Cseq.cpp`) (*2026-07-12*). Closes two open
  "Known bugs" items. `clampPlainCtrl` (a plain, un-prefixed out-of-range `Uint8`
  clamps to 127 with an approximation notice; unevaluated `Rnd`/`Var` stand-ins
  keep dropping through the `emitCtrl` notice, since a range midpoint or a
  variable index is not a real value to bake in) was wired at `0xC0`/`0xC1`/
  `0xC2`/`0xD5`/`0xDC` but not at four other plain-`Uint8`→7-bit sites: `0xC5`
  bend range, `0xC9` portamento control, `0xCA` modulation depth and `0xCF`
  portamento time all dropped a plain out-of-range write instead of clamping it.
  All four now call the helper. `0xCA` was the one with teeth: it latches its
  value into the mod-wheel shadow (`modShadow[0]`) that the `0xCC` retarget
  restore path later replays, so the value is now clamped **once** into a local
  used for both the shadow latch and the CC1 emission — clamping only the emitted
  copy would have left the `0xCC` path replaying an unclamped >127 that the writer
  drops.

  **The two silent approximations.** `0xD8` LPF cutoff → CC74 gets the direction
  and end-points of a relative darken right, but the mapped curve reads ~20%
  shallow (hardware steps 187.5 cents/unit against GM2's ~150); that residual was
  recorded only in a code comment. It now fires a default-visible per-execution
  notice — but only when an actual cut is emitted (`CC74 != 64`; a value of
  exactly 64 is a no-op open filter). `0xE0` mod delay scales into CC78's upper
  half saturating at 1,000 ms, so real delays above 1 s all flatten to
  CC78 = 127; that too was comment-only and now notices on the saturating branch
  (`ms > 1000`). Neither changes an emitted value.

  **Verification (corpus A/B) — and a refuted triage assumption.** Baseline HEAD
  `fe2d717`, 82-archive / 257,125-file corpus. Exit 1, as the fallback path
  anticipated: **47 `.mid` changed, 0 added, 0 removed**, every one in a single
  pair of archives — `Majora/sound/sequence/JokerSound.bcsar` (23) and
  `Ocarina/sound/QueenSound.bcsar` (24). The 2026-07-11 triage had assumed the
  corpus's remaining ~230 out-of-range drops were "100% `Rnd`/`Var` stand-ins";
  they are not. All **228** are plain (un-prefixed) `0xCF` portamento-time values
  above 127 — 160 in JokerSound, 68 in QueenSound — which previously dropped their
  glide-time write outright and now clamp to a single new **CC5 = 127** event per
  command (confirmed by parsing every changed pair: the sole per-file delta is an
  added CC5 = 127, its count matching the per-archive notice tally exactly).
  Clamping is the right call here — strictly better than the old silent drop (the
  glide command now reaches the synth as "slowest portamento time" instead of
  vanishing), surfaced by the "clamped to 127" notice, and exactly the
  plain-`Uint8`→7-bit convention the other five sites already use. `0xC5`/`0xC9`/
  `0xCA` never fired on this corpus (no CC6/CC84/CC1 added anywhere), so the value
  change is `0xCF`-only.

  **stderr classification.** 37 archives changed stderr, exhaustively accounted
  for: (a) `+4,121` new "lpf cutoff approximated (CC74 curve reads ~20% shallow)"
  notices across 35 archives; (b) `+2` new "mod delay saturated (CC78 caps at
  1000 ms)" notices, both in `CTR_SOUND.bcsar` (its two corpus copies),
  reconciling with the documented delay distribution (p99 = 500 ms, max =
  1,150 ms — only values in (1000, 1150] ms saturate); (c) in JokerSound and
  QueenSound the existing "control/parameter events dropped (value out of range)"
  category is renamed to "…values clamped to 127 (above range)" with its count
  unchanged (160 and 68). No other notice category's count moved, no existing
  category vanished, stdout and exit codes are unchanged everywhere, and the two
  Part-B notices change no output file. MSVC Release build warning-clean.

## Investigations

### 2026-07-11 — Dropped-parameter triage (full census + provenance)

Triggered by a user observation that `caesar -w` reports "pan" and "sustain"
as not implemented on `mset.bcsar`. Method: (1) a corpus census running the
stock binary with `-w` over all 82 BCSAR archives, aggregating every warning
message; (2) a second census with a scratch-instrumented build that also
tallies content the stock converter drops **with no warning at all**
(parse-time counters; the instrumentation was never committed); (3) parallel
research/adversarial-verify agents pinning each opcode's hardware semantics
against **Gota7/GotaSequenceLib `CtrCafe.cs`** — established here as the
authoritative CTR byte→command map (the NitroStudio2 enum is SSEQ-ordered and
does *not* match CSEQ bytes) — plus the byte-matched NW4R decomps
(doldecomp/ogws, zeldaret/ss), gbatek, and this repo's disasm handoff.

**Headline: the warned drops (~140k events) are the tip of an iceberg —
~2.4M additional sequence events convert wrong or vanish silently.**

Warned drops, census-ranked (occurrences / distinct sub-files / archives):

| Warning | Occ | Files | Archives | Verdict |
|---|---|---|---|---|
| span (0xD7) | 55,291 | 417 | 49 | **No MIDI target.** SurroundPan: front/rear axis of the DSP's quad voice-gain matrix. Silent in Mono/Stereo output modes, **audible under the System Settings *Surround* mode** — see the same-day addendum below. MIDI has no surround axis, so demote to a benign notice; the future player must model it. |
| sustain (0xD2) | 19,188 | 10,397 | 63 | Correctly dropped — ADSR sustain *level*; no GM2/GS CC exists for it. Not the pedal (that is 0xDF). |
| tie (0xC8) | 13,948 | 6,617 | 33 | Real articulation loss (notes re-attack); already the v0.5.1 stretch item. |
| conditional jump | 13,871 | 2,260 | 26 | Superseded by the convert-time VM plan (below). |
| bank select (0xB6) | 8,778 | 2,934 | 41 | Real: mid-sequence bank switch → wrong instrument. Needs Cbnk SF2-layout co-design. Top users: WarioWare Gold `SoundData1` (4,585), Majora `JokerSound`, `GreenCube`, Fire Emblem. |
| init pan (0xDC) | 8,438 | 1,212 | 46 | ~~**Exact one-line fix**: CC10 at the tick (engine sums init_pan+pan; set-once-before-notes is the common case).~~ **Both halves of this were wrong** — see the 2026-07-12 entry. A raw CC10 write *clobbers* the pan it is supposed to be summed with, and init_pan is used mid-track (74% of uses) far more often than as an initialiser. Fixed by combining the two terms. |
| mod type (0xCC) | 8,006 | 4,512 | 64 | Linchpin: LFO target select (0 pitch / 1 volume / 2 pan). caesar emits vibrato CCs unconditionally, so tremolo/auto-pan tracks render as pitch wobble. Fix = gate CC1/76/77/78 on tracked type. |
| biquad value/type (0xB5/0xB4) | 8,057 | ~1,030 | 27 | No audible MIDI target (filter response select + blend). Keep dropped; CC30/31 only if lossless round-trip ever matters. |
| front bypass (0xBF) | 7,129 | 550 | 15 | Surround-path routing bool — only meaningful under the Surround output mode (addendum below); no MIDI target. Demote. |
| lpf cutoff (0xD8) | 4,672 | 2,157 | 35 | CC74 brightness — but ~~near-identity 0–127~~ **darken-only 0–64**: the neutral-64 half was right, the range half was not. The engine clamps the cutoff scale at 64, so a raw pass-through would brighten past the sample's own tone. See the 2026-07-12 entry. |
| envelope hold (0xB1) | 3,709 | 2,439 | 21 | NW4C ADSHR hold-stage override; no CC exists. Keep dropped. |
| priority (0xC6) | 3,161 | 1,005 | 35 | Voice-steal priority; meaningless in MIDI (demote), but must be preserved as state for the future player. |
| mute (0xDD) | 512 | 4 | 2 | Mode byte (off/no-stop/release/stop). Faithful handling = per-track flag suppressing note emission (+CC120/123); fully doable in the one-pass walk. Rare — low priority. |
| print var / env reset / main send | 12 / 1 / 1 | — | — | Negligible. |
| velocity range (0xB3), mod phase/curve/period (0xBD/0xBE/0xE4) | **0** | 0 | 0 | Never occur in the corpus. Off the list. |

Silent drops (instrumented census; **no warning fires today for any of
these**):

- **`_t` (Time-suffix) ramps flattened to instant jumps**: 375,316 volume
  fades, 76,362 pan sweeps, 10,725 pitch-bend ramps, plus span/expression —
  the single largest fidelity gap in the converter, and completely silent.
- **Extended (0xF0) command space is 100% silently dropped**: the walker's
  extended branch is dead code (`cmd.Cmd` is never updated past `0xF0`, so
  none of its "not implemented" warnings can fire). Corpus: 353k `setvar`,
  210k `cmp_eq`, 64k `addvar`, 55k `randvar`, 42k `subvar`, 17k `cmp_ne`…
  Notably **zero** occurrences of the entire mod2/3/4 multi-LFO family
  (extended 0xA0–0xB1, 0xE1–0xE6) — NW4C's one genuine engine unknown is
  unused by every game in the corpus. (Also: caesar's mod4 warning labels
  0xAC–0xB1 are scrambled vs CtrCafe — cosmetic, since they never print.)
- **`Rnd`-prefixed values collapse to the range minimum**: 196k volume,
  177k pitch-bend, 94k rest durations (timing bias!), 71k transpose.
  Midpoint would be the honest deterministic choice.
- **`Var`-prefixed values emit the variable INDEX as the value** (e.g.
  volume=var[3] emits CC7=3): 59k transpose, 27k setvar operands, 15k
  pitch-bend, 13k volume. Garbage data, silent.
- **`[If]` on non-jump commands executes unconditionally**: 34k transpose,
  30k conditional Calls, and — control-flow-corrupting — **33k conditional
  Returns + 8.5k conditional Fins** that can truncate tracks early, plus
  ~11k conditional notes that always play. Concentrated in the adaptive-music
  engines: GardenSound (AC), Alice (Triforce Heroes), Jack (ALBW), ctr_dash.
- **`0xDE` = FxSendC** (third aux send, a real CTR command): parsed, no
  convert branch, no warning. Zero corpus occurrences, but the plain switch
  has no final `else`, so any future gap is silent too.
- **Latent desync**: the Time-suffix trailing s16 is consumed only for
  0xB0–0xDF; a `_t` on tempo/sweep/notes/extended leaves 2 bytes unread and
  misframes the rest of the track. Zero corpus hits (all ~473k observed `_t`
  sit in the safe range) — but it is the one genuine wrong-arg-count hazard.
- `0x90`/`0x96` (not in the CTR map; the original author's `Analyse` probes,
  2-byte length guessed) and `0xB7–0xBC` (also not real opcodes): zero corpus
  occurrences; if they ever appear it means an upstream desync, so they
  should fail fast rather than swallow bytes.

Audit of the *implemented* mappings (each finding adversarially verified):

- ~~**`0xDF` Damper bool bug (new)**: the argument is a Bool (0/1) but the raw
  value goes to CC64, and CC64 < 64 means pedal **off** — so "damper on"
  never engages on any GM/GS synth and ringing notes get cut. The sibling
  bool 0xCE is already normalized `? 127 : 0`; 0xDF was simply missed.
  One-line fix, output-changing for sequences using damper.~~
  **RETRACTED 2026-07-12 — this finding was wrong.** The "Bool" premise came
  from GotaSequenceLib's argument table, whose player never executes the
  command; the engine actually thresholds the argument at 64, exactly as MIDI
  CC64 does, so the raw pass-through was already correct and the prescribed
  `? 127 : 0` would have *inverted* the pedal for arguments 1–63. See the
  damper entry under "Fixed bugs" for the refutation and the mapping that
  shipped instead. **Standing lesson: Gota's `SequenceCommandParameter` typing
  is a modelling guess wherever his player leaves the command unimplemented —
  it is authoritative for the byte *map*, not for argument *semantics*. Check
  the NW4R matching decomps (and the corpus) before acting on it.**
- **`0xE3` → CC78 confirmed wrong** (it is SweepPitch, s16, units of 1/64
  semitone — the roadmap's existing v0.5.1 item stands, now provenance-backed:
  CtrCafe + GotaSequenceLib `Channel.SweepMain`); sweeps ≥ 2 semitones exceed
  127 and are the bulk of the ~1,020 "out of MIDI range" drops (census total
  for that notice: exactly 1,020, matching the roadmap figure).
- **`0xE0` ModDelay** transform `(v/2)+64` overflows for delays ≥ 128 ticks →
  spurious out-of-range notices; clamp (also already scoped in v0.5.1).
- **`(v/2)+64` on 0xCB/0xCD/0xD0/0xD1/0xD3** compresses 0–127 args into
  64–127 (can never say "faster/shorter than default"). Root cause: the
  parse phase types 0xD0/D1/D3 as *signed* Int8 under a mistaken signed
  model, so a fix must touch both phases. Low priority — FluidSynth-class
  players ignore CC72–79 entirely.
- Clean bill (verified correct, no change): 0xC9→CC84 portamento control,
  timebase, ~~mono/poly~~, pan, volume, master volume, transpose→RPN2,
  bend→14-bit, bend range→RPN0, notewait, expression→CC11, FX A/B→CC91/93,
  loop CC116/117 (finite-count pass-through stays a v0.5.1 item).
  **Correction (2026-07-12): mono/poly (`0xB2`) does not deserve its clean
  bill.** This audit checked the *value* domain and missed the *message class*:
  CC126/CC127 are Channel Mode messages, which the MIDI 1.0 spec mandates carry
  an implicit All Notes Off, so a mid-track mono toggle silences the channel's
  ringing notes — something the engine's voice-allocation flag never does. Filed
  as a v0.5.1 work item in ROADMAP.md.

The variable/conditional machinery ("vm-flow") conclusion, feeding the
roadmap's sequence-fidelity plan: a small deterministic convert-time VM —
three variable scopes initialised to 0 (matching power-on hardware state), the
12 arithmetic ops, the 6 comparisons setting a per-track flag, `[If]` gating
*every* command type, the existing revisit guard for backward jumps, a fixed
documented value for `randvar` — resolves sequence-internal `[If]`s
bit-exactly and defaults game-driven globals to the same "default section" the
heuristic aims for. It strictly supersedes the two-reachability heuristic
(which ignores the comparison operator and cannot handle 3-way dispatch), and
it is a direct down-payment on suite stage 4.

#### Addendum (2026-07-11): the 3DS *Surround* mode — span verdict corrected

The user pointed out that System Settings offers Mono/Stereo/**Surround**, and
asked whether span relates to it. It does. Research pass (Azahar DSP-HLE
source, libctru ndsp headers, doldecomp/ogws NW4R decomp, Pokémon Sun/Moon's
gflib2 wrapper over `nw::snd`, all adversarially verified) established:

- **The 3DS DSP mix model is quad end-to-end.** Every voice carries a
  3-bus × 4-channel gain matrix (`SourceConfiguration.gain[3][4]`: front-L/R
  + rear-L/R on main, aux A, aux B — Azahar `shared_memory.h:156`; mirrored
  by libctru's `ndspChnSetMix(float mix[12])`). The intermediate mix buffers
  are `QuadFrame32`. The earlier "3DS mixes to a stereo main bus" reading of
  our handoff was an over-simplification — the *final* output is 2-channel,
  the buses are not.
- **Surround is a DSP-firmware virtualization, selected by
  `DspConfiguration.output_format` (Mono=0/Stereo=1/Surround=2)** with live
  parameters `surround_depth`, `surround_speaker_position` (SQUARE/WIDE),
  `rear_ratio`, two "surround biquad filters" (3dbrew), and a
  `headphones_connected` flag that swaps the coefficient set (speaker
  crosstalk-cancellation vs headphone virtualization). No Dolby/SRS branding
  found; unlike the Wii's real DPL2 *encode*, the 3DS folds the quad field
  down for its own two transducers. libctru exposes the same controls
  (`NDSP_OUTPUT_SURROUND`, `ndspSurroundSetDepth/Pos/RearRatio`).
- **Corrected mechanism for span's silence in Stereo/Mono**: it is *not*
  that rear channels get dropped — Azahar's stereo fold-down **sums** rear
  into front (`L = FL+RL`, `R = FR+RR`). It is the *sequence runtime* that
  zeroes the rear sends outside Surround/DPL2 modes: NW4R's
  `Voice::CalcMixParam` computes the front/rear split from surroundPan but
  forces the surround term to 0 in `OUTPUT_MODE_STEREO`/`MONO`
  (ogws `snd_Voice.cpp:1001-1089`); span's byte scales as `/63 → 0.0–2.0`,
  1.0 = center (`snd_SeqTrack.cpp UpdateChannelParam`). Right verdict, wrong
  reason — now fixed above.
- **In Surround mode span IS a real, audible front/rear positioning
  command** (confirmed on Wii; inference-grade on 3DS — the one missing link
  is the un-decompiled NW4C ARM11 mix routine, but Sun/Moon's headers prove
  `nw::snd` carries `OUTPUT_MODE_SURROUND` and `SetSurroundPan` on 3DS, and
  the DSP plumbing is confirmed). Under the constant-power pan curves
  (`PAN_CURVE_SQRT`/`SINCOS`) a front↔rear move is not even level-neutral
  after fold-down.
- **Citra/Azahar are not an oracle here**: `mixers.cpp` has
  `case OutputFormat::Surround: // TODO(merry) … fallthrough` to Stereo —
  surround has never been emulated. Recovering the real virtualization means
  the same offline-`teakra` impulse method already planned for reverb.
- **Circumstantial gem from the census**: the heaviest span user in the
  corpus (12,602 of 55,291 occurrences, in just 6 sequence files) is
  `mset.bcsar` — **System Settings itself**, the applet hosting the
  Mono/Stereo/Surround selector. Its sound-config demo music appears to
  sweep voices through the surround field deliberately. Games use span far
  less (cplay 5.5k, cardboard 4.2k, Fire Emblem 460, Animal Crossing 203).
- **Hardware test (New 3DS + CFW) that settles the residual inference**,
  in order of rigor: (A) libctru homebrew playing a steady tone with
  `ndspChnSetMix` front-only vs rear-only under `NDSP_OUTPUT_STEREO` vs
  `SURROUND`, capturing line-out — prediction: bit-identical in Stereo,
  measurably different (level / L-R correlation / difference spectrum) in
  Surround; also A/B the headphone-detect state. (B) Dump the live
  `SourceConfiguration.gain[3][4]` while a span-sweeping `.bcseq` plays
  under each System Settings mode — rear lanes zero in Stereo but
  span-dependent in Surround confirms the whole chain at the source.

Converter impact: none (MIDI has no surround axis; the demote-to-benign call
stands, with corrected wording). Player impact (suite stages 2–3): span must
be preserved as voice state and the Surround virtualization captured
behaviourally via `teakra`, exactly like reverb.

##### Console confirmation (2026-07-11, surround-probe v2) — CONFIRMED

The residual inference above is now settled empirically on the user's New 3DS
(Luma3DS). The `tools/surround-probe/` homebrew was rebuilt as **v2** after v1
came back inconclusive — v1's steady, L/R-symmetric 440 Hz tone with mono-summed
analysis is structurally blind to front/back virtualization (a centered pure
tone carries no spectral handle for HRTF coloration, and symmetric routing is
collapsed by the naive stereo fold; every condition nulled to the ~−30 dB
capture floor, Surround showing only a ~+2 dB level bump with zero L/R
decorrelation). v2 fixed both the stimulus and the analysis:

- **Stimulus**: a periodic, band-limited (100 Hz–14 kHz) Schroeder-phase pink
  multitone (`source/probe_buf.h`, low crest, seamless loop at the DSP-native
  32728 Hz), hard-panned to **one quad corner** — front-left (FL) vs back-left
  (BL). Same physical side, so a naive stereo fold makes FL and BL identical,
  and the opposite (R) output channel becomes a clean crosstalk meter.
- **Matrix**: a hands-off 10-segment AUTO run — Stereo / Surround(0x7FFF) /
  Surround(0xFFFF, depth positive-control) / Surround(WIDE) / Mono, each ×
  {FL, BL} — recorded as one take with a countable pip burst (segment N = N
  pips) marking each segment.
- **Analysis** (`analyze_surround.py`, alignment-free, never mono-sums):
  per-channel Welch PSD → **MAGDEV** (median-subtracted front-vs-rear
  spectral-reshaping RMS over trusted bins — level-independent, so a pure gain
  change reads 0) and **XTALK** (energy bleeding into the silent channel).

**Capture** (`run.wav`, 48 kHz/24-bit line-in from the headphone jack, System
Settings = Surround): the results are unambiguous and every control fires
correctly.

| Mode | D (front-vs-rear reshaping) | XTALK into silent channel |
|---|---|---|
| **STEREO** | 0.23 dB (flat) | −53.5 dB (at the −85 dBFS floor) |
| **SURROUND** (0x7FFF) | **6.12 dB** | **−7.7 dB** |
| SURR + DEPTH (0xFFFF) | 6.07 dB | −7.6 dB |
| SURR + WIDE | 6.86 dB | −8.3 dB |
| **MONO** | 0.33 dB (flat) | 0.1 dB (L = R) |

Discriminators: **dD = +5.88 dB**, **dXTALK = +46 dB**. Verdict: **CONFIRMED**.
The result is robust against every alternative explanation:

- **Not a capture-rig artifact.** The Stereo segments use the *identical*
  routing (loud L, off-center source) yet their R channel sits dead at the
  −85 dBFS noise floor — so the cable/ADC contribute zero L→R bleed. The
  −7.7 dB of energy that appears in R only under Surround is generated inside
  the 3DS. The Stereo row *is* the rig-crosstalk null, and it is silent.
- **Not "Surround just spreads to mono."** The decisive metric is that FL and
  BL produce *different spectra* in Surround (D = 6.1 dB) but are
  indistinguishable in Stereo (D = 0.23 dB). That ~6 dB of front/back HRTF
  coloration is exactly the axis `span` moves a voice along.
- **Positive controls fire.** The WIDE speaker position raises reshaping to
  6.86 dB (the position parameter is live, not saturated); depth holds steady
  rather than collapsing — the effect is large and stable, not marginal.
- **Both null controls pass.** Stereo FL≡BL and Mono FL≡BL (D ≈ 0.3, and Mono
  correctly gives L = R) — the probe and routing do not manufacture a
  difference on their own.

So the 3DS Surround output mode performs genuine front/back spatial
virtualization on the headphone jack, and **`span` (SurroundPan, 0xD7) is
audible on real hardware** whenever the console is in Surround mode — upgrading
the entry above from inference-grade to console-confirmed, and closing the
"Surround-mode A/B probe" roadmap item. The register-level Part B (dumping live
`gain[3][4]` while a span-sweeping `.bcseq` plays) remains queued as a
follow-up; the *physics* is now settled. All converter/player impacts above are
unchanged — MIDI still has no surround axis, and the suite player must model the
virtualization behaviourally.

*Tooling note:* `split_run.py`'s pip-burst segmenter misfired on the real
broadband capture — the low-but-nonzero envelope dips of the multitone
fragmented the 8 s bodies, body fragments and transition ticks inflated the pip
counts, and the app's post-run idle (which keeps looping the probe) appeared as
a spurious 44 s "11th body". The synthetic validation had passed because the
fixture did not reproduce those envelope dips or the idle tail — classic
fixture unrealism. The analysis here was run on interior slices cut at the
ground-truth body boundaries (cross-checked two ways: the inter-body pip counts
read 1…10 in order, and the R-channel level signature — dead in Stereo, lifted
in Surround, equal in Mono — matches the schedule exactly). `split_run.py` was
subsequently hardened to name segments by schedule order with aggressive
gap-closing and a duration clamp that rejects the idle tail, keeping pip counts
as a soft cross-check only.

### 2026-07-13 — Suite stage-0 kickoff survey

Run before writing the first line of suite code, to check the settled stage-0
plan (SUITE-DESIGN.md "Library-core refactor", four steps) against the tree as
it stands after the v0.5.1 and VM work. Method: five parallel survey agents
(global-state census, disk-round-trip map, per-format parser/exporter
coupling, CLI/build/library split, plan-delta audit) plus one adversarial
critique agent instructed to verify every claim against source; the new bugs
it surfaced were re-verified by hand before filing under Known bugs.

**Verdict: the four steps survive, but the execution order changes —
2 (narrowed to the `.wav` handoff) → 1 (context fold) → 3 (per-file split,
widened) → 4 (library split).**

- **Why step 2 goes first, narrowed.** The in-memory sample handoff deletes
  the codebase's one non-RAII `Push`/`Pop` pair (and its latent stale-frame
  leak), it sits under the strongest byte signal the A/B has (SF2 bytes,
  default-on, corpus-wide), and it shrinks the later context fold. Its
  contract has four traps a naive port would miss: (a) the missing-`.wav`
  `RequireOpen` abort is currently the only thing between an out-of-range
  sample id and out-of-bounds UB at the `SampleMode` lookup — the in-memory
  path needs an explicit bounds check reproducing the same error text;
  (b) decode-failure (no `.wav` today) and IMA-ADPCM (valid `.wav`, zero
  samples) become indistinguishable in memory — a converted-successfully flag
  keeps the failure paths byte-equivalent; (c) the `<id>.wav` stdout echo
  lines come from the `Push` inside the loop and must be kept (push an empty
  range); (d) the unchecked `.wav` `ofstream` must gain its check or disk and
  SF2 output can silently diverge on I/O failure. The mono/stereo read-back
  quirks (first `smpl` loop record only, no-loop default of
  `0..LeftSamples.size()`, the >2-channel interleave-into-left degenerate)
  must be copied, not fixed.
- **Why step 1 is the riskiest step, not the safest.** Its ~600-site diff
  lands almost entirely on surfaces the default A/B never exercises: `-w`
  text, `Assert`/`Error` failure text, exception text, multi-input runs.
  Guard committed *before* the fold: diagnostics goldens — corrupted-archive
  stderr fixtures per failure family, a `-w` run restricted to
  Csar-direct-path archives (group-path `-w` positions are nondeterministic
  today), and one two-archive single-process run pinning the multi-input
  `.log` bleed. The first fold commit can be zero-call-site: a `ParseContext`
  struct holding the six members, with `Common`'s statics becoming references
  bound to one process-lifetime instance — byte-identical by construction —
  then the context threads through class-by-class on the existing `Options`
  injection pattern. The context stays per-process in step 1; per-input
  scoping (which fixes the `.log` bleed and changes bytes) is its own later
  commit.
- **Attribution is top-of-stack, never "my file" — and the wrong names are in
  the shipped bytes.** Long-lived objects leave frames on the shared stack,
  so `.log` rows and warnings on the Csar/Cgrp paths attribute to the
  last-extracted `.cwav`/group file. The context must reproduce that, not
  clean it up in the same change.
- **A second disk round-trip family the plan never named:** Csar and Cgrp
  write every embedded child (`.bcwar`/`.bcbnk`/`.bcseq`/`.bcgrp`) to disk
  and immediately re-open it through the child's constructor. Folding it into
  step 2 would broaden the weakest-verified step and touch all seven
  constructors twice; it belongs to step 3's per-file migrations (span +
  context + model per file, one signature change each; split order
  Cwar → Cwav → Cgrp → Cbnk → Csar → Cseq by measured difficulty).
- **Cseq is already model + interpreter.** Pass 1 builds a clean
  `map<offset, CseqCmd>` with no I/O; the VM lives entirely in the emit walk
  and needs no model state. The one genuine lossless-model blocker in the
  codebase: `ReadArgs` collapses random-range (`Rnd`) argument bounds to
  their midpoint *at parse time*, welding exporter policy into the model
  layer — the bounds must survive parse and the midpoint decision move to
  emit. Secondary: VM diagnostics rebuild raw source pointers for warning
  positions and must become stored offsets (also a stage-1 drop-the-buffer
  prerequisite).
- **Scale drift and state census.** Own-source is ~5,020 LOC (the doc sized
  ~3,744); Cseq.cpp alone is 2,165 — 43 % of the codebase, most of it one
  function. No new globals since the doc: the VM's state is all
  `Convert`-local, and the only mutable process state outside `struct Common`
  is the `cerr` format-flag leakage (hex/fill/uppercase persist across
  calls), which is itself part of the byte surface — reordering diagnostic
  calls changes later output.
- **Vendored code needs no touching** (zero `Common` references in
  sf2cute/libsmfc), with one caution: sf2cute holds pointer-keyed hash maps
  that are deterministic only because they are never iterated for output —
  do not casually upgrade or iterate it.
- **The library split (step 4) is cheap.** Format handlers have zero direct
  console I/O (every diagnostic funnels through `Common` — one class to give
  a pluggable sink), `Options` is already parameter-threaded, CI needs no
  edits, and ab-verify constrains only the CLI surface (exe name/location,
  `-o` semantics, exit codes, stdout echo, stderr notices). Keep sources
  under `src/` — the harness's stale-exe guard watches only that tree.
- **Doc hygiene:** SUITE-DESIGN's `Cbnk.cpp:213-293` citations were already
  stale on the day the doc was committed (the read-back is the
  `cwav.Id < 0xF000` block in `Cbnk::Convert`). Landmarks over line numbers
  in design docs from here on.

## Suite stage 0 — session 1: the in-memory sample handoff (2026-07-13)

First code of the suite plan, executed the same day as the kickoff survey and
in its revised order (this was sub-step 2, narrowed to the `.wav` trip and
pulled ahead of the context fold). `Cwav` now retains channel count, sample
rate, the raw INFO loop points, per-channel decoded PCM, and a
converted-successfully flag alongside the already-retained `SampleMode`;
`Cbnk` fills its per-sample state from the live `Cwav` (reached through the
unchanged positional `Cwars` lookup) instead of re-opening and re-parsing the
`.wav` it just wrote. The `.wav` user output is byte-unchanged. Deleted with
the read-back: the RIFF header asserts, the smpl scan, the per-sample heap
buffer, and the codebase's only manual `Push`/`Pop` pair — retiring the
stale-frame/bounds-Range leak on its return-false paths, filed that morning.
Added: the `.wav` write is checked (previously silent on I/O failure), and
the bank-side guard turns three formerly-crashing malformed references
(positional index past the map, absent/nullptr WARC, out-of-range sample id)
into the exact missing-file error text `RequireOpen` always threw. Quirks
reproduced, not fixed: >2-channel waves still collapse frame-interleaved into
`LeftSamples`; SampleMode-odd loop points are recovered even with empty PCM
(the IMA-ADPCM case); the no-loop default stays `0..LeftSamples.size()`. The
per-sample `<id>.wav` stdout echo is kept by pushing an empty context range.

Verification: full-corpus A/B (tools/ab-verify, baseline = the survey commit)
— 82 archives, **257,125 output files byte-identical, stdout/stderr
identical**, exit 0. Peak working set on the corpus's largest archive
(FatesB `IRON15_sound.bcsar`, 151 MB): 461 MB → 1,028 MB — the retained
decoded PCM, accepted and changelogged; it is the future player's sample
path — with single-run wall time 24.1 s → 22.2 s. Three adversarial Opus
reviewers (byte-surface, value-simulation across every channel-count ×
SampleMode × codec combination, lifetime/UB) found zero blockers; lifetimes
are anchored by the pre-existing live-object `SampleMode` deref in the SF2
build. One real latent divergence was found and deliberately kept: when two
WARCs share a symbol (and therefore an output directory), the old build
silently built SF2s from whichever colliding on-disk `.wav` won the
overwrite; the new build reads the correct per-archive samples. That is a
bugfix on a case absent from the corpus — the WARC naming-collision entry in
Known bugs stays open for the on-disk overwrite itself. Also surfaced:
`Cgrp`'s map-insert-by-assignment leaks an overwritten `Cwar` on group id
collisions (pre-existing, unobserved, now heavier — filed in Known bugs).

Pattern note: one Opus implementation agent working against the survey's
quirk contract, then parallel Opus adversarial review — the tier-1-fixes
pattern, holding up well.

## Suite stage 0 — session 2: the diagnostics-goldens harness (2026-07-13)

The guard the kickoff survey required **before** the `ParseContext` fold, built
as a tools+docs commit (no `src/` touched). `tools/diag-goldens/diag-goldens.ps1`
(PowerShell 7, single file + README), modelled on `ab-verify`'s discipline
(`Set-StrictMode`, a `Stop-DgHarness` funnel that says "nothing was verified", a
`trap`, atomic writes, an AST-derived shadowed-helper guard, a `-SelfTest`).
Two modes: `-Capture` builds fixtures + goldens from the current exe (refuses to
overwrite without `-Force`); default compare regenerates the fixtures, runs the
current exe, and byte-compares. Exit contract is `ab-verify`'s: 0 identical,
1 diffs, 2 harness error. Fixtures **and** goldens stay under
`%LOCALAPPDATA%\caesar-diag` (they embed corpus names / analysis rows), so only
the script and README are in the repo — the same reason `ab-verify` is
gitignored.

**Inventory — 17 invocation surfaces over 10 fixtures / 3 source archives.**
Sources chosen by property: `caravel` (`F-Zero`, version `0x02000000`, smallest
Csar direct-path archive with real banks/seqs/wave-archives) is the base for the
header/section mutations and the multi-input `a`; `pksnd` (`pokemon red`,
version `0x02030100`) is required for the `assert-length` fixture because the
stored-length `Assert` is *guarded out* for `0x02000000` — the survey
anticipated exactly this "use a second tiny archive" case; `queenstream`
(`Ocarina`) is a second deterministic `-w` archive. One mutation per
**mechanism**: magic/BOM/length Asserts, the INFO enum-default `Error`,
`CheckBounds` overrun (STRG string length → `0xFFFFFF00`, message pinned at the
deterministic `4294967039`-byte / offset `0x1D0` form) and outside
(`infoOffset` → `0x10000000`), and a zero-byte `RequireOpen`. Plus the five
CLI-error paths, the two deterministic `-w` runs, the two `-w`-on-failure runs,
and the multi-input `.log`-bleed run. Every corrupted fixture self-validates its
family marker + exit at capture *and* compare — a fixture that stops firing is
exit 2, not a pass. `require-open`'s stdout is pinned **empty** (the `Push` echo
is never reached), and the multi-input golden pins the bleed as-is:
`pksnd.log` = `caravel`'s 178 analysis rows **followed by** `pksnd`'s own (the
fold must reproduce this; fixing it is a later commit).

**Two survey claims corrected against the running exe.** (1) The survey said
group-path `-w` is the nondeterministic case; empirically it is **broader** —
any archive that warns during wave/bank decode subtracts a `pos` and a
top-of-stack buffer base from **different heap allocations**. `caravel`
(direct-path) prints `0xFFFFFFFF…`-class garbage; `pika` (direct-path) prints
*plausible-looking* positions (`0x0000D860` vs `0x0000E590`, a constant per-run
delta) that a "looks-like-garbage" heuristic would wrongly accept. So the `-w`
set is selected by an **empirical twice-run byte-identity check** (in capture
and compare; a mismatch is exit 2), which leaves only `pksnd` and `queenstream`
among the small archives. (2) The CLI's bad-`--pad-sustain` path is `=`-form
only (`--pad-sustain=x`); the space form `--pad-sustain x` would instead treat
`x` as a missing input file, so the golden uses the `=` form to pin the intended
argument-error path.

**Verification.** Built exe present (`caesar 0.5.1`); `-Capture` then compare
against the same exe → **exit 0, all 17 surfaces byte-identical**;
no-`-Force` recapture correctly refused (exit 2); `-SelfTest` → exit 0
(clean compare identical, an injected golden change reported and named as a
`fix-error-enum stderr` diff, restored set clean again, and the fixture-firing
guard proven to match a real family marker and reject empty stderr). `ab-verify`
was **not** run (its lock is another agent's to take). For the fold executor:
recapture from the pre-fold build, then compare after each rebuild; a
`--version` bump or any deliberate diagnostic-text change requires
`-Capture -Force`.

## Suite stage 0 — the `Rnd` parse/emit split: the one lossless-model blocker (2026-07-13)

The kickoff survey named a single genuine lossless-model blocker in the
codebase: `Cseq::ReadArgs` collapsed a random-range (`Rnd`) argument's two raw
`s16` bounds to their `(lo + hi) / 2` midpoint *at parse time*, so the parsed
command model (`map<offset, CseqCmd>`) never held the source bytes for a `Rnd`
argument — an exporter policy welded into the model layer, and the one thing
standing between Cseq and a raw-backed round-trip serializer.

The fix keeps the raw pair and moves the midpoint decision to the emit walk,
byte-for-byte. Parse: `ReadArgs` gained an optional out-parameter and now hands
the two bounds (in file order, **UNSORTED** — the hardware stores them unsorted
and both orders occur in the corpus; a v0.5.1 finding, never to be normalized)
back to the caller, which parks them on the command as `CseqCmd::Arg1Rnd` (the
Arg1-typed slot: a note length, rest, program, `0xB0`–`0xE4` parameter, or an
extended op's operand) or `Arg2Rnd` (the trailing `_t` `TimeRnd` ramp). `Args`
still carries exactly one value per argument — for a `Rnd` slot the first bound,
an inert placeholder that keeps the positional slot count identical for every
structural/velocity consumer — because no consumer reads a `Rnd` slot's `Args`
value directly. Emit: the sole `Rnd`-value consumer, `resolveArg`, computes the
same `(Arg1Rnd.first + Arg1Rnd.second) / 2` (identical C++ truncation toward
zero) at the exact point it previously read the pre-collapsed midpoint, so every
downstream path — MIDI emission, the convert-time VM's operand reads (including
`randvar`'s further `op / 2`), the clamps, and the "Rnd argument approximated by
its range midpoint" notice — sees the same number as before. The `_t` `Rnd`
midpoint was never emitted (the ramp flattens to a notice), so `Arg2Rnd` is
retained purely for the round-trip and consumed by nothing today.

Verification: full-corpus A/B (`tools/ab-verify`, baseline built fresh from the
survey commit `96aef7a` in an isolated detached worktree, so the baseline
genuinely lacks the change) — 82 archives, **257,125 output files
byte-identical, stdout/stderr identical**, exit 0. (The harness's
vacuous-comparison guard mis-fires when the new exe is built in a git worktree
whose uncommitted edits the main checkout can't see; `-AllowVacuousComparison`
was the correct override — the compared binaries genuinely differ, which is the
guard's own binary-hash test.) Warning-clean MSVC build (CI is warnings-as-
errors). This clears the stage-1 round-trip's Cseq blocker; the survey's
secondary stage-1 prerequisite — VM diagnostics rebuilding raw source pointers
rather than storing offsets — remains open.

## Suite stage 0 — the ParseContext fold (2026-07-13)

The stage-0 sub-step the kickoff survey flagged as the *riskiest*, not the
safest: its ~550-site diff (271 `Common::` references + 280 `ReadFixLen`/
`ReadVarLen` calls) lands almost entirely on surfaces the default corpus A/B
never exercises — `-w` text, `Assert`/`Error` and exception text, multi-input
runs. Executed behind the diagnostics-goldens guard built for exactly this diff
(session 2), in three verified commits, each passing the full gate (warning-clean
MSVC build + 17-surface goldens byte-identical + 82-archive / 257,125-file corpus
A/B byte-identical with identical stdout/stderr, exit 0). The whole fold is
output-identical by construction; the goldens and the A/B confirmed it at every
boundary.

- **Commit 1 (`62582a9`) — the reference facade.** Introduced `struct
  ParseContext` holding the six mutable members of the parse state
  (`ShowWarnings`, `FileNames`, `Offsets`, `Buffers`, `Log`, `Notices`) and one
  process-lifetime instance `gParseContext`; turned `Common`'s six statics into
  references bound to that instance (instance defined before the bindings in the
  same TU, so single-TU static-init order guarantees construction-before-bind).
  Zero call sites touched — byte-identical by construction.
- **Commit 2 (`55674ab`) — helpers onto the context.** Moved the real bodies of
  the parse helpers (the `Assert`/`Error` templates, `Warning`, `FlushNotices`,
  `Push`/`Pop`/`Reset`, `RequireOpen`, `CheckBounds`, `Analyse`, `Dump`,
  `ReadFixLen`/`ReadVarLen`) onto `ParseContext`; `TypedName`, being
  context-free, became a plain free function. The `Common::` facade methods and
  the free read functions kept their signatures and forwarded to
  `gParseContext`, so all ~550 call sites stayed byte-identical. This isolated
  the one genuine byte-risk in the fold — relocating the diagnostic emission
  code — behind a zero-call-site change, verified in isolation, so the next
  commit's large call-site swap was a pure rename over a proven-identical base.
- **Commit 3 (this commit) — thread and delete.** Gave each format class a
  `ParseContext& Ctx` member (first member, initialised first, so no `-Wreorder`
  under warnings-as-errors) and a constructor parameter mirroring the existing
  `Options` injection; swapped every `Common::X` → `Ctx.X` and `ReadFixLen`/
  `ReadVarLen` → `Ctx.ReadFixLen`/`Ctx.ReadVarLen`. `Cseq`'s four file-static
  helpers (`emitCtrl`, `emitProgram`, `clampCtrl`, `ReadArgs`) took a
  `ParseContext&` first parameter threaded from `Cseq::Convert`; `combinePan` and
  `collectEntryTracks` are context-free and were left alone. `main` now creates
  one `ParseContext` and threads it (`Csar` → `Cgrp` → `Cbnk`/`Cwar`/`Cwav`/
  `Cseq`), with `-w`, `Reset` and `FlushNotices` moving onto it. The `Common`
  struct, the free `ReadFixLen`/`ReadVarLen`, `gParseContext` and every forwarder
  were deleted; the compiler's clean rebuild proves no call site was missed. The
  context's lifetime still spans the whole run, so cross-input behaviour — the
  pinned byte surface the survey enumerated (the multi-input `.log` bleed, the
  top-of-stack attribution on the Csar/Cgrp paths, the leaked `cerr` format
  flags) — is reproduced exactly, not cleaned up. Per-input scoping, which fixes
  the `.log` bleed and *changes* output, is the deliberate next commit and is now
  a localized change rather than a global one.

Execution notes for the record: each commit was gated by the goldens after every
rebuild and the corpus A/B before committing (dirty tree, baseline the parent
commit). Commit 3's class-by-class threading was done in the working tree with a
build + goldens checkpoint after each class (the `Common::` facade kept everything
compiling until the final delete step), then committed as one unit with a single
A/B — every intermediate working-tree state was byte-identical by construction
(one shared context object), so the risk was compile-correctness, which the fast
build loop caught, not output bytes. No latent behaviour was changed; two
pre-existing hazards the survey already filed under Known bugs (the multi-input
`.log` bleed and the group-path `-w` position nondeterminism) are untouched and
now cheaper to fix.

## Suite stage 0 — per-input ParseContext scoping (2026-07-13)

The deliberate, output-changing commit the fold intentionally deferred, and the
change that made it a localized one. `main` created a single `ParseContext`
spanning the whole run, so in `caesar a.bcsar b.bcsar` the analysis `Log`
accumulated across both inputs and was cleared only on the exception path (via
`Reset()`); `a`'s rows therefore survived into `b`'s `Dump`, and `b.log` was
`a`'s rows *followed by* `b`'s. (The per-input dropped/approximated `Notices`
summary never bled — `FlushNotices` already clears it per input.)

**Mechanism.** The `ParseContext` declaration moved *inside* the per-input
branch of the argv loop, so each top-level input constructs a fresh context and
discards it at the end of its iteration. `-w` is still parsed positionally in
that same loop, so it is accumulated into a `bool showWarnings` and each
per-input context is initialised from it — positional semantics unchanged (it
applies only to inputs after it on the command line). The exception-path
`Reset()` call, and the `Reset()` method itself, were removed: its sole purpose
was cross-input cleanup ("so a later input is not blamed on a stale filename"),
which per-input construction now provides structurally — leaving it would have
been dead code carrying a now-false comment. Untouched by design: the leaked
`cerr` format flags (hex/uppercase/fill), which live on the stream rather than
the context and so still carry across inputs, and the shared process exit code.

**The honest new semantics, stated plainly.** A multi-input run is now exactly N
independent single-input runs (modulo the two shared-stream facts above). With a
fresh context per input, an earlier input's soft-fail (`Extract` returning
false) or any residual diagnostic stack frames can no longer influence a later
input's `.log`, notice attribution, or warning positions. This is the intended
change, not a side effect.

**Verification.**
- Warning-clean MSVC Release build (CI is warnings-as-errors).
- Corpus A/B (`tools/ab-verify`, dirty tree, baseline the fold commit
  `9c72def`): **82 archives, 257,125 output files byte-identical, stdout/stderr
  identical, exit 0.** `ab-verify` runs one input per process, so the bleed
  never manifests there — its role here is to prove single-input behaviour is
  untouched, which it is.
- Diagnostics goldens (`tools/diag-goldens`): before recapture, **exit 1 with a
  single differing surface — `multi-bleed` `logs` — and nothing else** (its
  `stderr`/`stdout`/`exit` and all 16 other surfaces byte-identical). Side-by-
  side inspection confirmed the diff is precisely the intended one: the a-side
  `caravel.log` is byte-identical (177 rows), and the b-side `pksnd.log` dropped
  from 303 body rows (`caravel`'s entire 177-row log prepended to `pksnd`'s 126)
  to 126 rows — `pksnd`'s own, zero `caravel` rows. Recaptured
  (`-Capture -Force`) and re-compared → **exit 0, all 17 surfaces identical.**
- Manual two-archive end-to-end: `caravel` + `pksnd` in one process (`-o` into a
  scratch dir) versus each run alone. The multi-run `pksnd.log` is **byte-
  identical** to a lone `pksnd` run (0 `caravel` rows); the multi-run
  `caravel.log` is identical to a lone `caravel` run (same 5,884 bytes; only the
  `-o` output-base path segment differs, as it must).

Nothing relied on the cross-input lifetime except `Reset()` (removed). The
survey's twin filing, the `-w` positional-position nondeterminism, was *not*
fixed here: it is a heap-layout bug (positions subtracted across unrelated
allocations), independent of context lifetime, and now stands as its own Known-
bugs entry with the corrected broader scope (any wave/bank-decode warning, not
only group-resident conversions).

## Suite stage 0 — the per-file split, tranche 1: Cwar + Cwav span construction (2026-07-13)

The first tranche of the per-file parser/exporter split (step 3), which the
kickoff survey widened to also retire the *second* disk round-trip family the
plan never named: `Csar`/`Cgrp` write every embedded child (`.bcwar`/`.bcwav`/…)
to disk as user output and then immediately re-opened it through the child's
file-path constructor (open → `tellg` → `new` buffer → `read` → `close`). This
tranche gives the two easiest classes — `Cwar`, then `Cwav`, the settled order
by measured difficulty — a **span-based** construction path: the parent hands the
child its bytes directly (full output name + pointer + length into the parent's
already-loaded buffer), so the child no longer re-reads the file it was just
written from. The disk **write** stays untouched — the extracted `.bcwar`/
`.bcwav` files (and `Cwav`'s decoded `.wav`) are the tool's output. One class per
commit, each passing the full gate. The `FileName` member stays the full output
path in both classes (it composes the child-write directory for `Cwar` and the
`.bcwav`→`.wav` output name for `Cwav`), and the `Push` echo keeps deriving the
bare filename from it, so the stdout name stream is untouched.

**Borrow vs copy — decided per construction site by proven lifetime, not by
class.** Borrowing the parent's span (rather than copying) preserves the
diagnostic position arithmetic exactly (`pos − Offsets.top()` is relative, so it
is identical whether the base is a heap copy or a window into the parent) and
costs zero extra memory, but it is only sound if the parent buffer outlives the
child. The three sites split two ways:

- **`Cwav` (sole site, `Cwar::Extract`) — borrow.** Each `Cwav` is stored in
  `Cwar::Cwavs` and `~Cwar` deletes every `Cwav` *before* releasing its own
  `Data`, so the parent span outlives the child unconditionally — including when
  that `Data` is itself a borrowed window (the borrow chains safely because the
  destruction order is child-before-parent at every link). `Cwav::Data` is now
  always borrowed and the destructor never frees it. (Confirmed harmless beyond
  parse: `Cbnk` reads decoded `Channels`/`SampleMode`/`ChanCount`/`Converted`
  off the live `Cwav`, never `Cwav::Data`, so the borrowed span is dereferenced
  only during `Convert`, synchronously, while the parent buffer is alive.)
- **`Cwar` @ `Csar` (`Cwars[id]`) — borrow.** The `Cwar` lives in `Csar`'s own
  `Cwars` map and `~Csar` deletes each `Cwar` before freeing `Csar::Data`, so the
  parent outlives the child. `ownsData = false`.
- **`Cwar` @ `Cgrp` (`(*Cwars)[id]`) — owned copy.** This is the one site where
  borrow is unsafe: the group-resident `Cwar` is inserted into the
  **archive-lifetime shared** `Cwars` map (owned by `Csar`, read later by
  `Cbnk`), but it is built from the **stack-local** `Cgrp`'s `Data`, which is
  freed when that `Cgrp` is destroyed at the end of `Csar`'s group loop — so the
  child outlives the buffer it was built from. `ownsData = true` takes a private
  copy; `~Cwar` frees `Data` only when owned. (The copy is a `std::copy` of the
  encoded `.bcwar` bytes, smaller than the decoded PCM already retained by the
  in-memory handoff.) Correctness beats the copy: even though today the group
  `Cwar`'s `Data` is dereferenced only inside `Cgrp::Extract` (while the buffer is
  still alive) and so a borrow would not *currently* fault, leaving a dangling
  `Data` pointer in a long-lived object is exactly the latent trap the next
  tranche (Cgrp/Cbnk) would trip on, so it is copied now.

**Error path.** The file-path constructor's `RequireOpen` throw ("could not open
or read file (missing, empty, or unreadable): …") fired only if the just-written
file could not be re-read — an I/O case the corpus never exercises — but it *also*
covered a zero-length re-read (`length <= 0`). The span path reproduces that
second, reachable-in-principle condition with an explicit
`Ctx.RequireOpen(true, Length, FileName)` (stream-ok true, so only the
`length <= 0` branch can fire), reusing the identical error text and the full
output path, and placed **before** the `Push` echo exactly as the old order was —
so a degenerate empty embedded child throws before echoing its name, keeping the
stdout stream byte-identical. Not observed on the corpus.

**File-path constructors removed.** After the migration no caller of either class
uses the old `(const char*, ParseContext&)` constructor (the only sites were the
three migrated here), so both were deleted; the compiler's clean build confirms
no site was missed.

**One disclosed, corpus-invisible edge in the bounds net.** A borrowed child now
registers its `CheckBounds` range as a *sub-range* of the parent's still-registered
range, where the old re-read gave the child an independent heap buffer. Because
`CheckBounds` scans `Buffers` top-down and the child range is pushed last, every
in-bounds read matches the child range first and behaves identically. The only
divergence is a read whose `pos` lands entirely **past the child's declared
length**: the old isolated buffer threw "a file offset points outside the loaded
data"; the borrowed sub-range falls through to the parent range and, if `pos` is
within the parent, returns OK. This can only happen on a malformed/truncated
embedded file (a well-formed child never addresses past its own length), so it is
absent from the whole A/B corpus — and the A/B itself is the guard: any corpus
file that hit it would flip old-throw to new-succeed and show as a diff. It did
not. Flagged here as the inherent flip-side of borrowing; if it ever matters, the
fix is to copy that path too.

**Per-commit verification (each commit passed all three gates).**

- **Commit 1 — `Cwar` (`21e2e8c`).** Signature `Cwar(const std::string&, uint8_t*
  data, std::streamoff length, bool ownsData, ParseContext&)`; `Csar` borrows,
  `Cgrp` owns; `Cwav` construction inside `Cwar::Extract` left on the old path
  this commit. Warning-clean MSVC Release build; diag-goldens **17/17 surfaces
  byte-identical** (exit 0); corpus A/B (dirty tree, baseline `HEAD` = `ba4566b`)
  **82 archives / 257,125 files byte-identical, stdout/stderr identical, exit 0**
  (baseline extract 84 s, new 71 s).
- **Commit 2 — `Cwav` (this commit).** Signature `Cwav(const std::string&,
  uint8_t* data, std::streamoff length, ParseContext&)` (borrow-only); the sole
  site in `Cwar::Extract` migrated; tranche docs. Warning-clean build;
  diag-goldens **17/17 byte-identical** (exit 0); corpus A/B (baseline `HEAD` =
  `21e2e8c`, the `Cwar` commit) **82 archives / 257,125 files byte-identical,
  stdout/stderr identical, exit 0** (baseline 82 s, new 69 s).

Runtime effect, observed not formally measured: dropping the per-child re-read
makes extraction modestly faster (the A/B's own new-vs-baseline extract times ran
~13 s / ~16 % quicker on the 82-archive corpus, though that A/B also rebuilds and
caches so it is a soft signal, not a benchmark). No memory change of note — the
only new allocation is the group-resident `Cwar` copy (encoded bytes, rare path).

**For the next tranche's executor (Cgrp, then Cbnk).** The Cgrp→Cbnk→Csar→Cseq
order continues. Two things this tranche surfaced that bear on Cgrp/Cbnk:
(1) `Cgrp` is the lifetime-hazard class — it is a stack-local in `Csar::Extract`
that populates the *archive-lifetime shared* `Cwars` map and defers its `Cbnk`
conversions (constructs `Cbnk` heap objects into `Cgrp::Cbnks` during the file
loop, converts them all later in the same `Extract`), so any child a migrated
`Cgrp` hands a borrowed span must have the same own-vs-borrow analysis done
against `Cgrp::Data`'s stack lifetime, not assumed safe. (2) The group `.bcbnk`/
`.bcseq`/`.bcgrp` writes in `Cgrp::Extract` are the direct analogues of the
`.bcwar` write migrated here (same `pos += 8; len = read; pos -= 16; write(pos,
len)` shape), so the same span hand-off applies — and `Cbnk`/`Cseq` are
constructed there too, which is why their migrations are entangled with `Cgrp`'s.
The pre-existing `Cgrp` map-insert-by-assignment WARC-id leak (Known bugs) is
untouched and now also copies on the group path, but the leak is the overwrite,
not the copy.

## Suite stage 0 — the per-file split, tranche 2: Cbnk + Cseq + Cgrp span construction (2026-07-13)

The second and final tranche of the embedded-child per-file split (step 3),
finishing what tranche 1 (`Cwar`/`Cwav`) began: the three remaining classes the
parent writes to disk and then re-opened through a file-path constructor —
`Cbnk`, `Cseq`, and the group `Cgrp` — now take a **span** (full output name +
pointer + length into the parent's already-loaded buffer) instead of re-reading
the file they were just written from. The disk **write** stays untouched (the
extracted `.bcbnk`/`.bcseq`/`.bcgrp` are user output); only the re-read goes.
With this tranche the "children no longer re-read the file they were just
written from" line item is **complete** — every embedded child (`Cwar`, `Cwav`,
`Cbnk`, `Cseq`, `Cgrp`) is span-constructed; the only file reader left is the
root `Csar`, which opens the actual CLI input, not a child it wrote (so it is
correctly *not* migrated, despite an earlier note listing it in the order).
`FileName` stays the full output path in all three classes (it composes the
`.sf2`/`.mid` output name and, for the group, the archive extract directory),
and the `Push` echo keeps deriving the bare filename from it, so the stdout name
stream is untouched.

**Every site in this tranche borrows — no owned copy is needed anywhere.** The
one owned copy in the entire split remains the group-resident `Cwar` from
tranche 1 (it enters the archive-lifetime shared `Cwars` map yet is built from
the stack-local `Cgrp` buffer). None of this tranche's children escape into a
longer-lived container, so each is provably outlived by its parent's buffer:

- **`Cbnk` @ `Csar` (`Csar.cpp`, stack-local `cbnk`) — borrow.** Constructed
  from `[pos, pos + cbnkLength)` into `Csar::Data`, converted immediately, and
  destroyed at the end of the loop body while `Csar::Data` is alive.
- **`Cbnk` @ `Cgrp` (`Cgrp::Cbnks`, heap) — borrow.** This is the site the
  tranche-1 handoff flagged as the hazard to re-check: `Cgrp` *defers* its bank
  conversions (it pushes `new Cbnk` objects into `Cgrp::Cbnks` during the file
  loop and calls `Convert` on them all later in the same `Cgrp::Extract`). The
  borrow is nonetheless safe because a `Cbnk` never leaves `Cgrp::Cbnks`, and
  `~Cgrp` deletes every `Cbnk` (and `Cseq`) *before* releasing its own `Data`,
  so `Cgrp::Data` outlives each child for its whole lifetime — deferred
  conversion included. (Unlike the group `Cwar`, which *does* outlive the buffer
  by entering the shared map; that is why the `Cwar` copies and the `Cbnk` does
  not.)
- **`Cseq` @ `Csar` (`Csar.cpp`, stack-local `cseq`) — borrow.** From
  `[pos, pos + cseqLength)` into `Csar::Data`; `Convert(startOffset)` runs
  immediately, then the object is destroyed while `Csar::Data` lives.
  `startOffset` addresses *inside* the child's own span (it is relative to the
  sequence's `DATA+8`), so it is entirely unaffected by whether the bytes came
  from a fresh read or a borrowed window.
- **`Cseq` @ `Cgrp` (`Cgrp::Cseqs`, heap) — borrow.** Same deferred-conversion
  structure as the group `Cbnk`; `~Cgrp` frees `Cseqs` before `Data`, so the
  borrow outlives the deferral.
- **`Cgrp` @ `Csar` (`Csar.cpp`, stack-local `cgrp`) — borrow.** From
  `[pos, pos + cgrpLength)` into `Csar::Data`; `Extract()` runs immediately and
  the group is destroyed at the end of the loop body while `Csar::Data` is
  alive. Because `Cgrp::Data` is now itself a window into `Csar::Data`, the
  group's own borrowing children (`Cbnk`/`Cseq`) transitively borrow into
  `Csar::Data` — a two-link borrow chain that holds because destruction is
  child-before-parent at every link (`~Cgrp` frees the children, then the
  stack-local group unwinds, all before `~Csar` frees `Csar::Data`). Migrating
  `Cgrp` last made this whole chain provable inside one commit.

**Construction-site census (what actually exists).** Exactly five sites, all
found and migrated: `Cbnk` ×2 (`Csar.cpp` direct, `Cgrp.cpp` group), `Cseq` ×2
(same two parents), `Cgrp` ×1 (`Csar.cpp` only). **No nested-group path
exists** — `Cgrp::Extract`'s file loop handles only `CWAR`/`CBNK`/`CSEQ`/`CWSD`
records; there is no `CGRP`-inside-`CGRP` case and no other `Cgrp` constructor
call anywhere, so the earlier "check for nested groups" is settled as "none."
The order chosen was `Cbnk → Cseq → Cgrp` (a deliberate reorder of the handoff's
`Cbnk → Cseq → Cgrp` suggestion kept, and of the older `Cgrp`-first note
dropped): doing the two leaf children first means the final `Cgrp` commit could
state the complete `Csar::Data → Cgrp window → child span` borrow chain in one
place, rather than leaving a transient commit where a migrated group borrows
into `Csar::Data` while still handing file-reading children their bytes.

**Error path.** Each span constructor reproduces the old file-path
constructor's zero-length rejection with an explicit
`Ctx.RequireOpen(true, Length, FileName)` (stream-ok `true`, so only the
`length <= 0` branch can fire), reusing the identical error text and full output
path, placed **before** the `Push` echo exactly as the old order was — so a
degenerate empty embedded child throws before echoing its name and the stdout
stream stays byte-identical. Not observed on the corpus.

**File-path constructors removed; one dead include dropped.** After migration no
caller uses the old `(const char*, …)` constructors — the only sites were the
five migrated here — so all three were deleted (the clean build confirms none
was missed). `Cseq.cpp`'s `<fstream>` include went with it (the removed
`ifstream` was its only user; `Cseq` writes MIDI through libsmfc's `FILE*`, not
`fstream`). `Cbnk.cpp` and `Cgrp.cpp` keep `<fstream>` — they still write SF2 /
the embedded `.bcbnk`/`.bcseq`/`.bcgrp` output respectively.

**Bounds net.** The disclosed tranche-1 edge (a borrowed child's `CheckBounds`
sub-range can let a read past the child's declared length fall through to the
parent range on malformed input) now also covers `Cbnk`/`Cseq`/`Cgrp`, which are
all borrows. The Known-bugs entry's class list was extended accordingly; the
mechanism is identical and corpus-invisible (the A/B would flip old-throw to
new-succeed on any real file that hit it — it did not), so no new filing.

**Per-commit verification (each commit passed all three gates).**

- **Commit 1 — `Cbnk` (`e05ce0f`).** Signature `Cbnk(const std::string&,
  uint8_t* data, std::streamoff length, std::map<int, Cwar*>*, const Options&,
  ParseContext&)` (borrow-only); both sites (`Csar.cpp`, `Cgrp.cpp`) migrated.
  Warning-clean MSVC Release build; diag-goldens **17/17 byte-identical**
  (exit 0); corpus A/B (dirty tree, baseline `HEAD` = `df89578`) **82 archives /
  257,125 files byte-identical, stdout/stderr identical, exit 0** (baseline
  extract 85 s, new 69 s).
- **Commit 2 — `Cseq` (`1e4e910`).** Signature `Cseq(const std::string&,
  uint8_t* data, std::streamoff length, ParseContext&)` (borrow-only); both
  sites migrated; dead `<fstream>` dropped. Warning-clean build; diag-goldens
  **17/17 byte-identical** (exit 0); corpus A/B (baseline `HEAD` = `e05ce0f`)
  **82 archives / 257,125 files byte-identical, stdout/stderr identical,
  exit 0** (new 69 s).
- **Commit 3 — `Cgrp` (this commit).** Signature `Cgrp(const std::string&,
  uint8_t* data, std::streamoff length, std::map<int, Cwar*>*, const std::map<int,
  bool>&, const std::map<int, std::string>&, const Options&, ParseContext&)`
  (borrow-only); the sole site (`Csar.cpp`) migrated; tranche docs. Warning-clean
  build; diag-goldens **17/17 byte-identical** (exit 0); corpus A/B (baseline
  `HEAD` = `1e4e910`) **82 archives / 257,125 files byte-identical,
  stdout/stderr identical, exit 0** (new 70 s) — this A/B exercises the group
  path (embedded-group archives such as `ctr_dash`), so the full two-link borrow
  chain is corpus-verified.

**For the model/exporter-split executor (the next step).** The whole embedded
re-read family is gone; every child parser is handed its bytes as a borrowed (or,
for the one group `Cwar`, owned) `Data` span, and `FileName` is now purely an
*output* path (the child-write directory / `.sf2`/`.mid`/`.wav` name), never an
input handle. Things that bear on promoting the parse structs to a lossless
model: (1) each class's `Data` pointer is the read cursor's backing store —
splitting the reader from the emitter must keep the parsed model's lifetime
inside the parent buffer's lifetime, or copy at the model boundary (the same
borrow-vs-own analysis, one level up). (2) `Cgrp` defers `Cbnk`/`Cseq`
conversion within `Extract`; a reader/emitter split should preserve that a
group's children are fully *parsed* before any is *emitted* if it relies on the
deferral (today it does not depend on cross-child state, but the shared `Cwars`
map is populated across the file loop, so bank emission already assumes all wave
archives are present). (3) The stale top-of-stack `-w AT POSITION` attribution on
group paths (Known bugs, heap-layout nondeterministic) is untouched and still
rides the shared `Offsets` stack — a per-model offset base is the real fix and
overlaps the stage-1 drop-the-buffer work.

## Suite stage 0 — the model/exporter-split blueprint (2026-07-13 survey)

Read-only survey run after the span-construction tranches, producing the
execution plan for step 3's second half (promote the half-models to lossless;
split parse-to-model from emit-from-model) and the stage-1 groundwork map.

**The two framing facts.** Diagnostics are two-tier: `Warning` tallies
`Notices` unconditionally but prints positional lines only under `-w`, and
`FlushNotices` prints the summary *sorted by category* — so under the default
surface, warning ordering is irrelevant; only the sorted summary, stdout
echoes, `.log`, and output bytes are pinned. And `.log` is already deferred
(every `Analyse` is parse-phase; `Dump` fires once at the end of
`Csar::Extract`), so a split that preserves parse order cannot disturb it.

**The headline: no output-file blocker exists.** SF2 (sf2cute sorts
generators), WAV (linear from PCM), and MIDI (libsmfc's stable time-sort;
equal-tick insertion order is fully determined by the model + walk) are all
pure functions of the model plus deterministic writers. The genuinely
un-replayable surfaces are diagnostic/progress only: (1) **the parser performs
stdout I/O** — `Push` echoes fire from every child constructor during parse,
so the "parsers do no I/O" rule needs an explicit exemption for progress
echoes (or a thin driver visiting nodes in identical depth-first order);
(2) **`-w` positional stderr is emit-order-dependent** — a *global*
parse-all-then-emit-all would interleave differently than today's per-file
parse-then-emit, so **the phase boundary stays per-file, never global**;
(3) the `cerr` format-flag leakage means diagnostic *calls* must not reorder
relative to `FlushNotices`.

**Lossless-model gaps per class** (full field lists in the survey report,
summarized): Csar has no persistent record tree at all (everything is
`Extract`-local); the player (0x2102) and set (0x2104) tables are never
parsed and become opaque spans; four header words are read-and-discarded and
several INFO words are `Analyse`-logged then dropped. Cgrp's INFX chunk is
located but never parsed (opaque span). Cwar is the thinnest (offset table +
blob section). **Cwav is the largest gap**: after `Convert`, only the
SF2-path fields survive — `codec`, the per-channel `SampOffset`/`AdpcmType`/
DSP coefficients/contexts, and the raw DATA payload are all discarded, and
decoded PCM cannot be re-encoded losslessly, so round-trip needs the raw DATA
span retained (or the explicit decision that BCWAV rides as an archive-level
opaque blob — stage 1's list is BCSEQ/BCBNK/BCSAR). Cbnk's `CbnkCwav` is a
parse/emit boundary violation: the parser fills it by reaching into live
`Cwav` objects, and the SF2 build still reads `->SampleMode` live — the raw
record is just two dwords, so the model must shrink to the raw ref and
resolution must move into the exporter (taking the per-sample `<id>.wav`
stdout echo with it). Cseq's model is the most complete (the Rnd blocker
already resolved); what remains is `cseqVersion`, storing each command's own
source offset + `dataOffset`, and canonical re-encoding rules for suffix
prefix order and VarLen args.

**The Cseq offset prerequisite = the open `-w` nondeterminism bug.** The emit
walk reconstructs `here = Data + dataOffset + 8 + i->first` (Cseq.cpp:913 and
three siblings) for ~40 warning sites — position-by-pointer-subtraction
against the shared stack top, the same defect behind the heap-nondeterministic
`-w` positions. The fix (offset-taking `Warning` overload + stored command
offsets) unblocks stage-1 drop-the-buffer AND makes `-w` deterministic in one
commit — an intentional `-w` golden change, re-pinned via `-Capture -Force`.

**Execution order (one commit each, per-file phase boundary, full gate):**
1. **Cwav** — retain raw DATA + channel/DSP context + `codec`; extract the
   WAV writer from the decoder. Leaf; strongest default-on byte signal.
2. **Cwar** — model = cwav table + FILE span; exporter = dump + recurse.
3. **Cbnk** — decouple parser from live Cwav (raw refs + `SampleMode` in the
   model; resolution + echo into the SF2 exporter; retain the dropped words).
4. **Cseq** — stored offsets + offset-based Warning + extract the emit walk;
   the isolated intentional `-w` golden change.
5. **Csar + Cgrp** — persistent archive record tree with opaque spans; the
   stage-1 whole-archive offset/size recomputation lives here. Preserve
   depth-first construction order and `Dump`-last.

Stage-1 handoff: after these five, the round-trip serializer is a new
model→bytes exporter per format; the recompute set (every offset/size table)
vs copy-through set (strings, locations, blobs, ADPCM payloads) is enumerated
per format in the survey; inter-record alignment padding is modeled nowhere
today and must be reproduced by rule or stored as opaque gap-spans.

## Suite stage 0 — model/exporter split, commit 1: Cwav (2026-07-14)

First of the blueprint's five per-class splits. `Cwav::Convert` used to decode
the INFO/DATA sections and write the `.wav` in one pass, discarding everything
but the SF2-path fields after the walk. It is now two members: **`Parse`** (INFO
walk + PCM decode into the object, no file output) and **`ExportWav`** (the RIFF
+ optional `smpl` writer, reading only model fields). The wave archive
(`Cwar.cpp`) invokes them back to back per file — `if (!Parse()) return false;`
then `ExportWav();` — so the parse-failure `return false` and the I/O-failure
throw keep their exact original positions and the whole observable surface (the
`.wav` bytes, the write-failure message, the stdout echo emitted at construction,
the IMA-ADPCM notice, and every `Assert`/`Error` text and order) is unchanged.
`Convert` was the only public entry and its sole caller was this one site, so the
rename is coherent, not a compatibility break.

**What the model now retains** (previously function-local and dropped): the
`Codec`; the per-channel records (`ChannelInfo`, one `CwavChan` each) carrying
`SampOffset`/`AdpcmType`/`AdpcmOffset`, the 16 `DspCoeffs`, and both `DspContext`
records; the raw DATA-section span as `DataSpanOffset`/`DataSpanLength` (a window
into `Data`, **no bytes copied**); and the three previously read-and-discarded
header words — `Version` (cwavVersion), `UnalignedLoopStart`, and the DATA length
(kept as `DataSpanLength`). The pre-existing SF2-path fields (`SampleMode`,
`ChanCount`, `SampleRate`, `LoopStart`, `LoopEnd`, decoded `Channels`,
`Converted`) are unchanged and are what `Cbnk` still reads live.

**Offset-vs-pointer choice: converted outright, no parallel pointers.** The old
`CwavChan` held three raw `uint8_t*` fields (`Offset`, `SampOffset`,
`AdpcmOffset`). As retained state those became `uint32_t` **offsets relative to
the span base (`Data`)** — `InfoOffset`/`SampOffset`/`AdpcmOffset` — and parse
reconstructs each read cursor as `Data + <offset>`. This adds **no new
raw-pointer state** to the model (the blueprint's stage-1-drop-the-buffer rule)
and is arithmetically identical to the old pointers: each stored offset is the
exact value the old `Data + …` expression added, so every `Assert` still sees the
same `pos`. Decoded PCM is written straight into `Channels[i]` during Parse
(via `Channels.assign(chanCount, {})`) rather than into a throwaway
`PcmSamples` per channel and moved at the end, so the decoded bytes are stored
**once**, not doubled.

**Memory impact.** The retained per-channel metadata is tiny (~40 bytes/channel:
three offsets + 16 `int16_t` coeffs + two 5-byte contexts) and the raw DATA
payload is recorded as **offset+length only — the parent `Cwar` buffer already
holds those bytes, so nothing is copied**. Net growth over the prior in-memory
handoff is negligible; the large `Channels` PCM buffer is the same one that
already existed.

**Stage-1 note.** The raw DATA span is borrowed from the parent `Cwar` buffer
(freed after this child), so offset+length resolves for the object's whole life
today. When stage 1 drops the source buffer, `Cwav` needs an explicit
copy-or-keep decision for `[DataSpanOffset, DataSpanOffset + DataSpanLength)` —
flagged in the header comment beside the field.

**Verification (full gate, all green).** Warning-clean MSVC Release build (3
compilers' `-Werror` discipline honoured; `std::move` no longer needed on the
decode path). Diagnostics goldens **17/17 byte-identical** (exit 0). Corpus A/B
(dirty tree, baseline `HEAD` = `89a58d4`) **82 archives / 257,125 files
byte-identical, stdout/stderr identical, exit 0** (baseline 84 s, new 85 s) — the
`.wav` bytes are the strongest default-on signal for exactly this change.

**For commit 2 (`Cwar`) and commit 3 (`Cbnk`).** `Cwav` exposes `Parse()` (bool)
and `ExportWav()` (void, throws on write failure) separately now; `Convert()` is
gone. When `Cwar` splits its own parse/emit (commit 2), the natural seam is to
call `cwav->Parse()` in `Cwar`'s parse phase and `cwav->ExportWav()` in its emit
phase — but keep them per-file back to back until then, because a global
parse-all-then-emit-all would reorder `-w` stderr. `Cbnk` (commit 3) still reads
`Converted`/`ChanCount`/`SampleRate`/`Channels`/`SampleMode`/`LoopStart`/
`LoopEnd` off the live `Cwav`; those field names and semantics are untouched, so
commit 3 can migrate `CbnkCwav` without touching `Cwav`. `Converted` is now set
at the end of `Parse` (decode succeeded) rather than after the write; on the only
path where that differs — a write-failure throw — the process is already
unwinding fatally, so it is unobservable and the A/B confirms it.

## Suite stage 0 — model/exporter split, commits 2–3: Cwar + Cbnk (2026-07-14)

The blueprint's next two per-class splits. Commit 2 (`14fb9b5`) is the small one
(`Cwar`); commit 3 is the boundary-violation fix (`Cbnk`). Both are
output-identical; both cleared the full gate. Commit order was Cwar → Cbnk
because Cwar is the leaf-of-the-two and Cbnk depends on `Cwar::Cwavs` staying
exactly as before.

**Commit 2 — Cwar: model = cwav table + FILE span.** `Cwar::Extract` did two
jobs in one method: parse the CWAR/INFO/FILE headers, then in a second loop write
each `.bcwav` blob and drive the child `Cwav`'s `Parse`/`ExportWav`. It is now
`Parse` (headers → model) and `Export` (dump + recurse), with `Csar`/`Cgrp`
calling them back to back per wave archive. The retained model gained three
members: `Version` (the previously `[[maybe_unused]]` `cwarVersion`),
`FileSpanOffset`/`FileSpanLength` (the FILE section as an offset+length window
into `Data`), and `CwavRecords` (the INFO cwav table, previously the
function-local `vector<CwarCwav> cwavs`). `CwarCwav::Offset` changed from a raw
`uint8_t*` to a **span-relative `uint32_t`** (against `Data`), matching commit
1's convention — the record carries no raw-pointer state for the stage-1 buffer
drop. `Export` reconstructs `Data + Offset`, which is arithmetically the exact
value the old `Data + fileOffset + 8 + …` pointer held, so every
`CheckBounds`/blob-write/child-construct sees the same address and the `.bcwav`
bytes, stdout echoes, and diagnostics are unchanged. The per-file interleave
(blob write → child echo → child `Parse` → child `ExportWav`) is preserved inside
the one `Export` loop — never hoisted into two loops — so the stdout echo order
is byte-for-byte what it was.

**Commit 3 — Cbnk: the parser stops reaching into live Cwav objects.** The
violation the blueprint flagged: the CWAV-table walk (parse) filled `CbnkCwav`
with exporter-resolved data (`ChanCount`, `SampleRate`, `LeftSamples`,
`RightSamples`, `Loop`, `LoopStart`, `LoopEnd`) by dereferencing live `Cwav`
objects through the `Cwars` map, and emitted the per-sample `<id>.wav` stdout
echo there via a balanced empty-range `Push`/`Pop`. Restructure:
  - **`CbnkCwav` shrank to the raw record** — `{ Cwar, Id, Key }` (war id,
    sample id, and the note's root key, filled by the instrument walk). The seven
    resolved fields are gone from the model.
  - **Live-`Cwav` resolution moved into the SF2 sample-creation loop** (the
    exporter): the positional wave-archive lookup, the missing/absent/unconverted
    **guard-throw**, the `<id>.wav` **echo**, the PCM read (mono / stereo / the
    >2-channel frame-interleave reproduction), and the odd-`SampleMode` loop-point
    recovery all now run there, into locals, feeding `NewSample` (with
    `std::move` of the decoded PCM, since `SFSample`'s ctor takes the vector by
    value). The instrument-zone build reads `ChanCount` from the same live `Cwav`
    it already resolves for `SampleMode`, rather than from the (now-absent) model
    field.
  - **The logged-then-dropped note words are retained as typed `CbnkNote`
    fields** — `Word08`, `Word0C`, the 0x6001 quartet
    (`Word6001_10/14/18/1C`), the `Flags` word at 0x14, `Word28`, and the ADSHR
    `DataRef2C/30/34` chain. Each is read into a local, then `Analyse`d (the
    `.log` calls are byte-unchanged — same read order, same value), then stored.
    They are round-trip state, unused by today's exporter.

**The echo-relocation verification story (the one risk in commit 3).** Moving the
`<id>.wav` echo out of the parse walk (before the instrument walk) into the SF2
sample-creation loop (after it) relocates the *only* observable output inside a
single `Cbnk` conversion. Verified byte-safe three ways from the code, then
confirmed by the A/B:
  - **stdout:** `Push` is the only thing that writes stdout (`cout <<
    FileNames.top()`), and the sole `Push` inside `Cbnk::Convert` is this echo
    (Cbnk's own frame is pushed in the *constructor*, not `Convert`). So the echo
    is the only stdout in the whole conversion; relocating it keeps the same
    cwav-index order (0…N−1, skipping `Id ≥ 0xF000` both before and after), and
    with nothing else on stdout to interleave against, the stream is identical.
  - **`.log` (Analyse):** `Analyse` tags each row with `FileNames.top()`. Because
    the `<id>.wav` `Push`/`Pop` is balanced, the instrument walk's `Analyse` rows
    saw the Cbnk frame on top *before* the move (echo already popped) and still
    see it *after* (echo not yet pushed) — identical rows, identical order.
  - **`-w` stderr (Warning):** the substitution warning (out-of-range note→cwav
    ref) and the release-127 warning position via `pos - Offsets.top()`; the
    balanced `Push`/`Pop` means `Offsets.top()` is the Cbnk `Data` at both
    warning sites regardless of where the echo lives. Neither warning moved. The
    corpus A/B reports **CONSOLE (stdout/stderr): identical**, which settles it.
  - One deliberate, unobservable behaviour change: the missing-sample guard-throw
    now fires in the exporter (after the instrument walk) instead of the parse
    walk. On healthy data it never fires (confirmed by the whole-corpus A/B); on
    corrupt data the abort would surface a few parse warnings earlier than before,
    but the process aborts either way and the corpus has no such archive.

**Gate evidence (each commit independently).**
  - *Commit 2 (`14fb9b5`)*: warning-clean MSVC Release build; diagnostics goldens
    **17/17 byte-identical** (exit 0); corpus A/B vs `4dc3c12` (commit 1) **82
    archives / 257,125 files byte-identical, stdout/stderr identical, exit 0**.
  - *Commit 3 (this commit)*: warning-clean MSVC Release build (the `std::move`
    handoffs and the new `<utility>` include compile clean under the three
    compilers' `-Werror` discipline); diagnostics goldens **17/17 byte-identical**
    (exit 0) — the `multi-bleed` fixture runs the F-Zero `caravel` bank, so the
    `<id>.wav` echoes and the note-word `.log` rows are directly pinned; corpus
    A/B vs `14fb9b5` (commit 2) **82 archives / 257,125 files byte-identical,
    stdout/stderr identical, exit 0**.

**For commit 4 (`Cseq`), the executor must know:**
  - The `Cseq` model is already the most complete (the `Rnd` parse/emit split
    resolved the one lossless-model blocker earlier in stage 0). What remains per
    the blueprint: retain `cseqVersion`; store each command's own **source offset
    + `dataOffset`**; and settle canonical re-encoding rules (suffix/prefix order,
    VarLen args).
  - **The stored-offset work is also the fix for the open `-w` heap-nondeterminism
    Known bug.** The emit walk today reconstructs `here = Data + dataOffset + 8 +
    i->first` at ~40 `Warning` sites (Cseq.cpp:913 and three siblings) — a
    position-by-pointer-subtraction against the shared stack top. An
    offset-taking `Warning` overload plus stored command offsets fixes both at
    once. This is the one **intentional, isolated `-w` golden change** in the
    five-commit plan: unlike commits 1–3 (which are golden-identical), commit 4
    will change the `-w` positional goldens and must be re-pinned with
    `diag-goldens -Capture -Force`, with the diff reviewed as expected. The
    default-surface A/B (no `-w`) must stay byte-identical.
  - Keep the phase boundary **per-file, never global** — `Cseq::Convert` splitting
    into parse-then-emit must not let a global parse-all-then-emit-all reorder the
    `-w` stderr across sequences.
  - `Cwar::Parse`/`Export` and `Cwav::Parse`/`ExportWav` are the precedent for the
    method split; `Cseq`'s public entry is `Convert` (single caller sites in
    `Csar`/`Cgrp`, invoked back to back if split).

## Suite stage 0 — model/exporter split, commit 4: Cseq (2026-07-14)

The blueprint's fourth per-class split, and the one it flagged as *the* commit
that would intentionally change the `-w` goldens (because it fixes the open `-w`
heap-nondeterminism Known bug at its root). It splits `Cseq::Convert` into
`Parse`/`Export`, stores each command's source offset on the model, and converts
the emit walk's warning sites to an offset-taking `Warning` overload. **The
headline correction: on this corpus the change is output-identical on *every*
surface, `-w` included — the "intentional golden change" premise did not
materialise, because an empirical whole-corpus scan proved no sequence is
group-resident.** The fix is real and correct, but latent (see below).

**The three strands (all one commit).**

1. *Stored offsets on the model.* `CseqCmd` gains `uint32_t Offset` (its own
   DATA+8-relative source offset — the value the command map is already keyed on,
   now carried on the record so the model is self-locating for stage 1). `Cseq`
   gains `DataOffset` and `Version` (the `cseqVersion` 0x40-block word, formerly
   read-and-discarded), plus the command map itself as a retained member
   (`Commands`). `Parse` fills all of them; nothing else about parse changed
   (parse-phase pointer arithmetic is byte-for-byte the same, so the command map
   and every `Assert`/`Error` are identical).

2. *Offset-based diagnostics.* A new additive overload
   `ParseContext::Warning(uint32_t position, …)` (Common.hpp/.cpp) prints the
   `AT POSITION` value the caller hands it, with **no** `pos - Offsets.top()`
   subtraction — same `hex / setfill('0') / uppercase / setw(8)` formatting, so a
   site that already resolved against its own buffer prints identical bytes
   through either overload. The pointer overload is untouched (other classes'
   attribution quirks stay pinned, per the plan). The three static emit helpers
   (`emitCtrl` / `emitProgram` / `clampCtrl`) took a `uint8_t* pos`; they now take
   a `uint32_t position`.

3. *The parse/emit seam.* `Convert` is gone; `Parse` (headers → command map, no
   I/O, every `Assert`/`Error` fires here) and `Export(startOffset)` (the
   convert-time VM + control-flow interpreter + MIDI writer + `smfWriteFile`) are
   separate members. `Csar` (direct) and `Cgrp` (deferred second loop) call
   `Parse()` then `Export()` back to back per sequence — the per-file phase
   boundary, so `-w` ordering is unchanged. `startOffset` belongs to `Export`
   (the parse phase never used it).

**Every emit-walk `Data` read, and what it became.** The walk had exactly four
reads of the live buffer, all warning-position computations; after the split the
emit phase (`Export`, ~1,570 lines) reads `Data` **nowhere** (grep-verified —
only comments name it), running purely off `Commands` + `DataOffset`:

| Old (Cseq.cpp) | Site | New |
|---|---|---|
| `Ctx.Warning(Data + dataOffset + 8, …)` | start-offset-not-a-boundary | `Ctx.Warning(dataOffset + 8, …)` |
| `Ctx.Warning(Data + dataOffset + 8 + tieCmdOffset, …)` | `finalizeTie` velocity drop | `Ctx.Warning(dataOffset + 8 + tieCmdOffset, …)` |
| `uint8_t* here = Data + dataOffset + 8 + i->first;` | the ~40-site `here` cursor | `uint32_t here = dataOffset + 8 + i->first;` |
| `Ctx.Warning(Data + dataOffset + 8 + i->first, …)` | stray-`Return` end-of-track | `Ctx.Warning(here, …)` |

`here` becoming a `uint32_t` re-points all ~40 `Ctx.Warning(here, …)` /
`emitCtrl(…, here)` / `clampCtrl(…, here)` calls at the new overload. No emit
site re-parses args through `Data` (all values live in `i->second.Args`), so
nothing else needed converting; `dataOffset` in `Export` is a local copy of the
`DataOffset` member.

**Why the intentional golden change did not happen — the empirical finding.**
For a **direct-path** sequence (construct → push → `Parse` → `Export` → pop,
nothing pushed between), `Offsets.top()` *is* this sequence's own `Data`, so the
old `pos - Offsets.top()` already equalled `dataOffset + 8 + i->first` — exactly
the value the stored offset now produces. Only a **group-resident** sequence (a
CSEQ inside an embedded CGRP, whose `Parse`/`Export` is deferred to a second loop
while sibling child frames sit on top of the stack) hit the bug. A whole-corpus
scan (82 archives, old exe, run twice each, differing `AT POSITION` lines
categorised by message) settled it:

- **Zero** nondeterministic Cseq warnings exist anywhere in the corpus. Nine
  archives are nondeterministic under `-w`, but every differing line is a `Cbnk`
  (`instrument N note … release 127`), `Cgrp` (`Skipping INFX`/`CWSD`) or `Csar`
  (`… external .bcgrp`) message — never a sequence-command message.
- So **every** corpus sequence is direct-path. Confirmed the other way:
  OLD-vs-NEW `-w` stderr on the seq-heavy `dlplay`/`safe`/`newslist`/`menu` is
  byte-identical.

Therefore the Cseq slice of the `-w` Known bug is fixed **at the code level** but
is **latent** on this corpus (it would only change bytes for a group-resident
sequence, of which there are none). The nondeterminism that actually manifests
belongs to the `Cwav`/`Cbnk`/`Cgrp`/`Csar` sites this commit deliberately did not
touch — the bug stays open for them (ROADMAP), and the new overload is theirs to
adopt next. A **separate** residue also left untouched: the `WARNING IN <file>`
line reads `FileNames.top()`, so a deferred/group warning still names the wrong
file — but *deterministically* wrong, not heap-nondeterministic.

**Determinism evidence.** The bug's mechanism, on `ctr_dash` (Mario Kart 7 — the
corpus's one embedded-group archive, whose groups hold *banks*, not sequences):
two old-exe runs differ on 408 lines, e.g. a `Cbnk` release-127 warning prints
`AT POSITION 0xFFFFFFFFFF5021B0` then `0xFFFFFFFFFF534240` — a 64-bit
`pos - Offsets.top()` across two heap allocations, shifting per run. The NEW exe
run 3× is still nondeterministic on those *same 408* `Cbnk`/`Cgrp`/`Csar` lines
(correctly — untouched) while every Cseq warning in the file (all direct-path)
prints a stable, plausible in-file offset (e.g. `Rnd argument approximated …
AT POSITION 0x00000485`). This is the before/after: garbage 64-bit value → true
DATA-relative offset, for exactly the class of site the Cseq fix now covers.

**Golden inventory.** The pre-existing 17-surface diagnostics goldens are
**byte-identical** after the change (exit 0) — the two `-w` goldens `w-pksnd`
(42 blocks) and `w-queenstream` (1 block) are Csar external-stream warnings, not
Cseq, so the blueprint's expectation of a diff there was a misattribution. Since
the corpus A/B runs **without** `-w` and the existing goldens never touch Cseq,
the ~40 Cseq emit-walk warning sites this commit rewrote had **no** automated
coverage at all. Closed that gap: added a fourth source archive `dlplay` and a
`w-dlplay` `-w` golden (DetCheck, 5/5 byte-identical) — the smallest archive
whose whole `-w` run is deterministic *and* exercises the Cseq sites (its
warnings span Rnd-midpoint, ramped-`_t`, sustain-level, span, LFO-retarget and
the stray-`Return` site — one of the four converted). The golden set is now 18
surfaces; the harness self-test still passes. (Goldens are LOCAL only; the
harness script + README carry the change.)

**Gate (all green).** Warning-clean MSVC Release build. Corpus A/B vs `HEAD`
(`c308412`): **82 archives / 257,125 files byte-identical, stdout/stderr
identical, exit 0**. Diagnostics goldens: 18/18 byte-identical after
re-`Capture` (`w-dlplay` added), self-test green. Direct-path Cseq `-w`
byte-identical old-vs-new; whole-corpus scan confirms no group-resident sequence
exists.

**For commit 5 (`Csar` + `Cgrp`), the executor must know:**
  - `Cseq`'s public entry is now `Parse()` + `Export(startOffset)` (both `bool`);
    `Convert` is gone. Both call sites already use `if (!…Parse() || !…Export…)`.
    Preserve `Cgrp`'s two-loop shape (construct all children in the file-table
    walk, then `Parse`/`Export` per child in the second loop) — that deferral is
    what makes group-path warnings fire with a sibling frame on top, so a `Csar`
    persistent-record-tree refactor must keep the per-file phase boundary or it
    will reorder `-w` stderr.
  - The `WARNING IN <file>` misattribution (stale `FileNames.top()`) and the
    `Cgrp`/`Csar`/`Cbnk`/`Cwav` `AT POSITION` nondeterminism are the *remaining*
    slices of the `-w` Known bug. The `Warning(uint32_t, …)` overload exists and
    is the tool to fix them; doing so **would** change `-w` goldens (unlike the
    Cseq slice), and any such commit needs its own `-Capture -Force` + review.
  - Stored offsets now exist on `CseqCmd`/`Cseq` (`Offset`, `DataOffset`,
    `Version`) — the stage-1 round-trip serializer's Cseq inputs. `Csar`/`Cgrp`
    still have no persistent record tree; that is commit 5's job.

## Suite stage 0 — model/exporter split, commit 5: Csar + Cgrp (2026-07-14)

The blueprint's fifth and last per-class split, and the riskiest: the two
containers orchestrate every other class. `Csar::Extract` and `Cgrp::Extract`
each walked headers/INFO/STRG/FILE and, interleaved in the same pass, created
directories, wrote child blobs, constructed children (whose constructors echo to
stdout), and drove their `Parse`/`Export`/`Convert`. Each is now **`Parse`** (the
whole archive → a persistent record tree; no I/O, no child construction) and
**`Export`** (the child-dump/recurse walk that emits every output byte).
`Csar::Extract` survives as the public entry `main` calls — a thin
compose-dir → `Parse` → `Export` driver — so `caesar.cpp` is unchanged. **The
change is output-identical on every surface.**

**The record tree per class (all offsets span-relative — no new raw-pointer
state, the stage-1-drop-the-buffer rule).**

- *Csar* gained retained members where before **everything** was `Extract`-local:
  `Strgs`, `Files`, `CwarRecords`, `CbnkRecords`, `CseqRecords`, `CgrpRecords`,
  `NamesById`, `CseqsFromCsar`. `CsarStrg`/`CsarFile`/`CsarCbnk`/`CsarCseq`/
  `CsarCgrp` changed their `uint8_t* Offset` to a **`uint32_t` span-relative
  offset** (Export resolves `Data + Offset`), matching commits 1–4. Added a
  `CsarCwar` struct (the wave-archive table was inline before). The
  internal/external file discriminator — an `Offset == nullptr` sentinel — is now
  an explicit `CsarFile::Internal` bool, with `Location` carrying the external
  (`0x220D`) sibling path.
- *Cgrp* promoted its `CgrpFile` records to a retained `Files` member and made the
  same pointer→span-relative + explicit-`Present` change; its `Cbnks`/`Cseqs`
  child vectors were already retained.

**Opaque-span / dropped-field inventory retained for the round-trip.** The four
`[[maybe_unused]]` Csar header words (`fileLength`, `strgStringsOffset`,
`strgUnknownOffset`, `infoEndOffset`) and `csarVersion` are now typed members; the
never-parsed **player (`0x2102`)** and **set (`0x2104`)** tables are kept as
opaque spans — the section start plus the entry-offset arrays `Parse` already
walks (see the residue note below). Cgrp retains `cgrpVersion`, the `0x7801`
`fileLength`, and the **INFX (`0x7802`)** chunk as a clean offset+length opaque
span (both come straight from the chunk table). The Analyse-logged-then-dropped
INFO words (`Cwar 0x04`; `Cbnk 0x04/08/0C`; `Cseq 0x04/08/14`; the `0x220C`
reserved third word) are retained as typed record fields. **CSEQ INFO bank-tail
choice:** only the two fields the converter reaches beyond `CbnkOffset`
(`StartOffset`, `BankIndex`) are parsed; the rest of that sub-structure is
unparsed and is covered by each sequence's own `Files[Id]` data span for stage-1
copy-through.

**The `.log`-tagging finding — why Analyse had to move into the export walk (the
one deviation from the blueprint's "Analyse stays in the parse phase").** The
blueprint assumed the `.log` is safe if parse order is preserved. It is not,
because `Analyse` stamps each row with `FileNames.top()`, and for Csar that top is
**not** the archive's own frame: a direct wave archive lives in the archive-
lifetime shared `Cwars` map, so its constructor's `Push` frame stays on the stack
for the rest of `Extract`. Every `Cbnk 0x04`/`Cseq 0x04` row is therefore tagged
with the *last wave archive's* name, and the interleave with `Cbnk::Convert`'s own
`Note` rows (the only child that Analyses) is per-entry. A pure parse pass — which
pushes no child frames — would retag every row with the archive name and reorder
them. So `Parse` does **no** `Analyse`; it stores the words, and `Export` replays
each `Analyse` at the exact point in the walk where today's interleaved pass
emitted it, with the identical stack top. This reproduces both the row order and
the (quirky) filename column byte-for-byte, verified by the `caravel`/`multi-bleed`
goldens and the whole-corpus `.log` A/B.

**No interleaving needed a partial seam.** The skip warnings (external-stream/CWSD
in the Cseq walk, external-group in the Cgrp walk) fire from the export walk at
their original positions via the **pointer-form** `Warning(Data + storedOffset,
…)` — deliberately *not* the `uint32_t` overload — so their `pos − Offsets.top()`
attribution (deterministic for these Csar sites, since the archive frame tops the
stack when they fire, but heap-nondeterministic for the external-group site once a
prior group left its wave-archive frames on the stack) is preserved **exactly**,
nondeterminism included. Cgrp's two-loop shape (construct all children in the
file-table walk, then `Convert`/`Parse`/`Export` in the deferred loops) is kept
verbatim, so a group-resident `Cbnk`'s release-127 `-w` position keeps firing with
a sibling frame on top — the pinned `Cbnk`/`Cgrp`/`Csar` `-w` nondeterminism is
untouched. The shared `Cwars` map semantics (including the id-collision leak) are
unchanged.

**One latent, unobservable change on corrupt input (documented, arguably a fix).**
Parse-phase `Assert`/`Error` now fire with only the archive frame on the stack, so
a corrupt archive that fails a *late* table marker (`Cbnk`/`Cseq`/player/set/`Cgrp`
offsets, or an invalid music/file type) after at least one wave archive was built
prints a `Data`-relative `AT POSITION` instead of the old
`pos − <last-wave-frame>` garbage. Not in the corpus (healthy) and not in the
goldens (all seven corrupt fixtures fail **early** — header/STRG/INFO, before any
child is constructed), so both harnesses are byte-identical; and the old value was
heap-nondeterministic garbage, so nothing meaningful is lost.

**Gate (full battery, all green).** Warning-clean MSVC Release build. Diagnostics
goldens **18/18 byte-identical** (exit 0) — including `multi-bleed` (the caravel
bank's `Note` `.log` rows + `<id>.wav` echoes, the surface most exposed to
container restructuring) and the three `-w` surfaces. Corpus A/B vs `HEAD`
(`65901aa`): **82 archives / 257,125 files byte-identical, stdout/stderr
identical, exit 0** (baseline 74 s, new 59 s) — `ctr_dash` exercises the
embedded-group path (its groups hold banks, so the deferred-loop nondeterminism
and the group `.log` `Note` rows are both on that path).

**Stage-1 handoff — what the serializer still lacks after this commit.** The
model→bytes writer does not exist; every offset and size table must be recomputed
on the way out (never copied). The retained-vs-still-needed split: **retained** —
all record fields, span offsets, blob spans, symbol strings, the discarded header
words, the INFX span. **Still needs work** — (1) the player/set tables have their
section start + entry-offset arrays but **no section byte-length and no per-entry
record payload** (the header gives no direct length; stage 1 must bound them from
the sorted section layout / `InfoEndOffset`); (2) inter-record **alignment
padding** is modeled nowhere and must be reproduced by rule or captured as opaque
gap-spans; (3) an out-of-range `0x220C` file's raw offset/length are dropped (as
before — they were nulled), a corrupt-only edge. Step 3 of stage 0 (parser/
exporter split) is **complete across all six classes**; only the `caesar_core`
library split (step 4) remains.

## Suite stage 0 COMPLETE — the caesar_core library split (step 4, 2026-07-14)

Stage 0's final step, and the cheapest by far: a pure CMake restructure with
**no code changes at all** — `git diff` touches only `CMakeLists.txt` and docs.
The one `caesar` executable target that compiled all eight first-party TUs is
now two targets:

- **`caesar_core`** (`add_library(… STATIC)`) — the six BCSAR format classes
  (`Cbnk`/`Cgrp`/`Csar`/`Cseq`/`Cwar`/`Cwav`), their shared
  `Common`/`ParseContext`, and the header-only `Options`. It `PUBLIC`-links the
  two vendored writers, which stay their **own** static-lib targets with their
  SYSTEM / `-external:W0` include treatment intact — so the vendored C/C++ never
  inherits caesar's `-Werror`.
- **`caesar`** (`add_executable`) — just `src/caesar.cpp` (the CLI entry, arg
  parsing, `--version`), linking `caesar_core PRIVATE`.

Sources stay under `src/` (the ab-verify stale-exe guard watches only that
tree) and the exe still lands at `build/<config>/caesar.exe`.

**Usage-requirement plumbing (why the split is output-identical).** The old
single target received every usage requirement directly; the two new targets
must reproduce each one on the correct scope:

- **C++17** — `PUBLIC` compile-feature on `caesar_core`: the core TUs get it and
  the exe inherits it.
- **`CAESAR_VERSION`** — `PUBLIC` define on `caesar_core`. It is only *used* by
  `caesar.cpp`, but the old build defined it on **every** first-party TU;
  `PUBLIC` preserves that exact per-TU parity (core TUs directly, exe by
  inheritance).
- **`sf2cute` + `libsmfc`** — `PUBLIC` link on `caesar_core`. This re-exports
  libsmfc's `PUBLIC src/` include dir (which is where the old target's `-I…\src`
  actually came from — there was never an explicit include on caesar) and
  sf2cute's SYSTEM include to the core sources, and transitively to the exe.
- **`-Werror` / `/W3 /WX`** — `PRIVATE` on **each** of `caesar_core` and
  `caesar`. This is the trap the task flagged: an `INTERFACE`/`PUBLIC` here would
  leak `-Werror` onto every downstream consumer of the library (the whole
  suite), and dropping it from the lib would rot silently. `PRIVATE` applies it
  to every first-party TU exactly as before and to nobody else. `/utf-8` +
  `_CRT_SECURE_NO_WARNINGS` are directory-scope (`add_compile_*`) and reach
  `caesar_core` automatically.

**Flag parity — verified, not assumed.** Captured `compile_commands.json`
(RelWithDebInfo, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`) **before and after** the
split and diffed the per-TU command lines for all eight first-party TUs. After
folding out the compiler path, `CMAKE_INTDIR`, and the `/Fd` PDB path, the flag
sets are **identical**. Per-TU presence was independently confirmed for
`-DCAESAR_VERSION`, `/W3`, `/WX`, `-std:c++17`, `_CRT_SECURE_NO_WARNINGS`,
`/utf-8`, the `-I…\src` include, and the sf2cute SYSTEM include on all seven
core TUs **and** `caesar.cpp`. The **only** difference is the PDB output
directory (`caesar_core.dir\` for the core TUs vs `caesar.dir\` for the exe) — an
inevitable, benign consequence of two targets owning separate object dirs; it
cannot change `caesar.exe`'s runtime output. The corpus A/B independently proves
this: any dropped optimization or define would have moved output bytes.

**CI needed nothing.** `build.yml` and `release.yml` run only generic
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` + `cmake --build … --config
Release --parallel`, and locate the binary purely by **path** (`build/caesar`
single-config on Linux/macOS Unix Makefiles; `build/Release/caesar.exe` on the
Windows Visual Studio multi-config default) — never by target name. The `caesar`
exe keeps its name and output path; the new `caesar_core` archive is an
intermediate (`libcaesar_core.a` / `caesar_core.lib`) that lands elsewhere and
cannot shadow the exe path. Every CMake construct used is generator-agnostic, so
the VS and Unix Makefiles CI paths behave the same as the local Ninja
Multi-Config build.

**Gate (full battery, all green).**

- **Clean configure from scratch** (build dir deleted, reconfigured in place
  with `-G "Ninja Multi-Config"`): warning-clean MSVC Release build — `/WX`
  would have failed on any warning. Produced `caesar_core.lib` + `caesar.exe` at
  `build/Release/`.
- **Diagnostics goldens 18/18 byte-identical** (exit 0) — new exe vs goldens
  captured from the pre-split exe.
- **Corpus A/B vs `HEAD` (`fd831f7`)**: **82 archives / 257,125 files
  byte-identical, stdout/stderr identical, exit 0** (baseline 75 s, new 63 s),
  baseline built from HEAD's old single-target CMakeLists in a detached
  worktree.

**Stage 0 is complete.** All four steps shipped: the `.wav` in-memory handoff,
the `ParseContext` fold (+ per-input scoping), the per-class parser/exporter
model split across all six classes, and this `caesar_core` split. The parser is
now a linkable static library; the CLI is merely its first consumer.

**For stage 1's executor — the library boundary.** `caesar_core` is where the
model→bytes serializer lands: it already owns the retained record tree
(span-relative offsets, opaque blob spans, discarded header words) from step 3,
with **no** public API beyond the class headers under `src/`. A consumer in a
different directory cannot yet `#include "Csar.hpp"` by that bare name — the
`src/` include dir reaches consumers today only because it rides in on
**libsmfc**'s `PUBLIC` include, an accident of the vendored dep rather than an
intentional API surface. When stage 1 (or the first real suite consumer) needs
the headers, give `caesar_core` its own explicit
`target_include_directories(caesar_core PUBLIC src)` instead of leaning on that
libsmfc side effect. Nothing about the split constrains the writer; the stage-1
gaps recorded in commit 5's handoff (player/set section lengths, alignment
padding, the `0x220C` corrupt-input edge) are unchanged.

## Suite stage 1 — the round-trip serializer blueprint (2026-07-14 survey)

Read-only survey run immediately after stage-0 completion, settling the design
for the stage-1 milestone (parse → drop buffer → re-serialize → sha256 matches,
corpus-wide, for BCSEQ/BCBNK/BCSAR). Full anchor-indexed report in the survey
record; the decisions:

**The correction: Cbnk is not where the stage-0 docs said it is.** The
model/exporter split is real for five classes; `Cbnk` still has a single
one-pass `Convert()`, its `insts`/`cwavs` model is *function-local* (the
lossless fields commit 3 added die when Convert returns), and
`CbnkInst::Offset`/`CbnkNote::Offset` are the codebase's last raw-pointer model
fields. The retained-model promotion + Parse/Export split is therefore stage-1
work (commit 2 below), budgeted as its own output-identical refactor with the
full stage-0 gate.

**Writer architecture:** a `Serialize()` member per class, symmetric to
`Parse()`, additive (nothing on the default export path calls it — ab-verify
stays a clean no-regression net every commit). Composition: BCSEQ and BCBNK
prove **standalone deep**; BCSAR proves **container-deep, children
shallow-opaque** (recompute every STRG/INFO/FILE table and section offset; copy
child blobs through as spans — CWAV can never re-encode its DSP-ADPCM, which is
why BCWAV/BCWAR stay opaque); the optional capstone re-embeds deep-serialized
CBNK/CSEQ children so the container provably consumes *computed* child lengths
(the edit-safe property). The load-bearing rule: every child blob's offset AND
length are derived from the laid-out blob, never reused from the parse.

**Driver:** a new dev executable (`caesar-roundtrip`) linking `caesar_core` —
the library's first real consumer (which also forces the explicit
`target_include_directories(caesar_core PUBLIC src)` the stage-0 handoff
scheduled) — plus a `tools/` corpus fan-out wrapper mirroring ab-verify's
discipline and exit contract (0 all-match / 1 mismatch / 2 harness-error,
never-a-false-pass guards, self-test that mutates one byte and must see it).
The shipped `caesar` exe is untouched; public CI builds the new target but
never runs the corpus.

**The three sized gaps:** (a) player/set table bounds — recoverable from the
archive's own layout: sort all seven INFO sub-section offsets plus
`InfoEndOffset`, each section spans to the next; store the resolved spans at
parse. (b) Alignment padding — the two CBNK sites are parser-proven zero
(reproduce-by-rule); the CSAR/CGRP inter-blob and section gaps are unread and
unproven (capture as opaque gap-spans first; a later corpus scan may prove
zero-fill). (c) CSEQ canonicality — suffix/prefix order is canonical **by
construction** (any other order is a parse failure, so the model can never
hold a non-canonical order), but **VarLen is a genuine hazard**: ReadVarLen
accepts padded encodings and the model stores only the decoded value, in
exactly three slots (note gate time, 0x80 wait, 0x81 program). A corpus scan
decides: canonical-only proven → emit canonical; else retain the raw byte
length per slot. The scan is stage-1 commit 0.

**Field-level retention still missing (all Cbnk except one):** `cbnkVersion`
(the only discarded version word), the instrument-type discriminator
(0x6000 vs 1-note 0x6001 is not recoverable from NoteCount), the note-level
`id` gating the 0x6001 quartet, and the raw cwav index (the model holds a
resolved pointer; the substitution path loses the original index; Parse also
subtracts 0x5000000 from the war id, which Serialize must re-add). Csar's one
corrupt-only edge (out-of-range 0x220C raw words) is noted, not blocking.

**Execution order:** 0 — scans + harness scaffold (+ the include-dir fix);
1 — BCSEQ Serialize (harness proves N/N sha over every embedded .bcseq);
2 — Cbnk retained-model split (full stage-0 gate, output-identical);
3 — BCBNK Serialize; 4 — BCSAR container Serialize (**the stage-1 proof
criterion**); 5 — optional deep-re-embed capstone.

## Suite stage 1 commit 0 — scans + round-trip harness scaffold (2026-07-14)

The blueprint's commit 0: the two decision-gate corpus scans, the round-trip
verifier scaffold, and the `caesar_core PUBLIC src` include the stage-0 handoff
scheduled. The shipped `caesar` is untouched (output-identical on every surface);
everything here is a new dev binary + a new `tools/` wrapper + docs.

**The new target.** `caesar-roundtrip` (`src/roundtrip/main.cpp`) is `caesar_core`'s
first external consumer — which is why `caesar_core` now carries an explicit
`target_include_directories(caesar_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")`
instead of leaning on libsmfc's PUBLIC include side effect. That change is provably
output-neutral: `src/` is already on caesar's include path via that same libsmfc
route, so CMake dedupes the directory and Ninja re-links **only** `caesar-roundtrip`
on reconfigure — `caesar_core`/`caesar` objects are not even recompiled (their
compile commands are byte-identical), and the corpus A/B confirms the exe bytes'
output is unchanged. The source lives under `src/` so the ab-verify / diag-goldens
stale-exe guards see edits to it; the CMake target is separate, links `caesar_core`
PRIVATE under the same `/W3 /WX`, and the release workflow packages only `caesar`
(`cp "$BIN"`), so the new exe is built by CI but never shipped and cannot break the
zip. `.github/workflows/build.yml` and `release.yml` both run generic
`cmake --build … --config Release` (all targets, located purely by path), so
nothing there needed editing.

**The exe.** Read-only: it calls `Csar::Parse()` (no I/O, no directories, no
child construction), never `Extract`/`Export`, and keeps the archive in memory.
Three modes:

- `--verify` (default, the scaffold) enumerates every embedded child — BCSEQ /
  BCBNK (the deep targets), BCWAR / BCWAV / BCWSD / BCGRP (opaque), plus the BCSAR
  container — recursing into embedded groups and **de-duplicating by archive
  offset** (a grouped archive lists a file both as a top-level FILE entry and
  through its container's file table; both resolve to one offset). For each it
  **copies the source span out** and compares a re-serialisation against that copy,
  never the live buffer — so the future buffer-drop honesty guard is structural. A
  format with no `Serialize()` reports SKIPPED. Exit contract mirrors ab-verify:
  `0` all-match, `1` mismatch, `2` harness error. With zero serializers today every
  format is SKIPPED and the run exits `2` ("nothing verifiable") — the harness can
  never print a false pass before the serializers land. Whole-corpus scaffold run:
  82 archives, **44,825 children walked** (BCSEQ 20,791 · BCBNK 11,136 · BCWAR
  10,564 · BCWSD 1,157 · BCGRP 1,095 · BCSAR 82), all SKIPPED, every archive exit
  `2`. The `tools/roundtrip-verify/roundtrip-verify.ps1` fan-out (ab-verify-grade:
  Stop-harness funnel, `trap`, AST shadowed-helper guard, stale-exe guard,
  zero-children guard, `-SelfTest`) aggregates to exit `2` — never a false pass.
- `--scan-varlen` / `--scan-gaps` carry the two gate scans (below).

**Scan 1 — VarLen canonicality (the commit-1 gate).** `ReadVarLen` accepts padded
encodings but the model stores only the decoded value, in exactly three Arg1 slots
(note gate time for status `< 0x80`, the `0x80` wait, the `0x81` program) — and only
when no `Rnd`/`Var` prefix retyped Arg1, so `cmd.Arg1 == VarLen` marks them uniquely
(verified against `Cseq::Parse` — VarLen is assigned nowhere else). Method: for each
such command, replay its own prefix + status (+ velocity for a note) byte layout to
reach the VarLen slot, decode it, and compare the measured byte count to the
canonical (minimum) length; a non-canonical encoding is equivalently a leading `0x80`
continuation byte, and the scan asserts both definitions agree. Every decode is
**self-checked** against `Args.back()` (the stored value), which throws on any replay
drift — that invariant held for all 3.27M args, proving the replay correct.
**Verdict: canonical-only.** 82 archives, 20,791 sequences, **3,268,437** VarLen
args, **0 non-canonical**, 0 harness errors. → **Commit 1 emits canonical VarLen
with no model change** (no raw-length field on `CseqCmd`).

**Scan 2 — FILE-section gaps (the commit-4 gate).** Sites: CSAR inter-blob, CSAR
section boundaries (FILE-section leading + trailing pad), CGRP inter-file. The scan
first surfaced a structural fact that the naive "flat array of non-overlapping
blobs" model got wrong: **the CSAR FILE section nests.** In grouped archives
(GardenSound, and the ctr_dash class) a CGRP container blob physically holds its
CBNK/CSEQ/CWAR, and each contained file **also** appears as its own top-level FILE
entry whose data offset points *inside* the container — so "gap between consecutive
FILE entries" is meaningless (entries nest, and a naive scan reports huge overlaps
and non-zero "gaps" that are really other files' magic bytes). The corrected model
splits blobs into the **maximal (top-level)** set — a blob whose end does not exceed
the running max-end of everything sorted before it is contained in an earlier
(≥ start, ≥ end) blob — and measures gaps only between top-level blobs (extents from
the INFO-declared allocation length). CGRP file offsets are group-relative and were
lifted to archive-relative; ctr_dash's FILE-section length runs past its own end
(external content), so the trailing bound is clamped to the physical archive. A
standalone Python reimplementation of the corrected model cross-checked the C++
number-for-number.

**Gap verdict: every gap byte is zero — reproduce-by-rule is viable at all three
sites.** 82 archives: CSAR top-level inter-blob **11,050 gaps, 0 non-zero, 0
partial-overlap anomalies**; CSAR section pads (78–80 lead/trail) **0 non-zero**;
CGRP inter-file **509 gaps, 0 non-zero, 0 overlaps, 0 nesting**. **The load-bearing
caveat for commit 4:** **4,747** CBNK/CSEQ/CWAR are nested inside CGRP container
blobs (across the 11 grouped archives). The BCSAR serializer must recognise a FILE
entry whose data lies inside another (container) blob and **re-point** it into the
copied container span rather than copy it independently — copying both would
duplicate the data and desync every downstream offset. Blueprint default was opaque
gap-spans; the proven all-zero result **upgrades all three sites to reproduce-by-rule
(align + zero-fill)**, with the container-nesting handling as the real new work.

**Gate (all green).** Warning-clean MSVC Release build (`caesar-roundtrip` under
`/W3 /WX`). Corpus A/B vs `HEAD` (`fd76cc3`): **82 archives / 257,125 files
byte-identical, stdout/stderr identical, exit 0** (baseline 77 s, new 61 s).
Diagnostics goldens **18/18 byte-identical** (exit 0). `roundtrip-verify -SelfTest`
green (exit-2-when-nothing-verifiable, missing-archive → harness error, verdict maps
0/1/2 and vacuous→2 exactly); the corpus fan-out aggregates to exit 2.

**For commits 1 and 4.** Commit 1 (BCSEQ Serialize): go — emit canonical VarLen, no
`CseqCmd` change; the harness will flip BCSEQ from SKIPPED to a per-`.bcseq` sha
proof (20,791 sequences). Commit 4 (BCSAR container Serialize): the three
padding/section-gap sites are reproduce-by-rule (align + zero-fill), but it must
carry the container-nesting model — a nested FILE entry is re-pointed into the copied
container, never re-copied. When a serializer lands, extend `roundtrip-verify
-SelfTest` with the byte-flip proof (mutate one source byte of a verified child,
require exactly one mismatch), the one contract the self-test cannot exercise until
there is something to verify.

## Suite stage 1 commit 1 — `Cseq::Serialize()`, the BCSEQ round-trip serializer (2026-07-14)

The blueprint's commit 1: `std::vector<uint8_t> Cseq::Serialize()` on `caesar_core`,
the exact inverse of `Cseq::Parse`. It reconstructs the whole `.bcseq` — CSEQ header,
DATA command stream, LABL symbol section — from model state alone, never re-reading
`Data`, and the round-trip harness proves it byte-identical over the entire corpus.
Additive: nothing on the shipped export path calls it, so `caesar` stays
output-identical.

**Empirical method.** Before touching C++, a standalone Python re-implementation of
the parser AND a candidate serializer was iterated against real `.bcseq` blobs pulled
straight from the archives until it round-tripped **all 20,791 sequences byte-for-byte**;
that reference pinned every layout fact below and every serializer decision the C++
then mirrors. (Kept in the session scratchpad, not committed.)

**The byte layout, confirmed from a corpus hexdump.** A `.bcseq` is `[header 0x40]
[DATA][LABL]`, contiguous, DATA before LABL, every section length a multiple of `0x20`.
The header is the 0x2C meaningful bytes (magic/BOM/`0x40`/version/file-length/block-
count `2`/the two `0x5000`+`0x5001` section refs) zero-padded to `0x40`. The command
stream is strictly linear (Parse's `while pos < end` walk has no gaps), so emitting the
command map in offset order reproduces the DATA body — with two exceptions that are the
whole story of this commit:

**Finding 1 — DATA padding parses as phantom commands that spill into LABL.** DATA is
`0x20`-aligned with trailing **zero** padding, but Parse does not know that: it walks
the padding as note commands (`0x00` = note key 0), and the last such phantom reads its
VarLen gate byte from the *first byte of LABL* (`'L'` = `0x4C`, high bit clear, so it is
a single-byte value and the spill is at most two bytes). So re-emitting the parsed
command map yields slightly MORE bytes than the section holds. Because serialize is the
exact inverse of parse, those emitted bytes equal the original parsed range exactly, so
the fix is to **truncate the emitted command bytes to the retained `DataLength - 8`** —
the leading part is the true DATA content (real commands + zero pad), the spilled LABL
bytes are sliced off and LABL is rebuilt independently. This is why `DataLength` must be
*retained*, not recomputed: a phantom padding-note is byte-identical to a real key-0
note, so the true section boundary cannot be inferred from the model.

**Finding 2 — LABL carries duplicate-target labels the parse model dropped.** The LABL
entry table lists `count` records, each `[0x1F00, target-offset, name-length, name]`
with the name null-terminated and zero-filled so the next record starts 4-byte aligned;
the section is then `0x20`-padded. The entries are stored name-sorted in the corpus, but
Serialize reproduces the retained file order verbatim rather than assume it. The load-
bearing catch: Parse keys labels by target *pointer* (`labls[labl.Offset]`), so when two
distinct symbol names point at the SAME command offset (e.g. `plog.bcsar`: 24 entries,
two sharing offset `0xE7`), the map keeps only the last and the command carries one name
where the file listed two. The first C++ attempt rebuilt LABL from the per-command
`CseqCmd::Label` and therefore emitted a count of 23 — **2,583 sequences mismatched**,
all at the LABL count/entry-table bytes. Fix: retain the **full** label table
`std::vector<std::pair<std::string,uint32_t>> Cseq::Labels` (every entry, file order) at
parse, separate from the per-command label Export still uses for MIDI markers — so
Export (and the `.mid`) is untouched while Serialize is lossless. After that change:
0 mismatches.

**No BCSEQ-internal opaque gaps.** Every byte between DATA+8 and the section end is a
(possibly phantom) command; the only "unread" bytes are the section-alignment zero pad,
which is reproduced by rule (truncate-to-length for DATA, `0x20`-pad for LABL). The
model additions are the section-header words `DataLength`/`LablOffset`/`LablLength`
(retained like `DataOffset`/`Version` already were) plus the full `Labels` table; there
is **no `CseqCmd` change** — VarLen is emitted canonical, as the commit-0 scan proved
safe (3,268,437 args, 0 non-canonical).

**Writer plumbing (for commit 3).** The low-level emit primitives `WriteFixLen` /
`WriteVarLen` — exact inverses of `ParseContext::ReadFixLen` / `ReadVarLen`, appending
onto a `std::vector<uint8_t>` — live as free functions in `Common.hpp`/`Common.cpp`
alongside their read siblings, deliberately reusable by the Cbnk/Csar serializers.
CSEQ command-stream multi-byte args are big-endian (`WriteFixLen(..., littleEndian=false)`);
header/section words are little-endian; `Rnd` bounds are written verbatim in file order
from the retained `Arg1Rnd`/`Arg2Rnd` pairs. Two per-file static helpers
(`emitArg`, `serializeCmd`) mirror the Parse dispatch branch-for-branch.

**Harness wiring + the exit contract.** `caesar-roundtrip --verify`'s `trySerialize`
seam now parses a BCSEQ span (read-only borrow, name echo hushed) and returns
`Serialize()`; a mismatch prints a `MISMATCH … firstdiff=0x… got=… src=…` line pointing
at the exact byte to hexdump. The per-archive exit rule was tightened to the honest
floor: **exit 0 only when every child re-serialised AND matched (`skipped == 0`)**; a
partial verify — some matched, some still SKIPPED — is exit 2, never a pass. Because the
BCSAR container is itself a verify target and stays SKIPPED until commit 4 (and
BCWAR/BCWAV/BCWSD/BCGRP are permanently opaque), **every archive is partial today, so the
corpus aggregates to exit 2 with the BCSEQ row shown green at its full matched count** —
the documented contract (in the wrapper's `.DESCRIPTION`): the BCSEQ count is the proof,
exit 0 becomes reachable at commit 4. A new exe `--selftest` mode and the wrapper's
upgraded `-SelfTest` carry the **byte-flip proof** the commit-0 note scheduled:
re-serialise the first BCSEQ child, assert it reproduces the source, then flip one output
byte and assert the compare catches it — returning 0 only when both hold.

**Result + gate (all green).** `--verify` corpus-wide: **BCSEQ 20,791 / 20,791
byte-identical, 0 mismatched, 0 skipped** (82 archives; overall run exit 2 = PARTIAL, by
contract). Warning-clean MSVC Release build (`/W3 /WX`). Corpus A/B vs `HEAD` (`bd296f2`):
**82 archives / 257,125 files byte-identical, stdout/stderr identical, exit 0** (baseline
72 s, new 59 s) — the shipped `caesar` is untouched. Diagnostics goldens **18/18
byte-identical** (exit 0). `roundtrip-verify -SelfTest` green, including the byte-flip
proof on `caravel.bcsar` (`roundtrip=1 byteflip_caught=1`).

**For commit 3 (BCBNK, after the Cbnk model split).** Reuse `WriteFixLen`/`WriteVarLen`
from `Common`. The retain-the-header-words pattern applies (Cbnk's discarded
`cbnkVersion`, the inst-type discriminator, the note `id`, the raw cwav index — per the
blueprint). And the general lesson from Finding 2: any table the parse deduplicates or
resolves-through-a-pointer needs a separate file-order retention for a lossless
round-trip — check Cbnk's cwav/instrument references for the same collapse before
trusting a rebuild from the resolved model.

## Suite stage 1 commit 2 — the Cbnk retained-model + Parse/Export split (2026-07-14)

The blueprint's commit 2: the one class that never got the stage-0 model/exporter
split. `Cbnk::Convert` parsed the CBNK/INFO walk and emitted the SF2 in a single pass,
with the model living in `Convert`'s function locals (`insts`/`cwavs`), and
`CbnkInst::Offset`/`CbnkNote::Offset` were the codebase's last raw-pointer model fields.
Now `Convert` is split into `Parse` (the walk → retained model; every parse-phase
`Analyse`/`Warning`/`Assert` fires here) and `Export` (the live-`Cwav` sample resolution,
the `<id>.wav` echoes, the release-127 warning, the SF2 write). Csar (direct banks) and
Cgrp (deferred, group-resident banks) call `Parse` then `Export` back to back per bank,
so the output stream is byte-for-byte unchanged. **Pure refactor — additive retention,
no behaviour change.**

**The retained model (lossless, for the commit-3 serializer).** Promoted onto the object:
`Version` (the discarded `cbnkVersion`), `InfoOffset`/`InfoLength`/`CwavOffset`/`InstOffset`,
`Cwavs`, `Insts`. The two raw-pointer fields become span-relative `uint32_t BodyOffset`
(file offset relative to `Data`); the release-127 warning reconstructs `Data + BodyOffset`,
which is *exactly* the pointer the old field held, so its `-w` `AT POSITION` is unchanged
(the Csar direct case resolves to the bank's own frame; the Cgrp deferred case keeps the
same heap-relative value the old code produced — a latent nondeterminism inherited
verbatim, not introduced). Per-record retention the blueprint §4 called for: the
instrument-type discriminator (`CbnkInst::Type` — 0x6000 is not recoverable from a 1-note
`NoteCount`), the note `id` gating the layered-note quartet (`CbnkNote::Id`), the raw CWAV
index (`CbnkNote::CwavIndex`, kept beside the resolved `Cwav*` because the out-of-range
substitution path repoints the pointer to the first sample and would otherwise lose the
original index), and the raw instrument/note offset-table words (`TableMagic`/`TableOffset`,
so non-existent entries and the exact offsets round-trip). The war id is stored resolved
(`Cwar = raw − 0x5000000`); the serializer re-adds the constant (exact under uint32 wrap).

**Finding (deferred to commit 3): the CWAV/instrument references do NOT collapse, but the
body layout is out-of-order with gaps.** Checked for the Finding-2 pointer/dedup collapse
per the commit-1 lesson: the cwav table is a flat file-order list (no dedup), and each
note carries its own raw index, so a rebuild from the model is faithful — no separate
file-order retention needed there. The real Cbnk-internal trap (surfaced by a standalone
Python parser+serializer over all 11,136 corpus banks, the commit-1 empirical method):
instrument and note **bodies are placed at arbitrary offsets, out of index order, with
inter-body gaps**, because the offset tables are authoritative and the parser locates every
body through them. The read fields nonetheless tile each bank almost perfectly — corpus-wide
only **1,584 bytes are never read, all in 5 banks** (MeetSound, SoundData1: note-body tails
where a 0x6001 record strides 0x80 but the parser reads 0x50); the other 11,131 banks have
**zero** unread bytes. This settles the commit-3 serializer as **positional reconstruction
(each body written at its retained offset) + a retained unread-gap overlay**, not a linear
re-emit. That gap-capture and the `Serialize()` itself are commit 3; this commit only lands
the split + the lossless scalar model.

**Gate (all green).** Warning-clean MSVC Release build (`/W3 /WX`). Corpus A/B vs `HEAD`
(`5f5ae2a`): **82 archives / 257,125 files byte-identical, stdout/stderr identical, exit 0**
(baseline 77 s, new 60 s). Diagnostics goldens **18/18 byte-identical** (exit 0) — including
`multi-bleed` (the caravel bank's Note `.log` rows + `<id>.wav` echoes) and the `-w`
`w-pksnd`/`w-dlplay`/`w-queenstream` surfaces that carry Cbnk warnings, so the split's
diagnostic output is proven unchanged.

## Suite stage 1 commit 3 — `Cbnk::Serialize()`, the BCBNK round-trip serializer (2026-07-14)

The blueprint's commit 3: `std::vector<uint8_t> Cbnk::Serialize()` on `caesar_core`, the
inverse of `Cbnk::Parse`. It reconstructs the whole `.bcbnk` from model state alone, never
re-reading `Data`, and the round-trip harness proves it byte-identical over the entire
corpus. Additive: nothing on the shipped export path calls it, so `caesar` stays
output-identical.

**Empirical method (the commit-1 discipline).** Before touching C++, a standalone Python
parser + serializer was iterated against every embedded CBNK blob pulled straight from the
archives until it round-tripped **all 11,136 banks byte-for-byte**. That reference settled
the layout facts below — in particular it was the tool that discovered the out-of-order,
gapped body region and measured the exact unread-byte set — and the C++ then mirrors it.
(Kept in the session scratchpad, not committed.)

**The byte layout.** A `.bcbnk` is `[header 0x20][INFO …]`, with the INFO section spanning
the rest of the file. INFO is `magic + length + two sub-table pointers (0x100 CWAV, 0x101
inst)`, then the CWAV reference table (count + `(war word, sample id)` records), the
instrument offset table (count + `(0x5900 magic, offset)` entries), and the instrument and
note **bodies**. The header/tables are recomputed at their fixed positions; the war word is
the retained resolved index plus `0x5000000` (exact under uint32 wrap).

**Finding 1 — the body region is placed out of order, with gaps, so it cannot be re-emitted
linearly.** Every instrument body (located via the inst table, offset relative to
`InfoOffset + 24`) and every note body (via that instrument's note-offset table, offset
relative to the inst body + 8) sits at an offset the table dictates — and the corpus lays
them **out of index order**, interleaved across instruments, with padding between. The
serializer therefore writes the whole region **positionally**: allocate a `Length`-sized
zero buffer and write each instrument/note body at its retained `BodyOffset`. The three
instrument types serialize with different body headers (0x6000 implicit single note; 0x6001
note count + one `EndNote` byte per note + the parser-proven 4-byte-alignment zero pad;
0x6002 a big-endian note-count-1 half-word + a zero half-word); a note body is 0x40 bytes,
or 0x50 when its `id` is 0x6001 (the layered-note quartet). The `NoteBodyLength` /
`InstHeaderLength` rules are factored into one place so the parse-side gap capture and the
serialize-side write agree on every extent.

**Finding 2 — a few note-body tails and inter-body pads are never read; they must be
retained.** The read fields tile each bank almost perfectly, but a coverage scan (rebuilt
in the C++ `Parse` from the model extents) shows **1,584 bytes are never read corpus-wide,
all in 5 banks** (`MeetSound`, `SoundData1`): note-body tails where a 0x6001 record strides
0x80 while the parser reads only 0x50. The other **11,131 banks have zero unread bytes**.
So `Parse` now walks a coverage bitmap after building the model and retains each maximal
unread run that carries a non-zero byte (`Cbnk::GapSpans`, offset + bytes); `Serialize`
zero-fills the buffer and overlays those runs (all-zero runs are not stored — the fill
reproduces them). This is the CBNK analogue of the BCSEQ commit's two layout traps, and it
is why "reconstruct every field" alone is not lossless here.

**No table collapse.** Checked for the Finding-2-class pointer/dedup collapse the commit-1
write-up warned about: the CWAV table is a flat file-order list (no dedup), and each note
carries its own raw index (retained beside the resolved `Cwav*` the substitution path would
otherwise clobber), so a rebuild from the resolved model is faithful — no separate
file-order retention needed, unlike LABL.

**Harness wiring + the exit contract.** `--verify`'s `trySerialize` seam now parses a BCBNK
span (read-only borrow, name echo hushed, a scratch empty `Cwars` map + default `Options`
since `Parse` touches neither) and returns `Serialize()`. The exe's `--selftest` and the
wrapper's `-SelfTest` byte-flip proof were generalised from "the first BCSEQ child" to "the
first serialisable child of each deep format", so both BCSEQ and BCBNK are round-tripped and
have a planted one-byte diff caught (the wrapper walks the corpus until both formats are
proven). The per-archive exit rule is unchanged: BCBNK matching lifts `matched` but the
BCSAR container and the opaque BCWAR/BCWAV/BCWSD/BCGRP children stay SKIPPED, so every
archive is still PARTIAL → the corpus aggregates to exit 2, with BCSEQ and BCBNK both shown
green at their full matched counts. Exit 0 becomes reachable at commit 4.

**Result + gate (all green).** `--verify` corpus-wide: **BCBNK 11,136 / 11,136 byte-identical,
0 mismatched, 0 skipped** (BCSEQ stays 20,791 / 20,791; 82 archives; overall run exit 2 =
PARTIAL, by contract). Warning-clean MSVC Release build (`/W3 /WX`). Corpus A/B vs `HEAD`
(`d2ccb25`, commit 2): **82 archives / 257,125 files byte-identical, stdout/stderr identical,
exit 0** — the `Parse` gap-capture emits nothing and `Serialize` is dev-tool-only.
Diagnostics goldens **18/18 byte-identical** (exit 0). `roundtrip-verify -SelfTest` green,
including the byte-flip proof on `caravel.bcsar` for **both** BCSEQ (`roundtrip=1
byteflip_caught=1`) and BCBNK (`roundtrip=1 byteflip_caught=1`).

**For commit 4 (BCSAR container).** BCSEQ and BCBNK are now the two deep-serializable
children; the container serializer copies child blobs through as spans (recomputing every
offset AND length from the laid-out blob), and the commit-0 scan already settled the
reproduce-by-rule padding + the container-nesting re-point (a nested FILE entry lands inside
its CGRP container span, never re-copied). Once BCSAR lands and the opaque formats are the
only skips, the per-archive exit reaches 0 and the corpus aggregate flips from 2 to 0 — the
stage-1 proof criterion. One Cbnk-specific note: `Cbnk::Serialize` reconstructs bodies
positionally from retained offsets, so the optional deep-re-embed capstone (commit 5) that
re-lays-out children would need Cbnk to expose a relocation path — out of scope here, and
not required for the container round-trip, which consumes the child blob verbatim.

## Suite stage 1 commit 4 — `Csar::Serialize()`, the BCSAR container round-trip serializer (2026-07-14)

The blueprint's commit 4 and **the stage-1 proof criterion**: `std::vector<uint8_t>
Csar::Serialize()` on `caesar_core`, the inverse of `Csar::Parse`. It rebuilds the whole
`.bcsar` container — CSAR header, STRG string table, INFO section, FILE section — from the
retained model, reading the source buffer only through copy-through spans (opaque record
tails, the STRG lookup tree, the `0x220B` block, the child blobs), and the round-trip harness
proves it byte-identical over the entire corpus. With BCSEQ + BCBNK + BCSAR all serialising,
the milestone is reached. Additive: nothing on the shipped export path calls it, so `caesar`
stays output-identical.

**Empirical method (the commit-1/3 discipline).** Before touching C++, a standalone Python
parser + serializer of the CSAR/STRG/INFO/FILE layout was iterated against every corpus
archive until it round-tripped **all 82 byte-for-byte**. That reference pinned every layout
fact below and each of the four Finding-2-class surprises, and the C++ then mirrors it. (Kept
in the session scratchpad, not committed.)

**The byte layout.** A `.bcsar` is `[header 0x40][STRG][INFO][FILE]`, the three sections
contiguous (each section's offset = the previous section's end; lengths are `0x20`-aligned).
The header carries the version, the file-length word, and three `(id, offset, length)` section
references. STRG is `[0x18 header][string-offset table: count + `0x1F01` records + string
data][0x2401 lookup tree]`. INFO is `[8 header][8-entry reference block][seven sub-tables in
physical order][0x220B block]`; each sub-table is `count + entry-offset array + record region`.
FILE is `['FILE' + length][0x20-aligned blobs, zero-filled gaps]`.

**Finding 1 — `declaredLength` ≠ physical `Length` for archives with external content.** The
header's `0x0C` file-length word equals the physical file size for a self-contained archive,
but is *larger* when the archive references external files (Mario Kart 7's `ctr_dash`: declared
`0x19ECE3C`, physical `0x9B7C40` — the declared size is `FileOffset + FileLength`, counting the
external region the FILE table points past the archive's own end at). Parse validated it against
`Length` only for non-`0x02000000` versions and then discarded it; it is now retained
(`DeclaredLength`) and re-emitted verbatim, while the output is exactly `Length` bytes.

**Finding 2 — some archives carry no STRG symbol table.** Four corpus archives (safe,
niji_sound ×3) set `StrgOffset == 0xFFFFFFFF`; their bank/sequence names are numeric. Parse
already keyed the STRG walk and every symbol lookup on `strgOffset != 0xFFFFFFFF` (so the record
symbol-index word is not even read), which is exactly why the serializer treats each sub-table's
record region as an **opaque span** rather than re-emitting per-field: the record layout itself
varies with STRG presence. Serialize emits the header section refs unconditionally (`0xFFFFFFFF`
for the absent STRG) and skips the STRG body.

**Finding 3 — the `0x220B` "end" reference is a real trailing metadata block, not padding.**
Parse reads the 8th INFO reference (`0x220B`) as `InfoEndOffset` and treats it as the end of the
enumerated content. But `[InfoEndOffset, InfoLength)` holds a real archive-wide block (max
sequence/player counts and the like — `0x14` bytes in caravel, `0x2C` in ctr_dash), not zero
pad. It is copied through opaquely, exactly like the STRG lookup tree; the initial serializer
that zeroed it mismatched at `InfoEndOffset` on every archive.

**Finding 4 — the record payloads are opaque, so only the framing is recomputed.** The INFO
records carry structure Parse does not fully model: the CSEQ record has a large unparsed
bank-reference tail (the `CbnkOffset` sub-structure), the CBNK record an 8-aligned trailing word,
and the raw string index is folded into a `TypedName`. Rather than extend per-field retention for
all of that, each sub-table's **record region** (everything past its count + entry-offset array,
up to the next sub-table) is copied through as a span — the same treatment the player/set tables
already got. What Serialize *recomputes* is the framing: every section and sub-table offset/
length, each entry-offset array (from the retained record offsets), the STRG string-record
offsets, and the FILE table's `0x220C` data offset/length words. So the container structure is
provably understood while the not-yet-parsed record internals ride through losslessly — the
honest state of the model, and the same "opaque span for the unparsed region" pattern used at
every level.

**The nesting re-point (the blueprint's flagged new work).** In the 11 grouped archives a CGRP
container blob physically holds its CBNK/CSEQ/CWAR children, and each contained file *also*
appears as its own top-level FILE entry whose data offset points inside the container (4,747 such
nested entries corpus-wide). The FILE section lays out only the **maximal (top-level)** blobs —
the `splitMaximal` rule the commit-0 gap scan proved: sort by start ascending / length
descending, and a blob whose end does not exceed the running max-end of everything before it is
contained in an earlier one. Those are copied at their retained offsets (inter-blob gaps left as
the zero fill the scan proved). A nested entry is **not** copied independently — its bytes come
from the container copy — and its `0x220C` data offset is re-pointed to `f.Offset - (FileOffset
+ 8)`, which resolves inside the copied container. For commit 4 (blobs copied verbatim at their
original positions) that re-point equals the original word; it becomes non-trivial only when a
container is re-laid-out, which is the commit-5 capstone. The alignment rule (`0x20`) was
cross-checked to reproduce every original blob position, but the layout is done **positionally**
(from the retained offsets) so byte-identity does not depend on the rule holding.

**Retention extensions to `Csar::Parse` (all additive, output-identical).** `DeclaredLength`;
the three section placements `StrgOffset`/`StrgLength`/`InfoOffset`/`InfoLength`/`FileOffset`
(FileLength already retained); the 8-entry reference block's slot order (`InfoRefIds`); the five
sub-table start offsets (`Cseq/Cbnk/Cwar/Cgrp/FileTableOffset`, the analogues of the existing
`PlayerTableOffset`/`SetTableOffset`); and `CsarFile::RecordOffset` (each file's INFO record
position, for the `0x220C` re-point). No record-payload retention was needed — the opaque
record-region spans cover the unparsed tails, the CBNK trailing word, and the corrupt-only
out-of-range `0x220C` edge the blueprint noted.

**Harness wiring + the exit contract's final form.** `--verify`'s `trySerialize` seam gained a
`Csar*` parameter: for BCSAR it calls the already-parsed container's `Serialize()` directly (no
span re-construction — there is no span ctor and re-parsing would be redundant). The per-archive
exit rule reached its final stage-1 form: **exit 0 = every DEEP format present (BCSEQ/BCBNK/BCSAR)
matched (`deep_skipped == 0`)**; the permanently-opaque BCWAR/BCWAV/BCWSD/BCGRP children report
SKIPPED informationally and no longer hold an archive short of a pass (the old `skipped == 0`
gate could never be met, since those never re-encode). The exe's `--selftest` and the wrapper's
`-SelfTest` byte-flip proof were generalised to cover the container (a `proveOne` helper over
BCSAR + the first serialisable BCSEQ/BCBNK child); the wrapper's exit-2-contract self-test was
flipped to the exit-0 contract, and its `.DESCRIPTION`/README updated. The `tools/roundtrip-verify`
`README.md` "Status" section moved from "commit 0, everything SKIPPED, exit 2" to "stage-1
complete, exit 0".

**Result + gate (all green).** `--verify` corpus-wide: **BCSAR 82 / 82 whole archives
byte-identical, 0 mismatched** (BCSEQ 20,791 / 20,791, BCBNK 11,136 / 11,136; every archive exit
0; corpus aggregate exit 0 — the milestone). Warning-clean MSVC Release build (`/W3 /WX`). Corpus
A/B vs `HEAD` (`77437b0`, commit 3): **82 archives / 257,125 files byte-identical, stdout/stderr
identical, exit 0** (baseline 76 s, new 60 s) — the added `Parse` retention emits nothing and
`Serialize` is dev-tool-only. Diagnostics goldens **18/18 byte-identical** (exit 0).
`roundtrip-verify -SelfTest` green, including the byte-flip proof on real BCSAR (QueenStream),
BCSEQ and BCBNK children (`roundtrip=1 byteflip_caught=1` each).

**What still separates the model from stage 2's consumer.** The container now round-trips, but
the record payloads (CSEQ bank-reference tails, player/set entries, the `0x220B` block) are
opaque spans, not typed fields — a future stage that *edits* those (re-assigning a sequence's
bank, adding a player) must parse them, not just copy them. And child blobs are copied verbatim
at their original offsets: growing a child (stage 6 write-back) needs the commit-5 deep re-embed,
which re-lays-out children and consumes *computed* lengths — for which `Cbnk` (positional body
reconstruction) would need a relocation path. Neither is required for the byte-identical
round-trip that is the stage-1 proof; both are the natural next parses when a consumer needs to
change bytes rather than reproduce them.

## Suite stage 2 — the dry-player blueprint (2026-07-14 survey)

Read-only survey producing the stage-2 execution plan. The player is a
real-time reinterpretation of `Cseq::Export`'s emit walk consuming the stage-1
models; the seq → bank → wave-archive → sample resolution chain already exists
(`CsarCseq` → `CbnkRecords` → `Cwars`).

**Architecture:** a new `caesar_play` static library (linking `caesar_core`;
the engine stays out of the core so ab-verify/roundtrip-verify keep guarding it
unchanged) + a `caesar-play` offline-render exe. Components: SeqRuntime (the
play-time VM — variable/comparison/[If]/control-flow semantics reused verbatim
from the convert-time VM; the ONE structural change is concurrent tracks
against a shared frame clock instead of the sequential stand-in walk),
VoiceAlloc (24-voice priority pool, refuse-if-front-outranks, verbatim from
the disasm), the per-voice DSP chain (loop-aware fetch → interpolation →
NW4R EnvGenerator → gain/pan → 32,728 Hz float bus), the 160-sample frame
clock (4.889 ms), and one final sinc resample → WAV. Offline render first;
no audio device until the offline path is golden-pinned.

**The load-bearing envelope correction:** the player ports NW4R
`EnvGenerator`/`CalcRelease` directly (127→65535/ms instant; the
DecibelSquareTable/attackTable pair byte-confirmed in the MiiPlaza binary) and
must IGNORE Cbnk's Attack/Hold/DecayTable + ConvertTime — those are SF2
timecent approximations for sf2cute, not engine truth.

**Fidelity inventory** (full table in the survey): exact-already = the whole
VM command space; needs-native = envelopes, the ~462k-event `_t` ramp mass,
tie single-voice, sweep pitch, LFO (one persistent retargetable LfoParam,
pitch depth = depth×range cents), additive pan, bend, portamento, mono/poly,
biquad LPF, fx sends (buses stubbed until stage 3), velocity range, mute,
damper, priority, mid-sequence bank switch, program/Tune. Genuinely unknown:
the console interpolation filter (per-note Interpolation byte + 3dbrew
"polyphase select"; Azahar routes it to linear behind a TODO — ship linear,
capture the truth later via the teakra oracle), the mod2/3/4 curves (stage 4),
the 3DS UpdateTick frame period (160/32728 physically forced, unconfirmed),
player/set table params (default: priority 64, 24 voices).

**Verification:** Net A — deterministic golden-hash renders (seeded, fixed
accumulation order, goldens in %LOCALAPPDATA%, diag-goldens discipline incl.
byte-flip self-test) from the first .wav. Net B — console-capture tolerance
(surround-probe Welch-PSD + envelope-fit precedent), EXCEPT the reverb tail.
**Capture inventory verdict: the old music captures are gone** (scratchpad
casualties; only the surround-probe run.wav survives, and it isn't music).
Net B needs fresh New 3DS captures: BGM_MAIN_Mii_Only_One (the discriminating
1.6 s-gap track), per-instrument isolated notes (also closes the queued
decay-table console spot-check), and the MeetSound SE set.

**Commit order:** Phase I — C1 scaffold targets, C2 single-voice DSP + mix bus
+ WAV out + the golden harness, C3 sequencer spine (notes/rests/tempo/
control-flow/noteWait, concurrent tracks) = **first audible .wav**. Phase II —
C4 EnvGenerator, C5 voice allocator + priority + mono, C6 native pan/vol/
pitch/Tune. Phase III — C7 ramp synthesis (the reusable flattener stage 5
shares), C8 tie/sweep/portamento, C9 LFO, C10 bank switch + velocity range +
mute + LPF. Phase IV — C11 the console tolerance net (needs the new captures).
Explicitly deferred: reverb/delay/surround (stage 3), real LCG + mod curves
(stage 4), real-time device output, IMA-ADPCM/CWSD coverage.

## Suite stage 2 Phase I — the dry player's first audible .wav (2026-07-14)

Executed C1–C3 of the dry-player blueprint: the `caesar_play` engine and
`caesar-play` CLI now render a real BCSAR sequence to an audible `.wav`. Three
commits, each warning-clean on all targets with `caesar` itself byte-identical
(ab-verify baseline exit 0, diag-goldens exit 0 at every commit).

**Commits:** C1 `784d2ed` (scaffold + loader), C2 `9c32155` (single-voice DSP +
mix bus + WAV + Net-A goldens), C3 (this) — the sequencer spine.

### Architecture as built

A `caesar_play` STATIC library links `caesar_core` PUBLIC and lives *outside*
it, so the converter's byte-identical A/B and round-trip guards keep watching
`caesar_core` unchanged — the player is a parallel consumer of the same parsed
models, never a modifier of any export path. Five translation units:

- `Loader.cpp` — read-only archive load + seq→bank→wave-archive→sample resolution.
- `Dsp.cpp` — `resolveVoice` (note→sample), `renderVoice` (loop-aware fetch +
  linear interpolation + trivial gate → native bus), `finalizeToPcm` (final
  resample + 16-bit quantize), `renderSingleNote` (the C2 proof).
- `SeqRuntime.cpp` — the concurrent per-tick VM producing note events.
- `Wav.cpp` — the RIFF/WAVE writer (Cwav::ExportWav's shape, explicit LE).
- `main.cpp` — the CLI.

The rate pipeline follows SUITE-DESIGN: every voice resamples to a float stereo
bus at **32,728 Hz** (linear per-voice interpolation for now — Azahar routes the
console's polyphase to linear behind a TODO, and the blueprint says ship linear,
recover the truth later via the teakra oracle), and **one** final resample takes
the bus to `--rate` (default 48 kHz).

### The loader without Export

`Csar::Export` is the only place children get constructed, and it writes files
as it goes. The player needed the construction+Parse *without* the writes, so
`loadArchive` replicates just that skeleton against the live archive buffer:

- `Csar::Parse` builds the record tree (no I/O, no child construction).
- For each `CsarCwar`: locate the blob (`Files[id].Offset + 12` holds the length
  word; the data span starts 16 bytes before it — exactly Export's arithmetic),
  bounds-check, `new Cwar(borrowed span)`, `Cwar::Parse`, then **manually** loop
  its `CwavRecords` constructing borrowed `Cwav`s and `Cwav::Parse`ing them (which
  decodes PCM into `Channels` and sets `Converted`) — never `ExportWav`. Absent
  (external) wave archives are still recorded as **null slots** in `Csar::Cwars`,
  because a bank resolves a sample's wave archive by *advancing `Cwars.begin()` by
  the stored index* and that positional count must include the nulls.
- `resolveSequence` builds the chosen sequence's `Cbnk` (samples resolve live
  against the decoded `Cwars`, no `.wav` re-read) and its own `Cseq`.

`ParseContext` frames stay balanced by RAII: `LoadedArchive` declares
`ctx → csar → bank → seq`, so reverse destruction pops seq/bank first, then
`~Csar` frees the wave archives it built, then the context outlives them all.
`cout` (which the constructors echo filenames to) is silenced during the load
with a restoring `ostringstream` guard; `cerr` errors still surface. Loader
diagnostics are accepted collateral of a new surface — they are not pinned.

`resolveVoice` mirrors `Cbnk::Export`'s SF2 resolution exactly: the instrument's
key-split zone whose `[StartNote, EndNote]` contains the key (uniform over the
0x6000/0x6001/0x6002 instrument types), then the live `Cwav` via the *same*
positional `Cwars` index. Pitch = `2^((key − rootKey)/12) × Tune` folded into the
sample-rate ratio; gain = `velocity/127 × zoneVolume/127`; loop = `SampleMode`
odd with valid `[LoopStart, LoopEnd)`, else one-shot.

### The resample choice

A Blackman-Harris-windowed sinc, HALF = 16 (32 taps), evaluated through a
2048-phase precomputed kernel table, cutoff `min(1, outRate/32728)`. Rationale:
the mix bus is band-limited to the native Nyquist (16.364 kHz) *by construction*,
so for the common upsample (48/96 kHz) the interpolation is provably transparent;
Blackman-Harris gives ~ −92 dB sidelobes (no Bessel needed, unlike Kaiser); the
phase table keeps it fast and fully deterministic (no per-sample transcendentals
on the hot path). It sits far below the real accuracy ceiling (the per-voice
linear interpolation and the not-yet-native envelope), so it is not the limiter.
16-bit quantize is clamp-to-[−1,1] then `lround(x·32767)`.

### The sequencer spine (C3)

The one structural change from `Cseq::Export`'s sequential stand-in walk:
**concurrent tracks against a shared 160-sample frame clock.** Each frame
(4.889 ms) a fractional-tick accumulator advances by `tempoBPM/60 × timebase ×
160/32728` ticks; on each whole tick every open track is stepped until it blocks
on a note/rest. Note-ons are stamped at the frame-quantized sample position
(this *is* the console model — the sequence runtime updates once per DSP frame),
and gate length = `noteLength × samplesPerTick`. Phase A runs the whole VM to
produce note events; Phase B renders each event's voice into the bus in event
order (deterministic accumulation). Defaults: tempo 120 BPM, timebase 48 (the
converter's own fallbacks); noteWait **on**; per-track program 0; cmpFlag true.

The 48-slot variable file keeps the converter's scoping — slots 0–31 shared
across the single player, 32–47 per track — and here the concurrency makes global
writes naturally visible (Export's sequential walk was the stand-in for exactly
this). The `[If]` gate, the 12 arithmetic and 6 comparison ops, Var/Rnd argument
resolution (Rnd → range midpoint, PRNG-free), and the counted/spin-loop bounding
(vmVersion + per-jump-site retake budget) are ported near-verbatim from
`Cseq::Export`. An **unconditional** backward jump to already-played code is the
whole-song loop: the track renders its body once and ends (the max-seconds cap
catches everything else). Call/Return keep a per-track stack; a stray Return ends
the track, exactly as Export does.

**Commands executed:** notes 0x00–0x7F, 0x80 rest, 0x81 program, 0x88 OpenTrack,
0x89/0x8A/0xFD/0xFF jump/call/return/fin, 0xB0 timebase, 0xE1 tempo, 0xC7
note-wait, 0xFE alloc-mask (consumed), and the extended 0xF0 VM (0x80–0x8B,
0x90–0x95).

**Commands safe-skipped (and why — none bear time, so the cursor never
desyncs):** every parameter/effect command is instant, so skipping it only omits
audio, never mis-times the stream. In `BGM_DEN_RESULT`/`BGM_MAIN_Mii_Only_One`
the live skips were **0xC0 pan, 0xC1 track volume, 0xD5 expression, 0xD9 reverb
send**. Their homes: pan/vol/expression → **C6** (native pan/vol/pitch/Tune);
reverb send (0xD9/0xDA/0xDE) and 0xC2 master vol + 0xD7 span → **stage 3** (fx
buses/reverb/surround); 0xC3/0xC4/0xC5 bend/RPN and 0xE3 sweep + 0xC9/0xCE/0xCF
portamento → **C6/C8**; 0xCA–0xE0 LFO family → **C9**; 0xB6 mid-sequence bank +
0xB3 velocity range + 0xDD mute + 0xB4/0xB5/0xD8 biquad/LPF → **C10**;
0xB1/0xD0–0xD3/0xFB envelope stage overrides → **C4**; 0xC6 priority + 0xB2 mono/
poly → **C5**; the extended mod2–4/userproc family → **stage 4**. The CLI prints
the skipped set per render; `RenderStats::skippedOps` carries it for the report.

### C3 render evidence

`BGM_DEN_RESULT` (MeetSound.bcsar, MiiPlaza) is the pinned audible proof — chosen
because it **ends naturally** (a track's unconditional backward jump closes the
whole-song loop, so the body renders exactly once), sets a **real tempo**
(140 BPM, not the 120 default), and runs 10 concurrent tracks:

- 419 notes fired, **0 dropped**; 10 tracks; timebase 96; 6437 ticks.
- Rendered 29.20 s. Expected tick-length duration = `6437 / (140/60 × 96)` =
  **28.74 s**; the 0.46 s remainder is the final notes' gate/tail — **duration
  matches tick-length at tempo.**
- Audibility (programmatic): overall RMS **0.30**, per-second RMS swings 0.15–0.58
  (dynamic musical structure, not a drone), ~29 energy-onset transients. Peak
  clips at full-scale — expected for the dry polyphonic sum without C6 track/
  master volume or a stage-3 limiter; documented, does not affect determinism.

FluidSynth structural comparison: `SE_SQUARE_CONGRATULATION` rendered by
`caesar-play` is **2.61 s**, byte-for-byte the same *duration* as its FluidSynth
render in `caesar_AB_MiiPlaza/…__FULLFIX.wav` (2.61 s), with correlated early
onsets (caesar-play 0.06/0.16/0.26/0.36 s vs FluidSynth 0.06/0.22/0.34/0.42 s).
The sequence timing lines up; the onset-count difference is the missing native
envelope (C4) and the clipping, not a timing error.

`BGM_MAIN_Mii_Only_One` (the blueprint's Net-B discriminator) also renders
cleanly (591 notes, 6 tracks) but loops forever, so it hits the `--max-seconds`
cap rather than ending — kept out of the golden set in favour of the
finite-length `BGM_DEN_RESULT`.

### Golden inventory (Net A — `tools/play-goldens`, local under %LOCALAPPDATA%)

- `note-caravel` — `--render-note` on caravel (F-Zero): the C2 single-voice DSP,
  one note at its root key. sha stable through C3 (the DSP is untouched by the
  sequencer), which the harness confirms.
- `bgm-den-result` — `--render BGM_DEN_RESULT --max-seconds 120`: the C3 audible
  proof (finite ~29 s render).
- `se-square` — `--render SE_SQUARE_CONGRATULATION`: the small FluidSynth-
  comparison SE.

The harness runs each render **twice** and requires byte-identical output (the
determinism guard), verifies exit codes, and its `-SelfTest` plants a golden flip
and confirms detection. Nothing corpus-derived is committed — only the script +
README are in the repo, exactly like ab-verify/diag-goldens.

### Whole-song-loop / cap policy

Sequences loop forever by design. Policy: an **unconditional** backward jump to
already-played code ends that track (render the loop body once); when all tracks
end, the render stops with a short voice tail. A sequence with no such natural
end (or a loop longer than the cap) stops at `--max-seconds` (default 300) and
says so. This gives finite, deterministic renders for the common case and a hard
safety bound for the rest.

### Handoff for Phase II

- **C4 — NW4R `EnvGenerator`** is the load-bearing next step. `renderVoice` today
  applies only a ~2 ms linear declick at both gate edges (explicitly *not* an
  envelope). Port `EnvGenerator`/`CalcRelease` directly (127 → 65535/ms instant;
  the DecibelSquareTable/attackTable pair is byte-confirmed in the MiiPlaza
  binary) and **ignore** `Cbnk`'s Attack/Hold/Decay/ConvertTime — those are SF2
  timecent approximations, not engine truth (see the blueprint). The note record
  already carries raw Attack/Decay/Sustain/Hold/Release bytes; `resolveVoice`
  should hand them to the voice. This also fixes the current hard-gate one-shots.
- **C5 — the 24-voice priority allocator.** C3 allocates voices freely (one per
  note event, unbounded). Add the pool (24, confirmed), the priority-sorted active
  list, and the refuse-if-front-outranks steal rule (verbatim from the disasm).
  0xC6 priority and 0xB2 mono/poly feed this and are safe-skipped today.
- **C6 — native pan/vol/pitch/Tune.** Wire 0xC0/0xC1/0xC2/0xD5/0xDC and the
  combinePan logic into per-voice gain/pan; this also resolves the current dry-sum
  clipping (track/master volume attenuate the mix).
- **Deferred within Phase I scope (noted, not blocking a BGM render):** the
  re-roll/gatedExits loop-escape (matters only for a few Pokémon SE files, not
  BGM); tie (0xC8) single-voice legato → C8; mid-note tempo changes affecting an
  already-scheduled gate length (gate is computed at note-on); sub-frame note
  timing (frame-quantized by design). A lazy per-bank wave-archive decode (the
  loader decodes *all* internal wave archives up front, like Export) would cut
  load time on large archives — a clean optimization when it matters.

## Suite stage 2 Phase II — the "mostly right" milestone: envelope, pool, gain/pan/pitch (2026-07-14)

Executed C4–C6 of the dry-player blueprint. The player now shapes notes with the
real engine envelope, contends for the true 24-voice pool, and mixes with native
gain/pan/pitch — the blueprint's "mostly right" tier. Three commits, each
warning-clean on all targets with `caesar` itself byte-identical (ab-verify
257,125 files + diag-goldens 18/18 at every commit). play-goldens deliberately
recaptured per commit (renders change by design), with the diff inventoried below.

**Commits:** C4 `4f5da8b` (EnvGenerator), C5 `0e636e5` (24-voice pool), C6 (this).

### C4 — the NW4R EnvGenerator (the load-bearing correction)

`renderVoice` ported the NW4C/NW4R `EnvGenerator` in place of Phase I's ~2 ms
declick gate. Phases: Attack (per-ms `mValue *= attackTable[a]` toward 0, snap at
`> -1/32`), Hold (dwell at peak for `round((h+1)²/4)` ms), Decay (rate down to the
sustain target), Sustain (hold), Release (rate down on note-off), updated once per
DSP frame (4.889 ms), gain linearly ramped across the 160 samples of each frame.
The player **ignores** Cbnk's `Attack/Hold/Decay/ConvertTime` (those are sf2cute
timecent approximations).

**Constant provenance (every constant documented inline in `Dsp.cpp`):**

- `DecibelSquareTable[128]` (s16) and `attackTable[128]` (f32): **read
  byte-for-byte from StreetPass Mii Plaza `code.bin`** at vaddr `0x328844` /
  `0x328944` (file offset vaddr − `0x100000`) — the exact addresses
  `NW4C-disasm-handoff.md` records. Head/tail match its fingerprint
  (`-723,-722,-721,-651,…,-1,0`; `0.9992175…0.0`). This is the strongest possible
  provenance (the console binary itself), not a formula reconstruction.
- `CalcRelease`: ported verbatim (127 → 65535/ms instant; 126 → 24; x<50 →
  (2x+1)/128/5; else 60/(126−x)/5), **re-confirmed by this session's own capstone
  disasm at `0x201D60`/`0x201E3C`**. One curve serves decay and release.
- `SetAttack` = `mAttack = attackTable[a]` (disasm `0x201DD8`), `SetHold`
  = `round((h+1)²/4)` ms (disasm `0x201D40`) — both re-disassembled this session.
- **Amplitude conversion = `10^(mValue/400)`** (the decibel-square / *power*
  domain — the "square"). Derived numerically: `10^(DecibelSquareTable[s]/400)`
  tracks `s/127` to <1 % across all 128 sustain levels (e.g. s=16→0.1259 vs
  0.1260; s=32→0.2527 vs 0.2520), i.e. the sustain byte maps ~linearly to
  amplitude. The `/40`-vs-`/20` split of `GetValue()=mValue/10` is flagged for
  Net-B. `0x14AFC8` (the doc's "sustain calc") turned out to be the **pan** L/R
  sqrt calc, not the volume conversion — corrected here.

Per-track ADSHR overrides wired (converter drops them, no MIDI equivalent): `0xD0`
attack / `0xB1` hold / `0xD1` decay / `0xD2` sustain / `0xD3` release latch a track
override the next note uses; `0xFB` resets. **Proof:** an isolated note traces a
clean attack→hold→decay→sustain envelope; a `release`-127 note reaches Done in one
frame (4.9 ms) with no 3.5 s fake; `BGM_DEN_RESULT` extends 29.2 s → 34.9 s (the
new release tails).

**Flagged for Net-B (chosen, unconfirmed against this binary):** the attack-start
floor (`-2000`), the `-1/32` attack-done threshold, the per-frame vs integer-ms
update cadence, and the amplitude `/40`-vs-`/20`. Decay/sustain *timing* is
independent of the floor (it uses the fixed `DecibelSquareTable` range), so only
slow-attack rise-time and the release tail-to-silence carry that uncertainty.

### C5 — the single 24-voice priority pool (verbatim from the RE)

Replaced C3/C4's unbounded per-note allocation with one 24-voice pool
(`NW4C-disasm-handoff.md` session 3, "the Wii crib HOLDS", 0 refutations): reuse a
free voice; else take the **front** of the priority-sorted active list (lowest
*current* priority, released voices dropped to priority 1); **refuse if the front
outranks the requester** (the note silently does not sound); else evict the front.
Priority = playerPriority(64) + the track's `0xC6` value. `0xB2` mono re-triggers
the track's single voice (its previous note releases at the new note-on). The
allocator is an extracted, testable free function `play::allocateVoicePool` over a
public `PoolVoice`. Tie-break among equal priority: the **oldest** (lowest
allocation index) is stolen — a deterministic FIFO-within-priority choice, flagged
for Net-B (the sorted-insert tie order was not byte-traced).

**Stress evidence** (scratchpad harness driving `allocateVoicePool` directly):
30 simultaneous uniform-priority notes → exactly 24 survive, the 6 oldest dropped;
24 held priority-100 notes + a priority-50 latecomer → **refused** (front
outranks); 20×prio-100 + 4×prio-30 held + a prio-64 requester → steals a prio-30
voice, never a prio-100; 40 time-spaced notes → zero drops. Corpus:
`BGM_DEN_RESULT` peaks 24/24 and steals 42 voices — **all releasing tails (0 still
held)**, so no audible note is lost, only inaudible tails truncated at saturation
(RMS unchanged 0.2914). `BGM_MAIN_Mii_Only_One` peaks 18/24, no steals.

### C6 — native pan / volume / pitch / Tune

Track volume (`0xC1`), master volume (`0xC2`) and expression (`0xD5`) each convert
through the **same decibel-square domain as the envelope** —
`amp = 10^(DecibelSquareTable[byte]/400)` — and multiply into the voice gain
(multiplying per-source linear gains ≡ summing their decibel contributions, the
NW4R volume model). Velocity → `(vel/127)²` (linear-squared, the NW4R precedent per
the blueprint; flagged). Pan is additive — the note's own `CbnkNote.Pan` plus the
track `0xC0` + init `0xDC` pans as offsets from centre (the `combinePan` model) —
split into L/R by a standard **equal-power cos/sin** law; the engine's actual curve
is a sqrt polynomial (disasm `0x14AFC8`, `pan*0.008` normalisation), flagged for
Net-B. Pitch: `0xC4` bend scaled by the `0xC5` range (RPN 0,0; default 2 semitones)
plus `0xC3` coarse-tune (RPN 0,2) fold into the playback step as a one-shot offset
at note-on (continuous bend ramps are C7/C8). **Note the blueprint said "0xC4/0xC3
bend/range"; the converter's own RPN mapping makes `0xC5` the range (sensitivity)
and `0xC3` the coarse-tune transpose — wired per the code, not the label.**

**Clipping (before → after):** `BGM_DEN_RESULT` peak-clip fell from **21,444
samples (0.64 %)** at C4 to **11 samples (0.0003 %)** at C6; RMS 0.2914 → 0.1213
(≈ −18 dBFS); with a real stereo field (L/R RMS 0.098/0.141, 31 % spread). The
residual 11 samples are transient voice-alignment peaks the **console DSP also
hard-clamps** — the engine has no soft-clip, so none was added (`finalizeToPcm`'s
hard clamp is the faithful behaviour). Absolute output-level calibration is a
Net-B item.

### Golden diff inventory (`tools/play-goldens`, Net A)

- **C4:** all 3 renders move — the envelope replaces the declick gate on every
  note (`note-caravel`, `bgm-den-result`, `se-square`).
- **C5:** only `bgm-den-result` moves — the 42 release-tail truncations at pool
  saturation; `note-caravel` (single note) and `se-square` (small SE) are
  unchanged (no pool pressure). A clean inventory.
- **C6:** all 3 move — velocity², decibel-square volumes and equal-power pan
  change every note's gain/pan.

Every commit's renders are byte-identical run-to-run (the harness double-render
determinism guard), and `caesar` stays byte-for-byte unchanged throughout.

### Handoff for Phase III

- **C7 — ramp synthesis (`_t` suffix).** The ~462k `_t` events (volume/pan/pitch
  ramps) are still one-shot at note-on; C7 flattens them into per-frame parameter
  ramps (the reusable flattener stage 5's `.it` export shares). The per-voice mix
  params are currently latched once at note-on (`NoteEvent.volAmp`/`panOffset`/
  `pitchSemitones`); C7 will need them to evolve over the note's life.
- **C8 — tie / sweep / portamento.** `0xC8` tie (single-voice legato, one voice
  both edges), `0xE3` sweep pitch, `0xC9`/`0xCE`/`0xCF` portamento. The mono
  re-trigger (C5) is the nearest existing machinery.
- **C9 — LFO.** The `0xCA`–`0xCD` + `0xE0` family (still safe-skipped): one
  persistent retargetable `LfoParam`, pitch depth = depth×range cents (per the
  vibrato-gating memory). `0xCC` target routing already parsed by the converter.
- **C10 — bank switch + velocity range + mute + LPF.** `0xB6` mid-seq bank,
  `0xB3` velocity range, `0xDD` mute, `0xB4`/`0xB5`/`0xD8` biquad/LPF.
- **Still safe-skipped after Phase II:** `0xCA`/`0xCB`/`0xCC`/`0xCD`/`0xE0` (LFO →
  C9), `0xD9` reverb send (→ stage 3). Everything else a `BGM_DEN_RESULT` /
  `BGM_MAIN_Mii_Only_One` render touches is now native.
- **Net-B flagged unknowns accumulated:** envelope floor / attack-done threshold /
  update cadence / amplitude `/40`-vs-`/20`; velocity linear-vs-squared; the pan
  sqrt-polynomial vs equal-power; absolute output level; the pool sorted-insert
  tie order. All are calibration questions the console captures (Phase IV) settle;
  none blocks a structurally-correct render.

## Suite stage 2 Phase III — the fidelity mass: `_t` ramps, tie/sweep/portamento, LFO, track features (2026-07-14)

Executed C7–C10 of the dry-player blueprint. The per-track parameters became
**live**, tie/sweep/portamento/LFO render, and the remaining track features
(bank switch, velocity range, mute, LPF, damper) are native. Four commits, each
warning-clean on all targets with `caesar` itself byte-identical (ab-verify
257,125 files + diag-goldens 18/18 at every commit). play-goldens recaptured per
commit; the golden set grew from 3 to 7 with a repro pinned per feature.

**Commits:** C7 `be68c98` (ramp synthesis + live modulation), C8 `4f71097`
(tie/sweep/portamento), C9 `bb45c56` (LFO), C10 `19aa58d` (bank/velrange/mute/
LPF/damper).

### The one structural change: live per-frame modulation (C7)

Phase II latched volume/pan/pitch into each `NoteEvent` at note-on. C7 replaces
that with a **reusable timeline-flattener** (`play::ParamCurve`, the struct the
blueprint says stage 5's `.it` export shares): each track parameter is a
piecewise-linear function of absolute bus-sample position. A plain command is a
zero-duration step; a `_t`-suffixed command (`Suffix2` Time/TimeRnd/TimeVar, whose
trailing s16 is the duration in **ticks**, converted to samples at the current
tempo) glides from the curve's current value to the target. `renderVoice` now
samples the voice's `TrackTimeline` (+ the sequence-wide master-volume curve)
**every frame** through a new `VoiceMod`, so a mid-note parameter change follows
the note — the engine model — instead of being frozen at onset. The single-note
DSP proof (`VoiceMod` null) is byte-for-byte unchanged, so `note-caravel` stays
identical across all four commits; the sequence renders move only where a track
actually modulates a sounding voice.

**The ramp-domain finding (C7).** NW4R ramps the stored **control value** (the
0..127 byte) linearly and converts it through the console-read `DecibelSquareTable`
each frame — so a `_t` glide is linear in the byte domain, not in dB. Verified
numerically against the exact table (the same bytes read from the MiiPlaza binary
at C4): a linear-byte volume fade is amplitude-linear to **<1 %** across the range,
because the table is calibrated so `10^(table[b]/400) ≈ b/127` — *except at the
floor*, where byte 0 lands at **−36 dBFS** (the table minimum, `10^(−723/400)`),
not digital silence. So a volume fade to 0 is not silence; the envelope release is
what reaches silence. Evidence class: **NW4R precedent** (the MoveValue linear-in-
control-value smoothing) + a numeric check against the **binary-read** table; the
ramp routine itself was not byte-read (the disasm doc stopped at the variable VM),
so the *domain choice* is documented and flagged. Corpus `_t` ramps are sparse
per-track (the 462k are spread thin; the heaviest sampled track has ~13); the
render witness is `SE_BossVo_KaenAttack` — a single ~10 s (1000-tick) `0xC1` fade
that now glides smoothly to silence where at C6 the volume was frozen at note-on.

### Tie, sweep, portamento (C8) — evidence per feature

- **Tie (`0xC8`)** — one continuous voice across tied notes: the first tied note
  opens a region, each later one retunes the LIVE voice to its key (a stepped
  semitone offset on a per-region pitch curve) with **no re-attack**; both edges of
  `0xC8`, a Fin, and the track end close it (the v0.5.1-settled both-edges-release).
  Evidence: **document** (the converter's tie machinery, `Cseq.cpp:910-924,
  1384-1403`, and the NW4R `SeqTrack::NoteOn`/`UpdateChannelLength` semantics it
  cites). Measured on `SE_Map_WarpstarUp2`: 246 tied notes that at C7 each
  re-attacked and stole a voice (peak 24/24, 222 steals) collapse into **3
  continuous voices, 243 no-attack retunes, ZERO steals** — the continuous glide,
  at the voice level.
- **Sweep (`0xE3`)** — a signed 1/64-semitone intra-note ramp gliding to nominal
  over the note's gate; **portamento (`0xC9`/`0xCE`/`0xCF`)** — a glide from the
  origin key (0xC9's, else the previous note's) to the note's key. Independent and
  additive (the 2026-07-11 research). Evidence: **precedent, flagged** — the disasm
  doc records no sweep/portamento address, so the portamento time→duration mapping
  is a tick-count glide chosen against NW4R precedent and flagged for the capture;
  a 40-archive sample found **no clean 0xE3 sweep repro** (it is genuinely rare),
  so sweep is verified against the 1/64-semitone spec + the glide math, not a
  corpus render.

### The LFO (C9) — controlled proof

ONE `LfoParam` per track (depth `0xCA` / rate `0xCB` / range `0xCD` / delay `0xE0`
in 5 ms units / target `0xCC` = pitch|volume|pan), stored as live curves that
**persist across a retarget** — a value commanded on any target survives the switch
(the converter needed a wire/shadow model for this; the player just keeps the
values). Applied per frame by a **four-quadrant sine** (the high-resolution limit
of NW4R's 32-step quarter table). Evidence: **precedent, flagged** — the disasm doc
records no LFO/sine/rate address, so the rate→Hz constant and the sine resolution
are NW4R-precedent. The proof is a **controlled test** (a synthetic sine driven
through the REAL `renderVoice` with a known LFO): the vibrato rate is exactly linear
in the commanded rate — 64→5.01, 32→2.51, 96→7.52 Hz (5/64 Hz per unit) — the peak
pitch deviation is **depth × range cents** (10×5→±49, 20×5→±96, 10×10→±96), and the
targets route correctly (0 → 386-cent pitch FM; 1 → 20 % amplitude swing, no pan;
2 → 0.68 L/R balance swing). `depth × range` is clamped to ±1 octave defensively.

### The remaining track features (C10)

- **Bank switch (`0xB6`)** — the headline. The loader now builds + caches any
  `CbnkRecords` bank on demand (`getBank`); each track carries a current bank index;
  Phase B resolves every note against the track's live bank. Evidence:
  **binary-behaviour** — the 0xB6 arg is a **global `CbnkRecords` index**, confirmed
  empirically: `SEQ_M_ZUKAN_TEST_NG1` goes from 12/12 notes DROPPED (the switch
  target's instruments unreachable in the default bank) to **0 dropped** once the
  switch loads the right bank; `SEQ_M_BLACKBOARD1` resolves all 122 notes across 5
  switches, `SEQ_M_TONIGHT1` 276 across 3. A switch to an **external** (not-in-
  archive) bank honestly drops those notes rather than sound the wrong patch (so a
  few sequences drop MORE than at C9 — correct, not a regression).
- **Velocity range (`0xB3`)** scales note-on velocities (127 = identity);
  **mute (`0xDD`)** suppresses a track's notes while time still advances;
  **damper (`0xDF`, threshold ≥64)** defers a note's release until the pedal lifts
  (a gate extension). Evidence: **document** (the task spec + the converter's
  threshold note); simple, correct by construction.
- **LPF cutoff (`0xD8`) + biquad (`0xB4`/`0xB5`)** — a standard **RBJ low-pass
  biquad** (187.5 cents/unit, cutoff clamped to [0,1] of Nyquist per the converter,
  64 = open). Evidence: **chosen, flagged** — the disasm doc has no filter topology,
  so the RBJ topology + Butterworth Q are flagged; `0xB4`/`0xB5` (biquad type/value)
  fold into the same LPF path since the type mapping is unknown. Controlled proof:
  the high/low band-energy ratio falls **0.11 → 0.023 → 0.0022** as the cutoff drops
  64 → 48 → 32. `0xFB` envelope reset was already wired at C4.

### Golden inventory (Net A — `tools/play-goldens`, 7 renders, 4 source archives)

- `note-caravel` — the C2 single-voice DSP proof; **unchanged through C7–C10** (the
  `VoiceMod`-null path is untouched), which the harness confirms every commit.
- `bgm-den-result` — moved at C7 (live modulation of sustaining notes) and C9 (it
  drives the LFO); a finite ~35 s MeetSound render.
- `se-square` — a small MeetSound SE carrying 6 `_t` ramps; moved at C7.
- `ramp-kaen` (`SE_BossVo_KaenAttack`, Torte) — the C7 ramp witness (a ~10 s fade);
  also ties, so it moved again at C8, and drives the LFO, so again at C9.
- `tie-warpstar` (`SE_Map_WarpstarUp2`, Torte) — the C8 tie witness.
- `vibrato-zelda` (`SEQ_M_ZELDA1`, SoundData1) — the C9 LFO witness.
- `bank-blackboard` (`SEQ_M_BLACKBOARD1`, SoundData1) — the C10 bank-switch witness
  (122 notes, 5 switches, 0 dropped).

Every render is byte-identical run-to-run (the double-render determinism guard) and
the `-SelfTest` plant-a-flip check passes at each commit; nothing corpus-derived is
committed (only the script + README).

### Handoff for Phase IV

- **C11 — the console-tolerance net** is BLOCKED on fresh New 3DS captures (the old
  music captures are gone). Ask the user for: `BGM_MAIN_Mii_Only_One` (the
  discriminating 1.6 s-gap track), per-instrument isolated notes (also closes the
  queued decay-table spot-check), and the MeetSound SE set. Method precedent:
  Welch-PSD + envelope-fit against the capture, EXCEPT the reverb tail (stage 3).
- **Flagged unknowns the captures recalibrate** (all chosen-and-documented, single
  constants where possible, none structurally blocking a correct render): the LFO
  **rate→Hz** constant (`kLfoRateHz = 5/64`) and the sine resolution (continuous vs
  NW4R's 32-step table); the **portamento** time→duration mapping; the **LPF/biquad
  topology** (RBJ) + Q; the **velocity** `(vel/127)^2` law; the **pan** sqrt-
  polynomial vs equal-power; the envelope **floor / attack-done / update cadence /
  amplitude `/40`-vs-`/20`**; **absolute output level**; the pool **sorted-insert tie
  order**; and the console **interpolation filter** (ship linear; recover via the
  teakra oracle).
- **Known small approximations** (noted, not blocking): a re-opened track's timeline
  resets, so notes from its first opening see the reset curves (extremely rare);
  velocity retune inside a tie region is not modelled (pitch retune is); mute
  suppresses only NEW notes (a sounding voice is not retroactively silenced); a
  mid-ramp tempo change slightly mis-sizes a `_t` duration (converted once at the
  command tick). All are single-line fixes if a capture ever shows them.

## Suite stage 2 Phase IV (part 1) — the console-tolerance net, built, self-validated, and run against the real captures (2026-07-14)

Executed C11 of the dry-player blueprint: the Net-B console-tolerance harness
(`tools/console-tolerance/`), built and **self-validated before depending on any
capture**, then run against the two console captures that turned out still to
exist. No C++ changed — this is dev tooling, `caesar` byte-for-byte unchanged
(ab-verify + diag-goldens + play-goldens all exit 0; the harness adds no build
input, so the play-goldens/diag-goldens freshness gates see nothing stale).

### What it is

`console_tolerance.py` (numpy only — scipy is not installed, so every DSP
primitive is hand-rolled on `numpy.fft`, exactly like `tools/surround-probe`)
ingests a New 3DS line-in capture of a sequence and a `caesar-play --render` of
the same sequence, and verdicts them to the stage-2 criterion: **match within
tolerance, EXCEPT the reverb tail.** A PowerShell driver
(`console-tolerance.ps1`) renders each captured sequence at the capture's own
sample rate and runs the analyzer, with the ab-verify/play-goldens discipline
(build-freshness gate, Stop-funnel, exit 0/1/2, `-AllowStaleExe`).

**Alignment** (captures have unknown lead-in / gain / clock): bring the capture
to the render rate; integer onset-lag; a **scale-search cross-correlation of the
two onset-strength envelopes** for the tempo/clock ratio (continuous, so it is
robust to the density/reverb mismatch that defeats discrete onset-peak matching);
a global gain offset over the trusted window.

**Metrics and tolerances** (each with a reasoned reverb exclusion):

| Metric | Validates | Pass tolerance | Reverb handling |
|---|---|---|---|
| envelope-fit | envelope shape; tracks output level | median-subtracted shape residual <= 3.0 dB (music) / 6.0 dB (short note); dB-slope in [0.85, 1.15] | trusted = render above floor+6 dB, below peak-1 dB, before the last shared onset; frames where the console sits > 4 dB above the render are reverb-dominated -> excluded |
| onset-timing | tempo / frame-period / clock | \|slope-1\| <= 1.0 %; xcorr >= 0.08 floor; needs >= 4 onsets over >= 1 s | onsets are attacks; a single note is not a tempo (N/A) |
| psd-distance | spectral fidelity (interp/pan/velocity) | per-channel MAGDEV <= 6.0 dB (median-subtracted, 100-14000 Hz, over the body) | body only; median-subtraction removes level |
| loudness | dynamics realism (EMPTY_LANDSCAPE precedent) | \|d(p95-p50)\| <= 4 dB, \|d(p95-p25)\| <= 6 dB over the trusted body | upper distribution only — reverb fills valleys, so a valley-depth range always reads the dry render as "more dynamic"; that is the reverb exception, not a fault |
| reverb residual | *report only — stage 3's target* | never fails | the console energy where the dry render is silent (trailing tail AND interior all-voices-off gaps), relative to body energy |

The reverb exception is realised three ways, all so the exception cannot mask a
real fault: (1) the trusted window's lower bound is the render's own floor, so the
render-silent tail is excluded by construction; (2) within the window, only frames
where the *console* exceeds the *render* are excluded (a padded/wrong envelope,
where the *render* is louder, still fails); (3) the reverb residual is computed and
reported separately over every render-silent frame, never contributing to pass/fail.
The envelope is lightly smoothed (~15 ms) so the fit measures the ADSHR-scale
shape, not 5 ms micro-structure a reverb reshapes.

**Per-flagged-unknown diagnostics** emitted alongside the verdict: output level
(the render-vs-console gain), pan (L/R RMS ratio -> sqrt-poly vs equal-power),
vibrato/tremolo rate (1-12 Hz modulation FFT -> `kLfoRateHz = 5/64`), exposed-tail
decay time (exp-grid fit aligned at the -25 dB crossing — the EMPTY_LANDSCAPE /
Only_One precedent — reported for console AND render since release+reverb are not
separable from the tail alone), and an interpolation HF-energy proxy. Velocity and
portamento are flagged as needing dedicated repros (>= 2 isolated notes / a glide SE).

### The fixture-realism discipline (the 2026-07-11 lesson)

`--self-validate` synthesizes a fake "console capture" from a real render by
applying the **whole capture chain** — resample to the capture rate, a -6 dB gain,
the surround-probe's measured **-85 dBFS** noise floor, a 37 ms lead-in, **+-50 ppm**
clock drift, and a synthetic exponential reverb tail (-20 dB send, tau 0.4 s) so the
reverb-exception path is exercised. This is a direct answer to the 2026-07-11
surround-probe lesson (the synthetic validation passed but the real capture broke
the segmenter because the fixture was unrealistic): an idealized fixture is
worthless. The synthetic noise is seeded, so the whole self-validate is
deterministic (byte-identical run to run).

**Self-validation evidence** (on two real `caesar-play` renders as the base,
`BGM_DEN_RESULT` and `BGM_DEN_MAP`, plus the short SE `SE_SQUARE_CONGRATULATION`):
the realistic fixture PASSES all applicable metrics (e.g. base_map envelope 1.11 dB,
tempo slope 1.0000, PSD 0.20 dB, loudness d<=0.2 dB), and the three negative controls
each FAIL their named metric — a **wrong-tempo** render (retimed 2 %) fails
onset-timing (slope 0.9805 = 1/1.02, correctly recovered), a **padded-release**
render fails envelope-fit (and ONLY envelope-fit on the music bases — clean
attribution), and a **different sequence** fails. Exit contract verified:
0 pass / 1 out-of-tolerance / 2 harness-error (bad input, missing numpy, any
exception -> 2, never a false pass).

Two design corrections found during self-validation (both real): a relative-to-max
onset threshold made the dry render's single-dominant-transient yield a sparse
clustered onset set while the reverb'd console yielded a dense spread one — the
scale-search cross-correlation replaced discrete peak-matching to fix it; and a
single percussive note's ~4 clustered strength peaks spuriously triggered
onset-timing, fixed by requiring the onsets to span >= 1 s.

### The real captures (the "gone" captures were not gone)

Mid-task the user surfaced the 2026-07-08 captures at
`...\MiiPlaza\sound\MeetSound\BANK_BGM\`: `BGM_MAIN_Mii_Only_One_console.wav` and
`EMPTY_LANDSCAPE_console.wav`, both **192 kHz / 16-bit stereo** New 3DS line-in.
The harness reads 16-bit at 192 kHz directly and its default `-CapturesDir` now
points there. First dry-player-vs-console comparison (tolerances were calibrated on
synthetic fixtures + the self-validate bases, NOT on these captures):

| Capture | envelope | onset (tempo) | psd | loudness | verdict | reverb residual |
|---|---|---|---|---|---|---|
| `BGM_MAIN_Mii_Only_One` (60.5 s) | 2.31 dB | slope **1.0000**, 450 onsets | L/R 0.81 dB | dmbp 1.2 / drange 0.5 dB | **PASS** | **-35.6 dB** over 3.08 s (the ~51 s gap) |
| `BGM_DEN_EMPTY_LANDSCAPE` (29.1 s) | 2.33 dB | slope **1.0000**, 132 onsets | L/R 2.59 dB | dmbp 1.1 / drange 0.9 dB | **PASS** | **-6.0 dB** over 4.61 s |

Both PASS all four metrics — a strong, honest first result (not tuned to these
files). What the numbers say about the flagged constants:

- **Tempo / frame-period: confirmed.** The onset-timing slope is exactly 1.0000 on
  both, so the 120 BPM default and the physically-forced 160/32728 frame period are
  right against hardware.
- **The reverb residual is stage 3's target, and it is quantitatively the
  2026-07-08 finding.** It is a modest -35.6 dB on the sparse `Only_One` (mostly the
  ~1.6 s all-voices-off gap at ~51 s, where the console rings and the dry render is
  silent) but a large **-6.0 dB** on the dense `EMPTY_LANDSCAPE` pads — the
  numerical form of "the console's sustain there is carried heavily by DSP reverb a
  soundfont cannot encode." The dry player is *correctly* dry; the gap is exactly
  the reverb stage 3 must model.
- **Interpolation filter: the capture shows HF energy the render lacks.** The
  high-band (> 0.6*Nyquist at 192 kHz) energy fraction is -42.9 dB (console) vs
  -90.9 dB (render) on `Only_One` — the render is band-limited to the native
  16.4 kHz Nyquist by construction and clean-upsampled, while the console output
  carries reconstruction/imaging energy far above it. The linear-vs-polyphase
  question is real and lives here (recover via the teakra oracle).
- **LFO rate: a first reading.** A clean 4.00 Hz modulation (SNR 19) is measured in
  `Only_One` -> a commanded rate byte ~= 51 at the current `kLfoRateHz = 5/64`
  (1.45 Hz, SNR 36, in `EMPTY_LANDSCAPE`). Consistent with the constant; an
  isolated vibrato note would pin it.
- **Absolute output level** reads -19.8 dB (`Only_One`) / -12.9 dB
  (`EMPTY_LANDSCAPE`) render-below-console — recording-gain-relative (the slider/
  interface gain set the console level), so only the ~7 dB *difference between
  tracks* is potentially meaningful, and only if the same gain was used.

### Still open (Phase IV part 2)

The per-instrument **isolated-note** captures a busy BGM cannot isolate — `docs/
CAPTURE-REQUEST.md` names `SE_NEW_DRUM01` (a percussive one-shot for the queued
decay-table spot-check), `SE_LEGEND_KEY_FLY` (a ~9 s sustained voice whose exposed
tail is the release-vs-reverb witness), and two optional short SEs. Those close the
envelope-floor / decay-table / pan-law / absolute-level constants; portamento and
the velocity-squared law need their own repros. The captures are the user's to take;
the harness verdicts whatever is present, so a partial set is immediately useful.

## Suite stage 3 — the reverb-oracle recon (2026-07-14)

Scratchpad reconnaissance run the same day the stage-2 proof landed, taking
the teakra feasibility question past research to a working build. Findings:

- **Tooling**: `ctrtool.exe` (at the dump root) produced the existing
  `re_extract` tree; the five system titles' `code.bin` were already
  extracted. Python 3.14 + capstone, CMake 4.3.4, and VS 2022 BuildTools are
  all present and sufficient.
- **Firmware**: all five titles' DSP1 segments re-sliced fresh with their
  embedded SHA-256 verified. Three distinct images (MiiPlaza `944b40b5`; the
  byte-identical eShop/Photos/SystemSettings triplet `8e213f3e`; the
  AAC-capable Sound app `5c03dd63`) — but the DATA coefficient tables are
  byte-identical across all five, so for reverb **one oracle firmware serves
  all**: MiiPlaza's 49.8 KB image, whose archives are already the project's
  EMPTY_LANDSCAPE repro set.
- **teakra builds and runs here today**: C++17, interpreter-only, CMake-4-safe,
  MSVC via the VS generator with no vcvars; `teakra.lib` + `dsp1_reader.exe`
  built clean, and `dsp1_reader` parsed all five segments of the real MiiPlaza
  firmware (24,531 disassembled lines). The API surface needed for the oracle
  exists: `GetDspMemory()`, `Run(cycles)`, the APBP mailbox, and
  `SetAudioCallback` (the final stereo mix, where the reverb tail emerges).
- **The port template is one file**: Azahar `src/audio_core/lle/lle.cpp` —
  DSP1 segment loading, the boot handshake (flags=3 → channel-2
  `pipe_base_waddr` reply), the `PipeStatus` circular-buffer pipe protocol,
  16384 cycles/slice. Porting it minus Citra's threading/services is the
  whole oracle harness.
- **The no-HLE ruling confirmed at the source**: Azahar's `shared_memory.h`
  maps `DelayEffect` fully (delay is codeable now from its known transfer
  function) but `ReverbEffect` is 52 bytes of
  `INSERT_PADDING_DSPWORDS(26); ///< TODO` — an 8-year opaque stub. HLE
  literally cannot produce the reverb; the real firmware under teakra can.
- **Riskiest unknown + de-risk**: a malformed reverb config is silently
  bypassed, so "off" and "misconfigured" both yield a dry capture. Before any
  parameter sweep, capstone-scan MiiPlaza's ARM11 driver for the site that
  writes the 52-byte `ReverbEffect` block and replay those exact bytes as the
  known-good "engaged" baseline.

Artifacts (scratchpad, session-local): the recon report, the extraction
script, five verified firmware images, the built teakra tree, and the
MiiPlaza DSP disassembly. First stage-3 code commit scoped on the roadmap.

## Suite stage 2 — first-listen artifact diagnosis: the steal-cut click and the missing per-sound volume stage (2026-07-14)

The first human listening pass over dry-player renders surfaced two artifacts:
a click ~8 s into `BGM_DEN_EMPTY_LANDSCAPE` (MeetSound) and bass distortion in
the eShop menu music (TigerSound `SEQ_TIGER_TOP_EF` — identified by exact
duration match, then confirmed SHA-identical to a fresh default render). The
question was whether they shared a root cause. A five-lane investigation
(waveform forensics on both files, an instrumented diagnostic render logging
steal timestamps + the pre-clamp native bus, a gain-stage audit, and a
click-source audit) answered it conclusively: **two independent defects.**

**The click = a voice-steal hard cut, proven to one sample.** The
discontinuity sits at output t=8.0616 s = native sample 263,840 — *exactly*
1649 × 160, a DSP frame boundary. A center-panned contribution of ~0.104 FS
vanishes in one native sample (sign flips across the step, so it is a voice
*removal*, not a gain change; the band split shows the low end dropping ~9×).
The instrumented render's steal log has the matching event: at t=263,840 the
24/24-full pool stole two of track 3's long pads (note-on at 0.010 s, note-off
at 7.822 s, only 0.24 s into their release and still clearly audible) to serve
new note-ons. A second, ~7× smaller instance of the same signature sits at
16.372 s — also exactly on a frame boundary (3349 × 160). The render never
exceeds |0.776| pre-clamp (zero clipped samples), so clipping is excluded on
this file. Mechanism: `allocateVoicePool` sets `victim.stopAt = t` and
`renderVoice` simply stops accumulating at that sample with the envelope gain
still nonzero — the in-source claim "no declick needed" is true only for
envelope-terminated voices. The audit found the same missing-final-ramp family
in two more paths: `voiceEndSample` breaks on `done()` *before* counting the
frame that performs the final ramp (so a release-127 instant-release note-off
drops its entire sustain→0 ramp and cuts hard — this also makes the mono
re-trigger click for release-127 instruments, though releases ≤126 ramp
cleanly), and the `--max-seconds`/renderCap cut ends the same way. One fix
covers all three: render one extra frame past stopAt/env-Done with the gain
ramped to zero — which is also the hardware behaviour (the DSP interpolates
per-voice gain across each 160-sample frame, so even a stolen voice gets a
~4.9 ms fade on console). Steal *frequency* is not the bug: both renders
saturate the confirmed 24-voice pool (EL: 62 steals/87 notes; eShop: 357/655,
all victims already releasing, none refused) — hardware steals just as much,
it just fades what it kills.

**The distortion = clamp clipping from a missing gain stage.** eshop.wav has
2,367 samples pinned at the rails (0.036%, 93 runs, longest flat-top 4.3 ms at
18.454 s), clustered exactly in the loud bass-dominated seconds 8–41 (sec 18:
98.3% of energy below 250 Hz). The instrumented bus scan measured pre-clamp
peaks to 1.479 (+3.4 dB over full scale, 1,618 native samples over |1.0|).
Loop-point buzz and every other mechanism were ruled out (no periodic
first-difference trains anywhere; loud-but-unclipped seconds are spectrally
clean; the loop-end convention audit found the Cwav→renderer→SF2 chain
self-consistently end-exclusive). The missing stage: **the CSAR INFO sound
entry's per-sound volume byte** (low byte of the retained `Word08`,
`Csar.cpp:401`) is parsed for round-trip but never reaches the player —
`SEQ_TIGER_TOP_EF` ships at 101 (−2.0 dB), `BGM_DEN_EMPTY_LANDSCAPE` at 120
(−0.5 dB), and retail archives almost never use 127 (TigerSound: 1 of 81
sequences; bytes range 0..150 corpus-side, and values >127 exist, so the
engine law is linear vol/127 with boost, not the 128-entry dB table). The
BCSAR PLAYER entries carry no volume field (dumped both archives' player
tables), so no further attenuation is statically recoverable; the runtime
`SoundPlayer::SetVolume` and the DSP master remain unknowable Net-B items.
The pan law was cleared as a headroom suspect: NW4C's own vsqrt pan calc is
the sqrt family with the same ~0.707 center as the render's cos/sin, max
divergence +0.56 dB at intermediate pans. Honesty note: −2.0 dB may not clear
*all* the observed overshoot (worst +3.4 dB measured; flat-top geometry
suggests +2.5..+5 dB) — and the console itself hard-clips with no soft-clip
(established in the Phase II entry), so exact parity is defined by the
console capture, not by zero rails. Calibration gift:
`BGM_MAIN_Mii_Only_One`'s volume byte is 64 (−5.95 dB) and its console
capture already exists, so the tolerance net's level-normalization offset
should move ~6 dB when the stage lands — directly confirming or refuting the
linear law and measuring the residual player/DSP gain in one shot.

Verification hygiene: the diagnostic build changed only stderr/stdout — both
diagnostic WAVs were `fc /b`-identical to the user's renders, and after
`git restore` + rebuild the pristine binary's fresh render was again
byte-identical. Analysis trap recorded for future clip censuses on caesar
output: `finalizeToPcm` maps −1.0 → `lround(−32767)`, so the negative rail is
**−32767, never −32768** — a census keyed to −32768 undercounts ~40%.
Secondary latent finding, filed not fixed: instant (non-`_t`)
volume/expression/pan sets step `mgL`/`mgR` once per 160-sample frame with no
intra-frame ramp — a full-scale step is possible in one sample (644 and 697
instant sets executed in these renders; none audible here, and the 8.06 s
event's sign-flip excludes gain-stepping as its cause).

## Suite stage 2 COMPLETE — the two first-listen fixes, verified end to end (2026-07-14)

Both defects from the first-listen diagnosis (previous entry) are fixed, and
with them stage 2 closes against its design-doc proof criterion ("rendered
sequence matches a console capture within tolerance, except the reverb tail",
SUITE-DESIGN stage table).

**Fix 1 — declick (d9f3703).** Two changes in `Dsp.cpp`, one defect family:
`voiceEndSample` now counts the frame whose `advance()` reaches Done (the frame
that renders the final sustain→0 ramp — excluding it cut every release-127
note-off dead at sustain gain), and `renderVoice` renders ONE extra frame past
a force-stop (`stopAt` steal / mono re-trigger / render cap), linearly faded to
zero — the hardware DSP interpolates per-voice gain across each 160-sample
frame, so a stolen voice fades ~4.9 ms on console. Verified on the diagnosed
events: the EMPTY_LANDSCAPE 8.0616 s step collapsed 1,942 → 368 int16 counts
(ordinary program-material slope) and the 16.3725 s sibling 504 → 313; render
stats identical (62/357 steals, 0 refused — pool behaviour untouched); every
render gains +160 native samples of final-ramp tail. A frame-alignment census
re-check also retired one over-claim from the diagnosis: eShop's 59.198 s step
is steep program material (82 aligned steps among 4,575 large ones ≈ the ~2%
chance rate), not a steal cut.

**Fix 2 — the per-sound volume stage (b124b65).** The CSAR INFO sound entry's
volume byte is now a typed `CsarCseq::Volume` (`Word08 & 0xFF`, still
round-tripped verbatim through the retained word), carried through
`SequenceInfo`/`LoadedArchive` and folded into the static per-voice gain as a
plain linear `vol/127` (bytes >127 exist in retail archives, so the law is
linear-with-boost, not a table lookup). Measured against prediction: eShop
(byte 101) shifted −1.98 dB vs −1.99 predicted and its clipped samples
collapsed 2,367 → 166; EMPTY_LANDSCAPE (byte 120) shifted −0.49 dB vs −0.49.
The 166 residual rail samples are the flagged absolute-level unknown — and the
console itself hard-clips with no soft-clip, so exact parity there is defined
by capture, not by zero rails.

**The verification battery (all exit 0, one run, post-both-fixes):**

- **ab-verify** (baseline 3a94c53): 257,125 files byte-identical corpus-wide,
  console axis identical — the converter never consumed the byte.
- **diag-goldens**: all 18 diagnostic surfaces byte-identical.
- **roundtrip-verify**: 82/82 BCSAR archives, 20,791 BCSEQ + 11,136 BCBNK
  children byte-identical, 0 mismatches — the typed field shadows, never
  replaces, the serialized word.
- **console-tolerance**: BOTH captures PASS all four metrics
  (`BGM_MAIN_Mii_Only_One`: envelope residual 2.65 dB, onset slope 1.0000,
  PSD 0.84/0.84 dB; `EMPTY_LANDSCAPE`: 2.33 dB, 1.0000, 3.34/3.11 dB; reverb
  residuals −35.0 / −5.9 dB re-quantified for stage 3).
- **play-goldens**: re-pinned on the deliberately-changed audio (7 renders,
  determinism enforced), immediate compare clean.

**The volume-law calibration closed.** The Phase IV "absolute output level"
diagnostic moved from −19.8 dB (`Only_One`) / −12.9 dB (`EMPTY_LANDSCAPE`) to
−13.95 / −12.43 — shifts of 5.85 and 0.47 dB against the bytes' predicted 5.95
and 0.49 dB (≤0.1 dB error on both, two independent data points). The
inter-track gap — the only recording-gain-independent quantity — collapsed
6.9 → 1.5 dB, exactly the predicted 5.46 dB relative attenuation. This
confirms the linear `vol/127` law against console captures and shrinks the
absolute-level unknown to a 1.5 dB residual (runtime `SoundPlayer` volume /
DSP master / capture-chain gain). Correction to the Phase IV entry's phrasing:
the diagnostic is the gain to APPLY to the render (negative = render hotter
than the line-in capture), so "render-below-console" there was a sign
mislabel — the render always sat above the capture level.

**Adversarial review + the byte-0 census.** An independent review of both
diffs (overflow/edge cases, fade-frame interactions, pool-accounting
consistency, serializer safety, the unqualified-move trap) confirmed zero
defects. Its one modeling question — does volume byte 0 exist in retail, and
is silence right? — was settled by a corpus census of all 92,135 INFO sound
entries: 1,241 sequences carry byte 0, and their names are self-describing
(`SE_SYS_SILENT`, `SE_SYS_WAITING_SILENT`, control/wait SEs, GardenSound's
game-triggered family, runtime-managed `SEQ_ABM_BGM*`) — deliberately
silent-at-rest sounds whose volume the game raises at runtime, the same
honest-silence stance as the VM's trigger-seed ruling. The census also
reinforced the law: 1,934 entries sit above 127 (max 255 = ×2.008), and the
most common retail bytes are 96 and 90, not 127. Distribution head:
96×19,611, 90×11,438, 127×10,873, 70×7,036, 100×5,743.

Stage 2's box closes on the roadmap; what remains around the player is
constant-refinement fed by the isolated-note captures (CAPTURE-REQUEST.md) and
the stage-3 oracle work (reverb, the interpolation filter, Surround) — polish
and successor-stage items, not stage-2 structure. The roadmap's stage-2 block
is compressed to a completion line in the same commit, per the docs rule.

## The capture cartridge — hands-free console calibration via LayeredFS (2026-07-14)

The stage-2 refinement plan asked for isolated-note console captures
(CAPTURE-REQUEST.md), but the user reported the trigger problem honestly: the
target SEs only fire during plaza music and cannot be requested on demand. No
existing homebrew plays BCSAR sequences (the NW4C runtime lives inside each
game's own code), and writing player homebrew would measure our
reimplementation instead of the console — worthless as ground truth. The
answer was to make the console's own engine play a probe: **patch the archive
it loads**.

`tools/capture-cartridge` (build_cartridge.py + build-cartridge.ps1 +
check_prediction.py) surgically patches a retail `MeetSound.bcsar` in three
same-size, in-place regions — the `BGM_MAIN_Mii_Only_One` INFO entry's volume
byte (64 → 127, a known reference level), its bank index (BANK_BGM →
BANK_MEET_SE), and its embedded `.bcseq` DATA payload (5,900-byte window,
rewritten with a 307-byte battery + phantom-note zero pad) — so the selectable
plaza tune becomes a **43.5 s hands-free calibration battery**, looping
forever via the retail backward-jump convention, played in silence because
the battery replaces the music itself. Delivery is a Luma3DS LayeredFS
whole-file override (`SD:/luma/titles/0004001000021800/romfs/sound/
MeetSound.bcsar`; USA base title CTR-N-HMEE, romfs copy SHA-verified
identical to the dump; "Game Patching" on; caveat: the plaza's v14 update
title needs a current Luma to intercept `ro2:` reads, and if the update ships
a different archive the builder re-targets a GodMode9 dump by name).

Recon was a four-Opus-agent fan-out: (1) the exact INFO/FILE byte-offset
recipes from Csar.cpp (the CbnkOffset-anchored sub-struct: start offset at
R+C+0x10, bank u16 at R+0x1C+C; no checksums anywhere; unchecked
file/bank-index lookups are the patcher's job to bound); (2) the CSEQ
authoring cheat-sheet from Cseq.cpp/SeqRuntime.cpp (varint/prefix/arg-table,
what the player renders vs safe-skips — counted loops D4/FC are skipped, so
the battery loops with 0x89); (3) MeetSound facts (the SEs are one-note
sequences in two shared .bcseq banks; entry volumes 30–75; Only_One = entry 3,
vol 64, standalone 5,920 B file); (4) the Luma path + title ID from the CIA
extract.

Design choices that mattered: the battery **replays the target SEs' own
command bytes**, lifted verbatim from the same archive by a strict
deterministic-replica walker (control flow, Rnd/Var/If, VM ops refused), so
the console executes exactly the retail commands; KEY_FLY's internal
volume-fade-to-0 forced the state-restore-after-ring-out rule (restoring
volume immediately would boost the release tail the capture exists to
witness); replica ADSHR overrides are cleared with 0xFB after each section;
KEY_FLY's notewait-off is restored after its run; the zero pad stays a
multiple of 3 (padded with 0xFB no-ops after the loop jump) so the parser
walks whole phantom notes to the exact DATA boundary and never overshoots
into LABL. Battery: 3× DRUM01 (decay/level, multi-take), KEY_FLY + 12 s ring
(release/reverb/LFO), pan L/R/C drum probes, MENU_CURSOR (attack) +
SLIDE_MAP (LPF), velocity ladder 96/64/32, and a monophonic portamento glide
(50→74, time byte 48) — the one flagged constant no capture has ever touched.

Verification, all green in one driver run: builder self-checks (by-name
re-parse of the patched bytes, whole-payload command-walk with
exact-boundary termination, diff ranges confined to the three intended
regions); converter parses the patched archive exit 0 with no new warnings;
**caesar-roundtrip re-serializes it byte-identically** (59 matched, 0
mismatched); `caesar-play` renders the prediction (14 notes fired, 4,177
ticks = the 43.50 s pass, loop detected); and the schedule check verdicts
the rendered audio numerically — every percussive onset within 10 ms of the
manifest, soft events verified by windowed energy, pan probes ±100.7 dB
hard-split / −0.11 dB centered, and the velocity ladder peaks 0.632/0.358/
0.159/0.040 matching the (vel/127)² law to three decimals. The prediction
WAV doubles as the tolerance-net reference once `BATTERY_console.wav`
exists. No `src/` change anywhere: converter and player outputs are
untouched by construction.

### Addendum, same day — the first deploy failed, and the cause was the update romfs

The cartridge was FTP-deployed to the console (3DS ftpd, SHA-verified
readback) at the base title's path and did nothing: plaza music unchanged,
Game Patching on, current Luma. Root cause, found in the user's
`MiiPlazaUpdate` dump: the v14 plaza update's romfs holds a **different,
larger archive at a different path** — `region_common/frame/sound/
MeetSound.bcsar`, 6,110,784 B vs the base's 3,521,880 — and the update romfs
has no top-level `sound/` at all, so the updated game never requests the
overridden path. Silent no-op, exactly as the recon caveat warned, just via
the path rather than Luma's `ro2:` interception.

The rebuild against the update archive surfaced a real design change: the
update **splits the target SEs across two banks** (`BANK_MEET_SE_MAIN` for
DRUM01/SLIDE_MAP, `BANK_MEET_LEGEND` for KEY_FLY/MENU_CURSOR), so the
single-INFO-bank-repoint design grew a per-section `0xB6` bank switch
(global-CbnkRecords-index form, retail-confirmed) — which makes the capture
double as a hardware test of that command's semantics. The SEs' lifted
command bytes are byte-identical between plaza versions (same tick counts,
identical schedule-check measurements), so the battery content is unchanged.
Builder/driver defaults now point at the update dump + update path
(`--romfs-rel`/`-RomfsRel` for future shifts); full verification chain green
against the update archive; redeployed to the correct path, stale old file
removed from the SD, SHA-verified readback. Awaiting `BATTERY_console.wav`.

### Addendum 2 — the battery went silent on hardware; 0xB6 is the suspect; two-track redesign

With the cartridge at the correct update path, the console DID load it —
and played total silence on "Main Theme 1". The sibling-entry theory died
on enumeration (the update's `BGM_MAIN_Mii_Only_One` owns file 11 alone, no
`_for_Soundlist` twin; the 2026-07-08 captures prove the music player plays
these exact entries), so the battery executed with the pokes in force and
still made no sound. Prime suspect: the very first sound-affecting command
in the stream, `0xB6 04`. caesar's player reads the argument as a GLOBAL
CbnkRecords index — "empirically confirmed" only from retail args that are
equally consistent with the OTHER reading, an index into the sound's
up-to-4 INFO bank SLOTS (caesar parses exactly one slot; the disasm handoff
has nothing on 0xB6). Under slot semantics, `B6 04` selects an invalid
slot before the first note and every subsequent note references nothing —
total silence, exactly as observed. **The two readings are
indistinguishable on all corpus data seen so far; only hardware can split
them** — filed on the roadmap.

The redesign removes 0xB6 entirely: TWO music-player tracks, one per SE
bank, each bank set through the hijacked entry's INFO record (the exact
retail mechanism): track A = `BGM_MAIN_Mii_Only_One` (drums, pans, slide,
ladder; BANK_MEET_SE_MAIN; 23.5 s/pass), track B = `BGM_DEN_EMPTY_LANDSCAPE`
(key-fly, cursor, portamento; BANK_MEET_LEGEND; 23 s/pass) — both hosts
proven playable by the 2026-07-08 captures. The split doubles as a
differential: both tracks silent => the BGM-player path doesn't load
non-BGM banks (the fallback battery would use BANK_BGM's own instruments);
one silent => partial load info; both audible => capture session proceeds.
Full verification chain green on both tracks (round-trip byte-identical;
schedule checks: onsets ≤10 ms, pan ±100.7 dB/−0.11 dB, ladder (vel/127)²
to three decimals); redeployed over ftpd, SHA-verified readback.

## The battery captures — every flagged constant measured (2026-07-14)

The user recorded both battery tracks (192 kHz/24-bit line-in, one full pass
each, noise floor −78 dBFS, zero clipping/hum/stray sounds). An 11-agent
analysis (2 alignment + 9 measurement, each comparing capture vs the dry
prediction so any difference = model error or reverb, never content)
returned. Alignment: every scheduled event found within ~10 ms; clock ratio
1.0000 again; battery t=0 pinned to ±3 ms on both tracks.

**Confirmed on hardware (no change):**
- **Velocity law (vel/127)²** — implied exponent 1.98–2.01 vs model 2.000;
  v64/v32 within 0.15 dB. (Isolated anomaly: the vel-96 hit reads ~1.2 dB
  low in capture vs model — single-point, orthogonal to the law; filed.)
- **Pan** — equal-power confirmed (center −2.86 dB vs extremes ≈ −3 dB,
  ruling out a linear −6 dB law); the byte-64 right-bias (−0.107 dB, byte 64
  ≠ true midpoint 63.5) reproduced in the capture to 0.002 dB; extremes are
  true digital zero in the model and analog-floor-limited in the capture.
  Bytes 0/64/127 still cannot discriminate cos/sin vs sqrt-poly — v2 needs
  probes at 32/96.
- **Absolute level / volume-law endpoint** — byte-127 point lands 0.17 dB
  from the byte-120 BGM point; the 1.5 dB cross-session residual does not
  widen (it is carried by the byte-64 Only_One capture); within-capture
  master gain uniform to 0.077 dB std across six vel-127 sections.
- **Attack + envelope cadence** — cursor rise 3.13 ms (capture) vs 2.32 ms
  (model), smooth at 192 kHz, no stepping: hardware interpolates gain
  within the DSP frame exactly like our per-frame linear ramp; the
  4.889 ms frame period stands.
- **Bonus**: prog-23 sample pitch confirmed to 0.5% via a 6.5 Hz
  partial-beating match (cap 6.53 / pred 6.50 Hz).

**Deviations (recalibrations filed on the roadmap):**
1. **Envelope decay/release dynamics ~2× too shallow** — drum tail:
   console −174 dB/s vs model −94 dB/s, ratio rock-stable 1.85–1.87 across
   fit windows, three takes std 0.2 dB/s; independently corroborated by the
   attack agent (cursor blip tail 38% too long in the model). Of the three
   degenerate candidates (amplitude divisor /400→/200; double calcRelease
   rates; halve kMsPerFrame), the divisor is disfavored (the validated
   volume law rides the same map) and the cadence is now pinned by the
   attack measurement — so the fix points at the **rate constants**
   (calcRelease ×2), with the disasm's time-unit interpretation as the
   suspected original error. Scoped to decay/release; attack-rate scaling
   for non-instant attacks remains unmeasured.
2. **Portamento ~17× too fast — and structurally wrong.** The measured
   glide is **linear in cents at a constant rate**: 2.841 st/s at time
   byte 48 (0.352 s/semitone), R²=0.99998, no pitch step at the note
   boundary, and it NEVER reaches key 74 — the note gate ends mid-glide.
   Our model glides the full distance in a fixed 0.5 s
   (portaTime×samplesPerTick, distance-independent). Fix = distance-
   proportional duration at the calibrated rate; the general
   portaTime→rate law needs one more capture point (different interval or
   time byte). This was the one constant flagged as a pure guess — now
   measured.
3. **LPF byte 48: corner ~4.1 kHz, ~6–7 dB/oct** (two independent methods
   agree) vs the model's 2,890 Hz 2nd-order Q=0.707 (~14 dB/oct) — half an
   octave too dark and twice too steep. Recalibrate corner ×~1.45 and
   soften toward 1-pole. Likely explains most of the linked finding: the
   slide SE renders ~9.5 dB quieter in the model than on console relative
   to the drum baseline.

**Inconclusive — the two constants that still need one more capture:**
- **Release table / reverb residual**: KEY_FLY's own internal volume fade
  hits the noise floor exactly at note-off — the byte-104 release tail and
  any reverb are sub-floor. A real behavioral finding in itself, and:
  **this SE bank is genuinely DRY** (post-event gaps = pre-onset floor to
  0.04 dB) — the DSP reverb send for BANK_MEET_SE_MAIN is ~0.
- **LFO rate 5/64**: KEY_FLY's LFO is a 0.47 Hz volume tremolo — one cycle
  inside the gate, unresolvable. TRAP recorded: the prominent 6.5 Hz
  modulation is instrument partial-beating (identical in the dry render);
  misreading it as the LFO would imply a 13.9× constant error.
  Battery v2 wants a fast pitch-vibrato instrument over many cycles, a
  loud unfaded sustain (release + reverb), pan 32/96, and a second porta
  point.

Also settled operationally: the two-track no-0xB6 cartridge played — the
INFO-bank mechanism carries the whole battery; 0xB6 slot-vs-global stays
open on the roadmap (the silent single-track battery remains weak evidence
for slot semantics).

## The decibel-divisor resolution — /400 was wrong, the pipeline is 10^(value/200) (2026-07-15)

**The report.** The user A/B'd `SEQ_SD_BGM_RESULT` (Mii Plaza DLC `mgExp`)
three ways — foo_midi/BASSMIDI over caesar's own SF2+MIDI, `caesar-play`,
and a New 3DS line-in capture — and heard the kick drum prominent in the
MIDI and on console but "too quiet and maybe even muffled" in the player.
That a track sounded MORE accurate through the converter's GM approximation
than through the accuracy-first player made it a hard discrepancy.

**Objective confirmation.** The kick is program 80, key 36, velocity 99 —
16 hits, and critically its track carries NO volume/expression commands, so
its level is purely velocity × instrument. Band analysis of all three WAVs:
kick low-band (35–130 Hz) prominence over the 300 Hz–5 kHz music bed came
out **BASSMIDI +4.6 dB, console +4.1 dB, caesar-play +0.7 dB** — the player
buried the kick by ~3.5 dB relative to both references. Kick fundamental is
80 Hz in all three (no pitch defect); the "muffled" impression was the
buried body plus masking, not a filter.

**The cause.** The accompaniment tracks ride expression bytes 74/83/59.
`gainFromValue` applied the byte-provenanced `DecibelSquareTable` as
amplitude `10^(value/400)` — under which the table (400·log10(v/127))
degenerates to BYTE-LINEAR amplitude — so expression 74 attenuated only
4.5 dB. The engine's actual law applies `value/10` as plain dB, i.e.
`10^(value/200)`, under which byte v maps to **(v/127)² amplitude** —
expression 74 = 9.4 dB. Every non-127 volume-class byte in the player was
too loud by 20·log10(127/v); the three music tracks each played 3.7–6.7 dB
hot, and the CC-less kick track alone stayed correct.

**Why this resolves three flagged unknowns at once:**
1. The `/40-vs-/20 GetValue split` flag (open since the envelope port) —
   settled: /20 (plain dB), i.e. the /200 divisor in amplitude form.
2. The battery's **"decay dynamics ×2"** finding (console −174 dB/s vs
   model −94, ratio stable 1.85–1.87): the divisor doubles every
   decay/release dB-rate — the model now runs −188 dB/s. The
   disasm-verbatim `calcRelease` rates were NEVER wrong, and the filed plan
   to alter them is cancelled. The remaining ~8% (188 vs 174) is a
   battery-v2 refinement question (measurement chain vs cadence), not a
   divisor candidate — no /200-adjacent value fits 174 exactly and the
   divisor is now pinned independently by the expression evidence.
3. The SF2/player split-brain: Cbnk's `ConvertVolume` has always written
   `(v/127)²` attenuation (why BASSMIDI matched console). Converter and
   player now share one volume law.

**Why the old "verification" didn't pin it.** The /400 reading was
"pinned by the validated volume law" — but the console-confirmed LINEAR
`vol/127` law belongs to the per-sound INFO volume byte, a CPU-side f32
multiply OUTSIDE the decibel pipeline. It never constrained this divisor.
The console-confirmed vel² law in fact SUPPORTS /200: velocity through
the same table under /200 gives exactly (v/127)².

**The fix.** One constant: `gainFromValue` 400 → 200 (`src/play/Dsp.cpp`),
comments corrected at both call-surface docs. Velocity keeps its explicit
`(v/127)²` (identical to table-under-/200 within table-rounding, <0.03 dB).

**Verification.**
- Patched render of `SEQ_SD_BGM_RESULT`: kick prominence **+5.07 dB** vs
  console +4.09 / BASSMIDI +4.61 (was +0.66). Within ~1 dB of console.
- `tools/console-tolerance`: **both** New 3DS captures still PASS
  (envelope-fit residuals 2.15 / 1.90 dB, onset slope 1.0000).
- `tools/play-goldens`: re-pinned (deliberate audio change), twice-run
  deterministic, 7/7.
- Converter untouched by construction — the full rebuild relinked only
  `caesar_play`/`caesar-play`; `caesar.exe` did not change.

### Addendum — the RESULT "kick punchiness" follow-up: the player is voice-exact; the gap is reverb (2026-07-15)

After the divisor fix, the user (listening on loudness-normalized copies)
still heard the kick's attack as "off" in the player, with BASSMIDI closest
to console. A long forensic chase produced two false leads before the truth:

**False leads (recorded as measurement traps):**
1. Threshold-based onset detection fires up to ~30 ms LATE on a quiet,
   slow-rising 80 Hz kick — and the bias differs per file with the kick's
   relative level. Every metric that inherited those onsets (band envelopes,
   matched-filter windows, spectrogram panels) showed a phantom "body cut at
   55 ms / 13 dB missing" in the renders. Template cross-correlation against
   the actual kick sample (WARC_48 wav 88) is the only trustworthy alignment.
2. Anchoring a hit grid to the STRONGEST template match picks a mid-song
   kick, and the sequence's groove rests (93/96/97 ticks) then produce
   phantom "+21 ms timing drift" against a grid extended from the wrong
   anchor. Anchor to the EARLIEST >=60%-of-max match.

**Verified findings (template-aligned, per-hit xcorr):**
- A solo-kick render (three OpenTrack offsets in a scratch copy of
  mgExp.bcsar retargeted to an immediate Fin — the player's first
  isolation-render trick) is IDENTICAL to the kick inside the full mix:
  no steal, no duck, level exact.
- The kick's 60–120 Hz envelope has the SAME shape in console, BASSMIDI and
  caesar-play: rise to ~40 ms, body plateau to the 130 ms gate, release
  cliff by ~150 ms. Kick-vs-accompaniment prominence: console 11.2 dB,
  BASSMIDI 12.0, caesar-play 13.9 — the player is CLOSE and slightly hot,
  though console's accompaniment reading is reverb-inflated, so the true
  overshoot is < 2.6 dB.
- The audible difference is AFTER the notes: console keeps ~10 dB of
  post-release low-band energy (the DSP reverb tail) that the dry player
  lacks; foo_midi/BASSMIDI applies its default GM reverb (CC91=40 on GM
  reset), which imitates console's glue — exactly why the user ranked
  midi closest. A dry kick against a wet mix reads as "less punchy" even
  at equal level.

**Outcome:** no player change. SEQ_SD_BGM_RESULT (MiiPlazaDLC mgExp) is
filed as a stage-3 validation track — its exposed, periodic kick + reverb
hump is a clean reverb-fit target.

## Suite stage 3 — commit 1: the DSP oracle boots the real firmware to the audio callback (2026-07-16)

The first stage-3 code commit, exactly as scoped by the 2026-07-14 recon: vendor
teakra, port the LLE boot/pipe/audio harness into a standalone `dsp_oracle`, boot
the MiiPlaza firmware to where the audio callback fires — paired with the de-risk
spike (statically recover the ARM11 driver's "reverb engaged" configuration).
Run as a three-lane fan-out; every lane landed.

**Lane 1 — vendoring + skeleton (`tools/dsp-oracle/`).** Teakra vendored at
upstream `3d697a18` (MIT — license-clean under caesar's GPLv3), 80 files, with
`.git`/CI/tests/hwtest/the 675 KB catch header excluded and **zero patches**:
as a subproject, teakra's own `MASTER_PROJECT` logic turns off tools, tests and
warnings-as-errors, and its `externals/CMakeLists.txt` only references the
removed catch directory inside the tests branch. `external/VENDOR.md` records
the commit, exclusions and re-vendor procedure. The build tree is **standalone
by design** (`cmake -S tools/dsp-oracle -B tools/dsp-oracle/build`) — the oracle
never enters the repo-root build or CI; only fitted coefficients + a golden IR
will ever ship into the player. `extract_dspfirm.py` regenerates the firmware
from local dumps with every DSP1 segment's embedded SHA-256 verified
(self-certifying); all five titles verified, MiiPlaza `944b40b5…` byte-identical
to the recon reference. `firmware/` is gitignored — the firmware is Nintendo
copyright and never committed.

**Lane 2 — the port (`src/`, 8 original C++ files + PORT-NOTES.md).** Original
code informed by (not copied from) Citra/Azahar `lle.cpp`/`dsp_dsp.cpp`, libctru
`ndsp.c`, and teakra's internals, with line-level citations recorded in
PORT-NOTES.md. The boot proof, reproduced independently after integration on the
repo tree's own build and firmware: DSP1 parse (5 segments, layout `0xDF03`,
`recv_data_on_start=1`) → handshake replies `1` on channels 0/1/2 →
`pipe_base_waddr = 0x0C9E` → Audio-pipe Initialize (`WritePipe(2, {0,0,0,0})` +
`SetSemaphore(0x4000)`, per ndsp.c) → the firmware replies with the 15-word
shared-region address table, **byte-for-byte equal to Citra's HLE exemplar**
(`frame_counter 0xBFFF, source_config 0x9E92, …, dsp_configuration 0x9430,
final_mix 0x8540`) — the real firmware and Citra agree on order AND concrete
addresses. A 5.00 s idle capture: 163,520 sample-pairs at **exactly 4096.00
cycles/sample = 32,728.3 Hz** (the native rate; DSP clock = half the 268 MHz
ARM11), zero nonzero samples, zero underruns, no stall, exit 0.

**The headline protocol fact** (the single answer the rest of stage 3 leans
on), settled by a 300-frame three-mode experiment counting BTDMP underruns:
teakra's I2S transmit **free-runs** — the audio callback fires regardless — but
with no per-frame ARM11 service the firmware stalls on its full DSP→ARM
channel-2 mailbox and the DAC emits underrun-silence (95,976 of 96,000
channel-samples underran). **Draining `RecvData(2)` each frame drops underruns
to zero**; the faithful full duty additionally bumps the write-bank frame
counter and calls `SetSemaphore(0x2000)` (ndsp.c's per-frame order), which is
what advances the double-buffer so the DSP re-reads config — not needed at
idle, mandatory for commits 2/3. `--service none|drain|full` keeps the
experiment reproducible.

**Lane 3 — the de-risk spike landed more than asked.** Verdict:
site-found-values-computed. The ARM11 write site (`FlushReverbEffect` at VA
`0x1368B4`) and the **full 26-word internal layout of the 52-byte
`ReverbEffect` block** — an ~8-year Citra/Azahar `INSERT_PADDING_DSPWORDS(26)`
TODO — are recovered, three-way-evidenced off the DspConfiguration setter
family (every scalar setter matches Azahar's field offset AND dirty bit), the
byte-for-byte delay-flush sibling, and a whole-.text only-writer scan. The
coefficient values themselves are runtime-computed from a heap reverb object's
floats (×255 gain packing, ×256 clamped delay-line length) — no static template
exists in `code.bin` — so the exact engaged bytes come later from a Luma 3GX
live read while `BGM_DEN_EMPTY_LANDSCAPE` plays (the analog-free capture
program's plugin) or a structured sweep with the recovered layout holding words
0–5 valid. Full addresses, evidence chain, layout table and the oracle replay
recipe: NW4C-disasm-handoff.md Session 5. Method trap for future capstone work:
`disasm()` over full `.text` halts silently at the first literal pool — build a
gap-tolerant disassembly (539,101 instructions) before trusting any scan.

**Verification.** Nothing outside `tools/dsp-oracle/` was touched (converter
untouched by construction; `git status` clean otherwise). The oracle was
rebuilt from the repo tree and re-run end-to-end by the orchestrator after
integration: same handshake, same table, same 4096.00/32728.3/0-nonzero stats,
exit 0. Roadmap effect: stage-3's "first code commit" milestone is done, and
the analog-free capture program's step 1 (`dsp_oracle` first — one artifact,
three consumers) is delivered. Next: commit 2 — one synthetic source playing a
click through AHBM-backed FCRAM, captured dry at the final mix (which also
answers route-a's `final_samples` readback question), then commit 3 — engage
reverb via the recovered layout.
