# surround-probe v2 — the definitive AUTO run

One hands-off recording answers the question: does the 3DS **Surround** output
mode actually reposition a voice front vs back — i.e. is the NW4C `span`
(SurroundPan / 0xD7) command audible on real hardware?

v1 (a steady, L/R-symmetric 440 Hz mono tone, mono-summed analysis) came back
inconclusive: a centered pure tone is blind to front/back virtualization, which
shows up as HRTF spectral coloration and inter-channel **crosstalk**, needing
BROADBAND content and an OFF-CENTER source. v2 fixes the probe design and the
analysis; the software chain is validated end-to-end on a synthetic run (it
correctly CONFIRMS a simulated effect and REFUTES a null).

## What v2 does differently
- **Signal**: a periodic, band-limited (100 Hz–14 kHz) Schroeder-phase pink
  multitone, embedded in the app (`source/probe_buf.h`, made by
  `tools/gen_probe.py`). Periodic ⇒ the wave buffer loops seamlessly and every
  recorded segment holds the same realization (the analysis is power-spectrum
  based, so per-segment phase offsets are harmless).
- **Routing**: the source is hard-panned to ONE quad corner — front-left (FL)
  vs back-left (BL). Same side, so a naive stereo fold makes them identical in
  Stereo but the front-left/back-left HRTF makes them differ in Surround, and
  the R output channel becomes a clean crosstalk meter.
- **Depth positive-control**: the matrix includes 0x7FFF and 0xFFFF depth rows
  so a null can be told apart from residual blindness (a real effect grows with
  the depth knob).

## The 10-segment matrix (pip count = segment number)
| Seg | Pips | Mode | Corner | Pos | Depth | Split file |
|-----|------|------|--------|-----|-------|------------|
| 1 | ● | STEREO | FL (front) | SQ | – | `stereo_front.wav` |
| 2 | ●● | STEREO | BL (back) | SQ | – | `stereo_rear.wav` |
| 3 | ×3 | SURROUND | FL | SQ | 0x7FFF | `surround_front.wav` |
| 4 | ×4 | SURROUND | BL | SQ | 0x7FFF | `surround_rear.wav` |
| 5 | ×5 | SURROUND | FL | SQ | **0xFFFF** | `surround_front_deep.wav` |
| 6 | ×6 | SURROUND | BL | SQ | **0xFFFF** | `surround_rear_deep.wav` |
| 7 | ×7 | SURROUND | FL | WIDE | 0x7FFF | `surround_front_wide.wav` |
| 8 | ×8 | SURROUND | BL | WIDE | 0x7FFF | `surround_rear_wide.wav` |
| 9 | ×9 | MONO | FL | SQ | – | `mono_front.wav` |
| 10 | ×10 | MONO | BL | SQ | – | `mono_rear.wav` |

Whole run ≈ 105 s.

## Do this
1. **Set System Settings → Sound → output to `Surround`** before capturing. The
   app forces the ndsp mode itself, but matching System Settings removes any
   firmware gating ambiguity. The top screen shows the System Settings value —
   confirm it reads `SURROUND` during the take.
2. Cable the 3DS **headphone jack → line input**; recorder **48 kHz, 24-bit,
   stereo**. Volume slider ~60–70 % and input gain so the loudest peak sits ~−6
   dBFS — then **do not touch either again**.
3. **Start the PC recording**, then press **RIGHT** on the 3DS. Leave it for the
   full ≈105 s (top screen shows `AUTO n/10`); it returns to "idle" when done.
   LEFT aborts if you need to restart.
4. **Stop the recording**, save as `run.wav` in the `surround-probe` folder.

Recording from the jack always reads as "headphones inserted"; a true
plugged-vs-unplugged (speaker) comparison is a separate, lower-rigor experiment.

## Then split + analyze
From the `surround-probe` folder (needs `python3` + numpy):

```
python3 tools/split_run.py run.wav        # -> the 10 WAVs + noise_floor.wav
python3 tools/analyze_surround.py .        # -> per-channel metrics + VERDICT
```

`analyze_surround.py` computes, per output mode, comparing the FRONT vs REAR
capture: **D** (front-vs-rear spectral reshaping, level-independent) and
**XTALK** (energy bleeding into the silent channel), and prints a verdict.

## What confirms / refutes
- **CONFIRMED** (span is audible in Surround): stereo front-vs-rear is flat
  (D_stereo ≤ 1 dB), and in Surround either the crosstalk lifts ≥ 10 dB above
  the stereo floor **or** the spectrum reshapes ≥ 3 dB front-vs-rear — **and the
  effect grows/holds at 0xFFFF depth** (the positive control).
- **REFUTED** (headphone-surround does not reposition front/back): no reshaping,
  no crosstalk, no growth with depth → a level/EQ enhancement or a speaker-only
  effect; `span` would be inaudible via headphones.
- **MONO** front-vs-rear must null (quad collapse sanity).

Paste the `analyze_surround.py` output into `RESULTS-TEMPLATE.md` and bring it
back to the caesar project — it closes the "Surround-mode A/B probe" roadmap
item and settles the `span` verdict in `docs/HISTORY.md`.

*(The old one-condition-at-a-time manual method is still in
[`CAPTURE-PROTOCOL.md`](CAPTURE-PROTOCOL.md) as a fallback; it uses the v1
tone/analysis and cannot resolve surround — prefer the AUTO run above.)*
