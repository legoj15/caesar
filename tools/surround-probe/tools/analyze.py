#!/usr/bin/env python3
"""
analyze.py -- null-test / difference analysis for the 3DS surround-probe captures.

Compares TWO recordings of the 3DS headphone jack (e.g. FRONT-only vs REAR-only,
or STEREO-front vs SURROUND-front). Aligns them sample-accurately on the leading
marker click, then reports:
  * per-channel RMS level (dBFS) of each recording
  * intra-recording L/R correlation (1.0 = mono/identical channels)
  * best-case null depth (dB) of A-B after fractional-sample alignment
  * a difference-spectrum summary (where the residual energy lives)

Only dependency is numpy.  Reads 16/24/32-bit PCM and 32/64-bit float WAV.

Usage:
    python3 analyze.py FILE_A.wav FILE_B.wav
    python3 analyze.py FILE_A.wav FILE_B.wav --expect null      # front-vs-rear in STEREO
    python3 analyze.py FILE_A.wav FILE_B.wav --expect differ     # front-vs-rear in SURROUND
    python3 analyze.py FILE_A.wav FILE_B.wav --tone 440 --win 3.0

The two files must be the SAME sample rate and channel count (stereo expected).
"""
import sys, struct, argparse

try:
    import numpy as np
except ImportError:
    sys.exit("This script needs numpy.  Install it with:  python3 -m pip install numpy")


# ---------------------------------------------------------------- WAV reading
def read_wav(path):
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(buf):
        cid = buf[pos:pos + 4]
        csz = struct.unpack("<I", buf[pos + 4:pos + 8])[0]
        body = buf[pos + 8:pos + 8 + csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and csz >= 40:           # WAVE_FORMAT_EXTENSIBLE
                sub = struct.unpack("<H", body[24:26])[0]  # first 2 bytes of SubFormat GUID
                fmt = (sub, fmt[1], fmt[2], fmt[3], fmt[4], fmt[5])
        elif cid == b"data":
            data = body
        pos += 8 + csz + (csz & 1)
    if fmt is None or data is None:
        sys.exit(f"{path}: missing fmt/data chunk")
    af, ch, sr, _, _, bits = fmt
    if af == 1:                                          # integer PCM
        if bits == 16:
            a = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
        elif bits == 32:
            a = np.frombuffer(data, dtype="<i4").astype(np.float64) / 2147483648.0
        elif bits == 8:
            a = (np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128) / 128.0
        elif bits == 24:
            b = np.frombuffer(data, dtype=np.uint8)
            b = b[:len(b) - (len(b) % 3)].reshape(-1, 3).astype(np.int32)
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            v = np.where(v >= (1 << 23), v - (1 << 24), v)
            a = v.astype(np.float64) / 8388608.0
        else:
            sys.exit(f"{path}: unsupported PCM bit depth {bits}")
    elif af == 3:                                        # IEEE float
        if bits == 32:
            a = np.frombuffer(data, dtype="<f4").astype(np.float64)
        elif bits == 64:
            a = np.frombuffer(data, dtype="<f8").copy()
        else:
            sys.exit(f"{path}: unsupported float bit depth {bits}")
    else:
        sys.exit(f"{path}: unsupported WAV format tag 0x{af:04X}")
    n = (a.size // ch) * ch
    return sr, a[:n].reshape(-1, ch)


# ---------------------------------------------------------------- helpers
def dbfs(x):
    r = float(np.sqrt(np.mean(np.square(x)))) if x.size else 0.0
    return 20.0 * np.log10(r) if r > 1e-12 else -np.inf


def find_marker(mono, sr):
    """Locate the marker: the tone-RESUME onset right after the ~150 ms silence
    gap the app inserts on each setting change. Robust to recordings that start
    mid-tone (finds a low-energy run then the return to tone). Falls back to
    0.2 s if no clean gap is present."""
    fr = max(1, int(0.005 * sr))                 # 5 ms analysis frames
    nf = mono.size // fr
    if nf < 8:
        return int(0.2 * sr)
    env = np.sqrt(np.mean(np.square(mono[:nf * fr].reshape(nf, fr)), axis=1))
    peak = float(np.max(env)) + 1e-9
    hi, lo = 0.30 * peak, 0.08 * peak            # tone-present / silence thresholds
    min_gap = max(2, int(0.05 * sr / fr))        # require >= ~50 ms of silence
    run = 0
    for i in range(nf):
        if env[i] < lo:
            run += 1
        elif env[i] >= hi and run >= min_gap:    # low run then return to tone
            return i * fr
        else:
            run = 0
    idx = int(np.argmax(env > hi))               # fallback: first tone onset
    return idx * fr if idx > 0 else int(0.2 * sr)


def frac_delay(x, tau):
    """Shift x by tau samples (fractional) via FFT phase ramp."""
    n = x.size
    f = np.fft.rfft(x)
    k = np.arange(f.size)
    f = f * np.exp(-2j * np.pi * k * tau / n)
    return np.fft.irfft(f, n)


def align(a, b, sr):
    """Coarse (marker) + fine (residual-min) alignment of two mono windows."""
    # coarse: integer lag by cross-correlation of a short slice
    seg = min(a.size, b.size, int(1.0 * sr))
    fa = a[:seg] - a[:seg].mean()
    fb = b[:seg] - b[:seg].mean()
    xc = np.correlate(fa, fb, mode="full")
    # Lock onto the IN-PHASE (positive) correlation peak, not argmax(|xc|):
    # for a pure tone the latter can latch the anti-phase (negative) peak and
    # then report a spurious ~+6 dB "difference" for two identical signals.
    lag = int(np.argmax(xc) - (seg - 1))
    if lag > 0:
        b2 = np.concatenate([np.zeros(lag), b])[:a.size]
    elif lag < 0:
        b2 = b[-lag:]
    else:
        b2 = b.copy()
    m = min(a.size, b2.size)
    a2, b2 = a[:m].copy(), b2[:m].copy()
    # fine: search fractional delay minimizing residual RMS
    best_tau, best_res = 0.0, np.inf
    for tau in np.arange(-1.0, 1.0001, 0.05):
        res = float(np.sqrt(np.mean(np.square(a2 - frac_delay(b2, tau)))))
        if res < best_res:
            best_res, best_tau = res, tau
    return a2, frac_delay(b2, best_tau), lag, best_tau


def diff_spectrum(d, sr, fundamental):
    """Summarize where the residual energy sits."""
    n = d.size
    w = np.hanning(n)
    mag = np.abs(np.fft.rfft(d * w)) / (n / 2)
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    peak = int(np.argmax(mag[1:]) + 1)
    bands = [(20, 200), (200, 2000), (2000, 8000), (8000, sr // 2)]
    out = []
    for lo, hi in bands:
        sel = (freqs >= lo) & (freqs < hi)
        e = float(np.sqrt(np.mean(np.square(mag[sel])))) if sel.any() else 0.0
        out.append((lo, hi, 20 * np.log10(e) if e > 1e-12 else -np.inf))
    return freqs[peak], out


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="3DS surround-probe null/difference analysis")
    ap.add_argument("file_a")
    ap.add_argument("file_b")
    ap.add_argument("--tone", type=float, default=440.0, help="expected tone Hz (default 440)")
    ap.add_argument("--win", type=float, default=3.0, help="analysis window seconds (default 3.0)")
    ap.add_argument("--skip", type=float, default=0.5, help="seconds to skip after marker (default 0.5)")
    ap.add_argument("--expect", choices=["null", "differ"], help="print PASS/FAIL vs this expectation")
    args = ap.parse_args()

    sr_a, A = read_wav(args.file_a)
    sr_b, B = read_wav(args.file_b)
    if sr_a != sr_b:
        sys.exit(f"sample-rate mismatch: {sr_a} vs {sr_b}")
    sr = sr_a
    if A.shape[1] != B.shape[1]:
        sys.exit(f"channel-count mismatch: {A.shape[1]} vs {B.shape[1]}")
    ch = A.shape[1]

    print(f"\n=== 3DS surround-probe analysis ===")
    print(f"A: {args.file_a}   ({A.shape[0]/sr:.2f}s, {ch}ch, {sr} Hz)")
    print(f"B: {args.file_b}   ({B.shape[0]/sr:.2f}s, {ch}ch, {sr} Hz)")

    # marker alignment on the L+R mono mix
    monoA, monoB = A.mean(axis=1), B.mean(axis=1)
    mA, mB = find_marker(monoA, sr), find_marker(monoB, sr)
    s0 = int(args.skip * sr)
    wlen = int(args.win * sr)
    segA = monoA[mA + s0: mA + s0 + wlen]
    segB = monoB[mB + s0: mB + s0 + wlen]
    if segA.size < sr or segB.size < sr:
        sys.exit("analysis window too short -- captures shorter than expected? try --win 1.0")
    a2, b2, lag, tau = align(segA, segB, sr)

    print(f"\n-- alignment --")
    print(f"marker A @ {mA/sr:.3f}s   marker B @ {mB/sr:.3f}s   coarse lag {lag} smp   fine {tau:+.3f} smp")

    print(f"\n-- per-recording levels (dBFS RMS) --")
    for name, X in (("A", A), ("B", B)):
        chs = " ".join(f"ch{i}:{dbfs(X[:, i]):6.1f}" for i in range(ch))
        if ch == 2:
            lr = float(np.corrcoef(X[int(mA+s0):int(mA+s0+wlen), 0] if name == "A" else X[int(mB+s0):int(mB+s0+wlen), 0],
                                   X[int(mA+s0):int(mA+s0+wlen), 1] if name == "A" else X[int(mB+s0):int(mB+s0+wlen), 1])[0, 1])
            chs += f"   L/R corr: {lr:+.3f}"
        print(f"  {name}: {chs}")

    # null test on aligned mono tone window
    ref = float(np.sqrt(np.mean(np.square(a2))))
    d = a2 - b2
    dr = float(np.sqrt(np.mean(np.square(d))))
    null_db = 20 * np.log10(dr / ref) if ref > 1e-12 and dr > 1e-12 else -np.inf
    print(f"\n-- null test  (difference A-B, best fractional alignment) --")
    print(f"  reference RMS (A): {20*np.log10(ref):.1f} dBFS")
    print(f"  residual  RMS    : {20*np.log10(dr) if dr>1e-12 else -np.inf:.1f} dBFS")
    print(f"  NULL DEPTH       : {null_db:+.1f} dB   (very negative = signals identical)")

    pf, spec = diff_spectrum(d, sr, args.tone)
    print(f"\n-- difference spectrum --")
    print(f"  residual peak near {pf:.0f} Hz")
    for lo, hi, e in spec:
        print(f"    {lo:5d}-{hi:5d} Hz: {e:6.1f} dB")

    if args.expect:
        print(f"\n-- verdict (--expect {args.expect}) --")
        DEEP, SHALLOW = -25.0, -15.0          # < DEEP = nulled; > SHALLOW = clearly differs
        if null_db < DEEP:
            state = "DEEP NULL (signals effectively identical)"
        elif null_db > SHALLOW:
            state = "NO NULL (signals clearly differ)"
        else:
            state = "PARTIAL (inconclusive - noise/alignment territory)"
        want_null = (args.expect == "null")
        if want_null:
            tag = "PASS" if null_db < DEEP else ("FAIL" if null_db > SHALLOW else "INCONCLUSIVE")
            hint = "STEREO front-vs-rear should DEEP-NULL: rear folds into front at unity."
        else:
            tag = "PASS" if null_db > SHALLOW else ("FAIL" if null_db < DEEP else "INCONCLUSIVE")
            hint = "SURROUND front-vs-rear should NOT null: virtualization repositions the voice."
        print(f"  {tag}: {state}  (null depth {null_db:+.1f} dB)")
        print(f"  {hint}")
        print("  NOTE: the decisive evidence is RELATIVE -- STEREO should null far")
        print("        deeper than SURROUND. Compare the two depths, not just one.")
    print()


if __name__ == "__main__":
    main()
