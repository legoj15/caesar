#!/usr/bin/env python3
"""
analyze_surround.py -- v2 verdict for the surround-probe AUTO capture.

Ingests the per-condition WAVs that tools/split_run.py cut from one AUTO run
(plus noise_floor.wav) and decides, on real hardware, whether the 3DS Surround
output mode repositions a voice front vs back -- i.e. whether the NW4C `span`
(SurroundPan / 0xD7) command is audible.

It is ALIGNMENT-FREE (all metrics are power-spectrum ratios, so the arbitrary
per-segment circular phase offset of the looping probe is harmless) and never
mono-sums: everything is per channel (L, R).

Within an output mode it compares the FRONT (FL-only) capture X against the
REAR (BL-only) capture Y:

  PRIMARY  MAGDEV_c = rms over trusted bins of ( 20log10|H_c| - median ),
           H_c(f) = sqrt(Syy_c / Sxx_c);  D_mode = mean MAGDEV over channels
           that have trusted bins.  Median-subtraction removes any flat level
           offset, so a pure gain change reads 0 -- only spectral RESHAPING
           (HRTF coloration) drives D up.

  STRONG   XTALK_dB = 10log10( sum_{L-trusted} Sxx_R / sum Sxx_L )  per capture.
  CORROB.  For a single-corner (hard-left) route R is silent unless the mode
           bleeds energy across -- so XTALK rises from the analog floor in
           Stereo to elevated in Surround iff the virtualizer spreads the voice.

Verdict is RELATIVE (floor and the ~+2 dB Surround level offset cancel), and
gated by the depth positive-control (0x7FFF vs 0xFFFF) so a null is
distinguishable from residual blindness.

Only dependency is numpy.
Usage:  python3 tools/analyze_surround.py [capture_dir]   (default: .)
"""
import sys, os, struct
try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy:  python3 -m pip install numpy")

NFFT = 16384
SNR_TRUST_DB = 15.0        # a bin is "trusted" if >= floor + this
FLO, FHI = 100.0, 14000.0

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
        elif bits == 24:
            u = np.frombuffer(data, np.uint8); u = u[:len(u)-(len(u)%3)].reshape(-1,3).astype(np.int32)
            v = u[:,0]|(u[:,1]<<8)|(u[:,2]<<16); v = np.where(v>=(1<<23), v-(1<<24), v); a = v/8388608.0
        else: sys.exit(f"{path}: PCM bits {bits}?")
    elif af == 3:
        a = np.frombuffer(data, "<f4").astype(np.float64) if bits==32 else np.frombuffer(data,"<f8").copy()
    else: sys.exit(f"{path}: fmt tag {af}")
    n = (a.size//ch)*ch
    return sr, a[:n].reshape(-1, ch)

def welch(x, sr):
    """Per-channel Welch PSD (Hann NFFT, 50% overlap). Returns freqs, PSD[bins]."""
    w = np.hanning(NFFT); step = NFFT//2
    if x.size < NFFT:
        x = np.pad(x, (0, NFFT-x.size))
    nseg = 1 + (x.size - NFFT)//step
    acc = np.zeros(NFFT//2+1)
    for i in range(nseg):
        seg = x[i*step:i*step+NFFT]*w
        acc += np.abs(np.fft.rfft(seg))**2
    acc /= max(nseg,1)
    return np.fft.rfftfreq(NFFT, 1.0/sr), acc

def psd(path, sr_expect=None):
    sr, x = read_wav(path)
    f, L = welch(x[:,0], sr)
    _, R = welch(x[:,1] if x.shape[1] > 1 else x[:,0], sr)
    return sr, f, L, R

def db(p): return 10*np.log10(np.maximum(p, 1e-30))

def load_dir(d):
    files = {}
    for name in ["stereo_front","stereo_rear","surround_front","surround_rear",
                 "surround_front_deep","surround_rear_deep","surround_front_wide",
                 "surround_rear_wide","mono_front","mono_rear","noise_floor"]:
        p = os.path.join(d, name+".wav")
        files[name] = psd(p) if os.path.exists(p) else None
    return files

def trusted(Sxx, floor):
    m = (db(Sxx) >= db(floor)+SNR_TRUST_DB)
    return m

def magdev(front, rear, floorL, floorR, band):
    """D over channels that have trusted bins; returns (D, per-channel dict)."""
    _,_,Lx,Rx = front; _,_,Ly,Ry = rear
    out = {}; devs = []
    for tag, Sx, Sy, fl in (("L",Lx,Ly,floorL), ("R",Rx,Ry,floorR)):
        m = band & trusted(Sx, fl) & trusted(Sy, fl)
        if m.sum() < 8:
            out[tag] = None; continue
        H = 20*np.log10(np.sqrt(np.maximum(Sy[m],1e-30)/np.maximum(Sx[m],1e-30)))
        H = H - np.median(H)
        d = float(np.sqrt(np.mean(H**2)))
        out[tag] = d; devs.append(d)
    D = float(np.mean(devs)) if devs else float("nan")
    return D, out

def xtalk(cap, floorL, band):
    _,_,L,R = cap
    m = band & trusted(L, floorL)
    if m.sum() < 8: return float("nan")
    return 10*np.log10(np.maximum(np.sum(R[m]),1e-30)/np.maximum(np.sum(L[m]),1e-30))

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    F = load_dir(d)
    missing = [k for k,v in F.items() if v is None and k!="noise_floor"]
    if missing: print("!! missing captures:", ", ".join(missing))
    ref = next(v for v in F.values() if v is not None)
    sr, freq = ref[0], ref[1]
    band = (freq >= FLO) & (freq <= FHI)

    if F["noise_floor"] is not None:
        _,_,nfL,nfR = F["noise_floor"]
    else:
        print("!! no noise_floor.wav; using 5th-percentile of stereo_front as floor")
        _,_,L,R = F["stereo_front"]; nfL = np.full_like(L, np.percentile(L,5)); nfR = np.full_like(R, np.percentile(R,5))

    print(f"\n=== surround-probe v2 analysis ({sr} Hz, band {FLO:.0f}-{FHI:.0f} Hz) ===\n")

    modes = [("STEREO","stereo_front","stereo_rear"),
             ("SURROUND","surround_front","surround_rear"),
             ("SURR+DEPTH","surround_front_deep","surround_rear_deep"),
             ("SURR+WIDE","surround_front_wide","surround_rear_wide"),
             ("MONO","mono_front","mono_rear")]
    D = {}; XT = {}
    print(f"{'mode':11s} {'D (MAGDEV front/rear) dB':26s} {'XTALK front/rear dB':22s}")
    for name, ff, rr in modes:
        if F[ff] is None or F[rr] is None: continue
        Dm, per = magdev(F[ff], F[rr], nfL, nfR, band)
        xf = xtalk(F[ff], nfL, band); xr = xtalk(F[rr], nfL, band)
        D[name] = Dm; XT[name] = (xf, xr)
        perstr = " ".join(f"{k}:{('%.2f'%v) if v is not None else '--'}" for k,v in per.items())
        print(f"{name:11s} D={Dm:5.2f}  [{perstr:14s}]  Xf={xf:6.1f} Xr={xr:6.1f}")

    # relative discriminators. Two independent lenses on "front != rear in
    # Surround": spectral reshaping (MAGDEV) and crosstalk into the silent
    # channel. For a single-corner route crosstalk is the cleaner, more physical
    # signature, so the verdict is crosstalk-led with reshaping as corroboration,
    # and BOTH must be gated by the depth positive-control (effect grows/holds
    # with the depth knob) so a null is distinguishable from residual blindness.
    D_st = D.get("STEREO", float("nan")); D_su = D.get("SURROUND", float("nan"))
    D_dp = D.get("SURR+DEPTH", float("nan"))
    xt_st = float(np.nanmean(XT.get("STEREO",(np.nan,np.nan))))
    xt_su = float(np.nanmean(XT.get("SURROUND",(np.nan,np.nan))))
    xt_dp = float(np.nanmean(XT.get("SURR+DEPTH",(np.nan,np.nan))))
    dD  = D_su - D_st
    dXT = xt_su - xt_st

    grow_D  = D_dp  >= D_su  - 0.5      # reshaping holds/grows at 0xFFFF
    grow_XT = xt_dp >= xt_su - 1.0      # crosstalk holds/grows at 0xFFFF (dB)

    print(f"\n-- discriminators --")
    print(f"  reshaping   dD = D_surround - D_stereo = {dD:+.2f} dB   (front-vs-rear spectrum)")
    print(f"  crosstalk   dXTALK = surround - stereo = {dXT:+.2f} dB  (energy into the silent channel)")
    print(f"  depth ctrl  D: {D_su:.2f}->{D_dp:.2f} (grow={grow_D})   XTALK: {xt_su:.1f}->{xt_dp:.1f} dB (grow={grow_XT})")
    if "MONO" in D:
        print(f"  MONO front-vs-rear D={D['MONO']:.2f}  (sanity: quad collapse, should be ~0)")

    # verdict (crosstalk-led OR reshaping, each depth-corroborated; or moderate both)
    baseline = (D_st <= 1.0)
    confirm = baseline and (
        (dXT >= 10.0 and grow_XT) or
        (dD  >= 3.0  and grow_D)  or
        (dXT >= 5.0  and dD >= 2.0 and (grow_XT or grow_D)))
    refute  = (dD <= 1.0) and (dXT < 3.0) and (not grow_D) and (not grow_XT)

    print(f"\n-- VERDICT --")
    if confirm:
        print("  CONFIRMED: Surround repositions front/back -> span (0xD7) is audible on hardware.")
        print("  (front vs rear differ in Surround but not Stereo, and the effect tracks the depth knob.)")
    elif refute:
        print("  REFUTED: Surround does NOT reposition front/back on the headphone jack.")
        print("  (no reshaping, no crosstalk, no growth with depth -> a level/EQ enhancement or a")
        print("   speaker-only effect; span would be inaudible via headphones.)")
    else:
        print("  INCONCLUSIVE: between thresholds. The depth positive-control already ran; if the")
        print("  0xFFFF row shows growth the effect is real but weak -- raise capture gain (probe")
        print("  peaks at a safe -9 dBFS) and/or lengthen FR_BODY for more Welch blocks.")
    print("  CONFIRM: D_stereo<=1 AND [ dXTALK>=10 & XTALK grows | dD>=3 & D grows |")
    print("           dXTALK>=5 & dD>=2 & either grows ].   REFUTE: dD<=1 & dXTALK<3 & no growth.\n")

if __name__ == "__main__":
    main()
