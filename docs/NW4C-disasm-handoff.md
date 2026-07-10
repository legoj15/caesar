# NW4C `snd` disassembly — session handoff

**Purpose.** Ground-truth the 3DS NintendoWare-for-CTR (`nw::snd`) sound runtime by
disassembling a game's ARM11 `code.bin`, to settle open fidelity questions in caesar
(chiefly: what does envelope byte `127` really mean). This file is the starting point
for the next session — it records what is *resolved*, the exact addresses/artifacts,
the Wii (NW4R) crib that made it readable, and what is still open.

Author date: 2026-07-09. Target binary: **StreetPass Mii Plaza**, `MeetSound` engine.

---

## TL;DR — the headline result is in

**Release/decay byte `127` = the FASTEST envelope rate (instant), NOT a long release.**
The NW4C rate-conversion routine is byte-for-byte identical to the Wii NW4R
`EnvGenerator::CalcRelease`, including the `if (x == 127) return 65535.0f;` branch.
65535.0f is the maximum per-millisecond rate → the envelope collapses in ~one step.

Consequence for caesar: the committed change mapping release-`127` to `ConvertTime(3.5)`
(~3.5 s, commit `b078932`) is **contrary to the hardware** and should be reverted toward
instant. The audible multi-second tail on those Mii Plaza pads is **DSP reverb**, not the
note's release — so it belongs in the reverb path (already emitted as CC91), not faked
with a long note-release. `decay == 127` is the *same* sentinel and is *also* instant,
which means caesar's existing treatment of decay-127 as instant is **correct** and needs
no change. (Do not edit code from this handoff alone — see "Recommended caesar change"
for the sign-off gate.)

---

## How we got here (state coming in)

- The engine ships **statically linked in each title's `code.bin`** (no shared system
  module). Confirmed present in every binary we checked via BCSAR magics + the NW4R
  envelope-table fingerprint. Binaries are **stripped** (no `nw::snd` symbols).
- Extraction is done. Fully-decrypted base `.cia`s are in
  `E:\legoj\Documents\3DSWii Dumps\3ds firmware` (ctrtool reports `Crypto key: None`,
  no keys needed). `ctrtool.exe` (at `E:\legoj\Documents\3DSWii Dumps\ctrtool.exe`,
  2020 build) does the whole chain and auto-decompresses `.code`. Extracted apps live in
  `E:\legoj\Documents\3DSWii Dumps\re_extract\<App>\` (`exheader.bin`, `plain.bin`,
  `exefs\code.bin`, romfs listing): **MiiPlaza** (MeetSound), **eShop** (TigerSound),
  **Photos_Camera** = the 3DS Camera app (PNOTE_Sound), **SystemSettings** (mset),
  **Sound_SNOTE** (3DS Sound app).
- The console has **no newer Mii Plaza build** — its installed copy is byte-identical
  to the launch build (SDK 5.2). The only "update" is data-only DLC. So the launch
  build is the final ground truth; nothing newer to chase (and the eShop SDK-11.2
  envelope tables are already proven identical to it anyway).

---

## Similarity to the Wii — why the disassembly was readable at all

NW4C `snd` is the direct descendant of the Wii's **NW4R** `snd`, which has a complete,
byte-matching public decompilation in **`doldecomp/ogws`** (Wii Sports). The envelope
module in particular is 100 % matched there. This gave us an exact structural + numeric
crib, so we could identify stripped NW4C functions by matching their *shape* and
*constants* to the NW4R source. The lineage is real and load-bearing:

| | Wii (NW4R) | 3DS (NW4C) |
|---|---|---|
| namespace | `nw4r::snd::detail` | `nw::snd::internal` |
| envelope class | `EnvGenerator` | (same, stripped) |
| decay/release curve | `CalcRelease(int)` | **identical** (see below) |
| `DecibelSquareTable[128]` | `-723,-722,-721,-651,…,-1,0` | **byte-identical** |
| `attackTable[128]` | `0.9992175f … 0.0f` | **byte-identical** |
| sequence opcodes | BRSEQ MML | BCSEQ MML (same command set) |

The decay/release rate table is *not* stored — like NW4R, NW4C **computes** it in
`CalcRelease`. (caesar's `DecayTable` is caesar's own precomputation of the same curve.)

### The Wii reference (`ogws/src/nw4r/snd/snd_EnvGenerator.cpp`)

```cpp
f32 EnvGenerator::CalcRelease(int release) {
    if (release == 127)     return 65535.0f;               // fastest rate = instant
    if (release == 127 - 1) return 24.0f;                  // 126
    if (release < 50)       return (release * 2 + 1) / 128.0f / 5.0f;
    return 60.0f / (127 - 1 - release) / 5.0f;             // 60 / (126 - release) / 5
}
```
`SetDecay` calls `CalcRelease` too — one curve serves both decay and release. Class
functions: `Init, Reset, GetValue, Update, SetAttack, SetDecay, SetSustain, SetRelease,
CalcRelease, CalcDecibelSquare`. `GetValue()` returns `mValue / 10.0f`.

---

## The NW4C match (Mii Plaza `code.bin`, load base `0x00100000`)

Two leaf functions implement the identical curve. Annotated disassembly of the first
(the decay-rate converter; writes the computed rate to object `+0x8`):

```
0x201D60:  cmp    r1, #0x7f            ; if (x == 127)
0x201D64:  vldreq s0, [pc, #0x58]      ;   s0 = *0x201DC4 = 0x477FFF00 = 65535.0f
0x201D68:  beq    0x201DBC             ;   -> store & return   (INSTANT)
0x201D6C:  cmp    r1, #0x7e            ; if (x == 126)
0x201D70:  vldreq s0, [pc, #0x50]      ;   s0 = *0x201DC8 = 0x41C00000 = 24.0f
0x201D74:  beq    0x201DBC
0x201D78:  cmp    r1, #0x32            ; if (x < 50)   (0x32 = 50)
0x201D7C:  vldr   s0, [pc, #0x48]      ;   s0 = *0x201DCC = 0.2f  (= 1/5)
0x201D80:  bge    0x201DA4             ;   else -> 60/(126-x)/5 branch
0x201D84:  mov    r2, #1               ; --- x < 50: (2*x + 1) ---
0x201D88:  add    r1, r2, r1, lsl #1
0x201D8C:  vmov   s1, r1
0x201D90:  vldr   s2, [pc, #0x38]      ;   s2 = *0x201DD0 = 0.0078125f  (= 1/128)
0x201D94:  vcvt.f32.s32 s1, s1
0x201D98:  vmul.f32 s1, s1, s2         ;   (2x+1) * 1/128
0x201D9C:  vmul.f32 s0, s1, s0         ;          * 1/5
0x201DA0:  b      0x201DBC
0x201DA4:  rsb    r1, r1, #0x7e        ; --- x >= 50: (126 - x) ---
0x201DA8:  vmov   s1, r1
0x201DAC:  vldr   s2, [pc, #0x20]      ;   s2 = *0x201DD4 = 60.0f
0x201DB0:  vcvt.f32.s32 s1, s1
0x201DB4:  vdiv.f32 s3, s2, s1         ;   60 / (126 - x)
0x201DB8:  vmul.f32 s0, s3, s0         ;          * 1/5
0x201DBC:  vstr   s0, [r0, #8]         ; store rate -> obj+0x8
0x201DC0:  bx     lr
; literal pool:
0x201DC4: 0x477FFF00 (65535.0f)  0x201DC8: 0x41C00000 (24.0f)
0x201DCC: 0.2f (1/5)  0x201DD0: 0.0078125f (1/128)  0x201DD4: 0x42700000 (60.0f)
```

This is a 1:1 match to the Wii `CalcRelease` — same four branches, same constants
(65535.0, 24.0, 1/128, 1/5, 60.0), same `(2x+1)` and `(126-x)` arithmetic. **The
`x == 127 → 65535.0f` branch is present and unchanged.**

The second, identical function at **`0x201E3C`** writes its rate to object `+0xC`
instead of `+0x8` — i.e. one is the **decay**-rate setter and the other the
**release**-rate setter, both using the one shared curve exactly as NW4R does. So
**both** decay-127 and release-127 resolve to 65535.0f (instant).

---

## Concrete artifacts & addresses (for the next session)

All vaddrs assume load base `0x00100000`; file offset = vaddr − 0x100000.

| item | vaddr | notes |
|---|---|---|
| `DecibelSquareTable[128]` (`s16`) | `0x328844` | in `.rodata`; head `-723,-722,-721,-651,…` |
| `attackTable[128]` (`f32`) | `0x328944` | head `0.9992175f`, tail `…0.0f` |
| decay-rate converter (`CalcRelease` form) | `0x201D60` | leaf; writes rate → obj `+0x8` |
| release-rate converter (`CalcRelease` form) | `0x201E3C` | leaf; writes rate → obj `+0xC` |
| EnvGenerator `Init`/reset | `0x201CD0` | zeroes byte params `+0xC..+0x10`, sets float fields `+0x14..+0x3C` |
| sustain / decibel-square calc | `0x14AFC8` | uses `DecibelSquareTable`; `vsqrt`-based |
| `DecibelSquareTable` load site | `0x14B10C` | `LDR r3, =0x328844` |
| `attackTable` load sites | `0x19051C`, `0x201DD8` | `LDR r1/r2, =0x328944` |

EnvGenerator object layout (partial, from `Init` @ `0x201CD0` and the setters): byte
params at `+0xC..+0x10` (attack/decay/sustain/release/… as bytes), computed **decay
rate** `f32` at `+0x8`, **release rate** `f32` at `+0xC`, and a block of `f32` state at
`+0x14,+0x18,+0x1C,+0x20,+0x24,+0x28,+0x30,+0x38,+0x3C`. (Confirm exact field names
against the NW4R `EnvGenerator` struct if you need them.)

### Segment layout (Mii Plaza exheader)
`.text` @ `0x00100000` size `0x210684` · `.rodata` @ `0x00311000` size `0x34DAC` ·
`.data` @ `0x00346000` size `0x1F5D4`, bss `0xA8158`. Flat, page-aligned — a raw import
at `0x00100000` (ARMv6K, little-endian) reproduces the loader's view.

---

## How to reproduce / continue

- **Disassembler:** none is installed on this machine (no IDA/Ghidra). We worked headless
  with **capstone 5.0.7** (`pip install capstone`, already done). The hunt script is
  `…\scratchpad\disasm_hunt.py`; its output is `…\scratchpad\disasm_report.txt`.
  (Scratchpad is session-temporary — re-run the script to regenerate.) For interactive
  work, install **Ghidra + `Martmists-GH/ghidra-ctr-loader`**, load
  `re_extract\MiiPlaza\exefs\code.bin` at `0x00100000` with `exheader.bin`, and jump to
  the addresses above.
- **Re-extract any app from a CIA:** `ctrtool -t cia --contents=c <cia>` then
  `ctrtool -t ncch --exheader=exheader.bin --plainrgn=plain.bin --exefsdir=exefs <c.0000.*>`.
  `.code` comes out already decompressed. Read SDK version tags from `plain.bin` (e.g.
  `[SDK+NINTENDO:NW4C_3_6_1_snd]`); older SDKs (Mii Plaza's 5.2) omit the per-module tag.
- **Other binaries for cross-checking drift:** eShop (`0x3C627C`/`0x3C637C` tables,
  SDK 11.2) has byte-identical envelope tables — a late-SDK confirmation. The 3DS FTP
  homebrew (ftpd, port 5000, **anonymous**) can pull more titles/CIAs if needed.

---

## Recommended caesar change (needs user sign-off — reverses a committed change)

`Cbnk.cpp::ConvertRelease` currently special-cases `release == 127 → ConvertTime(3.5)`.
Ground truth says `127` is the *fastest* rate (instant). The faithful fix:

1. Map release-`127` to instant/fastest (mirror `ConvertDecay`'s existing 127 handling),
   removing the `~3.5 s` special case.
2. Rely on the already-emitted CC91 reverb for the audible tail; optionally document that
   faithful playback needs a reverb-capable player (the tail is DSP reverb, not release).
3. Re-check `BGM_DEN_EMPTY_LANDSCAPE` and `BGM_MAIN_Mii_Only_One` with short-release +
   reverb against the console captures.

This changes `.mid`/SF2 output, so gate it on the user's ear + explicit approval, per the
project's verification norms. `decay == 127` needs **no** change (already instant, now
confirmed correct).

---

## Resolved in session 2 (2026-07-09) — reverb location, NAND, per-note fields

Settled by a 15-agent investigation (5 recon + 9 adversarial verifiers + synthesis); all three
load-bearing claims survived 3-of-3 refutation attempts. The practical upshot is that **no further
`code.bin` disassembly is warranted**, and the DSP-firmware work — while now precisely targeted —
has no consumer in caesar's output formats.

### Reverb runs on the DSP, not the ARM11 — confirmed

This *reverses the architecture* relative to the Wii, so it was worth checking rather than
assuming. On Wii, NW4R's `FxReverbHi`/`FxReverbStd`/`FxChorus`/`FxDelay` were thin parameter
wrappers around `AXFXReverbHiCallback`, which processed the aux-bus buffer **on the PowerPC CPU**
(`ogws` proves this; the comb/allpass tables live in the closed AXFX SDK, not the decomp). On
3DS, Nintendo moved effect computation onto the Teak DSP. Evidence:

- **Binary positive.** The only audio-DSP code in Mii Plaza's `code.bin` is the *upload path*.
  At vaddr `0x10D15C` it loads the `dsp::DSP` service string (vaddr `0x31F8DC`) and does the
  `srv` `GetServiceHandle` dance; at vaddr `0x10D204` it loads `r1 = 0x00355100` (firmware
  pointer) and `r2 = 0x0000C288` (firmware size) from its literal pool at `0x10D268`/`0x10D26C`,
  `stm`s them into the DSP descriptor at `obj+0x18`, and calls `LoadComponent` at `0x228F4C`.
  That pointer/size pair is byte-exact the embedded `DSP1` blob. The ARM11 connects, uploads and
  configures. It does not compute audio.
- **Binary negative.** No comb/allpass delay-length table (Freeverb/Schroeder `1116,1188,1277,
  1356,1422,1491,1557,1617` and `556,441,341,225`) exists in Mii Plaza *or* eShop `code.bin`, at
  any width or endianness; no `Reverb`/`AXFX`/`FxReverb`/`allpass`/`AuxBus`/`SoundSystem`/
  `nw::snd` strings. (The `Comb`/`Delay` substring hits are `EnemyCombination`, shader
  `combineRgb`, `EnterDelay` — UI, not audio.) Taken alone this is a heuristic and not the
  linchpin, but it rules out the one competing architecture 3dbrew documents: an ARM11-side
  custom effect over the intermediate-mix/aux buffers, i.e. the Wii model carried forward.
- **Architecture.** 3dbrew: the DSP reverb "consists of two comb filters and one all-pass filter
  in standard configuration". Azahar/Citra `audio_core/hle/shared_memory.h` models
  `ReverbEffect reverb_effect[2]` and `DelayEffect delay_effect[2]` as config blocks the
  *application writes* and the *DSP consumes each audio frame* (the `*_dirty` flags are "set by
  the application… The DSP clears these each audio frame"). `ReverbEffect` is
  `INSERT_PADDING_DSPWORDS(26); ///< TODO` — 52 bytes, not one field named, an ~8-year-old stub
  that survived the Citra→Azahar migration. That stub is *why* emulated NW4C audio sounds dry;
  it is corroboration, not counter-evidence.

So the seconds-long tail is DSP reverb, and only the Teak firmware can account for it. `0xD9`
"fx send a" sets an aux-bus send *gain* (`SourceConfiguration.gain[1]`/`gain[2]`, feeding
intermediate mixers 1/2) — a DSP mixer send, not a CPU-effect send. Mapping it to CC91 is
semantically correct, and is the format ceiling (see below).

### The NAND dump, `otp.bin` and `movable.sed` are not needed — for anything here

**There is no standalone `dspfirm.cdc` on 3DS NAND.** The firmware exists only embedded inside
application `.code`. Verified three ways:

- Scanning all 90 system modules under `nand/00040130/**/*.app` for the `DSP1` magic hits exactly
  one: title `0004013000001a02` (the `dsp` sysmodule, `0x6600` bytes). Its lone `DSP1` at `0x4842`
  is the magic-compare constant it uses to *validate* a firmware an app hands it via
  `DSP_LoadComponent`. Far too small to be a firmware.
- The canonical dumper, `zoogie/DSP1` (`source/main.c`), never touches NAND, OTP or
  `movable.sed`. It iterates six hardcoded Home Menu title IDs, opens Home Menu's `.code` via
  archive `0x2345678a`, LZSS-decompresses, `memmem`s for `DSP1`, checks the per-segment SHA-256s,
  and writes the file.
- The NAND `.app`s here are already decrypted (NCCH NoCrypto bit set) but still LZSS-compressed —
  strictly *less* usable than the five already-decompressed copies in `re_extract`. OTP and
  `movable.sed` are console-unique NAND/SD crypto secrets, irrelevant to this work.

### The 3 "discarded" per-note fields are structural plumbing — nothing to recover

Hypothesis (NW4R's `lfoTable`/`graphEnvTable`/`randomizerTable`) **refuted**. NW4R's
`BankFile::InstParam` is only `0x14` bytes and has no such note-level tables at all. In NW4C the
note wrapper holds a Velocity Region starting at note`+0x10`; subtract `0x10` and everything lines
up with Gota7's Citric-Composer BCBNK spec:

| caesar site | VR offset | meaning |
|---|---|---|
| `Note 0x24` | `+0x14` | **f32 tune** — dropped entirely (see below) |
| `Note 0x28` | `+0x18` | interpolation (read, never emitted) |
| `Note 0x2C` | `+0x1C` | `0x20` = offset from VR start to the ADSHR reference |
| `Note 0x30` | `+0x20` | ADSHR `Reference.type` = 0 |
| `Note 0x34` | `+0x24` | ADSHR `Reference.offset` = 8 |

`0x30 + 8 = 0x38`, exactly where caesar already reads attack/decay/sustain/hold/release. The three
words are the self-referential `DataRef` chain pointing at the envelope caesar already parses.
Across 1,628 notes in 57 banks spanning three engines (MeetSound, PNOTE_Sound, GardenSound) they
are perfectly invariant at `0x20`/`0x00000000`/`0x00000008`, with flags word `0x21F` throughout.
Binary confirmation: the voice-setup site at vaddr `0x192390`–`0x1923C8` installs
A→`0x201DD8`, D→`0x201D60`, S→`strb +0x18`, H→`0x201D40`, R→`0x201E3C`, and reads nothing else
alongside them.

**Do not try to recover them.** The one per-note field carrying genuinely lost musical intent is
the **f32 tune at `Note 0x24`** (`VR+0x14`): `1.0` for ~99 % of notes, but real values occur
(`0x3F7B9A21` ≈ 0.982 ≈ −31 cents; `0x3F811D26` ≈ 1.0087 ≈ +15 cents). It maps cleanly to SF2
`fineTune`/`coarseTune` via `cents = 1200·log2(tune)`. Small, real, cheap.

Latent robustness note: caesar hardcodes these offsets. All 1,628 sampled notes carry flags word
`0x21F`; a bank with a different flags word would shift every field and caesar would silently
misparse. Following the reference (`note+0x10 + *(note+0x2C)`, then `+8`) instead of the hardcoded
`+0x38` would harden it. Not an observed bug.

---

## Session 3 (2026-07-09) — voice manager & steal policy: the Wii crib HOLDS

**Strategic verdict: CONFIRMED, high confidence.** The NW4C voice runtime is a behavioural 1:1 port
of the Wii NW4R `snd_VoiceManager`. This was the de-risking dig for the whole player project: if the
crib held for the voice manager, the rest of the sequence runtime is a *bounded port* rather than a
research project. It held. Four recon agents, six adversarial re-disassemblies (two claims × three
lenses, **0 refutations**), and the orchestrator re-disassembled every load-bearing instruction
from the raw bytes this session (`scratchpad\verify_voice.py`).

### The pool: 24 voices, byte-confirmed three ways

There are two nested voice layers, and both cap at **24**:

| Layer | AllocVoice | Manager (BSS) | `sizeof` | priority @ | how sized |
|---|---|---|---|---|---|
| **Upper** — NW4R `VoiceManager` port (soft voices) | `0x14D7B8` | `0x00407828` | `0x60` | `+0x40`, masked `&0xFF` | dynamic `count = size / 0x60`; config default **24** |
| **Lower** — DSP-source pool (replaces Wii AX) | `0x14F3F4` | `0x004058D8` | ~`0x70` (est.) | `+0x20` | fixed **24**: 24-bit alloc bitmask + hard `popcount == 0x18` |

The upper layer computes `voices = poolSize / sizeof(Voice)` exactly like NW4R — `Setup` @ `0x12D0E0`
divides by `0x60` via a reciprocal-multiply (`umull … ; lsr #6`, magic `0xAAAAAAAB` ⇒ ÷96), and
`SoundSystem::Setup` (`0x11F0CC`) reads the config word at `.data 0x354DD4`, **which I read directly
as `0x18` = 24**. The lower layer hardcodes 24 to match the DSP's 24 hardware sources
(Citra/Azahar `num_sources = 24`). Two different sizing philosophies, same number. A separate config
path clamps a requestable soft-voice count to `[4, 32]`, so 32 is a soft ceiling but **24 is the
operative floor** — the DSP has only 24 sources regardless.

### The steal policy — the exact NW4R guard, verified from raw bytes

Both `AllocVoice`s implement the NW4R algorithm verbatim: reuse a free voice; else look at the
**front of a priority-sorted active list** (a single head-pointer deref `ldr [mgr+8]`, *not* a
linear min-scan — the list is kept sorted on insert); **refuse to steal if the front outranks the
requester**; else evict the front and fire its callback with `status = 2` (drop-voice). The upper
layer's decisive bytes, which I re-disassembled this session:

```
0x14D7F0  ldr  r0, [r5, #8]      ; front of priority list
0x14D7F4  sub  r4, r0, #0x58     ; intrusive list node at Voice+0x58
0x14D7F8  ldr  r1, [r0, #-0x18]  ; front->priority   (node-0x18 = Voice+0x40)
0x14D7FC  cmp  r1, r7            ; vs requested priority
0x14D800  bgt  0x14D890          ; front outranks -> return NULL   <-- NW4R guard, verbatim
...
0x14D86C  and  r0, r7, #0xff     ; priority & PRIORITY_MAX(255)
0x14D870  str  r0, [r4, #0x40]   ; store into the reused voice
```

The lower layer (`0x14F3F4`) is the same policy over the bitmask pool, with one NW4C addition — a
`0x7FFF` "never-steal" sentinel checked *before* the priority compare
(`sub #0x7f00 ; subs #0xff ; beq → NULL`), the 3DS analogue of NW4R's pinned `PRIORITY_MAX`; here
priority is a full signed word. The pool-full test at `0x14F418` is `popcount(bitmask) == 0x18`
(the popcount helper is `0x14FF70`, SWAR masks `55../33../0f..`) — i.e. the "24" is semantically a
pool-full test, not a stray constant.

### Correction to a recon misread — priority is NOT coarsened

One recon agent reported note priority collapsing to a 0..2 class (the `cmp #2 / movgt` at
`0x192364`). I re-disassembled it: that value is loaded from `[sp,#0x14]`, clamped to ≤2, and passed
as AllocVoice's **r1 = `numChannels`** (mono/stereo/≤3 channels per note) — *not* r2 = priority. The
real priority threads separately (note-on `r5` → wrapper `0x193E0C` arg1 → AllocVoice r2) and is
masked to 0..255. **The player must model the full 0..255 priority**, sourced per the crib as
`playerPriority + trackPriority` (`DEFAULT_PRIORITY = 64`, released notes dropped to
`PRIORITY_RELEASE = 1`), not a 3-level approximation.

### Key addresses (all `[self]` = orchestrator re-disassembled this session)

| vaddr | what |
|---|---|
| `0x14D7B8` | upper `VoiceManager::AllocVoice` (DropLowestPriorityVoice inlined) |
| `0x14D7F0`–`800` | the steal guard (front `[mgr+8]`, prio `[node-0x18]=+0x40`, `cmp/bgt → NULL`) |
| `0x14D86C` | priority write `and r0,r7,#0xff ; str [r4,#0x40]` |
| `0x14D890` | return-NULL epilogue (`mov r0,#0 ; pop {…pc}`) |
| `0x14F3F4` | lower DSP-source `AllocVoice` (bitmask pool, same policy) |
| `0x14F418` | pool-full test `popcount(bitmask) == 0x18` (popcount helper `0x14FF70`) |
| `0x14F428` | `0x7FFF` never-steal sentinel; `0x14F434/438` the guard; prio field `+0x20` |
| `0x12D0E0` | upper `Setup(mem,size)`: `count = size / 0x60` (reciprocal ÷96), node `+0x58` |
| `0x11F0CC` | `SoundSystem::Setup`: reads `0x354DD4`, carves `24 * 0x60`, calls `0x12D0E0` |
| `0x354DD4` | `.data` config word = **`0x18` = 24** (upper voice count) — read directly |
| `0x00407828` | upper VoiceManager singleton; `+8` prioList head, `+0x10` freeList |
| `0x004058D8` | lower DSP-source manager; bitmask `+0`, list head `+8`, `Voice*[24]` at `+0x15DC` |
| `0x193E0C` | two-layer note-on allocator (soft voice `0x192218` + upper `0x14D7B8`; handle → soft`+0x134`) |

Upper Voice: priority `+0x40` (masked 0..255), list node `+0x58`, `sizeof 0x60`.
Lower DSP voice: priority `+0x20`, DSP-source ptr `+0x68`, ~`0x70` B (sizeof estimated, not pinned).

### What it means & the next experiment

**The voice manager is a solved, bounded port**, so the presumption that the rest of the runtime
(SeqTrack MML interpreter, channel/tie/portamento, LFO, variables/conditionals/random) is *also* a
clean NW4R port is now well-supported. The ~7–10-session estimate for the sequence runtime stands.
Model a single 24-voice priority pool for a first cut; only pathological dense-stereo passages need
the 1–3-DSP-sources-per-note accounting.

**Most informative next dig:** trace the note-on priority argument (r5 into `0x193E0C`) upstream to
its sequence-data source and read the MML `SET_PRIORITY` handler. It confirms the
`playerPrio + trackPrio` computation, is the first probe into the **SeqTrack** module (early signal
on whether the parser is also a clean port), and directly feeds the caesar fix — teaching the
converter to *preserve* priority so a downstream player can steal correctly.

**Caveats.** How `0x14F3F4` is invoked was not byte-traced (no direct `bl` caller, no literal-pool
hit) — immaterial to the policy, but don't assert the call path. "1:1 port" is *behavioural*, not
byte-identical (ARM/VFP vs Wii PPC; offsets are 3DS-specific — `0x60`/`+0x40` here vs Wii
`0x12C`/`+0xB4`). All offsets are MiiPlaza's SDK-5.2 build; the logic is SDK-invariant, exact
offsets may shift in other titles.

---

## Why the RE stops here — *for the SF2/MIDI exporter only*

> **Scope correction (2026-07-09, later the same day).** Everything in this section is correct
> **conditional on caesar's output being SoundFont2 + MIDI.** The project's actual goal is a BCSEQ
> tool suite including a console-accurate **player**, which has no format between it and the
> speaker. Under that scope this verdict inverts: the reverb is *required*, and the ARM11
> `nw::snd` **sequence runtime** (voice stealing, LFO, variables/conditionals/random, tie,
> portamento) becomes the critical path — none of which an exporter ever needed. See
> "Beyond the converter: the BCSEQ tool suite" in `ROADMAP.md`.
>
> What survives unchanged: **Teak disassembly is still not the way in.** Recover the reverb
> behaviourally by running the real firmware in `teakra` offline, impulsing the aux bus, and
> fitting a comb+allpass model to the captured tail — then validate against a New 3DS line-in
> capture. And do **not** crib `ogws`' `snd_FxReverbHi.cpp`: it wraps the Wii's PowerPC-CPU
> `AXFXReverbHi*` SDK calls, so lifting it would import *Wii* reverb into a 3DS player.



**SoundFont2 and MIDI cannot carry a reverb algorithm.** SF2's only reverb hook is generator 16
`reverbEffectsSend`, a 0–1000 *send amount* into the **player's own** effects unit; the spec
declines to define any reverb algorithm and says the unit is optional and player-dependent. MIDI's
only hook is CC91 (reverb send depth). Neither can hold an impulse response, room size, decay
time, or coefficients. caesar already emits `0xD9 → CC91` and `0xDA → CC93`, so **it is already at
the format ceiling.** FluidSynth would apply Freeverb (8 comb + 4 allpass; four knobs — roomsize,
damping, width, level) — not the 3DS reverb, and not adjustable into it, because Freeverb is not a
convolution reverb.

So even with the exact Teak coefficients in hand, there is nowhere in caesar's output to put them.
The only faithful consumers would be (a) shipping a separate convolution-IR `.wav`, or (b) caesar
growing an offline renderer — both outside "the maintained, correct BCSAR → SF2/MIDI converter".
Treat Teak disassembly as a *separate hobby project* (a standalone faithful 3DS reverb IR or
plugin), never as a caesar feature. Baking reverb into the samples is also wrong: it is a mix
send, not a per-sample property; it kills dry/wet control, breaks SF2 loop points, and bloats
tails.

---

## Corrections to earlier claims in this file

- The embedded firmware starts at file offset **`0x255100`**, not `0x255200` — that is the magic,
  which sits at firmware `+0x100`, after the RSA-2048 signature.
- Each firmware's `content size` field at `+0x104` **includes** the `0x100` signature. Hashing
  `magic … magic+size` overshoots by `0x100` bytes and yields a wrong digest.
- DSP1 header layout: the segment count is 1 byte at **`+0x10E`** (`+0x10F` is flags), and segment
  records are **`0x30`** bytes, not `0x2C`. A `0x2C` stride yields a bogus 3-segment count.
- "The dumped firmware is effectively shared across titles … a once-off extraction, not a per-game
  problem" — **overstated**. Three distinct images across the five extracted titles:
  Mii Plaza `sha256 944b40b5…` (`0xC288`, SDK 5.2-era; 5 segments, PROG `0x1E5A` + `0x8CA6`);
  eShop = Photos/Camera = System Settings, byte-identical `sha256 8e213f3e…` (`0xC25C`; 5 segments,
  PROG `0x1E5C` + `0x8C78`); and 3DS Sound `sha256 5c03dd63…` (`0x34B1A`, `layout 0xFFFF`, 6
  segments, 3 PROG totalling ~210 KB — the AAC-capable firmware, matching its `plain.bin` tags for
  TMC `AACDec`/`M4ADemux`/`MP3Dec` and Nintendo `DSPAACEnc_1_0_0`). Only the DSP *program* code and
  the AAC data differ: the special segment (word `0xEF29`, `0x214` B) and two DATA tables (`0x854`
  and `0x24`) are byte-identical in **all five**, and the `0xC10` table is shared by the four
  non-AAC titles. Per-SDK-generation, not once-off — and not per-game either.

---

## Still open

- **DSP reverb coefficients.** Precisely targeted but **deliberately not pursued** — see "Why the
  RE stops here". If ever wanted: firmware at Mii Plaza `code.bin` file offset `0x255100`, content
  size `0xC288`. Parser + extractor: `…\3DSWii Dumps\re_extract\dsp_extract.py`, which writes
  standalone `*_dspfirm.cdc` and the PROG segments into `re_extract\dspfirm\`, self-certified by
  the firmware's own embedded per-segment SHA-256s (all `OK`).

  Mii Plaza's Teak code is two PROG segments — firmware offset `0x300` size `0x1E5A` at Teak
  program word `0x0`, and firmware offset `0x215A` size `0x8CA6` at word `0x2900` (≈43 KB total).
  Coefficient/delay-line DATA tables sit at firmware offsets `0xAE00` (word `0x3C2C`, `0xC10` B),
  `0xBA10` (word `0x4C00`, `0x854` B) and `0xC264` (word `0xC298`, `0x24` B). Tool:
  `wwylele/teakra` (MIT) — build with `-DTEAKRA_BUILD_TOOLS` for `dsp1_reader`, then wrap
  `Teakra::Disassembler::Do(uint16_t opcode, …)` in a ~30-line harness; there is no
  bytes-in/asm-out CLI. Nobody has publicly RE'd this reverb: Citra/Azahar's `ReverbEffect` has
  been a 52-byte `TODO` for ~8 years, while the sibling `DelayEffect` *is* mapped (transfer
  function `H(z) = a·z^-N / (1 − b·z^-1 + a·g·z^-N)`, 7-fractional-bit coefficients).
- **Confirm decay-vs-release object offsets** (`+0x8` vs `+0xC`) if you want to be 100 %
  sure which setter is which (immaterial to the 127 result — both are instant).
- **Thumb false positive:** the CMP-#127/#126 fingerprint also flagged a Thumb region at
  `~0x1CF2C0`; that disassembles as a jump/dispatch table, **not** CalcRelease. The real
  match is the ARM pair above. Ignore the Thumb hit.
