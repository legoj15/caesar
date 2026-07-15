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
