#!/usr/bin/env python3
"""Numerical schedule check for the battery-v2 prediction WAVs (gate step).

Reads the v2 MANIFEST probe schedule and asserts, against the recalibrated
caesar_play render, that every probe lands where scheduled and behaves as
designed:

  - pilot tone present at tick 0 (the per-session normalisation reference);
  - percussive/onset-able probes (pan, LPF, velocity, glide origins) detected
    within tolerance of the scheduled tick;
  - track A pan curve monotone in L-R split, and the 32/96 points sit BETWEEN
    center and the hard edges (the cos-sin vs sqrt-poly discriminators);
  - track A velocity ladder monotone-decreasing (the (vel/127)^2 law);
  - track A steal probe: >= 24 distinct pitches sound together (pool-saturated);
  - track B release curve monotone-faster with the byte (60<100<114<124).

numpy only (same dependency as tools/console-tolerance). The manifest is parsed
for section/tick anchors so the checks stay keyed to the exact build.
"""
import re
import sys
import wave

import numpy as np

ONSET_TOL = 0.070       # s; render quantises voice starts to ~160-sample frames


def db(x):
    return 10.0 * np.log10(np.asarray(x) + 1e-12)


def load(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    st = raw.reshape(-1, 2).astype(np.float64) / 32768.0
    return rate, st[:, 0], st[:, 1], st.mean(axis=1)


def parse_manifest(path, letter):
    """Return [(tick_seconds, section, label)] for the given track letter."""
    rows = []
    in_track = False
    want = f"## Track {letter} probe schedule"
    for line in open(path, encoding="utf-8"):
        if line.startswith("## Track "):
            in_track = line.startswith(want)
            continue
        if not in_track:
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) >= 6 and re.match(r"^\d+\.\d+$", parts[1]):
            rows.append((float(parts[1]), parts[3], parts[4]))
    return rows


def detect_onsets(mono, rate):
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
    return onsets


def band_rms(seg, rate, lo, hi):
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    f = np.fft.rfftfreq(len(seg), 1.0 / rate)
    m = (f >= lo) & (f < hi)
    return np.sqrt(np.mean(sp[m] ** 2)) if m.any() else 0.0


def main():
    wav_path, manifest_path, letter = sys.argv[1], sys.argv[2], sys.argv[3]
    rate, left, right, mono = load(wav_path)
    rows = parse_manifest(manifest_path, letter)
    if not rows:
        print("FAIL: no schedule rows for track", letter)
        return 1

    fails = 0

    # 1) pilot present at tick 0
    pilot = [r for r in rows if r[1] == "PILOT"]
    if pilot:
        seg = mono[int(pilot[0][0] * rate):int((pilot[0][0] + 0.8) * rate)]
        lvl = db(np.mean(seg ** 2))
        ok = lvl > -40.0
        fails += not ok
        print(f"  {'OK ' if ok else 'FAIL'} pilot tone @ {pilot[0][0]:.3f}s  "
              f"{lvl:6.1f} dBFS")
    else:
        fails += 1
        print("  FAIL pilot tone not in manifest")

    onsets = detect_onsets(mono, rate)

    # 2) onset-able probes: pan, velocity, glide origins (sharp attacks). LPF
    # probes are heavily filtered (soft/quiet attack, by design) so they are
    # checked spectrally below, not by onset.
    used = set()
    onsetable = ("pan probe", "velocity", "[origin]")
    for t, sec, label in rows:
        if not any(k in label for k in onsetable):
            continue
        d, o = min(((abs(o - t), o) for o in onsets if o not in used),
                   default=(9e9, None))
        ok = d <= ONSET_TOL
        if ok:
            used.add(o)
        fails += not ok
        print(f"  {'OK ' if ok else 'FAIL'} onset {t:7.3f}s d={d * 1000:6.1f}ms "
              f"| {label}")

    if letter == "A":
        # 3) pan curve: byte 0 -> RIGHT, 127 -> LEFT (the console-confirmed
        # inverted direction, player fix 9769a96 -- this check predates that
        # fix and used to expect the opposite slope), so the L-R split is
        # monotone-INCREASING with the byte; 32/96 strictly between center &
        # edge.
        pans = [(t, lbl) for t, _, lbl in rows if "pan probe" in lbl]
        splits = []
        for t, lbl in pans:
            s, e = int((t + 0.2) * rate), int((t + 1.0) * rate)
            dl = db(np.mean(left[s:e] ** 2)) - db(np.mean(right[s:e] ** 2))
            splits.append(dl)
            print(f"      pan {lbl.split()[-1]:>3}: L-R {dl:+7.2f} dB")
        if len(splits) == 5:
            mono_inc = all(a < b for a, b in zip(splits, splits[1:]))
            disc = (splits[0] < splits[1] < splits[2] < splits[3] < splits[4]
                    and abs(splits[2]) < 3
                    and 2 < -splits[1] < abs(splits[0])
                    and 2 < splits[3] < abs(splits[4]))
            fails += not (mono_inc and disc)
            print(f"  {'OK ' if mono_inc and disc else 'FAIL'} pan curve monotone "
                  f"+ 32/96 between center and edges")
        else:
            fails += 1
            print(f"  FAIL pan curve has {len(splits)} points")

        # 3b) LPF curve: HF band energy monotone-increasing with the cutoff
        # byte (24 < 40 < 48 < 64-open) -> the corner opens as the byte rises.
        lpf = [(t, lbl) for t, _, lbl in rows if "LPF cutoff" in lbl]
        hf = []
        for t, lbl in lpf:
            seg = mono[int((t + 0.3) * rate):int((t + 1.5) * rate)]
            hf.append(db(band_rms(seg, rate, 6000, 12000)))
            print(f"      LPF {lbl.split()[-1]:>3}: 6-12k band {hf[-1]:+7.1f} dB")
        if len(hf) == 4:
            minc = all(a < b for a, b in zip(hf, hf[1:]))
            fails += not minc
            print(f"  {'OK ' if minc else 'FAIL'} LPF corner opens with the "
                  f"cutoff byte (HF monotone-increasing)")
        else:
            fails += 1
            print(f"  FAIL LPF curve has {len(hf)} points")

        # 4) velocity ladder monotone-decreasing
        vels = [(t, lbl) for t, _, lbl in rows if "velocity" in lbl]
        peaks = [float(np.max(np.abs(mono[int(t * rate):int((t + 0.7) * rate)])))
                 for t, _ in vels]
        mdec = all(a > b for a, b in zip(peaks, peaks[1:]))
        fails += not mdec
        print(f"  {'OK ' if mdec else 'FAIL'} velocity ladder peaks "
              f"{' '.join(f'{p:.3f}' for p in peaks)} "
              f"({'monotone' if mdec else 'NOT monotone'})")

        # 5) steal probe present + loud (many voices). The AUTHORITATIVE voice
        # count (peak 24/24, stolen>0) is asserted by the driver from the render
        # log; here just confirm the stacked region sounds well above the pilot.
        steal = [t for t, _, lbl in rows if "steal saturation" in lbl]
        if steal:
            s = int((steal[0] + 0.2) * rate)
            seg = mono[s:s + int(1.5 * rate)]
            lvl = db(np.mean(seg ** 2))
            ok = lvl > -25.0
            fails += not ok
            print(f"  {'OK ' if ok else 'FAIL'} steal region energy {lvl:6.1f} "
                  f"dBFS (dense voice cluster present)")

    else:
        # 6) release curve: higher byte -> faster release (shorter -20 dB time)
        rel = [(t, lbl) for t, _, lbl in rows if "release byte" in lbl]
        times = []
        for t, lbl in rel:
            off = t + 2.5   # note-off at note length end (240 ticks = 2.5 s)
            seg = mono[int(off * rate):int((off + 5.0) * rate)]
            hop = int(0.02 * rate)
            n = len(seg) // hop
            e = np.sqrt(np.mean(seg[: n * hop].reshape(n, hop) ** 2, axis=1))
            e0 = e[1] if len(e) > 1 else e[0]
            below = np.where(db(e) < db(e0) - 20)[0]
            td = below[0] * hop / rate if len(below) else 9e9
            times.append(td)
            print(f"      {lbl.split('(')[0].strip()}: -20 dB after {td:.3f}s")
        mdec = all(a > b for a, b in zip(times, times[1:]))
        fails += not mdec
        print(f"  {'OK ' if mdec else 'FAIL'} release curve monotone-faster "
              f"with byte (60>100>114>124)")

    print(f"\nRESULT track {letter}: {'PASS' if fails == 0 else f'FAIL ({fails})'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
