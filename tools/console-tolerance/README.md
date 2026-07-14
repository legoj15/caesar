# console-tolerance — the suite stage-2 Phase IV (C11) net

Verdicts a `caesar-play --render` against a **New 3DS line-in capture** of the
same sequence, to the stage-2 proof criterion:

> the render matches the console **within tolerance, EXCEPT the reverb tail**.

The console has DSP reverb the dry player deliberately lacks (that is stage 3).
So every metric is measured over **dry-dominant regions** — attack transients and
sustained note bodies — and the reverb-attributable residual is **reported
separately and never fails** the verdict. That residual is stage 3's target.

This is the Net-B half of stage 2's verification (Net A is `tools/play-goldens`,
the deterministic golden-render hashes). It is **built and self-validated before
the captures exist**, so when they land, verification is one command.

## Layout

| Path | What |
|---|---|
| `console_tolerance.py` | the analyzer + `--self-validate` (numpy only; no scipy) |
| `console-tolerance.ps1` | the driver: renders each captured sequence at the capture's rate and verdicts it; `-SelfValidate` proves the harness |
| `captures/` | optional in-tree capture location (pass `-CapturesDir` to use it; gitignored) |

The two real captures already exist (2026-07-08 New 3DS line-in, **192 kHz /
16-bit**); the harness's default `-CapturesDir` is where they live, alongside the
extracted MeetSound bank:
`…\3DSWii Dumps\Dumps\MiiPlaza\sound\MeetSound\BANK_BGM\`. See
`docs/CAPTURE-REQUEST.md` for what exists and what is still wanted.

## Run

```powershell
# 1. Prove the harness works (no captures needed): renders two real sequences and
#    checks that a realistic capture-chain fixture PASSES and three negative
#    controls each FAIL their named metric.
tools\console-tolerance\console-tolerance.ps1 -SelfValidate

# 2. Verdict every present capture (default -CapturesDir = the BANK_BGM directory):
#    render each mapped sequence at the capture's own sample rate and verdict it.
tools\console-tolerance\console-tolerance.ps1
```

Or drive the analyzer directly:

```sh
python tools/console-tolerance/console_tolerance.py <console.wav> <render.wav>
python tools/console-tolerance/console_tolerance.py --self-validate <render.wav> [--other <render2.wav>]
```

Exit codes (same contract as ab-verify / diag-goldens / play-goldens):
`0` within tolerance / self-validate passed · `1` out of tolerance · `2` harness
error (nothing verified — never a pass).

## Metrics and tolerances

Alignment first: the capture is brought to the render's sample rate; an integer
onset lag aligns them; a scale-search cross-correlation of the two onset-strength
envelopes recovers the tempo/clock ratio; a global gain offset is estimated over
the trusted window. Then:

| Metric | Validates (flagged unknowns) | Pass tolerance | Reverb handling |
|---|---|---|---|
| **envelope-fit** | envelope shape (attack/decay/sustain), and tracks absolute output level | median-subtracted shape residual **≤ 3.0 dB** (music) / **≤ 6.0 dB** (short note), dB-slope **∈ [0.85, 1.15]**, over the trusted dB window | trusted = render **above floor+6 dB**, **below peak−1 dB**, and **before the last shared onset**; reverb-dominated frames (console **> 4 dB above** the render) are excluded and counted as reverb |
| **onset-timing** | tempo / frame-period / clock (default-tempo, the 160/32728 frame period) | **\|slope−1\| ≤ 1.0 %**; onset-strength xcorr ≥ 0.08 floor; needs **≥ 4 onsets over ≥ 1 s** (else N/A — a single note is not a tempo) | onsets are attacks; reverb adds none |
| **psd-distance** | spectral fidelity (interpolation, pan, velocity) | per-channel **MAGDEV ≤ 6.0 dB** (median-subtracted, 100–14000 Hz, over the body) | body only; median-subtraction removes any level offset |
| **loudness** | dynamics realism (the EMPTY_LANDSCAPE precedent) | **\|Δ(p95−p50)\| ≤ 4 dB** and **\|Δ(p95−p25)\| ≤ 6 dB** over the trusted body | measured on the **upper distribution only** — reverb fills valleys, so a valley-depth range would always read the dry render as "more dynamic"; that is the reverb exception, not a fault |
| **reverb residual** | *report only — stage 3's target* | never fails | the console energy living **where the dry render is silent** (relative to the body), in dB |

The tolerances are calibrated so a faithful capture-chain fixture passes with
margin while a 2 % tempo error, a padded release, or a different sequence each
fail — see `--self-validate`.

## Per-flagged-unknown diagnostics

Alongside the verdict, the harness emits a targeted measurement for each flagged
constant a capture can discriminate, so a capture immediately recalibrates it:

- **output level** — the render-vs-console gain offset (dB) → absolute output level.
- **pan (L/R dB)** — L/R RMS ratio, console vs render → the pan law (sqrt-poly vs
  equal-power), on a note with an intrinsic pan.
- **vibrato/tremolo rate** — dominant 1–12 Hz modulation of the fine log-envelope
  → the LFO `kLfoRateHz = 5/64` constant (measured Hz ÷ (5/64) = the commanded rate byte).
- **tail decay (tau)** — exponential-grid fit of the exposed final decay, console
  vs render, aligned at the −25 dB crossing over the trusted dB window (the
  EMPTY_LANDSCAPE / BGM_MAIN_Mii_Only_One precedent) → envelope floor / release,
  and the queued **decay-table console spot-check**. A console tail far longer than
  the render tail is the DSP reverb (release + reverb are not separable from the
  tail alone — cf. the 2026-07-08 dispute).
- **interpolation (HF)** — high-band (> 0.6·Nyquist) energy fraction, console vs
  render → the console interpolation filter (linear vs polyphase image rejection).
- **velocity** — needs ≥ 2 isolated-note captures at known velocities (the
  `(vel/127)²` law); reported as needs-repro in single-file mode.
- **portamento** — needs a monophonic glide SE capture; reported as needs-repro.

## Self-validation (the fixture-realism discipline)

`--self-validate` synthesizes a fake "console capture" from a real render by
applying the **whole capture chain** — resample to the capture rate, a gain
offset, a −85 dBFS noise floor (the surround-probe's measured line-in floor), a
few ms of lead-in, ±50 ppm clock drift, and a synthetic exponential reverb tail —
so the reverb-exception path is exercised. The harness must PASS that fixture.
Three negative controls must each FAIL their named metric: a wrong-tempo render
(retimed 2 %) → **onset-timing**; a padded-release render → **envelope-fit**; a
different sequence → fails.

This models the **2026-07-11 lesson** (HISTORY): the surround-probe's synthetic
validation passed but the real capture broke the segmenter because the fixture was
unrealistic. An idealized fixture is worthless — the fixtures here reproduce the
real capture chain's artifacts. The synthetic noise is seeded, so the whole
self-validate is deterministic.

## Provenance

- Numpy WAV reader and the Welch-PSD MAGDEV metric are reused from
  `tools/surround-probe/tools/` (numpy-only — scipy is not installed).
- The envelope / decay-fit precedent is the 2026-07-08 EMPTY_LANDSCAPE /
  `BGM_MAIN_Mii_Only_One` analysis in `docs/HISTORY.md`.
- Nothing corpus-derived is committed: captures and renders are gitignored, only
  the script + this README are in the repo — the same discipline as ab-verify /
  diag-goldens / play-goldens.
