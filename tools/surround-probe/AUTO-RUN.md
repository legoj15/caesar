# surround-probe — the definitive AUTO run

This is the recommended, low-effort, high-rigor process. Instead of hand-timing
one capture per condition (see CAPTURE-PROTOCOL.md for that manual fallback), you
make **one continuous recording** while the app plays the whole condition matrix
by itself. Because every segment shares one clock, the null test aligns
sample-accurately — which is exactly what the manual method can't guarantee.

## Why one recording matters
The headline result is a *null test*: FRONT-only vs REAR-only should cancel to
near-silence in Stereo and clearly **not** cancel in Surround. A null is ruined by
even a fraction of a millisecond of misalignment, and two separately-started
recordings are never that aligned. One take of all conditions removes the problem.

## What the AUTO run plays
Press **RIGHT** on the d-pad. The app steps through these 8 segments hands-off.
Each segment is announced by a **countable pip burst** (segment 3 = 3 pips), then
~0.6 s of silence, then ~8 s of steady 440 Hz tone:

| Seg | Pips | Output mode | Routing | Pos    | Split file                |
|-----|------|-------------|---------|--------|---------------------------|
| 1   | ●    | STEREO      | FRONT   | SQUARE | `stereo_front.wav`        |
| 2   | ●●   | STEREO      | REAR    | SQUARE | `stereo_rear.wav`         |
| 3   | ●●●  | SURROUND    | FRONT   | SQUARE | `surround_front.wav`      |
| 4   | ●●●● | SURROUND    | REAR    | SQUARE | `surround_rear.wav`       |
| 5   | ×5   | SURROUND    | FRONT   | WIDE   | `surround_front_wide.wav` |
| 6   | ×6   | SURROUND    | REAR    | WIDE   | `surround_rear_wide.wav`  |
| 7   | ×7   | MONO        | FRONT   | SQUARE | `mono_front.wav`          |
| 8   | ×8   | MONO        | REAR    | SQUARE | `mono_rear.wav`           |

Whole run is ~85 s. Surround params stay at ndsp defaults (depth 0x7FFF,
rear 0x8000); the rear-ratio sweep stays manual (it's a continuum — see the
optional section of CAPTURE-PROTOCOL.md).

## Do this
1. Cable the 3DS **headphone jack** to a **line input**. Recorder at **48 kHz,
   stereo, 24-bit** (24-bit gives a deeper measurable null floor than 16).
2. Set the 3DS **volume slider to ~60–70% and DO NOT touch it again.** Same for
   the interface input gain. The loudest moment is a Stereo front tone — make sure
   even that peaks around −6 dBFS so nothing clips.
3. **Start the PC recording.** Then press **RIGHT** on the 3DS.
4. Leave it alone for the full ~85 s. The top screen shows `AUTO n/8` and the
   current phase; it returns to "idle (manual)" when done.
5. **Stop the recording.** Save it as `run.wav` in the `surround-probe` folder.
   (LEFT aborts mid-run if you need to restart; just re-record from step 3.)

Record it **twice** if you can: once with headphones/target plugged as normal,
and — for the headphone-coefficient question — note that recording *from the jack*
always reads as "plugged in" (the app shows the live state). A true
plugged-vs-unplugged test needs mic'ing the speakers; treat that as a later,
lower-rigor experiment.

## Then split + analyze
From the `surround-probe` folder (needs `python3` + numpy):

```
python3 tools/split_run.py run.wav
```

That writes the 8 files above. Now the standard analysis (identical to the manual
protocol):

```
# THE headline test — does routing matter in each mode?
python3 tools/analyze.py stereo_front.wav   stereo_rear.wav   --expect null
python3 tools/analyze.py surround_front.wav surround_rear.wav --expect differ

# Does Surround color even a front-only voice vs plain Stereo?
python3 tools/analyze.py stereo_front.wav   surround_front.wav

# Secondary: speaker-position effect
python3 tools/analyze.py surround_front.wav surround_front_wide.wav

# Sanity: Mono routing should be identical
python3 tools/analyze.py mono_front.wav     mono_rear.wav     --expect null
```

## What confirms the hypothesis
- **stereo_front vs stereo_rear → deep null** (< −25 dB, likely much deeper). Rear
  folds into front at unity → routing inaudible → `span` is a no-op in Stereo/Mono.
- **surround_front vs surround_rear → no/shallow null** (> −15 dB). The virtualizer
  repositions the voice → routing audible → `span` is real in Surround.
- **The decisive evidence is RELATIVE:** Stereo must null *far deeper* than
  Surround. That gap is the proof, more than either absolute number.
- **stereo_front vs surround_front** difference-spectrum = the fingerprint of the
  Surround fold-down filter, which the future caesar player needs to reproduce.

Paste the analyze.py outputs into RESULTS-TEMPLATE.md and bring it back to the
caesar project — it closes the "Surround-mode A/B probe" item on the roadmap.
