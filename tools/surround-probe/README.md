# surround-probe

A Nintendo 3DS homebrew tool (`.3dsx`) that empirically tests the 3DS's
**Surround** audio output mode, to settle whether the NW4C sequence command
**`span`** (SurroundPan, opcode `0xD7`) is audible on real hardware.

## Why this exists

caesar's dropped-parameter triage concluded that `span` moves a voice on the
front/rear axis of the 3DS DSP's per-source quad gain matrix (`gain[3][4]`),
and is therefore **silent in Stereo/Mono output but audible under the System
Settings *Surround* mode** — see the 2026-07-11 addendum in
[`docs/HISTORY.md`](../../docs/HISTORY.md). That conclusion is *inference-grade*
for the 3DS: Citra/Azahar never implemented surround (their `Surround` case
falls through to `Stereo`), so emulators cannot confirm it. This tool captures
the real console to close that gap. It settles **Part A** (the DSP physics via
`ndspChnSetMix` routing); the register-level **Part B** (dumping live
`gain[3][4]` while a real sequence plays) is intentionally out of scope.

## What it does (v2)

Plays a periodic, band-limited **Schroeder-phase pink multitone** (100 Hz–14 kHz)
at the DSP-native 32728 Hz (no resampler), embedded as `source/probe_buf.h`. The
one-button **AUTO run** (d-pad RIGHT) plays a 10-segment matrix hands-off as one
continuous recording, hard-panning the source to a single quad corner —
front-left (FL) vs back-left (BL) — across Stereo / Surround / Mono, with a depth
positive-control (0x7FFF vs 0xFFFF). Each segment is announced by a countable pip
burst (segment N = N pips). See [`AUTO-RUN.md`](AUTO-RUN.md).

Why v2: v1 used a steady, L/R-symmetric 440 Hz tone and mono-summed analysis, and
came back inconclusive — a centered pure tone is blind to front/back
virtualization (HRTF coloration + crosstalk), and symmetric routing is collapsed
by the naive stereo fold. v2 uses broadband content + a single-corner (asymmetric)
route + strictly per-channel analysis so the effect is resolvable.

The app also shows the System Settings sound mode (cfg block `0x00070001`), the
live headphone-detect state, and the volume slider. Manual controls (A corner, X
mode, Y position, L/R depth) remain for spot checks.

**Hypothesis:** in Stereo, front (FL) and rear (BL) fold identically → no
difference; in Surround the front-left/back-left HRTF reshapes the spectrum and
bleeds crosstalk into R, growing with the depth knob → `span` is audible. The
*relative* Stereo-vs-Surround gap, gated by the depth control, is the proof.

## Layout

| Path | What |
|---|---|
| `source/main.c`        | the homebrew (single file, libctru) |
| `source/probe_buf.h`   | embedded broadband probe table (generated; do not hand-edit) |
| `Makefile`             | devkitARM build (stock 3ds template + SMDH metadata) |
| `AUTO-RUN.md`          | **the recommended one-recording process** (press RIGHT) |
| `tools/gen_probe.py`   | regenerate `source/probe_buf.h` (Schroeder-phase pink probe) |
| `tools/split_run.py`   | cut one AUTO-run recording into the per-condition WAVs + `noise_floor.wav` |
| `tools/analyze_surround.py` | **v2 per-channel verdict** (MAGDEV + crosstalk, depth-gated) |
| `tools/analyze.py`     | v1 pairwise null/difference diagnostic (numpy only) |
| `CAPTURE-PROTOCOL.md`  | manual v1 fallback: recording matrix + predictions |
| `RESULTS-TEMPLATE.md`  | fill-in results sheet |

## Build

Needs devkitPro `3ds-dev` (devkitARM + libctru). Then:

```sh
source /etc/profile.d/devkit-env.sh      # sets DEVKITPRO / DEVKITARM
make                                     # -> surround-probe.3dsx (+ .smdh, .elf)
```

> Note: `apt.devkitpro.org` is behind a Cloudflare User-Agent filter; default
> `curl`/`wget`/`apt` UAs get HTTP 403. If installing the toolchain, give apt a
> browser User-Agent (an `/etc/apt/apt.conf.d` override). pacman's own
> downloader UA is fine.

## Deploy

Copy `surround-probe.3dsx` to the console SD under `/3ds/surround-probe/`, e.g.
over FTP to the console's ftpd:

```sh
curl -T surround-probe.3dsx --ftp-create-dirs \
     ftp://<console-ip>:5000/3ds/surround-probe/surround-probe.3dsx
```

Then launch it from the Homebrew Launcher.

## Run & analyze

**Recommended — one hands-off recording** (full details in
[`AUTO-RUN.md`](AUTO-RUN.md)): set System Settings sound = Surround, start the PC
recording, press **RIGHT**, let the ~105 s / 10-segment matrix play, stop, save as
`run.wav`. Then:

```sh
python3 tools/split_run.py run.wav        # -> the 10 WAVs + noise_floor.wav
python3 tools/analyze_surround.py .        # -> per-channel metrics + VERDICT
```

The verdict is relative (Stereo-vs-Surround) and gated by the depth
positive-control, so a null can be told apart from residual blindness. The whole
probe→split→analyze chain is validated on a synthetic run (it correctly CONFIRMS a
simulated effect and REFUTES a null); only the console capture remains.

**Manual / v1 fallback.** [`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md) +
`tools/analyze.py` drive one condition at a time with the tone probe; kept for
reference but it cannot resolve surround — prefer the AUTO run.

Record the numbers in [`RESULTS-TEMPLATE.md`](RESULTS-TEMPLATE.md).
