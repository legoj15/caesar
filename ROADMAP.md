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

## Road to first release (v0.5.0)

### 1. Modern build system — ✅ done
A single CMake build that works on current (2026) toolchains.

- [x] Consolidate on CMake (CMake 3.21+, C++17); drop the hand-written Visual
      Studio solution/project and the dead AppVeyor config.
- [x] Build the two vendored libraries (`libsmfc`, `sf2cute`) as static libs.
- [x] Add `CMakePresets.json` for one-click Windows/MSVC builds.
- [x] Repo hygiene: slim `.gitignore`, fix `.gitattributes`, document building.
- [ ] Verify the build on Linux and macOS (the CMake is written to be portable;
      only Windows/MSVC is tested so far).

### 2. Robustness — never crash on real-world input
A tool whose job is parsing files ripped from game images should treat malformed
or unusual input as normal, not exceptional. Today, bad input crashes with no
explanation, and some failures corrupt output silently.

- [x] Check that every input file actually opened before using its size — a
      missing, empty, or unreadable path previously triggered a giant bogus
      allocation and crashed. (`Common::RequireOpen`, all load sites.)
- [x] Wrap each input in top-level error handling so one bad file reports
      cleanly and the rest still process (previously the first failure aborted
      the whole run), and restore the working directory between inputs so
      multi-archive runs (`caesar a.bcsar b.bcsar`) work.
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
- [ ] Replace the change-working-directory extraction model with composed output
      paths (enables an `--output-dir` option and removes remaining failure-path
      and parallelism limits).
- [x] Sanitize archive-supplied names before using them as file/dir names —
      illegal characters (including path separators, which could otherwise
      escape the output folder) are replaced. Done in the same `TypedName`
      choke-point; a no-op on normal names, so valid output is unchanged.

### 3. Surface what's being dropped
By default (without `-w`), the tool prints no warnings at all — so a normal run
reports success while silently omitting sound effects, whole-song loops, an
entire audio codec, and more. Users can't tell what they didn't get.

- [ ] Promote "content was skipped / approximated" notices so they show by
      default; keep `-w` for verbose per-command detail.
- [ ] Check the MIDI-writer's return values (it silently rejects out-of-range
      events today, so some notes/program-changes vanish with no warning).

### 4. High-value fidelity & UX wins
The cheapest changes with the most audible or visible payoff.

- [x] Give numeric output names a type prefix so numbered items are
      identifiable and can't collide. Symbol-table names (e.g. `BANK_CTR_COMMON`)
      were already used where present; now items the archive leaves unnamed —
      including everything inside groups, which had no naming at all — become
      `BANK_206`, `WARC_390`, `SEQ_…` instead of bare numbers (issue #17's
      `206.sf2` → `BANK_206.sf2`). Verified content-preserving over real
      archives (only the SF2 embedded name and the `.log` change, as intended).
- [ ] Deeper naming: groups keep symbol names too. Group-resident banks are
      extracted under a number while the matching symbol-named directory from the
      CSAR level sits empty (28 of 85 named dirs in MK7's `ctr_dash`). Mapping
      the CSAR symbol names across the group boundary would fill those and drop
      the numeric duplicates — belongs with the external-`.bcgrp` group work.
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
      The exact release *time*, however, is **not measurable from this track** —
      it is such a dense overlapping-pad texture that a voice is essentially
      always sounding, so the aggregate envelope is near-identical for a 1 ms vs a
      5 s release (verified by a dry+wet render sweep; p90 falling-slope moved
      <4 dB/s across the whole range). So 3.5 s stands on the ear, not on a
      measurement; nothing in the console data contradicts it. Pinning the exact
      constant needs either a console capture of a *sparse* track with isolated
      note-offs, or RE of `code.bin`. **Open
      follow-up:** `decay == 127` (`ConvertDecay`) is still treated as instant —
      probably the same sentinel, but every instrument seen with `decay 127` also
      had full sustain (decay unobservable), so it's left until there's evidence;
      the definitive answer for both is in the dumped `code.bin` (nn::snd rate
      handling) if someone wants to reverse-engineer it.

### 5. Licensing — ✅ resolved
The vendored `libsmfc` shipped without a license notice, but it is loveemu's
MIT-licensed code (identical to the copy in
[loveemu-lab](https://github.com/loveemu/loveemu-lab)); the copy had simply
dropped the repo-level license. MIT is GPL-3.0-compatible, so there is no
conflict with caesar's GPL-3.0.

- [x] Add the MIT notice for `libsmfc` (`src/libsmfc/LICENSE`) and document the
      third-party licenses in the README. Binaries are now distributable.

### 6. Continuous integration & first release — final step
- [ ] GitHub Actions workflow: build on every push (start with Windows/MSVC).
- [ ] Release workflow: attach built binaries to a GitHub Release when a version
      tag is pushed.
- [ ] Add a CI status badge to the README.
- [ ] Cut **v0.5.0** — the first maintained release.

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
  finite loop repeat counts.
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

## Known bugs

Concrete defects found while surveying and evolving the code. None crash the tool
on the archives tested so far, but each is a real correctness or portability
hazard. Not release-blocking on their own.

- **Group file-table desync** (`Cgrp.cpp`, the file-record loop). Each record's
  offset is only consumed when the preceding marker equals `0x1F00`; on any other
  marker the offset read is short-circuited away by the `?:`, so the length field
  and every following record are read from the wrong position. A group containing
  an external or absent file misparses everything after it.
- **32-bit reader invoked with an 8-byte width** (`Csar.cpp`, the `0x220C` and
  `0x220D` file-record branches call `ReadFixLen(pos, 8)`). That shifts a 32-bit
  value by up to 56 bits — undefined behavior — and silently discards the top
  four bytes. It works under MSVC today; a sanitizer build or a different compiler
  could flag or miscompile it.
- **Non-ASCII file names.** Input paths and archive-internal names pass through
  narrow `char*` / `std::string` into `std::filesystem`, so non-ASCII names
  (common for Japanese titles) can be mangled or throw. Illegal-character
  sanitizing is done; character encoding is not.
- **Compiler narrowing warnings** (`C4267` / `C4244`, `size_t`/`int` to smaller
  types). Harmless in practice but they flag real implicit truncations; worth a
  code-quality pass.
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
