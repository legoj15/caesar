#!/usr/bin/env python3
"""
split_run.py -- cut ONE continuous surround-probe AUTO-run recording into the
standard per-condition WAVs that tools/analyze.py expects.

The app's AUTO run (press RIGHT) plays, for each condition in a fixed schedule:
    N countable pips (segment N = N pips)  ->  ~0.6 s silence  ->  ~8 s tone body.
This script finds each long tone body, counts the pips that precede it to recover
the segment index, trims the body edges, and writes it out under the schedule's
filename. Feed the whole AUTO run (one long stereo WAV) to this once; then run the
analyze.py commands in AUTO-RUN.md verbatim.

Only dependency is numpy.

Usage:
    python3 tools/split_run.py RUN.wav [--outdir .] [--trim 0.4] [--plot]
"""
import sys, os, struct, argparse

try:
    import numpy as np
except ImportError:
    sys.exit("This script needs numpy.  Install it with:  python3 -m pip install numpy")

# Must match SCHED[] in source/main.c (order == pip count).
SCHEDULE = [
    "stereo_front.wav", "stereo_rear.wav",
    "surround_front.wav", "surround_rear.wav",
    "surround_front_wide.wav", "surround_rear_wide.wav",
    "mono_front.wav", "mono_rear.wav",
]
BODY_MIN_S = 3.0      # a tone run this long or longer is a segment body
PIP_MIN_S  = 0.03     # shorter tone runs are pips (ignore anything below this: noise)
PIP_MAX_S  = 0.60     # ... and shorter than this


# ---------------------------------------------------------------- WAV IO
def read_wav(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]
        csz = struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and csz >= 40:
                sub = struct.unpack("<H", body[24:26])[0]
                fmt = (sub, fmt[1], fmt[2], fmt[3], fmt[4], fmt[5])
        elif cid == b"data":
            data = body
        pos += 8 + csz + (csz & 1)
    if fmt is None or data is None:
        sys.exit(f"{path}: missing fmt/data chunk")
    af, ch, sr, _, _, bits = fmt
    if af == 1:
        if   bits == 16: a = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
        elif bits == 32: a = np.frombuffer(data, dtype="<i4").astype(np.float64) / 2147483648.0
        elif bits == 8:  a = (np.frombuffer(data, dtype=np.uint8).astype(np.float64) - 128) / 128.0
        elif bits == 24:
            u = np.frombuffer(data, dtype=np.uint8)
            u = u[:len(u) - (len(u) % 3)].reshape(-1, 3).astype(np.int32)
            v = u[:, 0] | (u[:, 1] << 8) | (u[:, 2] << 16)
            v = np.where(v >= (1 << 23), v - (1 << 24), v)
            a = v.astype(np.float64) / 8388608.0
        else: sys.exit(f"{path}: unsupported PCM bit depth {bits}")
    elif af == 3:
        if   bits == 32: a = np.frombuffer(data, dtype="<f4").astype(np.float64)
        elif bits == 64: a = np.frombuffer(data, dtype="<f8").copy()
        else: sys.exit(f"{path}: unsupported float bit depth {bits}")
    else:
        sys.exit(f"{path}: unsupported WAV format tag 0x{af:04X}")
    n = (a.size // ch) * ch
    return sr, ch, a[:n].reshape(-1, ch)


def write_wav_f32(path, sr, x):
    x = np.ascontiguousarray(x, dtype="<f4")
    ch = x.shape[1]
    data = x.tobytes()
    byte_rate = sr * ch * 4
    block = ch * 4
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 3, ch, sr, byte_rate, block, 32)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr + data)


# ---------------------------------------------------------------- segmentation
def tone_runs(mono, sr):
    """Return list of (start_sample, end_sample, seconds) tone runs, using a
    hysteresis threshold on a 5 ms energy envelope."""
    fr = max(1, int(0.005 * sr))
    nf = mono.size // fr
    if nf < 4:
        sys.exit("recording too short")
    env = np.sqrt(np.mean(np.square(mono[:nf * fr].reshape(nf, fr)), axis=1))
    peak = float(np.max(env)) + 1e-12
    hi, lo = 0.30 * peak, 0.12 * peak
    runs, inrun, start = [], False, 0
    for i in range(nf):
        if not inrun and env[i] >= hi:
            inrun, start = True, i
        elif inrun and env[i] < lo:
            runs.append((start * fr, i * fr, (i - start) * fr / sr))
            inrun = False
    if inrun:
        runs.append((start * fr, nf * fr, (nf - start) * fr / sr))
    return runs


def main():
    ap = argparse.ArgumentParser(description="split one surround-probe AUTO run into per-condition WAVs")
    ap.add_argument("run_wav")
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--trim", type=float, default=0.4, help="seconds to trim off each end of a body (default 0.4)")
    args = ap.parse_args()

    sr, ch, x = read_wav(args.run_wav)
    mono = x.mean(axis=1)
    runs = tone_runs(mono, sr)

    bodies = [r for r in runs if r[2] >= BODY_MIN_S]
    print(f"\n=== split_run: {args.run_wav} ({x.shape[0]/sr:.1f}s, {ch}ch, {sr} Hz) ===")
    print(f"found {len(runs)} tone runs, {len(bodies)} bodies (>= {BODY_MIN_S:.0f}s)\n")

    if len(bodies) != len(SCHEDULE):
        print(f"!! WARNING: expected {len(SCHEDULE)} bodies, found {len(bodies)}.")
        print("   Check the recording covers the whole run (lead-in .. last body).")
        print("   Proceeding by order for whatever bodies were found.\n")

    trim = int(args.trim * sr)
    os.makedirs(args.outdir, exist_ok=True)
    prev_end = 0
    for idx, (bs, be, bsec) in enumerate(bodies):
        # count pips = short tone runs between the previous body and this one
        pips = [r for r in runs if prev_end <= r[0] < bs and PIP_MIN_S <= r[2] <= PIP_MAX_S]
        npips = len(pips)
        name = SCHEDULE[idx] if idx < len(SCHEDULE) else f"segment_{idx+1}.wav"
        s, e = bs + trim, be - trim
        if e - s < int(1.0 * sr):
            s, e = bs, be
        out = os.path.join(args.outdir, name)
        write_wav_f32(out, sr, x[s:e])
        flag = "" if npips == idx + 1 else f"  <-- pips={npips}, expected {idx+1} (order used)"
        print(f"  seg {idx+1}: {name:26s} {bsec:5.1f}s tone, {npips} pips{flag}")
        prev_end = be

    print(f"\nwrote {len(bodies)} files to {args.outdir}/")
    print("next: run the analyze.py commands in AUTO-RUN.md\n")


if __name__ == "__main__":
    main()
