# surround-probe — capture protocol

Goal: record the 3DS headphone-jack output under each combination of **output
mode** and **voice routing**, so `tools/analyze.py` can null-test them and tell
us whether the DSP's Surround virtualization actually does anything — and
therefore whether the NW4C `span` (SurroundPan / 0xD7) command is audible on
real hardware.

The app forces the output mode itself via `ndspSetOutputMode`, so it does **not**
matter what System Settings is set to (the app displays both so you can confirm).

## What you need
- New 3DS running the app from the Homebrew Launcher.
- A cable from the 3DS **headphone jack** to a **line/instrument input** on your
  audio interface. (Headphone-out → line-in is fine; watch levels.)
- Recording software set to **48 kHz, stereo, 24-bit** (16-bit works; 24-bit
  gives a deeper measurable null floor).

## Rules that matter (read once — these prevent wasted captures)
1. **Never touch the 3DS volume slider once you start.** It scales the whole
   output; if it moves between captures, cross-mode comparisons are meaningless.
   Set it to roughly 60–70 % and leave it.
2. **Never change the interface input gain between captures.** Same reason.
3. **Do not let it clip.** Check the loudest case (STEREO, FRONT) first; back off
   input gain until peaks sit around −6 dBFS, then leave it.
4. Each capture is **~10 seconds**. The app plays a short **marker click**, then
   a steady 440 Hz tone. `analyze.py` aligns on that click, so just make sure the
   click is inside the recording (start recording, *then* change the setting).
5. One WAV per condition. Use the filenames below verbatim — the analysis
   commands assume them.

## The core matrix (4 captures — do these first)
On the app: **X** cycles output mode, **A** toggles FRONT/REAR routing. After each
change the app re-emits the marker click automatically.

| # | Output mode | Routing | Filename            |
|---|-------------|---------|---------------------|
| 1 | STEREO      | FRONT   | `stereo_front.wav`  |
| 2 | STEREO      | REAR    | `stereo_rear.wav`   |
| 3 | SURROUND    | FRONT   | `surround_front.wav`|
| 4 | SURROUND    | REAR    | `surround_rear.wav` |

For each: start recording → press X/A until the top screen shows the target
mode+routing → wait for the click + ~10 s of tone → stop → save under the name.

## Optional captures (higher rigor / secondary questions)
5. `surround_front_wide.wav` — SURROUND, FRONT, position **WIDE** (press **Y**).
6. `surround_rear_wide.wav`  — SURROUND, REAR, position **WIDE**.
7. Rear-ratio sweep: SURROUND, REAR, nudge rear-ratio with **L/R** shoulders;
   capture at min and max as `surround_rear_rrlo.wav` / `surround_rear_rrhi.wav`
   (note the hex value shown on screen in the results template).
8. MONO sanity: `mono_front.wav` / `mono_rear.wav` (press X to MONO) — both should
   be identical mono.

## About the headphone-detect (`headphones_connected`) question — read this
The DSP reportedly swaps surround coefficients depending on whether headphones
are plugged in. **This is the one thing this capture setup cannot cleanly A/B**,
because you are recording *from the jack* — so the console always sees "plugged
in." The app displays the live headphone-detect state (if libctru exposes it) so
you can at least record what the console believes. A true plugged-vs-unplugged
comparison would require mic'ing the built-in speakers instead (much lower
rigor); treat that as a separate, later experiment. Note in the results template
whatever headphone state the app reports during your captures.

## Analysis (after you have the WAVs)
Run these from the folder containing the captures (needs `python3` + numpy):

```
# 1. THE headline test — does routing matter in each mode?
python3 tools/analyze.py stereo_front.wav   stereo_rear.wav   --expect null
python3 tools/analyze.py surround_front.wav surround_rear.wav --expect differ

# 2. Does Surround color even a front-only voice vs plain Stereo?
python3 tools/analyze.py stereo_front.wav   surround_front.wav

# 3. (optional) position + rear-ratio effects
python3 tools/analyze.py surround_front.wav surround_front_wide.wav
python3 tools/analyze.py surround_rear_rrlo.wav surround_rear_rrhi.wav
```

## Predictions (what confirms the hypothesis)
- **Test 1a** (`stereo_front` vs `stereo_rear`): **deep null** (< −25 dB, likely
  much deeper — limited only by capture noise). Rear folds into front at unity in
  Stereo → routing is inaudible → `span` is a no-op in Stereo/Mono. **PASS = deep null.**
- **Test 1b** (`surround_front` vs `surround_rear`): **no null** (> −15 dB). The
  virtualizer repositions the voice front↔rear → routing is audible → `span` is a
  real command in Surround. **PASS = shallow/no null.**
- **The decisive evidence is RELATIVE**: test 1a should null *far deeper* than
  test 1b. If 1a hits −40 dB and 1b only −8 dB, that gap is the proof — more so
  than either absolute number, which capture noise and channel imbalance affect.
- **Test 2** (`stereo_front` vs `surround_front`): characterizes the surround
  biquad coloration — even a front-only voice may change level / spectrum when
  Surround is engaged. Any consistent difference-spectrum shape here is the
  fingerprint of the virtualization filter.

If 1a nulls deep and 1b does not, the whole span-audibility chain is confirmed on
metal, and the difference-spectrum from test 2 tells the future caesar player
what the Surround fold-down actually does.
