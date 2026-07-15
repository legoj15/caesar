# Console captures — what exists, what is still wanted (stage-2 Phase IV / C11)

The dry player (`caesar-play`) is feature-complete through Phase III. The
console-tolerance net (`tools/console-tolerance/`) verdicts it against real New
3DS line-in captures — match within tolerance EXCEPT the reverb tail — and
measures each flagged constant. The harness is built and self-validated, and has
**already been run against the two existing captures**. This doc records what is
in hand and what one more short session would add.

Default capture directory (override with `-CapturesDir`):

```
E:\legoj\Documents\3DSWii Dumps\Dumps\MiiPlaza\sound\MeetSound\BANK_BGM\
```

Run the harness with:

```powershell
tools\console-tolerance\console-tolerance.ps1              # verdict every present capture
tools\console-tolerance\console-tolerance.ps1 -SelfValidate  # prove the harness (no capture needed)
```

---

## What EXISTS (2026-07-08 New 3DS line-in, 192 kHz / 16-bit, stereo)

Both are in the default directory above and are verdicted automatically.

| File | Sequence | What it calibrates | First-comparison result |
|---|---|---|---|
| `BGM_MAIN_Mii_Only_One_console.wav` (~60.5 s) | `BGM_MAIN_Mii_Only_One` | tempo/frame-period, envelope shape, spectral fidelity, loudness; the ~1.6 s gap at ~51 s exposes the DSP reverb tail | **PASS all 4 metrics** (envelope 2.31 dB, tempo slope 1.0000, PSD 0.81 dB, loudness Δ≤1.2 dB); reverb residual **−35.6 dB** |
| `EMPTY_LANDSCAPE_console.wav` (~29.1 s) | `BGM_DEN_EMPTY_LANDSCAPE` | dense-pad spectral/loudness; quantifies the reverb-carried sustain | **PASS all 4 metrics** (envelope 2.33 dB, tempo slope 1.0000, PSD 2.59 dB, loudness Δ≤1.1 dB); reverb residual **−6.0 dB** |

The directory also holds the 2026-07-08 FluidSynth A/B renders
(`EMPTY_LANDSCAPE__A_dry_old` / `__B_reverb_new` / `__C_FIXED`,
`Mii_Only_One_FIXED_3p5s` / `_MID_1p0s` / `_SHORT_0p3s`) — historical, not used by
this harness.

**What the first comparison already tells us:** the dry player reproduces the
console's dry-dominant behaviour within tolerance on both real BGM tracks. The
tempo/clock slope is **exactly 1.0000** on both (the 120 BPM default and the
160/32728 frame period are confirmed against hardware). The reverb residual is the
stage-3 target and is much larger on the dense pads (−6 dB) than on the sparse
gap track (−36 dB) — the numerical form of the 2026-07-08 finding that
`EMPTY_LANDSCAPE`'s sustain is carried by DSP reverb a soundfont cannot encode. A
strong **4.00 Hz** vibrato is measured in `Only_One` (→ LFO rate byte ≈ 51 at the
current `kLfoRateHz = 5/64`) and a **1.45 Hz** modulation in `EMPTY_LANDSCAPE`.

---

## What is STILL WANTED — one recording of the capture cartridge

The in-game SE triggers proved impractical (the target sounds only fire
during music and can't be requested on demand), so the isolated-note plan is
superseded by **`tools/capture-cartridge`** (2026-07-14): a Luma3DS
LayeredFS-patched `MeetSound.bcsar` that turns the selectable plaza tune
`BGM_MAIN_Mii_Only_One` into a hands-free, looping **43.5 s calibration
battery** played by the console's own engine — in silence, because the
battery *replaces* the music. One recording covers everything the old
per-SE table asked for, plus pan, velocity-law, and portamento probes the
in-game triggers never could.

The batteries — TWO music-player tracks, one per SE bank, after the first
single-track attempt came back silent with a `0xB6` bank switch in the
stream (its slot-vs-global semantics is an open question; the two-track
form needs no bank switching at all). Schedules with exact timestamps:
`build\cartridge\MANIFEST.md`; expected audio:
`build\cartridge\PREDICTION_battery_A.wav` / `_B.wav`.

**Track A** (replaces `BGM_MAIN_Mii_Only_One`, "Main Theme 1", ~23.5 s/pass):

1. 3× `SE_NEW_DRUM01` (verbatim replica) — decay-table spot-check, envelope
   floor, absolute output level, multi-take averaging
2. Pan probes: the drum at hard-left / hard-right / center — pan curve
3. `SE_NEW_SLIDE_MAP` — LPF depth
4. Velocity ladder: the drum at vel 96/64/32 — the `(vel/127)²` law

**Track B** (replaces `BGM_DEN_EMPTY_LANDSCAPE`, ~23 s/pass):

1. `SE_LEGEND_KEY_FLY` + 12 s ring-out — release/decay tail, reverb
   residual, LFO
2. `SE_LEGEND_MENU_CURSOR` — attack
3. A monophonic portamento glide (key 50→74, time byte 48) — the
   time→duration mapping, the one constant no capture has ever touched

### Procedure

1. Build (or rebuild) with `tools\capture-cartridge\build-cartridge.ps1`;
   copy the contents of `build\cartridge\sd\` onto the SD card root.
2. Boot holding SELECT → enable **Game Patching** (use a current Luma
   build — old ones don't intercept the plaza update-title's romfs).
3. Rig, same as the 2026-07-08 session: headphone jack → line-in, **stereo
   192 kHz (or 48 kHz)**, 16/24-bit, **volume slider fixed**, peaks ~−6 dBFS.
4. In the plaza music player: play "Main Theme 1", record **two full
   passes** (~50 s) as `BATTERY_A_console.wav`; then play the Empty
   Landscape den track, record two passes as `BATTERY_B_console.wav`.
   Save both in the captures directory above.
5. Delete the SD file (or toggle Game Patching off) to restore the music.

The cartridge targets the plaza's **v14 update** romfs path
(`region_common/frame/sound/MeetSound.bcsar`) and is built from the
`MiiPlazaUpdate` dump — the base title's `sound/` path is never requested by
the updated game (the first deploy proved this the hard way). If the music
still sounds unchanged, Luma is outdated or Game Patching is off; if a
future plaza update misbehaves, dump the active title's romfs with GodMode9
and rebuild with `-Source <that file> -RomfsRel <its path>`.

### Also useful (lower priority)

- A capture in **System Settings = Surround** of a span-using sequence (feeds the
  stage-3 surround virtualization model; the Part-B register dump is the rigorous
  follow-up).

---

## After you record

Drop `BATTERY_A_console.wav` / `BATTERY_B_console.wav` in the directory
above and run
`tools\console-tolerance\console-tolerance.ps1`, or just hand the file over.
Each deviating constant is a one-line recalibration against the battery's
manifest schedule, and the reverb residual is the stage-3 target. Nothing you
capture is committed (WAVs are gitignored, like the corpus).
