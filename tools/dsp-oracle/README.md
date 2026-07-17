# dsp-oracle

The DSP oracle is an **offline measuring instrument**. It boots the 3DS's real DSP1
(Teak) firmware under the vendored [Teakra](external/VENDOR.md) interpreter, drives
the LLE boot/pipe protocol from the ARM11 side, and captures the final stereo mix
from the audio callback — so caesar can recover the console's reverb (and, later,
drive the analog-free capture program) **behaviourally**: impulse → fit, never by
disassembling the firmware's reverb code.

It deliberately stays **out of `caesar_core` and out of CI**. Teakra and the firmware
never ship. Only a handful of fitted coefficients plus a golden impulse-response wav
ever make it into the player.

Protocol design notes, with line-level citations into the reference sources
(Citra/Azahar `lle.cpp`/`dsp_dsp.cpp`, libctru `ndsp.c`, teakra internals), live in
[src/PORT-NOTES.md](src/PORT-NOTES.md) — including the measured frame-flow contract:
the audio callback free-runs at exactly 4096 cycles/sample (32728 Hz), but the ARM11
must drain the DSP→ARM channel-2 mailbox every frame or the firmware's mix loop
stalls (and, when config changes, must bump the write-bank frame counter and
`SetSemaphore(0x2000)` so the DSP consumes it).

## Build

Standalone CMake tree — never invoked by the repo-root build.

```sh
cmake -S tools/dsp-oracle -B tools/dsp-oracle/build -G "Visual Studio 17 2022" -A x64
cmake --build tools/dsp-oracle/build --config Release
```

Targets:
- **`dsp_oracle`** — the oracle proper (`build/Release/dsp_oracle.exe`).
- **`dsp_oracle_smoke`** — liveness check: instantiates Teakra, steps the core,
  prints `OK`. Useful when re-vendoring.
- **`dsp_oracle_config_smoke`** — firmware-free correctness check for the
  commit-2 SourceConfiguration/DspConfiguration byte builders (struct offsets +
  u32_dsp middle-endian). Prints `OK`. No firmware needed.

## Extract firmware

```sh
python extract_dspfirm.py                 # defaults: dumps-root + firmware/ out dir
python extract_dspfirm.py --dumps-root DIR --out DIR
```

Slices each title's DSP1 image out of its `exefs/code.bin`, **verifies every
segment's embedded SHA-256**, and writes `firmware/<Title>_dspfirm.cdc`. The oracle
image is `MiiPlaza_dspfirm.cdc` (sha256 begins `944b40b5`) — the smallest
reverb-bearing firmware, whose title's archives are the project's own reverb repros.

> **The extracted firmware is Nintendo copyright.** `firmware/` is gitignored and must
> **never** be committed. Regenerate it locally from dumps you own.

## Run

```sh
dsp_oracle firmware/MiiPlaza_dspfirm.cdc [--seconds N | --frames N] [--out out.wav]
           [--service none|drain|full] [-v]
           [--click [--click-amp N] [--click-len N] [--click-frame N]
                    [--region-wav out.wav]]
```

Boots the firmware (`recv_data_on_start` handshake on channels 0/1/2, then
`pipe_base_waddr` from channel 2), sends the Audio-pipe Initialize message, prints
the 15-word shared-region address table the firmware replies with, then runs the
requested duration servicing the DSP per frame and writes the captured mix as a
16-bit stereo 32728 Hz WAV. `--service` selects the per-frame ARM11 duty (default
`full`; `none`/`drain` exist to reproduce the frame-flow experiment in PORT-NOTES).
Exit codes: 0 ok, 2 usage, 3 file read, 4 parse, 5 boot/handshake, 6 pipe/init,
7 run, 8 wav write.

Healthy idle boot (commit-1 baseline): `cycles/sample : 4096.00`, `implied_rate_hz :
32728.3`, `nonzero_samples : 0`, `stalled_out : no`, `audio_callback_fired : YES`,
exit 0.

`--click` (commit 2) injects one dry PCM16-mono source: a `--click-len`-sample
pulse of `--click-amp` (default 64 samples @ 8192) placed in AHBM-backed FCRAM,
enabled at frame `--click-frame` (default 8), routed unity-gain to the main mix
with no SRC and effects off. It renders at the final mix (a click at unity gain,
silence elsewhere) and reports the **route-a** verdict: whether the firmware also
writes the final mix back into the ARM11-visible `final_samples` region (word
0x8540). Verdict on MiiPlaza: **YES** — the region peak equals the DAC peak at
every amplitude. `--region-wav` additionally dumps that region readback as its
own WAV. See [src/PORT-NOTES.md §6](src/PORT-NOTES.md).

## Vendored Teakra

See [external/VENDOR.md](external/VENDOR.md) — upstream URL, pinned commit, MIT
license, exclusions, and the re-vendor procedure.
