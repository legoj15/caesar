# surround-probe / Part B — bind `span` (0xD7) to the DSP `gain[3][4]` rear lanes

> **DONE — live run 2026-07-19.** `span` (0xD7) IS the DSP MAIN rear gain lanes:
> on the sustained span-sweep voice, `SourceConfiguration.gain[0][2]`/`[0][3]`
> grew with span through a constant-power front↔rear crossfade
> (0.7071/0.0000 → 0.6108/0.3563 → 0.3452/0.6171, front²+rear² = 0.500), in
> Surround (`output_format` = 2). The same dual-dump **settled the gain address**:
> Candidate A (f32 @ slot+0x04) is the live DSP shared-memory layout; Candidate B
> (s16 @ slot+0x3c) is disproven — a fixed constant in every voice, the ARM11
> intermediate the disasm found. `dsp_gain_geometry.h` is flipped to settled.
> Full write-up: `docs/HISTORY.md` (2026-07-19). The run-book below is retained as
> the re-verification / different-firmware procedure.

**Part A** (`../`) proved on real hardware that the NW4C `span` command is
*audible* under System Settings **Surround** (front/rear virtualisation on the
headphone jack; console-confirmed 2026-07-11). Part A settled the *physics*.
**Part B** settled the *cause*: it reads the live per-voice DSP quad gain matrix
`SourceConfiguration.gain[3][4]` while a sequence sweeps `span`, and showed the
**rear gain lanes tracking the span value** — binding the opcode to the register
at the source.

This directory contains (the run-book is now a re-verification procedure):

| file | what |
|---|---|
| `build_partb_cartridge.py` | builds the span-sweep **stimulus** cartridge (patched `MeetSound.bcsar`) + a time→value `MANIFEST.md`, gated on byte-identical stage-1 round-trip |
| `gdb_read.py` | one-shot **GDB-RSP client** that reads the live SourceConfiguration over the console's Rosalina GDB stub and **dual-dumps both candidate gain layouts** (f32 @ +0x04 and s16 @ +0x3c) + raw slot bytes, labelled by lane |
| `test_gdb_read.py` | offline unit test of `gdb_read.py` against a fake RSP stub (no console) |
| `README.md` | this file — the live run-book + the derived addresses + the honest blockers |

Part A (`../source`, `../tools`, `../run.wav`, …) and capture-cartridge v1/v2
are untouched.

---

## 1. Build the stimulus cartridge (offline)

```sh
python build_partb_cartridge.py \
  --source "E:/legoj/Documents/3DSWii Dumps/Dumps/MiiPlazaUpdate/region_common/frame/sound/MeetSound.bcsar" \
  --out ../../../build/partb
# optional: --hold 6   (seconds each value is held; default 5, >=3-4 recommended)
```

Same LayeredFS mechanism as capture-cartridge: two Mii Plaza music-player
entries are hijacked in place, INFO volume→127 + INFO bank re-pointed, the
embedded `.bcseq` DATA payload rewritten **same-size** (byte-identical stage-1
round-trip), **NO 0xB6** (one bank per track, via INFO). Program 23 (the shared
1.512 s sustained looping tone) is the voice.

Two tracks, a deliberate differential on the one open modelling question —
**is `span` a continuous per-frame track control, or latched at note-on?**

- **Track A** (`BGM_MAIN_Mii_Only_One`, `BANK_MEET_SE_MAIN`) — **held-note span
  sweep**: ONE sustained note (note-wait OFF so the stream keeps running while
  the voice sounds), then `0xD7` span steps `0/32/64/96/127`, each held. This is
  the primary stimulus and assumes `span` is continuous (the NW4R/NW4C
  behaviour: pan & span are recomputed each audio frame in `Voice::CalcMixParam`).
- **Track B** (`BGM_DEN_EMPTY_LANDSCAPE`, `BANK_MEET_LEGEND`) — **retrigger span
  sweep** (a FRESH note per span value → a reading is guaranteed even if `span`
  latches at note-on) **+ front_bypass sweep** (`0xDB` = `0/64/127`, the optional
  second phase).

Read: **A's rear lanes move with span ⇒ span is continuous (done). A flat but
B's move ⇒ span is latched.** Either way the operator gets the binding.

### Gate (runs automatically)

1. capture-cartridge Python self-checks: locate-by-name re-parse, exact-boundary
   payload walk, INFO-poke assert.
2. the **real converter**: `caesar-roundtrip --verify` on the *patched* archive
   must report `mismatched=0` (byte-identical stage-1 round-trip), then `caesar`
   must parse it. (Skipped with a printed notice if `build/Release/caesar*.exe`
   isn't built; pass `--no-converter-gate` to force-skip.)

Verified run (`--hold 5`): gate **PASS** — `matched=78 mismatched=0`, parse OK;
track A pass = 29.0 s, track B pass = 44.0 s. Deploy target:
`SD:/luma/titles/0004001000021800/romfs/region_common/frame/sound/MeetSound.bcsar`.

The **time→value map** is in `build/partb/MANIFEST.md` — it gives, per pass,
the `[t_start, t_end)` window during which each span / front_bypass value is
active. The pilot tone at t=0 is the stopwatch sync anchor.

---

## 2. The gain address arithmetic + the TWO candidate layouts

The slot geometry (stride, voice count, and BOTH candidate gain offsets) is owned
by ONE cross-tool authority — `tools/dsp-shared/dsp_gain_geometry.h` — which
`shared_mem.h`, `dsp_regions.h` and `gdb_read.py` all consume (the Python parses
it at import). Do not hardcode a gain offset anywhere; that is exactly the bug
this replaced. The GDB `m` packet reads the *debuggee's* virtual memory, and the
DSP RAM is mapped at the same fixed VA in MiiPlaza's process — so the plugin's
arithmetic applies verbatim.

```
DSP data space base VA          = 0x1FF40000        (0x1FF00000 + 0x40000)
word W  -> VA                   = 0x1FF40000 + W*2
region-1 (double-buffer) word   = W | 0x10000       (VA += 0x20000)

MiiPlaza firmware (sha 944b40b5) region-0 WORD addresses:
  SourceConfiguration[24]   word 0x9E92   stride 192 B/voice
  FrameCounter (bank sel.)  word 0xBFFF   u16
  DspConfiguration          word 0x9430   196 B

Per-voice slot = base + v*192.  TWO rival gain layouts inside it:
  Candidate A (Azahar HLE):   float_le gain[3][4] @ slot+0x04, 12 f32 = 48 B
     gain[0][0] MAIN frontL +0x04   gain[0][2] MAIN rearL +0x0C  (span axis)
     gain[0][1] MAIN frontR +0x08   gain[0][3] MAIN rearR +0x10
     gain[1][*] AUX-A +0x14..0x23   gain[2][*] AUX-B +0x24..0x33
  Candidate B (disasm S7):    s16 lanes @ slot+0x3c, L-before-R
     FL +0x3c   FR +0x3e   RL +0x40 (span axis)   RR +0x42   aux +0x44..
  enable (u8) @ slot+0xA0
```

**WHY TWO.** `gdb_read.py` reads the live SourceConfiguration and **dumps both
candidates + the raw slot head +0x00..+0x50** per voice. Offline the two conflicted:
the disasm (Session 7) said s16@+0x3c; the oracle's unity-click LLE spike
(`dsp-oracle` PORT-NOTES §6) said the firmware read gains from f32@+0x04 (it
rendered unity with +0x3c left zero), and the disasm never traced +0x3c's base
pointer. **The 2026-07-19 live run decided it: Candidate A (f32@+0x04) is the live
shared-memory layout; Candidate B (+0x3c) is a disproven ARM11 intermediate** (full
verdict in `tools/dsp-shared/dsp_gain_geometry.h`). The dual-dump is retained so the
record stays self-describing on a different firmware.

**Concrete VAs, voice 0, region 0 / region 1** (any voice v: add `v*192`):

| field | region 0 | region 1 |
|---|---|---|
| SourceConfiguration base | `0x1FF53D24` | `0x1FF73D24` |
| A: gain[3][4] block (48 B) `+0x04` | `0x1FF53D28` | `0x1FF73D28` |
| A: MAIN rearL `gain[0][2]` `+0x0C` | `0x1FF53D30` | `0x1FF73D30` |
| A: MAIN rearR `gain[0][3]` `+0x10` | `0x1FF53D34` | `0x1FF73D34` |
| B: FL/FR/RL/RR s16 `+0x3c..+0x42` | `0x1FF53D60` | `0x1FF73D60` |
| enable byte `+0xA0` | `0x1FF53DC4` | `0x1FF73DC4` |
| FrameCounter (u16) | `0x1FF57FFE` | `0x1FF77FFE` |
| DspConfiguration base | `0x1FF52860` | `0x1FF72860` |
| output_format (u16) | `0x1FF52876` | `0x1FF72876` |
| surround_depth (f32) | `0x1FF5287C` | `0x1FF7287C` |

**Value formats.** Candidate A = IEEE-754 32-bit LE float (`struct.unpack("<12f",
…)`; ~1.0 = unity, 0.0 = muted). Candidate B = u16 lanes (gains are magnitudes;
the Session-7 pan law is a **constant-sum crossfade**, so on the stimulus voice
`FL+FR ≈ 32768` is candidate B's fingerprint). The raw hex head preserves the
bytes so nothing rides on choosing wrong.

**Which bank to read:** SourceConfiguration and DspConfiguration are
*application-written inputs*, so read the **CURRENT (higher-counter) bank**
(the one the DSP is acting on now). `gdb_read.py` reads both frame counters,
picks the higher with signed-wrap (`counter_newer`), and reads from that bank.

**Which voice:** the active-voice index is **not fixed** — ndsp allocates it.
`gdb_read.py` reads all 24 slots in one halt and flags every `enable!=0` voice
with its gain matrix. The stimulus voice is the one that stays ACTIVE across
snapshots and whose MAIN front/rear balance shifts with span. (Reverb/chorus
sends are 0 in the stimulus, so the AUX rows stay 0 and the MAIN row alone
carries the span signal — a clean read.)

---

## 3. Run the reader (offline test first)

```sh
python test_gdb_read.py                       # exit 0 = all 36 checks pass
pytest tools/surround-probe/partb -q          # 5 test items, checks assert red
```

The test spins a fake Luma-style RSP stub on a loopback socket and verifies the
checksum, `}`-escape / `*`-RLE decode, the chunked `m<addr>,<len>` read + hex
decode, the higher-counter bank selection, that the geometry is parsed from the
single authority header, and the **dual-dump decode** (candidate A f32 @ +0x04,
candidate B s16 @ +0x3c, and the raw slot head).

Live one-shot (per span window, per the MANIFEST):

```sh
python gdb_read.py --host <console-ip> --port <port-from-rosalina> --label span=64
# --all-voices to dump all 24 slots; --source-config-word 0x… for other firmware
```

---

## 4. LIVE RUN-BOOK (documented, NOT executed here) — with the honest blockers

Everything below needs the physical console (powered, on WiFi); none of it was
run in this prep. It does **not** need a human at the console — every step is
n3ds-mcp-drivable as of 2026-07-18 (see step 3).

1. **System Settings → Sound → Surround.** Console navigation. Required — the
   whole point is that the sequence runtime only writes non-zero MAIN rear gains
   in Surround (the in-band cross-check: `output_format` should read **2** and
   the rear lanes should be non-zero; in Stereo=1 / Mono=0 they should be ~0).
2. **Deploy the stimulus.** `ftp put` the built
   `.../MeetSound.bcsar` to the SD path in §1; enable Luma3DS "Game patching".
3. **Enable the Rosalina GDB stub** — Rosalina menu (L+Down+Select) →
   *Debugger options* → *Enable debugger*, then attach MiiPlaza in the
   *Process list*. **NO LONGER A BLOCKER (corrected 2026-07-19).** This step was
   previously documented as requiring a human to hold the console, on the grounds
   that InputRedirection cannot drive the Rosalina overlay (it reads raw HID,
   which the overlay bypasses) and that opening Rosalina freezes the NTR stream.
   Both premises still hold in isolation, but the conclusion was overtaken on
   2026-07-18: the n3ds-mcp fork drives the overlay through its own command
   channel and reads the menu back as text over UDP 4951, and
   `tools/console-capture`'s closed-loop `RosalinaMenuDriver` navigates it
   live-verified on hardware. The *Debugger options → Enable debugger* walk in
   particular has not been run yet — expect to confirm the label text — but it is
   ordinary menu navigation, not a physical-presence requirement.
   **Note the port the menu shows** (per selected
   process; typically 4000–4002, some Luma builds/guides show 4003) — pass it as
   `--port`.
4. **Launch plaza → Music Player → the hijacked track.** Track A ("Main Theme 1")
   is the primary held-note span sweep; Track B (the ex-`EMPTY_LANDSCAPE` entry)
   is the retrigger + front_bypass sweep. Start a stopwatch at the pilot tone.
5. **Snapshot each value.** For each span in the MANIFEST, wait until the
   stopwatch (mod the pass length) lands inside that value's `[t_start,t_end)`
   window, then run `gdb_read.py --label span=<v>`. It attaches (halting the
   ARM11 → the config is frozen at the current span), reads all voices' gains +
   `output_format`, detaches (audio resumes). One shot per value.
6. **Expected result — the binding AND the offset verdict.** Each dump prints
   BOTH candidates. On the stimulus voice, in Surround, the MAIN **rear** lanes
   should be **non-zero and grow with span** (front lanes fall as span→127); at
   span 0 rear≈0/front high, at 127 rear high/front≈0. In Stereo/Mono the rear
   lanes stay ~0 for every span — the in-band cross-check. That binds
   `span (0xD7) →` the MAIN rear gains. **Simultaneously it settles which offset
   is live:**
   - candidate A (f32 @ +0x04) tracks span, candidate B (s16 @ +0x3c) is
     unrelated ⇒ **+0x04 is the live shared-memory layout** (Azahar correct;
     +0x3c is an ARM11-side intermediate the DSP-region tap cannot see).
   - candidate B (s16 @ +0x3c) tracks span (`FL+FR≈32768`), candidate A garbage
     ⇒ **+0x3c is the live shared-memory layout** (Azahar wrong for this firmware).
   - neither tracks span at the DSP-region slot ⇒ the gains are marshalled from
     an ARM11 heap the DSP-region read can't reach (driver-heap GDB read = future
     work). **Then make the one follow-up edit** in
     `tools/dsp-shared/dsp_gain_geometry.h` (set `DSP_SRC_GAIN_SETTLED=1`, point
     `DSP_SRC_GAIN_CANONICAL_*` at the winner, mark the loser DISPROVEN) — all
     three tools pick it up.

### What is uncertain (flag before the session)

- **Does this Luma build's Rosalina GDB stub work / attach to MiiPlaza?**
  Untested here. Luma has a known class of attach failures on some system
  processes (e.g. issue #1370, NWM freezes Rosalina). MiiPlaza is a normal
  application and should attach, but confirm on a throwaway first. If attach
  hangs, power-cycle.
- **The exact port.** Read it from the Rosalina screen; do not assume 4000.
- **Continuous vs latched span** (see §1). If Track A's rear lanes don't move,
  use Track B (retrigger) — that's exactly why both tracks exist.
- **Active-voice index.** Not fixed; identify it from the enable+gain table.
  The plaza's own ambient SFX may also hold enabled voices — the stimulus voice
  is the *sustained* one whose front/rear balance tracks span across snapshots.
- **Halt-on-attach side effects.** Attaching stops the ARM11 → audio glitches
  during the read; harmless for a frozen-config snapshot, and it resumes on
  detach. If the game misbehaves after many attach/detach cycles, relaunch.
- **`gain` value scale.** Decoded as f32 (documented), but the absolute unity
  value the runtime writes for a full-front vs full-rear pan is what the run
  will *measure* — the qualitative front↔rear shift with span is the proof;
  the exact curve is the bonus.

### The 3GX-tap alternative (later, not now)

A headless read via the `tools/dsp-tap` 3GX plugin would avoid the attach-halt
(the Rosalina step is automatable as of 2026-07-18 — see §4 step 3), but it needs
devkitARM + on-console bring-up (see
`tools/dsp-tap/DSP-TAP-DESIGN.md` §6). The GDB stub is the *available-now* path;
the tap is the follow-up once the toolchain is stood up.
