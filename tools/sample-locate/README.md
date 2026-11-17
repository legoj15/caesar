# sample-locate

Finds where, at what playback speed, in which stereo channel and at what level a
known sound-bank sample occurs inside a reference recording, by sample-exact
matched filtering (normalised cross-correlation). One file, five subcommands:
`speeds`, `semis`, `pursue`, `fit`, `all`.

## What it is for

A game's sequenced sound (a bank sample played as notes) that is reused in a
mastered recording can be pinned exactly, because the engine plays samples at
semitone steps from the sample's native rate. Locating the sample at an exact
semitone, in a named channel, at a measured level gives a recipe that can be
rebuilt from the bank and checked by subtraction.

Worked case: Kid Icarus: Uprising wave archive 62 slot 35 was found in a CD track
as chords: (centre 0 st, left -7 st, right -8 st), then 1.3005 s later
(left -5 st, right -7 st, 100 ms fade-in). By ear the pitches were off by 4
semitones; the matched filter was exact to half a cent.

## Requirements

- Python 3 with numpy and scipy. No other packages.
- Run it as `python -I sample_locate.py <subcommand> ...` from any directory.
- Reference: lossless WAV, int16 or float (stereo for `pursue`, `fit`, `all`;
  `speeds` and `semis` also take mono with `--chan mid`).
- Sample: a WAV of the raw bank sample (a stereo sample is averaged to mono).
- Samples and references are normalised to [-1, 1] (int16 is divided by 32768,
  float is used as is); the score is scale-invariant, so level does not matter.

## Workflow

1. Get a lossless stereo reference and align it to the game's own timeline.
   Every time the tool prints is a time in the reference file; `pursue
   --report-shift` adds a second column with a fixed number of seconds
   subtracted, for converting to the other timeline.
2. `speeds` on a short window (about 60 ms at 0.1% steps) over a small stretch,
   to learn whether matches land on exact semitones (speed = 2^(n/12)).
3. `semis` per channel, L and R separately (not mid), with a 250-300 ms window,
   over the whole region of interest. Peaks above 1.5 x the printed 99.999th
   percentile are marked `<<<`.
4. `pursue` to peel every copy off both channels: refined cents and sub-sample
   time, level, first-50-ms level, how long it was heard, and the residual WAV
   with the copies subtracted. `fit` re-fits one copy and prints gain and
   explained fraction per 50 ms (use `--in` to fit on a residual).
5. `all` over every wave of the game for stretches that stay unexplained after
   `pursue`: it ranks each wave by its best score at any semitone in either channel.
6. Rebuild the stretch from the recipe (sample, semitone, channel, level, time)
   and subtract it from the reference to confirm.

## Commands

Short stretch: `t1 - t0` must be at least `--window-ms`, otherwise the tool exits 2
with a message (the correlation is undefined for a window longer than the stretch).
`pursue` and `all` also need `--t0` of at least 0.091 s (4000 frames at 44.1 kHz of
context for the high-pass to settle); a `--t1` closer than that to the end of the
reference is trimmed to fit.

```
python -I sample_locate.py speeds --ref cd.wav --sample sfx.wav --t0 197.33 --t1 197.52 --speed-lo 0.3 --speed-hi 3.2 --step 0.0005 --window-ms 120 --chan mid --peaks 6
```
Fine log-spaced speed grid against time on one channel (`mid`, `side`, `L`, `R`).
`--step` is a log-speed fraction (0.001 = 0.1%). Prints the strongest separate
peaks (30 ms suppressed around each) with speed, semitones and signed ncc, then
the noise floor. Defaults: window 60 ms, speeds 0.25 to 4, step 0.001, 14 peaks.
`--tpl-offset-ms` skips into the sample before matching.

```
python -I sample_locate.py semis --ref cd.wav --sample sfx.wav --t0 197.2 --t1 204.5 --semi-lo -24 --semi-hi 24 --window-ms 300 --chan L --cents-step 100 --peaks 14
```
Exact semitone grid (or any `--cents-step`) against time on one channel; `--chan` is
required. Same peak list (40 ms suppressed; default 16 peaks), then median, 99.9th
and 99.999th percentile of |ncc|. Default window 250 ms.

```
python -I sample_locate.py pursue --ref cd.wav --sample sfx.wav --t0 197.2 --t1 204.4 --semi-lo -19 --semi-hi 19 --window-ms 250 --threshold 0.42 --max-iter 24 --out residual.wav --report-shift 0.670 --tsv copies.tsv
```
Matching pursuit, high-passed at 300 Hz: find the strongest copy on either channel,
refine cents (+-20 in 1-cent steps) and sub-sample time, fit 50 ms block gains over
the copy's full length, subtract with a median-smoothed gain, repeat until the best
remaining ncc is below `--threshold` or `--max-iter` is hit. Prints one row per
copy (`level dB` is the median fitted gain, `first50ms dB` the first block,
`heard for` how long the fit explained the signal, `ncc` of the refinement) and
writes the residual as float32 WAV. `--tsv` writes the same copies with columns
`channel, ref_time_s, shifted_time_s, cents, speed, level_db, first50ms_db,
heard_for_s, ncc`. A copy whose fit runs past the end of the reference is cut at the end.

```
python -I sample_locate.py fit --ref cd.wav --sample sfx.wav --chan L --t 198.4681 --cents -700 --duration 3.5 --out residual.wav
```
Fit one copy on one channel near `--t` and `--cents` (refines cents +-15 in 0.5 steps and
time), print the refined time, cents, ncc over the first 500 ms and the full length at
that speed, then gain and explained fraction per 50 ms (`--quiet` prints only the
summary line). `--out` writes the reference with the copy subtracted; `--in prev.wav`
reads the audio from an earlier residual instead of `--ref`; `--hp` changes the
300 Hz high-pass.

```
python -I sample_locate.py all --ref cd.wav --wav-dir waves/ --t0 197.45 --t1 197.66 --semi-lo -19 --semi-hi 19 --window-ms 80 --out ranked.tsv --names-tsv occurrences.tsv
```
Every `*.wav` in the directory at every semitone against one short stretch, both
channels, high-passed at 300 Hz (oversampling 4 instead of 8). Writes a TSV ranked by
|ncc| (`ncc, wave, rate, len_s, semis, ch, ref_time, where`) and echoes the first
`--top` rows (default 25). `--names-tsv` is an optional table of `wave name <tab>
description`; the description lands in the `where` column. Files that are unreadable,
shorter than 30 ms or nearly silent are skipped. About 0.14 s per wave at 39 semitones
and an 80 ms window on the machine this was built on.

## Reading the numbers

- ncc is signed; the sign is the polarity of the match. The score is the cosine
  between the template and the reference window.
- Reported times are template-onset times (the first sample above 10% of the
  sample's peak). The note-on is earlier by `onset_samples / (native_rate * speed)`;
  `semis` prints the onset sample.
- A true match sits on an exact semitone: `speeds` should report speed
  2^(n/12) within about 0.1%; `pursue` cents should be multiples of 100.

## Pitfalls

- A near-mono or lossy master hides left/right chords. In the worked case the 3DS
  re-encode did; use the widest lossless source available.
- Scan per channel. Hard-panned copies at different pitches halve each other's score
  in mid, so a chord can vanish in mid and show clearly in L and R.
- Window length trades speed precision against noise: about 60 ms at 0.1% steps to
  discover a speed, 250 ms or more on the semitone grid to confirm it.
- Low speeds make smooth templates that score 0.4-0.5 by chance on short windows.
  Judge a peak against the printed noise percentiles and against whether its speed is
  an exact semitone, not against a fixed ncc.
- A copy with pitch sweep, vibrato or reverb will not match well; expect a low ncc
  and a short `heard for` even when the sample is there.
- Ears are unreliable for pitch: in the worked case they were off by 4 semitones.
- `pursue` subtracts a smoothed gain, so a residual that still carries a copy at
  low level means the copy was not a pure scaled replay of the sample.
- `speeds` and `semis` list several copies that start together in one channel
  only when their pitches are more than about 50 cents apart; `pursue` finds
  them either way, one subtraction at a time.

## Worked example

`examples/` holds the two scripts that closed the worked case (step 6 of the
workflow), with that case's times and file roles hardcoded; copy and adapt.

- `p10_music_bed.py`: aligns the music-only stream to the recording, fits a
  257-tap filter on a clean stretch and saves the predicted music bed.
- `p10_rebuild.py`: places the 14 notes of the game sequence on the engine frame
  grid (160/32728 s), measures the slow attack per frame, and solves one gain
  per note and channel jointly with the music bed (free music gain per 20 ms
  block across the cut). Prints the recipe table and how far the rebuild cancels
  the recording, stretch by stretch.
