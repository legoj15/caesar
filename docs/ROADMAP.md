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

====OVERARCHING GOALS====
The plan is big, and the roadmap shows a slice. Here's what's represented now, each being their own large step:
- Get the caesar midi/SF2 debugged to the best effort (AKA fix the known bugs and apply the obvious UX fixes, ship it as version 0.5) <-- We are here, at the beginning
- Rebrand the repo as "caesar salad", a play on the mix of things this repo strives to bring
- Make an honest, accurate to console player
- Make said player play at a higher fidelity than console [Ultimate Goal]
- Add PC compatible tracker format exporting (choose the best format or have multiple available)
- Editor [Low priority]
========

## Road to first release (v0.5.0)

### 1. Modern build system — ✅ done
A single CMake build that works on current (2026) toolchains.

- [x] Consolidate on CMake (CMake 3.21+, C++17); drop the hand-written Visual
      Studio solution/project and the dead AppVeyor config.
- [x] Build the two vendored libraries (`libsmfc`, `sf2cute`) as static libs.
- [x] Add `CMakePresets.json` for one-click Windows/MSVC builds.
- [x] Repo hygiene: slim `.gitignore`, fix `.gitattributes`, document building.
- [ ] Verify the build on Linux and macOS (the CMake is written to be portable;
      only Windows/MSVC is tested so far). **Linux is now fully verified
      end-to-end (2026-07-10); macOS/Clang is the only remainder.**

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

### 2. Robustness — ✅ done
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

### 3. Surface what's being dropped — ✅ done
By default (without `-w`), the tool prints no warnings at all — so a normal run
reports success while silently omitting sound effects, whole-song loops, an
entire audio codec, and more. Users can't tell what they didn't get.

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
      7-bit scaling instead of dropping, tracked as a follow-up fidelity item below.

### 4. High-value fidelity & UX wins — ✅ done
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
      group work (still open, under "After the first release").

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
      EMPTY_LANDSCAPE confound reverb+release) but wrong for busy tracks. **Likely
      correction (NOT yet applied, needs user sign-off — reverses a committed
      change):** shorten release-127 toward short/instant, rely on the emitted
      reverb; re-check EMPTY_LANDSCAPE with short-release+reverb; exact value wants
      the user's ear or `code.bin` RE.

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

      **`decay == 127` is the same branch and
      also instant → caesar's decay-127 handling is already correct.** **Recommended
      (needs user sign-off — reverses the committed `b078932`):** change
      `ConvertRelease`'s `release == 127` from `ConvertTime(3.5)` to instant/fastest
      (mirror `ConvertDecay`), rely on the emitted CC91 reverb for the tail, and re-check
      `EMPTY_LANDSCAPE`/`BGM_MAIN_Mii_Only_One` with short-release + reverb. Not yet
      applied (changes `.mid`/SF2 output; gate on the user's ear).

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
          reverb is required and is on the critical path. See "Beyond the converter" below.
          Teak *disassembly* still isn't needed: the reverb is recoverable behaviourally by
          running the firmware in `teakra` offline and capturing its impulse response.

      **ROOT CAUSE (user's ear, 2026-07-08): no single release can ever be right.**
      A per-instrument A/B ("0.3 s perfect for some notes, too short for others;
      1.0 fits nothing; 3.5 fits a couple but muds the rest") plus `variance.py`
      shows all 34 sentinel programs are ENVELOPE-IDENTICAL (attack/hold instant,
      decay 127, full sustain, release 127). The tail variation the user hears is
      NOT in the envelope — it lives in the **sample** (6 one-shot vs 28 looped;
      loop lengths 225…28k samples) and the **per-note reverb sends**. So byte 127
      is one global behaviour; caesar is being asked to fake, with one release knob,
      a variation that physically lives in the samples + reverb. Accurate-fix
      options, best→cheapest (option 1 is now done and option 4 is now the shipped
      default; the rest stand):
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

### 5. Licensing — ✅ resolved
The vendored `libsmfc` shipped without a license notice, but it is loveemu's
MIT-licensed code (identical to the copy in
[loveemu-lab](https://github.com/loveemu/loveemu-lab)); the copy had simply
dropped the repo-level license. MIT is GPL-3.0-compatible, so there is no
conflict with caesar's GPL-3.0.

- [x] Add the MIT notice for `libsmfc` (`src/libsmfc/LICENSE`) and document the
      third-party licenses in the README. Binaries are now distributable.

### 6. Continuous integration & first release — final step
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
- [ ] Cut **v0.5.0** — the first maintained release. (Everything above is in
      place; cutting the release is pushing a `v0.5.0` tag, which fires
      `release.yml`. Recommend doing it once the tune fix + this CI are on
      `master` and the first `build.yml` run is green on all three OSes.)

      Validation before first push: both workflows pass `actionlint` 1.7.12 clean
      and parse as valid YAML; the build commands are the same ones verified
      end-to-end on Debian today. The live green run across all three hosted OSes
      is the final confirmation.

---

## After the first release (not blocking v0.5.0)

Larger efforts that expand what caesar can do. Rough priority order:

- **Sequence (CSEQ → MIDI) fidelity.** Implement the full variable/conditional/
  random command machinery. Note-less `[If]` dispatchers are now resolved
  heuristically (a fully-silent track follows the default branch), but the
  underlying variables are still not evaluated, so: `[If]`-prefixed *non-jump*
  commands (e.g. `[If] Program`, `[If] Return`) currently execute
  unconditionally; the extended `setvar`/`cmp`/mod commands are parsed but
  dropped (their walker branch is dead code — `cmd.Cmd` is never set to the
  extended opcode); and random/variable values convert wrong. Also tie mode and
  finite loop repeat counts. **Controller range mapping:** surfacing the MIDI
  writer's rejections (section 3) exposed ~1,020 control/parameter events
  corpus-wide dropped for exceeding MIDI's 0-127 range — some are 8-bit source
  values (volume/pan/expression parse as `Uint8` 0-255) that should be *scaled*
  to 7 bits rather than dropped, and the `Int16` vibrato-delay (`0xE0`/`0xE3`)
  and tempo (`0xE1`) sends want their true encodings worked out. Each is now
  visible in the default run, so they can be triaged one controller at a time.
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

---

## Beyond the converter: the BCSEQ tool suite

The long-term goal (stated 2026-07-09) is that caesar is the **foundation** of a suite of
BCSEQ tools, not an end in itself: a sequence **editor**, a **player that behaves like
console** while rendering at any PC sample rate, **tracker export**, and the existing
best-effort SF2/MIDI export. Everything below was settled by a 20-agent design workflow
(7 recon + 12 adversarial verifiers + synthesis). Three of the four load-bearing claims
were **refuted on their headlines** and are recorded here in their corrected form.

### The decision that determines everything else: internal pipeline rate

**Mix internally in float at the DSP's native rate and resample exactly once, at the very
end, to whatever the user asked for.** Verified in Azahar/Citra `src/audio_core/audio_types.h`:
`native_sample_rate = 32728` Hz, `samples_per_frame = 160` (so 204.55 frames/s, and a
**Nyquist of 16,364 Hz**). Every voice is resampled to that bus before anything else
happens; the reverb's delay lines are whole-sample counts on it. A 32,728 Hz mix is
band-limited to 16.364 kHz *by construction*, so a single final sinc upsample to 48/96/192 kHz
is provably transparent. **"Behaves like console" and "48 kHz output" have zero tension** —
they only conflict if you clock the *mix* at 48 kHz, which would force every rate-dependent
constant (above all the reverb) to be re-derived, and would swap the console's own
interpolator for a cleaner one. That path yields neither goal.

**Correction (the verifiers refuted the first draft here).** A "remaster" mode that preserves
source bandwidth above 16.364 kHz is **impossible on a native-rate bus** — Nyquist forbids it,
no per-voice interpolator can rescue it. Remaster is therefore a genuinely *second pipeline*
clocked at the output rate, not a flag on the accurate one. It is optional and buys little:
a survey of the corpus (`scratchpad\cwav_rates.py`, 2,495 `.bcwav`, 0 parse failures) finds
**450/2,495 = 18.0 % of samples stored above 32,728 Hz** — 409 at 44,100, 17 at 96,000, 11 at
65,456 (exactly 2× native), 5 each at 48,000 and 44,000 — and the console throws all of that
away. Codecs: DSP-ADPCM 2,399, IMA-ADPCM 53, PCM16 39, PCM8 4.

### LLE vs HLE: build the engine, keep `teakra` as an oracle

Build the player as our own engine (HLE) in C++. Use **`teakra` + the user's own extracted
firmware strictly offline**, as a measuring instrument and correctness reference — never as
the thing that plays audio. LLE is rate-locked to 32,728 by construction and is a black box
you cannot step into when a note sounds wrong. (This was the one claim that survived
verification unrefuted, 0/3.)

**Do not believe "port Citra's HLE and you're 85 % done."** Two independent traps, both verified:
1. Citra/Azahar's HLE is the *known-inaccurate* path. `wwylele` wrote `teakra`/LLE precisely
   because HLE was wrong, then used LLE to debug it. Azahar issue #1070 (KORG DSN-12) has HLE
   audio "very wrong compared to real hardware" in the **core source/filter/mixer path**, not
   the reverb. Concretely: `source.cpp` routes `InterpolationMode::Polyphase` to
   `AudioInterp::Linear` behind `// TODO(merry): Implement polyphase interpolation`, and
   `interpolate.cpp` contains no polyphase table at all — while 3dbrew documents a separate
   per-source byte at offset 57, "Polyphase filter select", implying several undocumented
   filters. `DelayEffect` is fully mapped (`ASSERT_DSP_STRUCT(..., 20)`) but never invoked;
   limiter and compressor are stubbed "assume disabled".
2. Citra's HLE is the DSP **sink**. It *reads* `SourceConfiguration` (gains, ADPCM coefficients,
   rate multiplier, filter coefficients) out of shared memory. It never *produces* them —
   on console that is the game's own ARM11 sound driver. Nothing public implements that, our
   files don't contain it, and caesar doesn't produce it. **That driver is the real project.**

### RE priorities, inverted

With only SF2/MIDI in scope, RE was correctly ranked last. With a player in scope that verdict
is void. The critical path is the **ARM11 `nw::snd` sequence runtime**, and the Wii `ogws`
decompilation is a strong crib for it. Verified verbatim in `doldecomp/ogws`:

- **Voice stealing — CONFIRMED IN THE 3DS BINARY (2026-07-09, session 3; 0/6 refutations).**
  The NW4C voice manager is a behavioural 1:1 port of NW4R's. Verified by re-disassembling
  Mii Plaza `code.bin` from raw bytes: upper `VoiceManager::AllocVoice` @ vaddr `0x14D7B8`
  reuses a free voice, else reads the **front of a priority-sorted active list** (`ldr [mgr+8]`,
  not a min-scan), and **refuses to steal if the front outranks the requester** — the exact
  NW4R guard `cmp front.prio, req ; bgt → return NULL` at `0x14D7FC`–`800`. Priority lives at
  Voice `+0x40`, masked `& 0xFF` (full **0..255**, *not* coarsened). The pool is **24 voices**:
  the upper layer computes `count = poolSize / 0x60` and its config word at `.data 0x354DD4`
  reads `0x18` = 24; a lower DSP-source pool (`0x14F3F4`) hardcodes 24 to match the DSP's 24
  hardware sources, gated by `popcount(bitmask) == 0x18`. A note draws **1–3 DSP sources**
  (mono/stereo/3ch). caesar discards note priority entirely today, so any dense passage plays
  the wrong notes. Highest-payoff runtime feature — and now a *solved, bounded* port, which is
  the evidence that the rest of the runtime is a port too. Full addresses in
  `NW4C-disasm-handoff.md` §"Session 3".
- **Variables / conditionals** — `snd_MmlParser.cpp`: `MML_EXECIF` sets `doExecCommand = cmpFlag`;
  `MML_EX_EQ … MML_EX_GE` set `cmpFlag = *pVar op arg2`. `VARIABLE_NUM` = 16 local + 16 global
  + 16 track. caesar's `[If]` handling is a note-reachability *heuristic* and its extended
  opcodes are dead code.
- **Randomness** — `Util::CalcRandom` is an LCG seeded `0x12345678`,
  `u = u * 0x19660D + 0x3C6EF35F`; `Rnd` scales `rand *= (max-min)+1; rand >>= 16; rand += min`.
  Bit-exact reproduction is therefore possible.
- **Release semantics** — `snd_Channel.cpp`: `Release()` only sets `STATUS_RELEASE`; `Stop()`
  fires when `volume * veInitVolume == 0.0f`. This independently confirms release-127: rate
  65535 drives the envelope to zero in one step, so the voice stops immediately.
- **LFO** — `snd_Lfo.cpp` is a single four-quadrant **sine, with no curve select**. NW4C's
  `mod2`/`mod3`/`mod4` selectable curves are therefore a genuine NW4C *addition* and a real
  unknown. Least audible of the unknowns; defer, don't gate on it.
- **Clock** — `snd_SeqPlayer.cpp` `UpdateTick(3)` = a 3 ms Wii frame with a fractional-tick
  accumulator. The 3DS frame period must be confirmed (the DSP frame is 160/32728 ≈ 4.889 ms).

Honest effort for the sequence runtime alone: **~7–10 focused sessions** for full accuracy
(~3–4 for "right in most passages"). **Reverb is the long pole, not the LFO curves** — it
cannot be cribbed from the Wii (opposite architecture) and the emulators have left it
unimplemented for ~8 years. It gets its own milestone.

### The next milestone: byte-identical round-trip

Before any editor, player, or tracker work: parse an archive into a model, **drop the source
buffer**, re-serialize purely from the model, and compare `sha256` against the original across
the whole corpus. This is the cheapest complete proof that the format is understood — it
validates every offset/size/label computation, proves no byte was unaccounted for, and pins
the padding/alignment rules, with no console, no ear, and no golden audio. The serializer it
forces you to write is the exact one the editor, the player's sample path, and the tracker
exporter all sit on.

Two honesty guards. **(a)** If the serializer may copy through bytes it "didn't change", an
unmodified file round-trips trivially and the green check is a lie — drop the buffer.
**(b)** Round-trip proves you can *read* the format, not *edit* it: an unknown field parked as
an opaque span survives untouched right up until you resize its neighbour.

A prerequisite the design workflow flagged — "fix the `Cgrp` file-table desync first, or archives
holding an external/absent file fail round-trip for a *reading* reason and muddy the signal" —
**is now satisfied**: that bug and the `Csar` 8-byte-width UB were both fixed on 2026-07-09
(`8c811e7`, `8869f89`). Group-bearing archives (MK7 `ctr_dash`) can therefore be in the round-trip
corpus from day one rather than quarantined. The remaining reader-side hazard is the hardcoded
bank-note offsets (below), which only bites on a bank whose flags word differs from `0x21F` —
none observed.

### Library-core refactor (a strangler, not a rewrite)

**Stay in C++; do not port to Rust.** caesar is only ~3,744 LOC, and its one irreplaceable
asset is a parser proven byte-identical across 40,000+ files. A language port discards that
proof and reopens every endianness and bounds bug the project has already closed, and both
vendored writers (`libsmfc`, `sf2cute`) are C/C++. Steps, each keeping the existing A/B green:

1. Fold `Common`'s six process-globals (`FileNames`/`Offsets`/`Buffers`/`Log`/`Notices`/…)
   into a `ParseContext` passed by reference. Mechanical, output-preserving; unlocks
   reentrancy and parallelism.
2. Kill the disk round-trip: `Cwav` retains decoded PCM + loop points; `Cbnk` reads them from
   the live object instead of re-opening the `.wav` it just wrote (`Cbnk.cpp:213-293`). Keep
   *writing* the `.wav` as user output. Verify sample-for-sample — the `smpl`-chunk loop
   recovery at `Cbnk.cpp:257-293` has quirks the direct path must reproduce exactly.
3. Promote the half-existing structs (`CbnkCwav`, `CbnkNote`, `CseqCmd`) into a lossless model;
   split parsers (bytes → model, no I/O, no globals) from exporters (model → SF2/MIDI).
4. Split a `caesar_core` static library from the CLI.

**Model shape:** one owned copy of the file bytes plus a tree of records. Each record knows its
offset and length, holds typed fields for what is understood, and an **opaque byte-span for
everything that isn't** (unimplemented opcodes, CWSD, INFX, the `0x6001` mystery words,
IMA-ADPCM payloads). Serialize by walking the tree and **recomputing every offset and size table
from scratch**, never copying them.

**The safety net does not cover audio.** The byte-identical A/B guards only the current export
path; the player's output is invisible to it. A second net is required — deterministic golden-hash
renders (fixed rate, seeded randomness, pinned reverb) plus tolerance-band comparison against the
existing New 3DS line-in captures. Over-trusting the familiar green check is the most likely way
a broken player ships unnoticed.

### Staged plan

Ordered so each stage is worth having even if work stops there.

| # | Stage | Effort | Proof it works |
|---|---|---|---|
| 0 | Library-core refactor (context object, kill disk round-trip, split parser/exporter, `caesar_core`) | several sessions | existing A/B stays byte-identical; parser runs reentrant |
| 1 | Raw-backed model + **byte-identical round-trip** of BCSEQ/BCBNK/BCSAR | several sessions | parse → drop buffer → re-serialize → `sha256` matches, corpus-wide |
| 2 | **Dry player**: in-memory voices at native rate, console interpolation, solved envelope, priority voice stealing, tie/portamento, gain + aux mix, tempo clock, single final upsample | ~7–10 sessions (~3–4 for "mostly right") | rendered sequence matches a console capture within tolerance, *except* the reverb tail |
| 3 | **Reverb + delay** (delay is codeable now from its known transfer function; reverb via offline `teakra` impulse capture → comb/allpass fit → hardware validation) | its own milestone | `EMPTY_LANDSCAPE` sounds right; tail matches console |
| 4 | Exact variables/conditionals/random + the `mod2/3/4` LFO curves | moderate + one capstone dig | `[If]`/random sequences take the same branch as console |
| 5 | **Tracker export (`.it`)** | moderate | opens in OpenMPT with correct instruments, envelopes, flattened structure |
| 6 | **Editor (write-back)** | largest, open-ended | an edited BCSEQ plays correctly on the New 3DS via LayeredFS |

Stage 5 shares its hard part — flattening loops/conditionals/randomness into one linear
playthrough — with Stage 2's front end; build that flattening once and let both consume it.

### Tracker export: `.it`, framed as a lossy authoring bridge

Target **Impulse Tracker `.it`** with a hand-rolled writer, and say plainly in the docs that it
is a *preview/authoring bridge*, not a fidelity path — the fidelity path is our own player. It
wins because it is the free lingua franca (OpenMPT, Schism) and its instrument model — sample
keymap + volume/pan envelopes + NNA virtual voices — is the closest classic analogue to a BCBNK
velocity-region keymap with ADSR and a release tail; 64 channels with NNA covers 16 polyphonic
BCSEQ tracks. Reject `.xm` (no NNA, 32 channels, 12-point envelopes) and Furnace (register-level
chiptune; its sample support is chip-PCM, not a general sampler). Offer `.mptm` as a one-flag
upgrade sharing ~95 % of the code path. Watch the tempo ceiling: fast pieces at fine rows/beat
can exceed IT's tempo 255, so a per-sequence rows/beat solver is needed, not a constant.

### Risk register

1. **Under-scoping the sound runtime** because "the mixer port is 85 %". The mixer is the easy
   half; the voice/sequence engine that feeds it exists nowhere to copy. *De-risk:* one capstone
   session on the Mii Plaza voice allocator to confirm the pool is 24 and the policy is
   priority-only. That single dig sizes the whole engine.
2. **Reverb doesn't yield to impulse-response capture** — breaks if a game changes reverb settings
   mid-song, or computes them at runtime rather than storing presets. *De-risk:* two spikes —
   statically find where a reverb-using title writes the 52-byte `ReverbEffect` block, and do one
   offline `teakra` impulse capture to prove the harness works. Both before committing.
3. **The player has no safety net and the green A/B lulls you.** *De-risk:* stand up golden-hash +
   console-tolerance comparison the moment the dry player emits its first `.wav`.
4. **The oracle may be lying** — if `teakra` has bugs in the ops the reverb firmware uses, the
   captured IR is a faithful model of a bug. *De-risk:* capture the same reverb once on the real
   New 3DS via the 192 kHz line-in and compare. The console is the authority; `teakra` is a
   convenience.
5. **Write-back stalls** — the hard, unproven part is the shared multi-entry `.bcseq` banks: resize
   one entry and every later offset and every pointer into the blob must move in lockstep.
   *De-risk:* after round-trip proves the offset math, make exactly one *size-preserving* edit
   (a note's velocity), re-serialize, and play it on the New 3DS via LayeredFS (the archive carries
   no hash or signature, and file redirection imposes no size limit). Prove the smallest edit
   end-to-end before touching anything that resizes a block.

### Where the 3.5 s release hack lands

The model stores the **truth** (release-127 = instant). Each exporter renders that truth for its
own consumer. The **player** renders it as instant note-off plus a real reverb tail (stage 3) —
and must *not* inherit the 3.5 s fake, which would double the tail. The **SF2/MIDI exporter**
keeps the 3.5 s value as a *labelled compensation*, because its consumer is a reverb-less synth
and telling it the truth produces the original dry-and-chopped bug. This is not a wart: it is the
parser/exporter split doing its job — one truthful model, several honest renderings for consumers
of differing capability.

---

## Known bugs

Concrete defects found while surveying and evolving the code. None crash the tool
on the archives tested so far, but each is a real correctness or portability
hazard. Not release-blocking on their own.

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
  under GCC 13 on Linux, but Linux functionality remains untested against real
  archives (see the open Linux/macOS build-verification item under section 1).
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
- **Tempo `bpm == 0` is undefined behavior** (vendored `libsmfcx.c`,
  `smfInsertTempoBPM`). A `0xE1` tempo command decodes bpm as a signed 16-bit
  value, so `bpm == 0` makes `60000000 / bpm` evaluate to `+inf` and the
  `(int) microSeconds` cast is UB. Pre-existing in loveemu's library, and benign
  in practice: on x86 the cast yields `INT_MIN`, which fails the microseconds
  range check, so the garbage tempo is dropped (and, since section 3, surfaced as
  a control/parameter drop). A defensive `bpm > 0` guard in the caller or the
  library would remove the UB; left as-is for now to keep the vendored copy
  pristine. Found by the section-3 MIDI-return-value audit.
- **Bank note fields are read at hardcoded offsets** (`Cbnk.cpp`, the note-parse loop).
  The format actually locates the ADSHR envelope through a `DataRef` chain
  (`note+0x10 + *(note+0x2C)`, then `+8`), and which optional parameters are present is
  gated by the flags word at `note+0x14`. Every one of 1,628 notes sampled across 57 banks
  and three engines carries flags `0x21F`, so the fixed layout is empirically safe for the
  games tested — but a bank that omits or adds a parameter would shift every field and
  caesar would silently misparse it into plausible garbage rather than fail. Following the
  reference instead of hardcoding `+0x38` would remove the hazard. Latent, not observed.
- **Non-ASCII file names.** Input paths and archive-internal names pass through
  narrow `char*` / `std::string` into `std::filesystem`, so non-ASCII names
  (common for Japanese titles) can be mangled or throw. Illegal-character
  sanitizing is done; character encoding is not.
- **Compiler narrowing warnings** (`C4267` / `C4244`, `size_t`/`int` to smaller
  types). Harmless in practice but they flag real implicit truncations; worth a
  code-quality pass.
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
  allocations perturbed the heap enough to expose the latent read. (A wider
  value-initialisation audit of the parser structs may be worthwhile.)

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
  dispatchers that select a section via conditional (`[If]`) jumps; the latter are
  now resolved (see "Resolve note-less conditional `[If]` jump dispatchers" under
  the first-release fidelity wins).
