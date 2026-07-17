# DSP tap — design & license verdict

The console-capture side of the **analog-free capture program** (ROADMAP →
"Analog-free capture program", sub-points 3–4). A small **Luma 3GX game plugin**
runs inside StreetPass Mii Plaza, copies the live DSP audio shared-memory frame
state to an SD-card ring/file every ~4.889 ms audio frame, and the PC-side
**replay reader** feeds that dump to `dsp_oracle` (route-b) and diffs it against
caesar's stage-2 player output.

This document is the research deliverable: the license verdict, the plugin's
concrete mechanism, the on-disk dump contract, the build story, and exactly what
could not be verified without a devkitARM toolchain or a live console.

> **Status: SCAFFOLD + design.** The sources under `src/` are complete and
> self-consistent but were **not compiled or run** — no devkitARM toolchain and
> no console were available in the authoring session. Every place that needs a
> device or the real toolchain to confirm is marked `VERIFY` in the code.

---

## 1. License verdict — barebones `.3gx`, not CTRPluginFramework

**caesar is GPLv3** (repo-root `LICENSE`, verified — full GNU GPL v3 text).

The ROADMAP flagged this as "check CTRPluginFramework's GPL license vs a
barebones 3gx." That premise turns out to be **factually off in a way that
matters**: **CTRPluginFramework is _not_ GPL.** It ships under a bespoke
source-availability license (thepixellizeross / Nanquitas), whose three
conditions are, in substance:

1. binary-form access to any derived work must not be gated behind a fee;
2. you are exempt from (1) if you provide the derived work's source alongside
   the binary;
3. any copy/modification/distribution of the source must retain that same
   license text.

That is a weak-copyleft, non-OSI, non-standard license. For a GPLv3 project this
is **worse** than a clean permissive dependency, not better:

- GPLv3 §7 forbids adding "further restrictions." A bespoke license with its own
  retention/disclosure terms is a further restriction, so CTRPF-covered source
  is **not relicensable under GPLv3** and cannot be melded into the repo's GPLv3
  license. A CTRPF-based plugin's own source would have to **carry CTRPF's
  license** (condition 3), creating a non-GPL licensing island inside a GPL repo.
- It is not the clean, GPL-compatible quarantine the repo already uses for
  `tools/dsp-oracle/external/teakra` (MIT). Vendoring CTRPF would import a large
  framework under a non-standard license plus its transitive bundled deps.

Two things that are **not** actually problems, to be precise:

- There is **no GPL "combined work" between caesar and the plugin** either way.
  The plugin is a separate ARM binary that links libctru (and, in the rejected
  option, CTRPF) — it never links `caesar_core`. caesar-the-converter and the
  plugin are two programs that never share an address space at build time.
- The **`3gxtool`** packer (and `arm-none-eabi-gcc`) are **build tools**, not
  linked into the plugin. GPL §1 explicitly excludes build tools from
  Corresponding Source, and a tool's license never propagates to its output. So
  `3gxtool`'s own license is irrelevant to the plugin's license (worth a glance
  anyway; it is permissive in practice).

### Recommendation: **build a barebones `.3gx`.** Reasons, in priority order:

1. **License cleanliness.** The whole repo stays GPLv3. The plugin source is
   plain GPLv3 C linking only libctru (permissive, GPL-compatible). No
   non-GPL/non-OSI island, no framework vendoring — it mirrors the discipline the
   repo already applies to teakra.
2. **CTRPF is massive overkill.** Its value is a cheat engine, an on-screen menu,
   input hooks, an OSD, Action-Replay — a headless memory tap uses **none** of
   it. The tap's whole job is: locate DSP RAM, poll a counter, `memcpy` ~5 KB to
   a ring. Pulling in a UI framework to do that is all cost, no benefit.
3. **Smaller, self-contained footprint.** A few hundred lines + one linker script
   + one `.plgInfo`, versus vendoring a framework and its bundled libc/ctrulib.

**Honest cost of the barebones path:** you write the 3GX plumbing yourself — the
linker script (`3gx.ld`), a from-scratch `_start`, your own service init, and
getting `3gxtool` packing right. CTRPF would have handed you that boilerplate.
But it is a one-time cost on a genuinely tiny plugin, and it is exactly the
boilerplate that is well-documented by the loader ABI (below). The first build on
a real devkitARM machine is where this bites; after that it is inert.

The `.3gx` format and loader contract are public and stable (Luma3DS has shipped
the Rosalina plugin loader since ~v10.3), so a barebones plugin is a supported,
first-class citizen of the loader — not a hack around it.

---

## 2. The 3GX loader ABI (what a barebones plugin must satisfy)

Verified from the Luma3DS/Rosalina plugin-loader headers
(`sysmodules/rosalina/include/plugin/3gx.h`, `plgldr.h`) and PabloMK7's public
`3gx.ld`:

- **`.3gx` file** = `_3gx_Header` (magic `0x3230303024584733` = `"3GX$0002"`),
  then `_3gx_Infos` (author/title/summary/description + a flags word:
  `memoryRegionSize`, `eventsSelfManaged`, `swapNotNeeded`, …), `_3gx_Executable`
  (`codeOffset/rodataOffset/dataOffset` + sizes + `bssSize`), `_3gx_Targets`
  (allowed title-ID list), and `_3gx_Symtable`. `3gxtool` builds this from our
  ELF + `.plgInfo`.
- **Install path**: `sd:/luma/plugins/<TitleID>/<name>.3gx`. The loader injects it
  only for that title — the plugin is **inherently per-title**. MiiPlaza
  (`0004001000021800`) hosts **both** the capture cartridge and this tap.
- **Address-space model (the key fact):** the loader maps the plugin **into the
  game's own process** and runs it on a thread it creates. The plugin therefore
  **shares the game's virtual address space** — it does not "open a handle to the
  game process," it is *already inside it*. The 0x100-byte **`PluginHeader`** is
  mapped at **`0x07000000`**; plugin code begins at **`0x07000100`**; ENTRY is
  **`_start`** at the top of `.text`. `PluginHeader` carries `heapVA/heapSize`,
  `plgldrEvent/plgldrReply` (to talk to `plg:ldr`), and a `config[32]` array we
  can drive from the `.plgInfo` / at runtime.
- **PHDRS**: `code` RX, `rodata` R, `data` RW — `3gx.ld` reproduces this.

`src/main.c` reads `PluginHeader` at `0x07000000`, checks its magic, then runs.

---

## 3. Mechanism, concretely

### (a) Mapping / reading the DSP shared memory

Because the plugin runs in MiiPlaza's process, and MiiPlaza has DSP access in its
exheader (it uses NW4C/ndsp), the **512 KB DSP RAM is already mapped at the fixed
`0x1FF00000`** in that process. The plugin reads it **directly** — the simplest,
safest mechanism:

- DSP **data** space begins at `0x1FF40000` (`= 0x1FF00000 + 0x40000`). A DSP data
  **word** address `W` (words are 16-bit) → ARM11 vaddr `0x1FF40000 + W*2`. This
  is exactly libctru's `DSP_ConvertProcessAddressFromDspDram` result
  (`(W<<1) + (DSP_RAM_VADDR + 0x40000)`), and it matches the oracle's
  `kDspDataOffset = 0x40000`. (`include/dsp_regions.h`.)
- **No `dsp::DSP` session, no `dspInit`.** `dspInit` *"unloads any previously
  loaded DSP binary"* — calling it would kill the running firmware and the game's
  audio. We never call it. (The libctru IPC `DSP_ConvertProcessAddressFromDspDram`
  is the "robust" alternative to hardcoding `0x1FF40000`, but it needs a dsp
  session; direct reads at the fixed mapping avoid all session risk, which is why
  they are the primary path.)
- **Region word addresses are firmware-specific but known for MiiPlaza.** The
  pipe-2 region table's *order* is fixed across all audio firmware; only the
  concrete word addresses vary between firmware builds. MiiPlaza's are the oracle
  exemplar values (`frame_counter 0xBFFF`, `source_config 0x9E92`,
  `dsp_config 0x9430`, `source_status 0x8680`, `final_mix 0x8540`), and MiiPlaza's
  build is byte-identical to launch (no newer build exists — NW4C handoff), so
  hardcoding them for this per-title plugin is safe. `tap_probe()` still
  sanity-checks by requiring the frame counter to advance before any file is
  created.

### (b) Frame-parity bank selection (the tearing trap)

The application-visible region is **double-banked**: region 0 (word base
`0x8000`, vaddr `0x1FF50000`) and region 1 (word base `0x18000`, vaddr
`0x1FF70000`); a region-1 word is the region-0 word `| 0x10000`. Each frame the
game writes next-frame config into the bank it is about to make current, **then
bumps that bank's frame counter**; the DSP reads whichever bank has the **higher
counter** (config finished, stable) and writes its outputs into the **other**
bank. So:

- **SourceConfiguration + DspConfiguration** (application-written inputs) are read
  from the **current (higher-counter)** bank — what the DSP is acting on now.
- **SourceStatus + final_mix** (DSP-written outputs) are read from the **other
  (lower-counter)** bank — the DSP's most recent results.

`tap_snapshot()` picks the current bank by comparing the two 16-bit counters
(with signed-wrap), copies, then **re-reads the current bank's counter (seqlock)**:
if it changed mid-copy the game came back around and started overwriting the
bank, so the record is discarded and retried. In practice the game does not
rewrite a bank until ~2 frames (~9.8 ms) later while the copy takes microseconds,
so retries essentially never fire; the seqlock is correctness insurance. Retries
exhausted → the record is still written with `DSPTAP_REC_TORN` set (honest data
beats dropped data).

### (c) Copy → SD without tearing or blowing the frame budget

SD writes are slow and bursty and **must never block** the game's audio. So the
copy path and the write path are split across two threads (`src/ring.c`):

- **Sampler thread** (`src/main.c` loop): polls the frame counter every ~1 ms
  (frame is ~4.889 ms, so every frame is caught), does the seqlock snapshot into
  a reusable ~5.7 KB staging record, and `ring_push`es it. `ring_push` **never
  blocks**: if the ring is full (SD stalled), it drops the record and the next
  one is flagged `DSPTAP_REC_DROP_BEFORE`. A 4 MiB ring absorbs multi-frame SD
  latency spikes (~0.8 s of records at the no-final-mix size).
- **Writer thread**: drains large contiguous ring chunks to the open file via
  libctru FS on an app core, so blocking there never starves the sampler. SPSC
  lock-free ring (published head/tail + DMB).

Per-frame producer cost is a `memcpy` of ~5 KB (single-digit microseconds) — a
rounding error against the 4.889 ms budget. Throughput: ~5.1 KB/record ×
204.5 rec/s ≈ **1.04 MB/s** (config + status); with `final_mix`, +131 KB/s.

### (d) On-disk dump format — the same-repo replay contract

`include/dsptap_format.h` is the **single shared header** both the plugin and the
PC replay reader `#include`; changing the layout means bumping
`DSPTAP_FORMAT_VERSION`. All little-endian (both ends are LE); payload sections
are the **raw shared-memory bytes** verbatim (including the DSP's middle-endian
`u32_dsp` fields) — decoding is the reader's job.

**File header** (`DspTapFileHeader`, fixed 128 B): magic `DSPTAP01`,
`format_version`, `header_bytes`, `record_bytes` (so an older reader can stride
records it doesn't fully parse), `title_id`, **`firmware_sha_prefix`** (binds the
dump to the exact `dspfirm.cdc` the oracle must replay it under — MiiPlaza
`0x944b40b5`), geometry (`native_sample_rate 32728`, `samples_per_frame 160`,
`source_config_stride 192`, `num_sources 24`, `dsp_config_bytes 196`,
`source_status_stride 12`), `section_flags` (which optional sections every record
carries), and `start_tick`.

**Per-frame record**: a 16-byte `DspTapFrameRecord` header (`frame_index`,
`dsp_frame_counter`, `region_parity`, `status`, `tick`) followed by the present
sections in fixed order:

| section | bytes | source bank | gate |
|---|---|---|---|
| `SOURCE_CONFIG` (24×192) | 4608 | current | always |
| `DSP_CONFIG` | 196 | current | always |
| `SOURCE_STATUS` (24×12) | 288 | other (DSP-written) | recommended on |
| `FINAL_SAMPLES` (s16[160][2]) | 640 | other (DSP-written) | **route-a gated, default OFF** |

Helpers `dsptap_record_bytes()` / `dsptap_section_offset()` give the reader exact
strides/offsets from `section_flags` alone.

**Not in the dump (by design, for v1):** the actual PCM/ADPCM **sample buffers**
the sources point at. `SourceConfiguration.physical_address` references FCRAM, not
the shared region, so the config says *which* buffer + gains/rate/interpolation,
not the samples. This is deliberate and sufficient for the **primary** consumer —
the command-stream diff ("is caesar's ARM11 runtime model right?" isolates cleanly
by comparing caesar's *computed* SourceConfiguration against the console's
*actual* one, no samples needed). The more ambitious consumer (full WAV re-render
through the oracle) additionally needs the referenced buffers; capturing each
unique buffer once on first sighting (not per frame — that would blow the budget)
is a versioned v1 extension: add a `DSPTAP_SECT_*`/side-section, bump the version.
Flagged as future work, not v0.

---

## 4. Build story

`Makefile` targets devkitARM (`arm-none-eabi`) and is **optional and quarantined
from `caesar_core`/CI**, mirroring how `tools/dsp-oracle` keeps teakra out of the
core:

- It **detects** the toolchain (`DEVKITARM`/`DEVKITPRO` env, else
  `arm-none-eabi-gcc` on PATH) and, if absent, prints how to get it and **exits 0**
  — running `make` here can never break a converter-only machine.
- With devkitARM present: compile `main.c`/`tap.c`/`ring.c` (`-lctru`), link with
  `3gx.ld` (`-nostartfiles`, our own `_start`), then `3gxtool` packs the ELF +
  `dsp-tap.plgInfo` into `build/dsp-tap.3gx`.
- **Route-a gate** is a compile flag: `DSPTAP_CAPTURE_FINAL_SAMPLES` (default `0`)
  → `make FINAL_SAMPLES=1` once route-a confirms final-mix readback.

Deploy: `build/dsp-tap.3gx` → `sd:/luma/plugins/0004001000021800/`, enable Plugin
Loader in Rosalina. n3ds-mcp deploys/toggles it (ftp + input; `config[0]!=0` stops
capture) — no new MCP code, per the ROADMAP placement decision.

---

## 5. Dependency on route-a (explicit)

`final_samples` capture is **entirely gated on the concurrent `dsp_oracle`
commit-2 route-a spike**: does the real firmware write the final mix back to
ARM11-visible shared memory, or only feed BTDMP internally? Until that says
**yes**, `DSPTAP_SECT_FINAL_SAMPLES` stays off (the region may be stale/garbage on
hardware) and the tap captures **per-voice SourceConfiguration + DspConfiguration
(+ SourceStatus) unconditionally**. If route-a says yes, one compile flag turns on
bit-perfect console ground-truth audio with no analog rig anywhere. The dump
format already reserves the section and the version discipline covers the flip.

---

## 6. What is NOT verified (no devkitARM / no console here)

Everything here is design + scaffold; nothing was compiled or run. Concretely
unconfirmed (all marked `VERIFY` in code):

- **Compiles/links under devkitARM.** Written against the documented libctru API
  from memory; header names, `threadCreate` core/priority args, `LightEvent`
  timeout units, and the FS calls need a real build.
- **Barebones `_start` viability.** Whether a `-nostartfiles` plugin with our own
  `_start` runs correctly without CTRPF's runtime (TLS/`.bss` zeroing/global-init
  assumptions), and whether the loader expects the entry thread to **return**
  (spawn detached workers) vs **run the loop and never return**. The scaffold does
  the latter and flags the alternative.
- **The exact `.plgInfo` keys/flag enums** for the installed `3gxtool` version
  (`MemoryRegionSize` units, flag names).
- **DSP region reads on hardware.** That `0x1FF40000 + W*2` is readable from the
  plugin without a fault, that MiiPlaza's word addresses match the oracle
  exemplar on the physical console, and that the higher-counter-is-current bank
  rule holds in the live firmware exactly as the HLE models it.
- **Frame budget under real SD latency** — that a 4 MiB ring + app-core writer
  never forces drops during normal play.
- **`final_mix` readback** — the route-a question itself.

First device bring-up should: build → install → confirm `tap_probe()` sees the
counter advance → capture a few seconds → validate the dump header + a couple of
records against a caesar stage-2 render of the same MiiPlaza track.

---

## References (protocol, not code — cite, never copy)

- `tools/dsp-oracle/src/shared_mem.h`, `src/PORT-NOTES.md` — region table, struct
  sizes, word/byte addressing, frame-parity double-buffering (the tap reads the
  **same** structures the console DSP driver writes).
- `docs/NW4C-disasm-handoff.md` §5 — the DspConfiguration/region pointers in
  MiiPlaza's ARM11 driver; confirms the region layout and that reverb/config live
  where the oracle expects.
- Luma3DS `sysmodules/rosalina/include/plugin/{3gx,plgldr}.h`, PabloMK7 `3gx.ld` —
  the loader ABI (GPL; referenced as protocol, no code copied).
- libctru `os.h` (`OS_DSPRAM_VADDR 0x1FF00000`, size `0x80000`), `services/dsp.c`
  (`DSP_ConvertProcessAddressFromDspDram`) — the DSP mapping.
- CTRPluginFramework license (thepixellizeross/Nanquitas) — the custom
  source-availability terms the verdict rejects.
