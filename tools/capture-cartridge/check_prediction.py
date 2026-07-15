#!/usr/bin/env python3
"""Verify the battery prediction WAV against the cartridge manifest.

Percussive events (drum takes, pan probes, velocity ladder, menu cursor)
must produce a detected onset within 60 ms of the manifest schedule.
Soft events (KEY_FLY's ramped fade-in, the LPF'd slide, the legato
portamento notes) have no sharp edge to detect, so they are verified by
windowed energy at their scheduled position instead. Also asserts the
pan probes' L/R energy split and the velocity ladder's monotonic decay.

Requires numpy (same dependency as tools/console-tolerance).
"""
import sys
import wave

import numpy as np

SHARP = ("SE_NEW_DRUM01", "pan probe", "velocity ladder", "MENU_CURSOR")
ONSET_TOL = 0.060       # s; render quantizes voice starts to 160-sample frames
SOFT_WINDOW = 0.6       # s of energy integration for un-onset-able events
SOFT_FLOOR_DB = -50.0   # windowed RMS must exceed this


def db(x):
    return 10 * np.log10(x + 1e-12)


def main():
    wav_path, manifest_path = sys.argv[1], sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else ""   # e.g. "A" / "B"
    with wave.open(wav_path, "rb") as w:
        rate = w.getframerate()
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    stereo = raw.reshape(-1, 2).astype(np.float64) / 32768.0
    left, right = stereo[:, 0], stereo[:, 1]
    mono = stereo.mean(axis=1)

    expected = []
    for line in open(manifest_path, encoding="utf-8"):
        parts = [p.strip() for p in line.split("|")]
        if len(parts) >= 4 and parts[1].replace(".", "").replace(" ", "").isdigit():
            t, sec, label = float(parts[1]), parts[2], parts[3]
            if sec[:1].isalpha() and sec not in ("setup", "loop") \
                    and sec.startswith(prefix):
                expected.append((t, label))
    if not expected:
        print("FAIL: no schedule rows found in manifest")
        return 1

    # onset detection: rising edge of 5 ms RMS windows out of silence
    win = int(0.005 * rate)
    nw = len(mono) // win
    rms = np.sqrt(np.mean(mono[: nw * win].reshape(nw, win) ** 2, axis=1))
    onsets, quiet = [], True
    for i, v in enumerate(rms):
        if quiet and v > 2e-3 and v > 4 * (rms[max(0, i - 4):i].mean() + 1e-4):
            onsets.append(i * win / rate)
            quiet = False
        elif v < 1e-3:
            quiet = True

    fails = 0
    used = set()
    for t, label in expected:
        if any(k in label for k in SHARP):
            d, o = min(((abs(o - t), o) for o in onsets if o not in used),
                       default=(9e9, None))
            ok = d <= ONSET_TOL
            if ok:
                used.add(o)
            detail = f"onset d={d * 1000:6.1f} ms"
        else:
            seg = mono[int(t * rate):int((t + SOFT_WINDOW) * rate)]
            level = db(np.mean(seg ** 2))
            ok = level > SOFT_FLOOR_DB
            detail = f"energy {level:6.1f} dBFS"
        fails += not ok
        print(f"  {'OK ' if ok else 'MISS'} {t:7.3f}s  {detail}  | {label}")

    pan = [(t, lbl) for t, lbl in expected if "pan probe" in lbl]
    if pan:
        print("\npan probes (L-R energy):")
        for (t, lbl), want in zip(pan, ("L", "R", "C")):
            s, e = int(t * rate), int((t + 0.8) * rate)
            d = db(np.mean(left[s:e] ** 2)) - db(np.mean(right[s:e] ** 2))
            ok = {"L": d > 20, "R": d < -20, "C": abs(d) < 3}[want]
            fails += not ok
            print(f"  {'OK ' if ok else 'MISS'} {d:+7.2f} dB | {lbl}")

    lad = [t for t, lbl in expected if "ladder" in lbl]
    ref = [t for t, lbl in expected if "hit 1/3" in lbl]
    if lad:
        peaks = [float(np.max(np.abs(mono[int(t * rate):int((t + 0.8) * rate)])))
                 for t in ref + lad]
        mono_dec = all(a > b for a, b in zip(peaks, peaks[1:]))
        fails += not mono_dec
        print(f"\nvelocity ladder peaks 127/96/64/32: "
              f"{' '.join(f'{p:.4f}' for p in peaks)} "
              f"({'monotonic' if mono_dec else 'NOT monotonic'})")

    print(f"\nRESULT: {'PASS' if fails == 0 else f'FAIL ({fails})'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
