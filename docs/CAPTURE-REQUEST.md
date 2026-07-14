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

## What is STILL WANTED — per-instrument isolated notes

A busy BGM cannot isolate the envelope/level/pan constants. A handful of
**single-note** captures would close them (and the queued decay-table console
spot-check). Each is a single note in `MeetSound` — record it **note → full
silence**, one sound per WAV, and name it `<SEQ>_console.wav` in the directory
above (matching the existing convention).

### Rig (same as the 2026-07-08 session)

- New 3DS headphone jack → line-in, **stereo 192 kHz (or 48 kHz)**, 16/24-bit.
- **3DS volume slider fixed** (do not move it — it scales the absolute level, which
  is what the isolated notes calibrate); interface gain fixed, peaks ~−6 dBFS.
- Half a second of silence before each note; let it ring fully out.

### The notes (2 priority, 2 optional)

| File to save | Sequence | In-game trigger (best-effort) | Identifying feature | Calibrates |
|---|---|---|---|---|
| `SE_NEW_DRUM01_console.wav`   | `SE_NEW_DRUM01`   | the drum hit in a new-Mii greeting/arrival | one **percussive hit** (~0.5 s) | **decay-table spot-check**, envelope floor, absolute output level |
| `SE_LEGEND_KEY_FLY_console.wav` | `SE_LEGEND_KEY_FLY` | StreetPass Quest / *Find Mii* — a key flying to its lock | **one long sustained ring (~9 s)** | envelope **release / decay tail**, reverb residual, pan L/R, output level |
| `SE_NEW_SLIDE_MAP_console.wav` *(optional)* | `SE_NEW_SLIDE_MAP` | sliding/scrolling the plaza map | one short note (~0.25 s) | output level, pan |
| `SE_LEGEND_MENU_CURSOR_console.wav` *(optional)* | `SE_LEGEND_MENU_CURSOR` | moving the cursor in a *Find Mii* command menu | one short blip (~0.18 s) | attack, output level |

The pair that matters: **`SE_NEW_DRUM01`** (a clean percussive one-shot for the
decay table) and **`SE_LEGEND_KEY_FLY`** (a long sustained voice whose exposed tail
is the release-vs-reverb witness). If a specific SE is impractical to trigger,
**skip it** — the harness verdicts whatever is present. If StreetPass Mii Plaza
exposes a sound-test / soundlist (note the `_for_Soundlist` sequence names), that is
the cleanest way to trigger a specific SE; otherwise trigger it via the UI action
noted. You can also tell me which single-note SEs you *can* reliably trigger and I
will map those instead.

### Also useful (lower priority)

- A capture in **System Settings = Surround** of a span-using sequence (feeds the
  stage-3 surround virtualization model; the Part-B register dump is the rigorous
  follow-up).
- A **portamento** SE (a monophonic pitch glide) — would calibrate the
  portamento time→duration mapping, the one flagged constant no current capture
  touches.

---

## After you record

Drop the WAVs in the directory above and run
`tools\console-tolerance\console-tolerance.ps1`. It prints, per capture, a
PASS/FAIL on each metric, the reverb-attributable residual (reported, never
failing), and a measured value for each flagged constant. Paste the output back
into the caesar session — each deviating constant is a one-line recalibration, and
the reverb residual is the stage-3 target. Nothing you capture is committed
(WAVs are gitignored, like the corpus).
