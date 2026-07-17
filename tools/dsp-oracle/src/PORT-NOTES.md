# dsp-oracle — port notes (stage-3 commit 1)

Standalone LLE harness that boots the real 3DS DSP1 (Teak) firmware under Teakra
and captures the final stereo mix. This file records the protocol facts the port
depends on, with line-level citations into the reference sources. Commit 1 boots
to the audio callback (§1-§5); commit 2 renders a dry click and answers route-a
(§6); commit 3 engages reverb (§7).

**Sources cited** (all read at the pinned versions in this session):
- `lle.cpp` = Azahar `src/audio_core/lle/lle.cpp` — the transport (load, boot
  handshake, pipe read/write, AHBM, audio callback). GPL.
- `dsp_dsp.cpp` = Azahar `src/core/hle/service/dsp/dsp_dsp.cpp` — the ARM11 DSP
  service (per-frame interrupt semantics, address conversion). GPL.
- `hle.cpp` / `shared_memory.h` = Azahar `src/audio_core/hle/*` — the region
  table order and struct layout. GPL.
- `ndsp.c` = libctru `libctru/source/ndsp/ndsp.c` — the real-hardware init and
  per-frame duty the emulated game performs. (This is the ARM11 side that
  lle.cpp does NOT contain.)
- teakra `src/apbp.cpp`, `src/btdmp.cpp`, `src/teakra.cpp` — the emulator's
  mailbox / I2S-transmit / API mapping. MIT.

The oracle code is ORIGINAL caesar (GPLv3) informed by, not copied from, the GPL
sources; teakra is vendored MIT.

---

## 1. Boot sequence (verified, with citations)

DSP1 header parse — `dsp1.cpp Dsp1::Parse`, layout from `lle.cpp:26-65`
(`static_assert(sizeof(Header)==0x300)`), field offsets cross-checked against the
recon extractor:
- magic `"DSP1"` @0x100; `binary_size` u32 @0x104; `memory_layout` u16 @0x108;
  `num_segments` u8 @0x10E; flags u8 @0x10F (bit0 = `recv_data_on_start`,
  `lle.cpp:38-41`); `segments[10]` @0x120, stride 0x30, each: `offset` u32 @+0,
  `address`(word) u32 @+4, `size` u32 @+8, `memory_type` u8 @+0x0F, sha256 @+0x10.
- Observed for MiiPlaza: 5 segments (2 PROG type0, 3 DATA type2),
  `memory_layout=0xDF03`, `recv_data_on_start=1`. Matches the recon exactly.

Load — `dsp1.cpp BootFirmware`, mirrors `lle.cpp:314-327`:
- `teakra.Reset()`, then per segment: PROG (`ProgramA`/`ProgramB`) → `mem +
  target*2`; DATA → `mem + 0x40000 + target*2`. The `target` is a **word**
  address; `*2` converts to bytes. `0x40000` is `Impl::DspDataOffset`
  (`lle.cpp:151`).

Handshake — mirrors `lle.cpp:338-350`:
1. If `recv_data_on_start`: for channel `i` in {0,1,2}, spin `RunTeakraSlice()`
   until `RecvDataIsReady(i)`, then loop until `RecvData(i)==1`
   (`lle.cpp:338-345`; the firmware may post a non-1 first, hence the inner
   `do/while`). **Observed: ch0=1 ch1=1 ch2=1.**
2. Then read `pipe_base_waddr = RecvData(2)` (`lle.cpp:348-350`). **Observed:
   `pipe_base_waddr = 0x0C9E`** (a word address into DSP data space).

`RunTeakraSlice()` here is a direct `teakra.Run(TeakraSlice)` with
`TeakraSlice = 16384` (`lle.cpp:152`). Single-threaded — see §5.

## 2. Audio-pipe init + region-address table (the pipe-2 table)

lle.cpp is only the transport; the Initialize handshake lives in the emulated
game (`ndsp.c ndspInitialize`, resume=false, lines 233-275). Ported into
`main.cpp`:
1. `WritePipe(2, {0,0,0,0})` — the 4-byte Initialize message. `ndsp.c:254-255`
   writes a u16 `val=0` with length 4; `dsp_dsp.cpp:85-90` (WriteProcessPipe)
   force-zeros bytes [2],[3] of the Audio pipe, so the payload is four zero
   bytes. `WritePipe` internally notifies the DSP via `SendData(2, slot)`
   (`lle.cpp:254-259`), slot = `(2<<1)|1 = 5` (CPUtoDSP).
2. `SetSemaphore(0x4000)` — `ndsp.c:257`. Kicks the DSP (see §4).
3. Run slices, draining `RecvData(2)`, until pipe 2 (DSPtoCPU) is readable;
   read one u16 `count` then `count` u16 word addresses
   (`ndsp.c:260-263`; pipe read = `lle.cpp:262-306`).
4. `SetSemaphore(0x4000)` again (`ndsp.c:270`).

**Observed pipe-2 table (15 words), byte-for-byte the HLE exemplar** in
`shared_memory.h:72-87` and `hle.cpp:356-385`:

| idx | word   | region                         |
|-----|--------|--------------------------------|
| 0   | 0xBFFF | frame_counter                  |
| 1   | 0x9E92 | source_config[24] (192B each)  |
| 2   | 0x8680 | source_status[24]              |
| 3   | 0xA792 | adpcm_coefficients             |
| 4   | 0x9430 | dsp_configuration (196B)       |
| 5   | 0x8400 | dsp_status                     |
| 6   | 0x8540 | final_mix_samples (s16[160][2])|
| 7   | 0x9492 | intermediate_mix_samples       |
| 8   | 0x8710 | compressor                     |
| 9   | 0x8410 | dsp_debug                      |
| 10–14 | 0xA912/0xAA12/0xAAD2/0xAC52/0xAC5C | surround-related |

The **order** (pipe index) is fixed across the real firmware and Citra; only the
word addresses can vary between firmware builds — which is why the harness reads
them at runtime. On MiiPlaza they happen to equal the exemplar exactly.

First-frame kick (`ndsp.c:271-275`): set region-0 frame counter (word
`table[0]=0xBFFF`) to 4, advance `frame_id`, then `SetSemaphore(0x2000)`.

## 3. THE frame-flow answer (the single most valuable protocol fact)

**Question:** once booted, what must the ARM11 do per frame for audio to keep
flowing?

**Measured** (MiiPlaza firmware, three `--service` modes, 300 frames each; the
harness counts BTDMP transmit underruns via teakra's `btdmp.cpp:31` print):

| --service | per-frame ARM11 action                    | sample_pairs | DAC underruns | frame_events |
|-----------|-------------------------------------------|-------------:|--------------:|-------------:|
| none      | nothing                                   | 48000        | **95976 / 96000** | 0 |
| drain     | drain `RecvData(2)` each slice            | 48000        | **0**         | 4 |
| full      | drain + bump frame counter + `SetSemaphore(0x2000)` | 48000 | **0** | 303 |

**Answer, in two layers:**

1. **The audio callback fires unconditionally in every mode.** Teakra's BTDMP
   (I2S transmit) free-runs from boot: once the firmware enables transmit,
   `btdmp.cpp:23-47` pushes one stereo sample to the DAC every `transmit_period`
   cycles regardless of the ARM11. Measured `transmit_period = 4096.00`
   cycles/sample exactly → **32728.3 Hz** (32728·4096 = 134.06 MHz = half the
   268 MHz ARM11 clock, matching `lle.cpp:185`). So "callback produces frames" is
   never in doubt, and **at idle every sample is 0 in all modes** — you cannot
   distinguish firmware-silence from underrun-silence by the samples alone.

2. **For the firmware to actually run its mix loop (keep the transmit FIFO fed),
   the ARM11 must drain the DSP→ARM channel-2 mailbox every time the DSP posts to
   it (`RecvData(2)`).** With `none`, the mailbox stays full after the firmware's
   first `SendData(2)`; the firmware blocks on its next notify and stops mixing,
   so the FIFO underruns on 95976 of 96000 channel-samples — the DAC is emitting
   *hardware* underrun-silence, not firmware output. Draining `RecvData(2)`
   (drain/full) drops underruns to **0**: the firmware's loop stays live.

The faithful **full** duty additionally bumps the write-bank frame counter and
`SetSemaphore(0x2000)` per frame (`ndsp.c ndspThreadMain:445-446`). At idle this
is not required to avoid underrun (drain alone suffices), but it is what advances
the double-buffer so the DSP re-reads config — **mandatory for commit-2/3** when
we feed a changing SourceConfiguration/DspConfiguration. The deliverable capture
uses full. Net per-frame contract for a live mix:

> Each frame, ack the DSP's channel-2 pipe interrupt by reading `RecvData(2)`,
> write next-frame config into the write bank, bump that bank's frame counter,
> and `SetSemaphore(0x2000)`. Skipping the `RecvData(2)` drain stalls the
> firmware and underruns the DAC; skipping the counter/semaphore leaves the DSP
> reading stale config.

Deliverable: 5.00 s idle capture, **163520 sample-pairs, 4096.00 cyc/sample,
32728.3 Hz, 0 nonzero, 0 underruns, 1025 frame events, no stall** → `dsp_oracle`
exit 0.

## 4. Word/byte addressing and the semaphore, verified

- **DSP data space starts at byte 0x40000** in `GetDspMemory()` (`lle.cpp:151`,
  `GetDspDataPointer` 193-196). A data **word** `W` lives at byte `0x40000 +
  W*2`. `dsp_dsp.cpp:69 ConvertProcessAddressFromDspDram` computes `(W<<1) +
  (DSP_RAM_VADDR + 0x40000)` — the same `W*2 + 0x40000`, confirming both the
  shift and the offset. `shared_mem.h DataWordToByte` encodes this.
- **`pipe_base_waddr` is a word address**; PipeStatus for slot `s` is at byte
  `0x40000 + pipe_base_waddr*2 + s*sizeof(PipeStatus)` (`lle.cpp:207`,
  `sizeof(PipeStatus)==10`). Slot = `(pipe<<1)|dir`, dir 0=DSP→CPU, 1=CPU→DSP
  (`lle.cpp:119-121`). **Verified at runtime:** `GetStatus` asserts the record's
  embedded `slot_index` equals the expected slot, and it matched for pipe 2 both
  directions — proving the `*2` scaling and `0x40000` base are correct against
  live firmware (`pipe_base_waddr=0x0C9E`).
- **Frame counter** word `0xBFFF` → byte `0x40000 + 0xBFFF*2 = 0x57FFE`, the last
  word of region 0 (`0x50000..0x58000`) — matches `shared_memory.h:87` "Frame
  Counter" being the final u16 of the bank. The counter is a **plain** u16
  (little-endian), written directly (`ndsp.c ndspSetCounter:58-61`), not the
  middle-endian `u32_dsp`.
- **Semaphore direction** (`teakra.cpp:114-128`, `apbp.cpp:109-133`):
  `Teakra::SetSemaphore` sets the **ARM→DSP** semaphore (`apbp_from_cpu`); with
  mask 0 (default, never changed) any bit signals the DSP via `icu 0xE`. So
  `SetSemaphore(0x4000)` and `(0x2000)` are the ARM kicking the DSP.
  `GetSemaphore`/`MaskSemaphore` act on the **DSP→ARM** side. We do **not** call
  `MaskSemaphore`: `ndsp.c`'s `DSP_SetSemaphoreMask(0x2000)` sets the HLE
  *preset* value that `SetSemaphore` later sends (`dsp_dsp.cpp:278-286, 394-395`),
  not teakra's hardware mask, and `lle.cpp` never masks either.

## 5. Deviations from lle.cpp (and why)

1. **Single-threaded, synchronous.** No `teakra_thread`, no `Core::Timing`
   events (`lle.cpp:154-191, 331`). The harness calls `teakra.Run(16384)`
   directly in a loop. Deterministic, no Citra core needed.
2. **Teakra owns its DSP memory.** `UserConfig{}` (nullptr) → teakra allocates
   0x80000; lle passes `memory.GetDspMemory(0)` (`lle.cpp:125`). No Citra
   `MemorySystem`.
3. **AHBM is a 16 MiB zero-filled guard buffer** masked to FCRAM base
   `0x20000000`, instead of lle's real FCRAM map (`lle.cpp:471-494`). **Verified
   0 AHBM accesses at idle**, so this stub is never touched now; commit-2 turns
   it into the real sample source.
4. **Polling instead of interrupt handlers.** lle drives pipes from
   `SetRecvDataHandler`/`SetSemaphoreHandler` + `ProcessPipeEvent`'s
   `semaphore_signaled && data_signaled` gate (`lle.cpp:408-455`), because the
   emulated game runs concurrently. We ARE the game and step synchronously, so
   we poll `RecvDataIsReady(2)` after each slice and drain. The double-signal
   gate is a Citra event-ordering concern that does not apply here.
5. **The init + per-frame protocol is added, not in lle.cpp.** lle.cpp has no
   Initialize message, no region-table read, no frame-counter bump — those live
   in libctru `ndsp.c`. Ported into `main.cpp` (§2, §3).
6. **No UnloadComponent finalize** (the `0x8000` on channel 2, `lle.cpp:364-374`).
   The oracle just exits; add it if a clean DSP teardown is ever needed.

## 6. Commit 2 — dry source at the final mix (DONE) + THE route-a answer

`--click` injects one synthetic voice and captures it at the final mix. The
per-voice `SourceConfiguration::Configuration` (192 B, Azahar
`shared_memory.h:117-309`) and a minimal `DspConfiguration` (196 B,
`shared_memory.h:326-432`) are built byte-for-byte in `shared_mem.h`
(`BuildDrySourceConfig` / `BuildDryDspConfig`) — every field offset, dirty bit
and encoding is cited inline there. What the source does:

- **Embedded buffer, PCM16 mono, no SRC.** `enable=1`,
  `gain[0][0]=gain[0][1]=1.0` (front L/R of the MAIN mixer; `gain[1]`/`gain[2]`
  are the aux/effect buses, left at 0 so nothing routes to reverb),
  `rate_multiplier=1.0`, `interpolation_mode=None`, `format=PCM16`, `mono`.
  The pulse (`--click-len` samples of `--click-amp`) is placed in the AHBM
  backing at FCRAM offset 0x8000; `physical_address = 0x20000000 + 0x8000`. The
  DSP fetches it over AHBM — confirmed: `ahbm_reads` goes 0 → 32 for a 64-sample
  buffer (the sample DMA, `dma.cpp:53/90` → `ahbm.Read16/32` → the oracle's
  `read_external*`).
- **Middle-endian trap handled:** `physical_address`, `length`, `play_position`
  are written via `PutU32Dsp` (halves swapped, `shared_memory.h:46-67`); gains,
  floats and the s16 final-mix stay plain little-endian.
- **Frame-parity discipline:** the config is written into the SAME bank whose
  counter is about to be bumped (`buffer_cur_id`; region-0 word or `| 0x10000`
  for region 1), on ONE trigger frame with the dirty bits set. The DSP reads
  that bank that frame and clears the dirty mask; no re-assert afterward → a
  single clean click, not one per frame.

**Measured (MiiPlaza firmware, `--click`, unity gain):** input amp A → DAC peak
`A−1` for A ∈ {2000, 8192, 16000, 32000} — a consistent −1-LSB fixed-point
truncation in the mixer; linear and unity otherwise. A 64-sample click yields
exactly 64 non-zero, contiguous output pairs, `L==R`, silence everywhere else;
0 underruns; exit 0. Onset latency trigger→DAC ≈ **326 samples (~10 ms)** — the
DSP-mix + BTDMP-FIFO pipeline depth, useful for commit-3/4 IR alignment.

**Settles the commit-1 open question:** the counter-bump + `SetSemaphore(0x2000)`
"full" duty DOES make the DSP consume *new* SourceConfiguration — the click only
renders because config lands in the freshly-counted bank. Hypothesis confirmed.

### THE route-a answer — **YES.**

**Question:** besides feeding the DAC (BTDMP), does the firmware ALSO write the
final mix back into ARM11-visible shared memory (Azahar HLE `final_samples`,
region-table[6])?

**Method:** each DSP frame, snapshot BOTH banks of the final_samples region via
`GetDspMemory()` (region-0 word 0x8540 → byte 0x50A80; region-1 → 0x70A80;
`FinalMixSamples = s16 pcm16[160][2]`, 640 B, plain LE — `shared_memory.h:448-453`)
and compare its peak to the DAC peak.

**Result: YES.** The region carries the click at the SAME amplitude as the DAC at
every tested level (`final_region_peak == btdmp_peak`: 1999/1999, 8191/8191,
15999/15999, 31999/31999), and is silent in every frame except the ~2 the click
spans (2 of 203); at idle it stays all-zero. The DSP writes the mix into the bank
it is NOT currently reading (observed bank 0 — the double-buffer "results go into
the *other* bank" rule), so both banks must be scanned. This confirms the Azahar
comment on the region ("Final mixed output … what you hear out of the speakers",
`shared_memory.h:448-449`) **behaviourally, on the real firmware** — not assumed
from HLE.

**Consequence:** route a is LIVE for the analog-free capture program. A Luma 3GX
plugin can read `final_samples` from the just-retired bank each ~4.9 ms frame for
a bit-perfect digital capture of console output — no line-in, no resampling.
Commit 3/4 (reverb) will read the SAME region, so the wet tail is capturable the
same way.

## 7. Road to commit 3 (engage reverb)

- Write `DspConfiguration` (196 B, `shared_memory.h:326`) at `table[4]` (word
  0x9430): `master_volume`, `aux_return_volume[2]`, `aux_bus_enable[2]` (route
  the source to the aux/reverb bus), and the **`reverb_effect[2]`** block — 52 B
  = 26 opaque DSP words (`shared_memory.h:414-416`, an ~8-year Citra stub, which
  is exactly why LLE is the only path to the reverb). Set the dirty bits
  (`reverb_effect_0_dirty` b12 / `_1_dirty` b13, `aux_bus_enable_*_dirty`,
  `master_volume_dirty`).
- **De-risk first:** replay the 26 reverb words statically recovered from Mii
  Plaza's ARM11 `code.bin` (the recon's de-risk spike) to get a *known-good
  engaged* config; confirm a decaying wet tail appears from the audio callback
  before sweeping the 26 words. A malformed 52-byte block is silently bypassed →
  dry (SUITE-DESIGN risk #2).
- **Honesty check (SUITE-DESIGN #4):** validate the captured IR against a New 3DS
  192 kHz line-in capture — teakra could faithfully model a teakra bug.

## 8. Unresolved / to watch

- drain(4) vs full(303) `frame_events`: draining alone avoids underrun but yields
  far fewer DSP→ARM signals. **Answered by commit-2:** the "full" counter-bump +
  `SetSemaphore(0x2000)` is what makes the DSP consume *new* config (the click
  renders only under full). Whether drain-alone could also latch a one-shot
  config change was not separately tested — full is the deliverable path.
- 1025 frame events over ~1022 frames (slight excess) — likely occasional
  double-signals; harmless, note if it grows.
- teakra prints BTDMP-underrun / unhandled-MMIO lines to stdout (32 MMIO lines at
  init, 0 underruns in full mode). Cosmetic; the vendored core is not ours to
  quiet.
