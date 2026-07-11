# surround-probe v2 — results

Fill this in and paste it back into the caesar project session.

## Rig
- Console: New 3DS ___________ (region/model)  •  CFW: Luma3DS ______
- System Settings sound mode (must read SURROUND during the take): __________
- Interface: __________  •  record format: ____ kHz / ____ bit
- 3DS volume slider position (fixed all captures): ______
- Headphone-detect state the app reported: __________
  (expected "INSERTED", since we capture from the jack)

## split_run.py
- bodies found: ____ / 10   •   any pip-count warnings? __________

## analyze_surround.py — per-mode metrics
Copy the table the analyzer prints (D = front-vs-rear spectral reshaping in dB;
XTALK = energy in the silent channel, front/rear, in dB):

| mode        | D (dB) | XTALK front / rear (dB) |
|-------------|--------|-------------------------|
| STEREO      |        |                         |
| SURROUND    |        |                         |
| SURR+DEPTH  |        |                         |
| SURR+WIDE   |        |                         |
| MONO        |        |                         |

## Discriminators (from the analyzer)
- reshaping  dD = D_surround − D_stereo = ______ dB
- crosstalk  ΔXTALK = surround − stereo = ______ dB
- depth ctrl D: ____ → ____ (grow? __)   XTALK: ____ → ____ dB (grow? __)
- MONO front-vs-rear D = ______ (sanity, should be ~0)

## VERDICT (what the analyzer printed — tick one)
- [ ] **CONFIRMED** — Surround repositions front/back → `span` (0xD7) is audible
      on hardware. (D_stereo flat; surround crosstalk ≥10 dB or reshaping ≥3 dB;
      grows with depth.)
- [ ] **REFUTED** — headphone-surround does not reposition front/back (no
      reshaping/crosstalk/depth growth) → a level/EQ or speaker-only effect;
      `span` inaudible via headphones.
- [ ] **INCONCLUSIVE** — between thresholds. Note whether the 0xFFFF row showed
      any growth, and re-capture with more gain / a longer body if so.

## Anything odd during capture
(clipping, dropouts, the app crashing, headphone-detect flipping, System
Settings not on Surround, etc.)

______________________________________________________________
