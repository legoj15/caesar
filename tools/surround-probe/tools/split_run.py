#!/usr/bin/env python3
"""
split_run.py -- cut ONE continuous surround-probe AUTO recording into the
per-condition WAVs that tools/analyze_surround.py expects, plus noise_floor.wav.

The app's AUTO run (press RIGHT) plays, per condition:
    N countable pips (segment N = N pips)  ->  ~0.6 s silence  ->  ~8 s body,
preceded by ~1.5 s of lead-in silence. This script finds each long body, counts
the pips before it to recover the segment index, keys the output name by that
count, and also writes the lead-in silence as noise_floor.wav (the analyzer's
trusted-bin floor). Feed it the whole take, then run analyze_surround.py.

Only dependency is numpy.
Usage:  python3 tools/split_run.py RUN.wav [--outdir .] [--trim 0.4]
"""
import sys, os, struct, argparse
try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy:  python3 -m pip install numpy")

# Must match SCHED[] in source/main.c (order == pip count).
SCHEDULE = [
    "stereo_front.wav", "stereo_rear.wav",
    "surround_front.wav", "surround_rear.wav",
    "surround_front_deep.wav", "surround_rear_deep.wav",
    "surround_front_wide.wav", "surround_rear_wide.wav",
    "mono_front.wav", "mono_rear.wav",
]
BODY_MIN_S = 3.0
PIP_MIN_S, PIP_MAX_S = 0.03, 0.60


def read_wav(path):
    b = open(path, "rb").read()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit(f"{path}: not RIFF/WAVE")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid = b[pos:pos+4]; csz = struct.unpack("<I", b[pos+4:pos+8])[0]; body = b[pos+8:pos+8+csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and csz >= 40:
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        pos += 8 + csz + (csz & 1)
    af, ch, sr, _, _, bits = fmt
    if af == 1:
        if   bits == 16: a = np.frombuffer(data, "<i2").astype(np.float64)/32768.0
        elif bits == 32: a = np.frombuffer(data, "<i4").astype(np.float64)/2147483648.0
        elif bits == 8:  a = (np.frombuffer(data, np.uint8).astype(np.float64)-128)/128.0
        elif bits == 24:
            u = np.frombuffer(data, np.uint8); u = u[:len(u)-(len(u)%3)].reshape(-1,3).astype(np.int32)
            v = u[:,0]|(u[:,1]<<8)|(u[:,2]<<16); v = np.where(v>=(1<<23), v-(1<<24), v); a = v/8388608.0
        else: sys.exit(f"{path}: PCM bits {bits}?")
    elif af == 3:
        a = np.frombuffer(data, "<f4").astype(np.float64) if bits==32 else np.frombuffer(data,"<f8").copy()
    else: sys.exit(f"{path}: fmt tag {af}")
    n = (a.size//ch)*ch
    return sr, ch, a[:n].reshape(-1, ch)


def write_wav_f32(path, sr, x):
    x = np.ascontiguousarray(x, "<f4"); ch = x.shape[1]; data = x.tobytes()
    hdr = b"RIFF"+struct.pack("<I",36+len(data))+b"WAVE"
    hdr += b"fmt "+struct.pack("<IHHIIHH",16,3,ch,sr,sr*ch*4,ch*4,32)
    hdr += b"data"+struct.pack("<I",len(data))
    open(path,"wb").write(hdr+data)


FR_S = 0.005              # envelope frame (5 ms)
CLOSE_GAP_S = 0.40        # bridge sub-gaps this short when finding BODIES
                          # (< the 0.6 s inter-segment silences, > any within-
                          #  body crest dip and the 0.133 s pip gaps)

def _env(mono, sr):
    fr = max(1, int(FR_S*sr)); nf = mono.size//fr
    if nf < 4: sys.exit("recording too short")
    env = np.sqrt(np.mean(np.square(mono[:nf*fr].reshape(nf,fr)), axis=1))
    kw = max(1, int(0.055/FR_S))                       # ~55 ms smoothing
    env = np.convolve(env, np.ones(kw)/kw, mode="same")
    return fr, nf, env

def _runs_from_mask(mask, fr, sr):
    runs, inrun, start = [], False, 0
    for i, on in enumerate(mask):
        if on and not inrun: inrun, start = True, i
        elif not on and inrun: runs.append((start*fr, i*fr, (i-start)*fr/sr)); inrun = False
    if inrun: runs.append((start*fr, len(mask)*fr, (len(mask)-start)*fr/sr))
    return runs

def detect(mono, sr):
    """Two passes: FINE runs (for counting pips) and BODIES (gap-closed, robust
    to within-body envelope dips from a high-crest broadband probe)."""
    fr, nf, env = _env(mono, sr)
    peak = float(np.max(env))+1e-12
    hi, lo = 0.30*peak, 0.12*peak
    # fine (hysteresis) runs -> pip counting
    fine, inrun, start = [], False, 0
    for i in range(nf):
        if not inrun and env[i] >= hi: inrun, start = True, i
        elif inrun and env[i] < lo: fine.append((start*fr, i*fr, (i-start)*fr/sr)); inrun = False
    if inrun: fine.append((start*fr, nf*fr, (nf-start)*fr/sr))
    # body mask: tone-present, then close gaps shorter than CLOSE_GAP_S
    mask = env >= lo
    g = int(CLOSE_GAP_S/FR_S); i = 0
    while i < nf:
        if not mask[i]:
            j = i
            while j < nf and not mask[j]: j += 1
            if 0 < i and j < nf and (j-i) < g: mask[i:j] = True   # bridge interior sub-gap
            i = j
        else:
            i += 1
    bodies = [r for r in _runs_from_mask(mask, fr, sr) if r[2] >= BODY_MIN_S]
    return fine, bodies


def main():
    ap = argparse.ArgumentParser(description="split one surround-probe AUTO run")
    ap.add_argument("run_wav"); ap.add_argument("--outdir", default=".")
    ap.add_argument("--trim", type=float, default=0.4)
    args = ap.parse_args()

    sr, ch, x = read_wav(args.run_wav)
    mono = x.mean(axis=1)
    runs, bodies = detect(mono, sr)
    os.makedirs(args.outdir, exist_ok=True)

    print(f"\n=== split_run: {args.run_wav} ({x.shape[0]/sr:.1f}s, {ch}ch, {sr} Hz) ===")
    print(f"found {len(runs)} tone runs, {len(bodies)} bodies (>= {BODY_MIN_S:.0f}s)\n")
    if len(bodies) != len(SCHEDULE):
        print(f"!! WARNING: expected {len(SCHEDULE)} bodies, found {len(bodies)}. "
              f"Naming by pip count regardless.\n")

    # noise_floor.wav from the lead-in silence before the first tone run
    first = runs[0][0] if runs else x.shape[0]
    nf_end = max(0, first - int(0.10*sr)); nf_start = max(0, nf_end - int(1.0*sr))
    if nf_end - nf_start >= int(0.2*sr):
        write_wav_f32(os.path.join(args.outdir, "noise_floor.wav"), sr, x[nf_start:nf_end])
        print(f"  noise_floor.wav  <- lead-in [{nf_start/sr:.2f}..{nf_end/sr:.2f}]s")
    else:
        print("  !! lead-in too short for noise_floor.wav (analyzer will fall back)")

    trim = int(args.trim*sr); prev_end = 0
    for oi, (bs, be, bsec) in enumerate(bodies):
        pips = [r for r in runs if prev_end <= r[0] < bs and PIP_MIN_S <= r[2] <= PIP_MAX_S]
        npips = len(pips)
        if 1 <= npips <= len(SCHEDULE):
            name = SCHEDULE[npips-1]
        else:
            name = f"segment_pip{npips}.wav"
            print(f"  !! body {oi+1}: pip count {npips} out of range; parked as {name}")
        s, e = bs+trim, be-trim
        if e-s < int(1.0*sr): s, e = bs, be
        write_wav_f32(os.path.join(args.outdir, name), sr, x[s:e])
        print(f"  {name:26s} <- body {oi+1} ({bsec:.1f}s tone, {npips} pips)")
        prev_end = be

    print(f"\nnext: python3 tools/analyze_surround.py {args.outdir}\n")


if __name__ == "__main__":
    main()
