# capture-cartridge — hands-free console calibration batteries

Builds a surgically patched `MeetSound.bcsar` that turns TWO StreetPass
Mii Plaza music-player tracks into looping calibration batteries, played
by the console's own NW4C engine via Luma3DS LayeredFS. One line-in
recording of each track closes every flagged stage-2 constant
(decay-table spot-check, absolute output level, pan curve, velocity law,
attack, LPF depth, portamento time, release tail);
`tools/console-tolerance` verdicts the captures against the
`PREDICTION_battery_A/B.wav` renders.

- **Track A** replaces `BGM_MAIN_Mii_Only_One` ("Main Theme 1"):
  3× `SE_NEW_DRUM01`, pan probes L/R/C, `SE_NEW_SLIDE_MAP`, velocity
  ladder 96/64/32 — all in `BANK_MEET_SE_MAIN`. ~23.5 s/pass.
- **Track B** replaces `BGM_DEN_EMPTY_LANDSCAPE` (the den track captured
  2026-07-08): `SE_LEGEND_KEY_FLY` + 12 s ring, `SE_LEGEND_MENU_CURSOR`,
  a portamento glide — all in `BANK_MEET_LEGEND`. ~23 s/pass.

The probes replay the target sound effects' **own command bytes**, lifted
verbatim from the same archive. Each hijacked entry gets three same-size,
in-place pokes (volume byte -> 127, bank index -> the sections' bank, DATA
payload -> the battery); the archive round-trips byte-identically through
the stage-1 serializer.

**Why two tracks / no `0xB6`:** the first single-track battery came back
**silent** on hardware with a `0xB6` bank switch in the stream — whether
its argument is a global bank index (caesar's data-derived reading) or a
per-sound bank-SLOT index is unresolved, so the batteries avoid the
command entirely; each track's bank is set through the INFO entry, the
exact mechanism retail playback uses. The split doubles as a diagnostic:
if both tracks are still silent, the problem is bank *loading* for
BGM-player playback, not command semantics.

## Build

```powershell
tools\capture-cartridge\build-cartridge.ps1     # defaults to the MiiPlazaUpdate dump
```

Outputs under `build\cartridge\`:

- `sd\luma\titles\0004001000021800\romfs\region_common\frame\sound\MeetSound.bcsar`
  — the cartridge (the plaza's v14 **update** romfs path; the base title's
  `sound/MeetSound.bcsar` is never requested by the updated game — an
  override there is a silent no-op, found the hard way 2026-07-14)
- `PREDICTION_battery_A.wav` / `_B.wav` — what the console should output
- `MANIFEST.md` — patched byte ranges + both event schedules
- `verify\` — converter/round-trip/render logs

The driver gates the build on: converter parse, byte-identical
round-trip, both prediction renders, and a numerical schedule check
(onsets, pan L/R split, velocity-ladder monotonicity) per track.

## Console procedure

1. Copy the **contents** of `build\cartridge\sd\` onto the SD card root
   (or FTP the `.bcsar` straight to
   `/luma/titles/0004001000021800/romfs/region_common/frame/sound/`).
2. Boot with SELECT held; enable **Game Patching** in the Luma3DS config
   (current Luma build required — old ones don't intercept the update
   title's romfs).
3. Capture rig, same as the 2026-07-08 session: headphone jack ->
   line-in, stereo 192 kHz (or 48 kHz), 16/24-bit, volume slider FIXED
   (peaks ~-6 dBFS).
4. In the plaza music player: play "Main Theme 1" and record two full
   passes (~50 s) as `BATTERY_A_console.wav`; then play the Empty
   Landscape den track and record two passes as `BATTERY_B_console.wav`.
   Each battery starts with 1 s of silence and loops forever.
5. Drop both WAVs in the captures directory (or hand them over) and run
   `tools\console-tolerance\console-tolerance.ps1`.

Remove the file from the SD card (or disable Game Patching) to restore
the original music. Nothing on the console is modified.

## If it misbehaves

- **Music unchanged:** Game Patching off, Luma outdated, or a future
  plaza update moved the archive again — dump the active title's romfs
  with GodMode9 and rebuild with `-Source <dumped.bcsar>
  -RomfsRel <romfs path>` (the builder locates every entry by name).
- **Both tracks silent:** bank loading is implicated — the BGM player
  path may not load non-BGM banks/wave archives on demand; report back,
  the fallback battery uses `BANK_BGM`'s own instruments.
- **One track silent:** partial load info — also report back.
- The regular in-game SEs still play normally over the batteries — record
  on a quiet screen and avoid input during a pass.

---

# Battery v2 — the four still-open constants

`build_cartridge_v2.py` + `build-cartridge-v2.ps1` + `check_prediction_v2.py`
are the parallel v2 builder (v1 above stays the shipped tool). v2 closes the
constants v1 could not: unfaded release + reverb residual, isolated pitch
vibrato, pan bytes 32/96, a 2nd portamento point, a 2nd LPF point, the
decay-table console spot-check, and the steal-saturation tie order. Every probe
is a **crafted** command stream on the banks' own sustained looping tone —
program 23 is the *same* 1.512 s loop in both `BANK_MEET_SE_MAIN` and
`BANK_MEET_LEGEND` — so v2 lifts no SE replicas at all; the envelope, LFO,
portamento, and LPF are shaped with the engine's own sequence commands.

```powershell
tools\capture-cartridge\build-cartridge-v2.ps1     # -> build\cartridge-v2\
```

Outputs mirror v1 (`sd\...\MeetSound.bcsar`, `PREDICTION_battery_A/B.wav`,
`MANIFEST.md`, `verify\`). The driver gates on: build + static schedule check,
converter parse, byte-identical round-trip, both prediction renders (recalibrated
player), a numerical schedule check per track, and the steal voice-count from
the render log. **Predictions require the recalibrated `caesar_play`**
(constant-rate portamento, 1-pole ~4.1 kHz LPF, /200 divisor).

## Pilot tone (both tracks, tick 0.000)

Program 23, key 60, vel 127, 1.0 s, center pan — bytes `81 17 3C 7F 60`.
Analysis normalises **each channel** to the pilot's RMS per session, so slider,
knobs, and temperature divide out and absolute levels become tone-referenced.
The pilot is the first thing after `setup()` on both tracks; find it at the
manifest's tick-0 row.

## Console run-sheet

Same rig as v1 (headphone jack -> line-in, slider FIXED, ~-6 dBFS peaks; the
capture-rig-v2 Rosalina volume pin replaces the slider when available). Play
each track and record **two full passes** — track A pass = 36.1 s, track B pass
= 85.8 s.

| Track | Sect | t (s) | Measures | Expected analysis |
|---|---|---|---|---|
| A | PILOT | 0.0 | reference tone | normalise ch to this |
| A | A1 | 2.0-11.5 | pan 0/32/64/96/127 | L-R split; **32->+7.6 / 96->-7.9 dB** in the player (equal-power). Console tells cos-sin vs sqrt-poly at 32/96 |
| A | A2 | 11.5-21.1 | LPF cutoff 24/40/48/64 | corner per byte; **64 = open** (confirm); 2nd point at 24/40 pins the per-byte slope + 1-pole order |
| A | A3 | 21.1-30.9 | velocity 127...16 | fit (vel/127)^2; resolve the vel-96 -1.2 dB anomaly with neighbours |
| A | A4 | 31.1 | 30 stacked voices | 24 survive, 6 stolen — which pitches sound = sorted-insert tie order |
| B | PILOT | 0.0 | reference tone | normalise ch to this |
| B | B1 | 2.0-31.9 | release 60/100/114/124 (dry) | note-off slope per byte; **114 & 124 are corrected-DecayTable-tail indices** (first console A/B). Model -20 dB after 2.04/0.86/0.42/0.10 s |
| B | B1r | 32.0 | R127 instant cut, reverb send 127 | pure DSP reverb IR (no release confound) — stage-3 reverb fit |
| B | B2 | 42.5 | decay 122 / sustain 40 | decay slope at corrected index 122 |
| B | B3 | 50.0 / 54.4 | vibrato rate 48 / 96 | **FM-demodulate the 262 Hz C4 fundamental** (prog 9); model 3.75 / 7.50 Hz, dev +-64 cents. Two rates separate the vibrato from the fixed partial-beat (the 6.5 Hz trap) |
| B | B4d | 58.8 / 62.3 | porta +3 / +12 st @ byte48 | glide duration proportional to distance (model 1.06 / 4.22 s) |
| B | B4r | 68.8 / 73.8 | porta +12 st @ byte24 / byte96 | glide rate vs time byte (model 2.11 / 8.45 s); tests the 1/byte law |

All predicted numbers are the recalibrated player's; the point of the capture is
the **console minus prediction** residual. The `MANIFEST.md` carries every
probe's tick, payload offset, and signature bytes for deterministic location.

## Caveats / what a battery v3 still owes

- **B1/B2/B1r depend on the `0xD1`/`0xD3`/`0xD9` track overrides being honored
  in BGM-player playback** (untested on hardware; every MeetSound instrument is
  the A127/D127/S127/R127 default, so there is no natural non-127 release to fall
  back on). If the console shows instant cuts where the prediction shows slopes,
  the overrides are not honored in this path — itself a finding. B1r's natural
  R127 reverb-residual is override-free and always valid.
- **Portamento tempo-independence** (item 5c) was dropped to keep the manifest
  tick accounting honest — a v3 owe (needs a per-tempo battery or a tempo-aware
  assembler).
- The DSP reverb (B1r) is a **console-only** signal: the dry player safe-skips
  `0xD9`, so the prediction is dry by design; the residual IS the reverb.
