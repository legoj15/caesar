# dsp-tap — Luma 3GX DSP capture plugin

The **console-capture side** of the analog-free capture program (route-b). A
headless, per-title **3GX game plugin** that runs inside StreetPass Mii Plaza,
reads the live DSP audio shared memory each ~4.889 ms frame, and streams a
tear-free per-voice `SourceConfiguration` + `DspConfiguration` snapshot to an SD
ring/file. The PC-side replay reader feeds that dump to `dsp_oracle` and diffs it
against caesar's stage-2 player — isolating "is the ARM11 runtime model right?"
from "is the DSP model right?" at exactly the stage-2/3 boundary.

> **Status: builds and packs (2026-09-09, devkitARM on the `debian-on-s1` VM);
> not yet run on a console.** The first build corrected five scaffold
> assumptions against the real loader/3gxtool (entry semantics, plgInfo keys,
> low-u32 targets, no-crt0 constraints, srv-before-fs) — all recorded in
> DSP-TAP-DESIGN.md §2/§4 and HISTORY. Everything only an on-console run can
> confirm stays marked `VERIFY` in code (see DSP-TAP-DESIGN.md §6).

## Why barebones `.3gx`, not CTRPluginFramework

caesar is **GPLv3**. CTRPluginFramework is **not GPL** — it uses a bespoke,
non-OSI source-availability license that would form a non-GPL licensing island
inside this repo (a GPLv3 "further restriction" problem), and it is pure overkill
(cheat menu / OSD / input) for a headless memory tap. A barebones `.3gx` links
only libctru (permissive, GPL-compatible), keeps the whole repo GPLv3, and is a
few hundred lines. Verdict + reasoning: DSP-TAP-DESIGN.md §1.

## Layout

```
include/dsptap_format.h   the on-disk dump contract (SHARED verbatim with the
                          PC replay reader — the single source of truth)
include/dsp_regions.h     DSP shared-mem region map (MiiPlaza word addresses)
src/main.c                entrypoint (_start), PluginHeader, sampler loop
src/tap.c/.h              frame-parity bank pick + seqlock snapshot
src/ring.c/.h             SPSC ring + SD writer thread (never blocks audio)
3gx.ld                    barebones linker script (loader ABI)
dsp-tap.plgInfo           3gxtool metadata (author/title/targets)
Makefile                  OPTIONAL devkitARM build; skips cleanly if absent
```

Nothing here is referenced by caesar's root CMake build or CI — like
`tools/dsp-oracle` keeps teakra out of `caesar_core`, dsp-tap keeps devkitARM out
of the core.

## Build (devkitARM machine only)

```sh
make               # no devkitARM -> prints how to get it and exits 0
make FINAL_SAMPLES=1   # also capture the final mix (route-a confirmed YES; default
                       # off so the first on-console run can A/B the tap's overhead)
```

Needs devkitPro `3ds-dev` (devkitARM + libctru) and **3gxtool**
([ThePixellizerOSS/3gxtool](https://gitlab.com/thepixellizeross/3gxtool) — C++,
CMake, build-tool only; devkitPro's pacman does not ship it). On the dev VM
(`debian-on-s1`) 3gxtool is already built at `~/caesar-build/3gxtool/build/`
(`git clone --recursive` + `cmake .. && make`). Add it to PATH, then:
Output: `build/dsp-tap.3gx` → install to
`sd:/luma/plugins/0004001000021800/`, enable Plugin Loader in Rosalina.

## Route-a dependency

`final_samples` capture is gated on the `dsp_oracle` commit-2 route-a spike
(does the firmware write the final mix back to ARM11-visible memory?). Default
**OFF** — the tap captures per-voice + `DspConfiguration` unconditionally; the
final mix is one compile flag away once route-a confirms it. See DSP-TAP-DESIGN.md
§5.

## Per-title note

The plugin is inherently per-title (loader injects it only from the Title-ID
folder). **MiiPlaza (`0004001000021800`) hosts both the capture cartridge and
this tap.**
