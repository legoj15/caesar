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

## What it does

Plays a steady **440 Hz mono sine at the DSP-native 32728 Hz** (no resampler in
the path) on one NDSP channel, and lets you flip, live:

- **Output mode** (`ndspSetOutputMode`): Stereo / Surround / Mono
- **Routing**: front-only (`mix[0]=mix[1]=1`) vs rear-only (`mix[2]=mix[3]=1`),
  or a continuous front↔rear blend
- **Surround params**: speaker position (SQUARE/WIDE), rear ratio

On every setting change it drops a **~200 ms silence marker** so separate PC
recordings of the headphone jack can be sample-aligned. It also displays the
System Settings sound mode (cfg block `0x00070001`), the live headphone-detect
state (`DSP_GetHeadphoneStatus`), and the volume-slider level — so you can see
the OS setting vs the mode the app forces.

**Hypothesis:** in Stereo, front-only and rear-only null deeply (rear folds into
front at unity → `span` inaudible); in Surround they do not (the firmware
virtualizes them → `span` audible). The *relative* gap between the two null
depths is the proof.

## Layout

| Path | What |
|---|---|
| `source/main.c`        | the homebrew (single file, libctru) |
| `Makefile`             | devkitARM build (stock 3ds template + SMDH metadata) |
| `tools/analyze.py`     | host-side null-test / difference analyzer (numpy only) |
| `CAPTURE-PROTOCOL.md`  | step-by-step recording matrix + predictions |
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

Follow [`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md): record the four core
captures (Stereo/Surround × Front/Rear) at 48 kHz, then:

```sh
python3 tools/analyze.py stereo_front.wav   stereo_rear.wav   --expect null
python3 tools/analyze.py surround_front.wav surround_rear.wav --expect differ
python3 tools/analyze.py stereo_front.wav   surround_front.wav
```

Record the numbers in [`RESULTS-TEMPLATE.md`](RESULTS-TEMPLATE.md).
