# Beyond the converter: the BCSEQ tool suite — design

The long-term goal (stated 2026-07-09) is that caesar is the **foundation** of a suite of
BCSEQ tools, not an end in itself: a sequence **editor**, a **player that behaves like
console** while rendering at any PC sample rate, **tracker export**, and the existing
best-effort SF2/MIDI export. Everything below was settled by a 20-agent design workflow
(7 recon + 12 adversarial verifiers + synthesis). Three of the four load-bearing claims
were **refuted on their headlines** and are recorded here in their corrected form.

This is the settled design record. Stage *status* is tracked as checkboxes in
[ROADMAP.md](ROADMAP.md); completed-work narratives live in [HISTORY.md](HISTORY.md);
disassembly addresses and evidence in [NW4C-disasm-handoff.md](NW4C-disasm-handoff.md).

## The decision that determines everything else: internal pipeline rate

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

## LLE vs HLE: build the engine, keep `teakra` as an oracle

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

## RE priorities, inverted

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

## The next milestone: byte-identical round-trip

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
bank-note offsets (tracked under "Known bugs" in ROADMAP.md; a default-visible warning now
fires on any bank whose note-flags word differs from `0x21F` — never yet observed).

## Library-core refactor (a strangler, not a rewrite)

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

_Status (2026-07-14): **stage 0 is complete (all four steps).** Step 3 done — all six classes now
parse into a retained model and export from it (`Cwav`/`Cwar`/`Cbnk`/`Cseq`, then `Csar`/`Cgrp`).
Every record carries **span-relative offsets**, not raw pointers, so the tree survives a stage-1
buffer drop. Step 4 done — a **`caesar_core` static library** now holds the six classes plus their
shared `Common`/`ParseContext` and the header-only `Options` (PUBLIC-linking the vendored
`sf2cute` + `libsmfc`); the `caesar` executable is just `src/caesar.cpp` linking it, and the
suite's player/tracker/editor will link the same library directly. Build-only, output-identical
(compile flags verified identical per translation unit). Record offsets/lengths
are stored; the discarded header words are retained as typed fields; and the regions never parsed
today are held as opaque spans — CWAV IMA-ADPCM/raw DATA, CBNK `0x6001`/`DataRef` words, CGRP
**INFX** (a clean offset+length span from the chunk table), and CSAR's **player (`0x2102`)** and
**set (`0x2104`)** tables (section start + the entry-offset arrays; the per-entry record payload
and the section byte-length are the remaining stage-1 gap — the header gives no direct length, so
stage 1 must bound them from the sorted section layout / `InfoEndOffset`). What still does **not**
exist: the model→bytes writer itself (every offset/size table must be recomputed on the way out),
and the inter-record alignment padding, which is modeled nowhere and must be reproduced by rule or
captured as opaque gap-spans. Those are stage 1._

**The safety net does not cover audio.** The byte-identical A/B guards only the current export
path; the player's output is invisible to it. A second net is required — deterministic golden-hash
renders (fixed rate, seeded randomness, pinned reverb) plus tolerance-band comparison against the
existing New 3DS line-in captures. Over-trusting the familiar green check is the most likely way
a broken player ships unnoticed.

## Staged plan

Ordered so each stage is worth having even if work stops there. (Stage status is
tracked in ROADMAP.md.)

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

## Tracker export: `.it`, framed as a lossy authoring bridge

Target **Impulse Tracker `.it`** with a hand-rolled writer, and say plainly in the docs that it
is a *preview/authoring bridge*, not a fidelity path — the fidelity path is our own player. It
wins because it is the free lingua franca (OpenMPT, Schism) and its instrument model — sample
keymap + volume/pan envelopes + NNA virtual voices — is the closest classic analogue to a BCBNK
velocity-region keymap with ADSR and a release tail; 64 channels with NNA covers 16 polyphonic
BCSEQ tracks. Reject `.xm` (no NNA, 32 channels, 12-point envelopes) and Furnace (register-level
chiptune; its sample support is chip-PCM, not a general sampler). Offer `.mptm` as a one-flag
upgrade sharing ~95 % of the code path. Watch the tempo ceiling: fast pieces at fine rows/beat
can exceed IT's tempo 255, so a per-sequence rows/beat solver is needed, not a constant.

## Risk register

1. **Under-scoping the sound runtime** because "the mixer port is 85 %". The mixer is the easy
   half; the voice/sequence engine that feeds it exists nowhere to copy. *De-risk:* one capstone
   session on the Mii Plaza voice allocator to confirm the pool is 24 and the policy is
   priority-only. That single dig sizes the whole engine. *(Done 2026-07-09 — see "Voice
   stealing" above: pool 24 confirmed, priority-only confirmed, full 0..255 priority.)*
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

## Where the 3.5 s release hack lands

The model stores the **truth** (release-127 = instant). Each exporter renders that truth for its
own consumer. The **player** renders it as instant note-off plus a real reverb tail (stage 3) —
and must *not* inherit the 3.5 s fake, which would double the tail. The **SF2/MIDI exporter**
keeps the 3.5 s value as a *labelled compensation* behind `--pad-sustain` (default is the truth:
instant), because its consumer is a reverb-less synth and telling it the truth produces the
original dry-and-chopped bug. This is not a wart: it is the parser/exporter split doing its job —
one truthful model, several honest renderings for consumers of differing capability.
