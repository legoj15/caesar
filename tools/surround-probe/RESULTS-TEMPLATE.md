# surround-probe — results

Fill this in and paste it back into the caesar project session.

## Rig
- Console: New 3DS ___________ (region/model)  •  CFW: Luma3DS ______
- System Settings sound mode (as shown by app): __________
- App output mode forced via ndsp: (independent of the above — that's the point)
- Interface: __________  •  record format: ____ kHz / ____ bit
- 3DS volume slider position (fixed all captures): ______
- Headphone-detect state the app reported during captures: __________
  (expected "plugged", since we capture from the jack — see protocol)

## Core matrix — raw levels (from analyze.py "per-recording levels")
| Capture              | ch0 dBFS | ch1 dBFS | L/R corr |
|----------------------|----------|----------|----------|
| stereo_front.wav     |          |          |          |
| stereo_rear.wav      |          |          |          |
| surround_front.wav   |          |          |          |
| surround_rear.wav    |          |          |          |

## Headline null tests
| Comparison                                   | null depth (dB) | expect | PASS/FAIL |
|----------------------------------------------|-----------------|--------|-----------|
| stereo_front vs stereo_rear                  |                 | null   |           |
| surround_front vs surround_rear              |                 | differ |           |
| stereo_front vs surround_front (coloration)  |                 |  —     |    n/a    |
| mono_front vs mono_rear (sanity, optional)   |                 | null   |           |

## Difference-spectrum notes
For `surround_front vs surround_rear` and `stereo_front vs surround_front`, record
where the residual energy sits (from the "difference spectrum" block):

- surround_front vs surround_rear — residual peak near ____ Hz; band levels:
  20–200 ____  / 200–2k ____ / 2k–8k ____ / 8k+ ____ dB
- stereo_front vs surround_front — residual peak near ____ Hz; band levels:
  20–200 ____  / 200–2k ____ / 2k–8k ____ / 8k+ ____ dB

## Optional: position / rear-ratio
- SQUARE vs WIDE (surround_front vs surround_front_wide): null depth ____ dB
- rear-ratio low hex ____ / high hex ____ ; rrlo vs rrhi null depth ____ dB

## Bottom line (tick one)
- [ ] **Confirmed**: STEREO routing nulls deep AND SURROUND routing does not →
      `span` is audible on hardware only in Surround mode. (Expected outcome.)
- [ ] **Surprise**: STEREO did NOT null / SURROUND DID null / other — describe:

      ______________________________________________________________

## Anything odd during capture
(clipping, dropouts, the app crashing, headphone-detect flipping, etc.)

______________________________________________________________
