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
