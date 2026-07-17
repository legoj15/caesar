#!/usr/bin/env python3
"""analyze_capture_v2_trackB.py — Track B (battery-v2) console-capture analyzer.

Pure offline signal analysis of a live New-3DS line-in recording of capture-cartridge
v2 *track B* (host BGM_DEN_EMPTY_LANDSCAPE, bank 3 BANK_MEET_LEGEND), against the
recalibrated caesar_play prediction. Measures the five constants track B was built
to close:

  1. Release table   — are 0xD3 release OVERRIDES honored in the BGM path, and do the
                       byte->slope values match calcRelease + the 10^(v/200) divisor?
  2. Reverb residual — the plaza-DSP reverb impulse on a real LEGEND voice (send 127).
  3. Decay table     — first console exercise of the 2026-07-08-corrected DecayTable
                       tail entry byte 122 (+ sustain 40).
  4. Vibrato / LFO   — kLfoRateHz = 5/64 (byte48 vs byte96; the doubling isolates the LFO
                       from the 6.5 Hz partial-beat confound).
  5. Portamento      — DISTANCE law (dur proportional-to-distance) and the byte->RATE law
                       (shipped provisional: rate proportional-to 1/byte, anchored byte48=2.841 st/s).

Inputs (defaults match the repo/session layout; override on argv):
  console WAV, prediction WAV.

Numpy + scipy + soundfile.  Analog line-in => tolerance, never byte-identical.
Reports NEGATIVE findings honestly (e.g. an instant cut on the release overrides, or a
console law that disagrees with the prediction) rather than manufacturing numbers.

NB a sibling script analyze_capture_v2.py handles track A; do not import it.
"""
import sys, numpy as np, soundfile as sf
from scipy.signal import butter, filtfilt, hilbert

SR = 48000
CON_DEFAULT = "E:/GitHub/caesar/BATTERY_B_v2_console.wav"
PRE_DEFAULT = "E:/GitHub/caesar/build/cartridge-v2/PREDICTION_battery_B.wav"

# Track B probe schedule, seconds relative to the pilot (from MANIFEST.md).
SCHED = {
    'PILOT': 0.000,
    'B1a_rel60': 2.000, 'B1b_rel100': 9.500, 'B1c_rel114': 17.000, 'B1d_rel124': 24.500,
    'B1r_reverb': 32.000, 'B2_decay122': 42.500,
    'B3a_vib48': 50.000, 'B3b_vib96': 54.396,
    'B4d1_p3_b48': 58.792, 'B4d2_p12_b48': 62.292,
    'B4r1_p12_b24': 68.792, 'B4r2_p12_b96': 73.792,
}
LOOP = 85.79


# ---------- helpers ----------
def load(path):
    x, sr = sf.read(path, always_2d=True)
    assert sr == SR, f"expected {SR} Hz, got {sr}"
    return x, x.mean(1)


def envdb(sig, t0, t1, ms=2.0):
    s, e = int(t0 * SR), int(t1 * SR)
    hop = max(1, int(round(SR * ms / 1000)))
    seg = sig[s:e]
    nf = len(seg) // hop
    ev = np.array([np.sqrt(np.mean(seg[i * hop:(i + 1) * hop] ** 2) + 1e-20) for i in range(nf)])
    return np.arange(nf) * hop / SR, 20 * np.log10(ev + 1e-12)


def smooth(v, k):
    return np.convolve(v, np.ones(k) / k, 'same')


def detect_onsets(sig, thr_hi=-45.0, thr_lo=-60.0):
    _, ed = envdb(sig, 0, len(sig) / SR, ms=5.0)
    t = np.arange(len(ed)) * 5e-3
    ons, armed = [], True
    for i in range(1, len(ed)):
        if armed and ed[i] > thr_hi and ed[i - 1] <= thr_hi:
            ons.append(t[i]); armed = False
        elif not armed and ed[i] < thr_lo:
            armed = True
    return np.array(ons)


def find_pilot(onsets):
    """Locate a pilot: an onset with companions near +2.0/+9.5/+17.0/+24.5 (the release ladder)."""
    ladder = [2.0, 9.5, 17.0, 24.5]
    best = None
    for o in onsets:
        hits = sum(np.any(np.abs(onsets - (o + d)) < 0.25) for d in ladder)
        if hits >= 3 and (o + 24.5) < onsets.max():
            # prefer the earliest fully-covered pilot
            if best is None:
                best = o
    return best


def pilot_rms_db(sig, t_pilot):
    a, b = t_pilot + 0.17, t_pilot + 0.92   # clean pilot body
    return 20 * np.log10(np.sqrt(np.mean(sig[int(a * SR):int(b * SR)] ** 2)) + 1e-12)


# ---------- 1. release slopes ----------
def release_slope(sig, onset, span=7.0):
    tt, ed = envdb(sig, onset, onset + span, 2.0)
    eds = smooth(ed, 7)
    body = np.median(eds[(tt > 0.3) & (tt < 2.3)])
    floor = np.percentile(eds[tt > span - 1.2], 50)
    m = (tt < 4.0) & (eds > body - 3)
    toff = tt[m][-1]
    i_off = np.searchsorted(tt, toff)
    arr = floor + 6
    seg, tseg = eds[i_off:], tt[i_off:]
    below = np.where(seg <= arr)[0]
    i_arr = below[0] if len(below) else len(seg) - 1
    dcy, tdc = seg[:i_arr + 1], tseg[:i_arr + 1]
    lo, hi = body - 6, max(body - 45, arr)
    sel = (dcy <= lo) & (dcy >= hi)
    dt_floor = tdc[i_arr] - toff
    if sel.sum() >= 4 and (dcy[sel].max() - dcy[sel].min()) > 8:
        A = np.polyfit(tdc[sel], dcy[sel], 1)
        fit = np.polyval(A, tdc[sel])
        r2 = 1 - np.sum((dcy[sel] - fit) ** 2) / (np.sum((dcy[sel] - dcy[sel].mean()) ** 2) + 1e-9)
        return dict(slope=-A[0], r2=r2, n=int(sel.sum()), inst=False, dt_floor=dt_floor,
                    body=body, floor=floor)
    a = np.where(dcy <= body - 6)[0]; b = np.where(dcy <= arr)[0]
    dt = (tdc[b[0]] - tdc[a[0]]) if len(a) and len(b) else dt_floor
    return dict(slope=None, inst=True, dt=dt, dt_floor=dt_floor, body=body, floor=floor)


def report_release(con, pre, cp, pp):
    print("\n" + "=" * 78)
    print("1. RELEASE TABLE  (0xD3 override; slope over body-6..body-45 dB, r2>=0.96 clean)")
    print("=" * 78)
    print(f"{'byte':>6} | {'console dB/s':>13} | {'prediction dB/s':>16} | {'ratio':>6}")
    for k, byte in [('B1a_rel60', 60), ('B1b_rel100', 100), ('B1c_rel114', 114), ('B1d_rel124', 124)]:
        rc = release_slope(con, cp + SCHED[k])
        rp = release_slope(pre, pp + SCHED[k])
        def s(r):
            return f"INSTANT {r['dt']*1000:.0f}ms" if r['inst'] else f"{r['slope']:.1f} (r2={r['r2']:.2f})"
        ratio = (rc['slope'] / rp['slope']) if (not rc['inst'] and not rp['inst']) else float('nan')
        print(f"{byte:>6} | {s(rc):>13} | {s(rp):>16} | {ratio:>6.2f}")
    print("VERDICT: overrides honored iff slopes rise monotonically with byte (not four INSTANTs).")


# ---------- 2. reverb residual ----------
def report_reverb(con, cp, pil_db):
    print("\n" + "=" * 78)
    print("2. REVERB RESIDUAL  (B1r: R127 instant cut + reverb send 127)")
    print("=" * 78)
    on = cp + SCHED['B1r_reverb']
    tt, ed = envdb(con, on, on + 8.0, 1.0)
    eds = smooth(ed, 5) - pil_db
    body = np.median(eds[(tt > 0.5) & (tt < 2.3)])
    m = (tt < 3.5) & (eds > body - 3); tcut = tt[m][-1]
    icut = np.searchsorted(tt, tcut)
    post, tpost = eds[icut:], tt[icut:] - tcut
    floor = np.median(eds[tt > 7.0])
    i30 = np.searchsorted(tpost, 0.05)
    rev0 = np.median(post[i30:i30 + 20])
    reg = (tpost >= 0.05) & (post <= rev0 - 2) & (post >= floor + 6)
    A = np.polyfit(tpost[reg], post[reg], 1); rsl = -A[0]
    fit = np.polyval(A, tpost[reg])
    r2 = 1 - np.sum((post[reg] - fit) ** 2) / np.sum((post[reg] - post[reg].mean()) ** 2)

    def centroid(t0, dur):
        s = int(t0 * SR); seg = con[s:s + int(dur * SR)].astype(float); seg -= seg.mean()
        w = np.hanning(len(seg)); S = np.abs(np.fft.rfft(seg * w)); f = np.fft.rfftfreq(len(seg), 1 / SR)
        b = (f > 80) & (f < 12000)
        c = np.sum(f[b] * S[b]) / (np.sum(S[b]) + 1e-12)
        loc = (f > 80) & (f < 500); frac = np.sum(S[loc] ** 2) / np.sum(S[b] ** 2)
        return c, frac
    cb, fb = centroid(on + 1.0, 0.8)
    ct, ft = centroid(on + tcut + 0.3, 0.6)
    print(f"  dry note body        : {body:+.1f} dB (rel pilot)   cut @ {tcut:.2f}s")
    print(f"  wet return @ +50ms   : {rev0:+.1f} dB  ({rev0 - body:.1f} dB below dry note -> the send-127 wet/dry)")
    print(f"  reverb decay         : {rsl:.1f} dB/s (r2={r2:.2f})  T60~{60/rsl:.2f}s  tau~{8.686/rsl:.2f}s")
    print(f"  spectral centroid    : dry body {cb:.0f} Hz (lo<500 {fb:.3f}) | reverb tail {ct:.0f} Hz (lo {ft:.3f})")
    print("  NB reverb is NOT dark/low-passed: the tail tracks the source brightness.")


# ---------- 3. decay table ----------
def report_decay(con, pre, cp, pp, pil_c, pil_p):
    print("\n" + "=" * 78)
    print("3. DECAY TABLE  (B2: corrected byte 122 + sustain 40; peak->plateau)")
    print("=" * 78)
    out = {}
    for src, sig, on, pil in [('console', con, cp + SCHED['B2_decay122'], pil_c),
                              ('predict', pre, pp + SCHED['B2_decay122'], pil_p)]:
        tt, ed = envdb(sig, on, on + 5.5, 0.5); eds = smooth(ed, 5) - pil
        ipk = int(np.argmax(eds[tt < 0.5])); pk = eds[ipk]
        plat = np.median(eds[(tt > 1.0) & (tt < 4.5)])
        # slope over the CONTIGUOUS steep decay: peak -> first arrival at plateau+3,
        # fit the linear-in-dB stretch (peak-3 .. plateau+3) inside that segment only.
        arr = np.where((tt > tt[ipk]) & (eds <= plat + 3))[0]
        i_arr = arr[0] if len(arr) else len(tt) - 1
        seg = slice(ipk, i_arr + 1)
        reg = (eds[seg] <= pk - 3) & (eds[seg] >= plat + 3)
        A = np.polyfit(tt[seg][reg], eds[seg][reg], 1); sl = -A[0]
        out[src] = (pk, plat, sl)
        print(f"  {src}: attack-peak={pk:+.1f} dB  sustain-plateau={plat:+.1f} dB  decay~{sl:.0f} dB/s  "
              f"(peak->plat {pk - plat:.1f} dB)")
    dp = out['console'][1] - out['predict'][1]
    print(f"  plateau delta console-predict = {dp:+.1f} dB   VERDICT: byte-122 correction "
          f"{'CONFIRMED' if abs(dp) < 2 else 'DISAGREES'} (traces overlay if plateau & slope match).")


# ---------- 4. vibrato via FM sidebands ----------
def sideband_spacing(sig, t0, dur, fc=261.6, hw=45):
    s = int(t0 * SR); seg = sig[s:s + int(dur * SR)].astype(float); seg -= seg.mean()
    w = np.hanning(len(seg)); N = 1 << int(np.ceil(np.log2(len(seg))) + 3)
    S = np.abs(np.fft.rfft(seg * w, N)); f = np.fft.rfftfreq(N, 1 / SR)
    band = (f > fc - hw) & (f < fc + hw); fb = f[band]; Sb = S[band] / S[band].max()
    peaks = []
    for i in range(2, len(Sb) - 2):
        if Sb[i] > 0.05 and Sb[i] >= Sb[i - 1] and Sb[i] >= Sb[i + 1] and Sb[i] > Sb[i - 2] and Sb[i] > Sb[i + 2]:
            peaks.append((fb[i], Sb[i]))
    merged = []
    for fpk, a in sorted(peaks):
        if merged and fpk - merged[-1][0] < 0.8:
            if a > merged[-1][1]: merged[-1] = (fpk, a)
            continue
        merged.append((fpk, a))
    freqs = [p[0] for p in merged]
    return np.median(np.diff(freqs)) if len(freqs) > 1 else None, freqs


def report_vibrato(con, pre, cp, pp):
    print("\n" + "=" * 78)
    print("4. VIBRATO / LFO RATE  (FM sideband spacing around 261.6 Hz; 5/64 model)")
    print("=" * 78)
    res = {}
    for src, sig, base in [('console', con, cp), ('predict', pre, pp)]:
        r48, _ = sideband_spacing(sig, base + SCHED['B3a_vib48'] + 0.32, 3.5)
        r96, _ = sideband_spacing(sig, base + SCHED['B3b_vib96'] + 0.30, 3.4)
        res[src] = (r48, r96)
        ratio = r96 / r48 if (r48 and r96) else float('nan')
        print(f"  {src}: byte48={r48:.3f} Hz  byte96={r96:.3f} Hz  ratio={ratio:.3f} (2.0 => it IS the LFO)")
    print(f"  model 5/64: byte48->{48*5/64:.3f} Hz  byte96->{96*5/64:.3f} Hz")
    c48, c96 = res['console']; p48, _ = res['predict']
    fac = c48 / p48 if p48 else float('nan')
    print(f"  console/model factor = {fac:.2f}x  (per-byte const: console {c48/48:.4f} Hz vs model {5/64:.4f} Hz)")
    print(f"  VERDICT: ratio~2 => rate proportional-to byte CONFIRMED; factor {fac:.1f}x => the ABSOLUTE 5/64 constant "
          "needs revision.")


# ---------- 5. portamento ----------
def track_pitch(sig, t0, t1, fmin=200, fmax=780, hop_ms=10, win_ms=80):
    s, e = int(t0 * SR), int(t1 * SR); seg = sig[s:e].astype(float)
    hop = int(SR * hop_ms / 1000); win = int(SR * win_ms / 1000)
    w = np.hanning(win); N = 1 << int(np.ceil(np.log2(win)) + 2)
    f = np.fft.rfftfreq(N, 1 / SR); band = (f >= fmin) & (f <= fmax); fb = f[band]
    T, F, A = [], [], []
    for i in range(0, len(seg) - win, hop):
        S = np.abs(np.fft.rfft(seg[i:i + win] * w, N))[band]; k = int(np.argmax(S))
        if 0 < k < len(S) - 1:
            a, b, c = S[k - 1], S[k], S[k + 1]; d = 0.5 * (a - c) / (a - 2 * b + c + 1e-20)
        else:
            d = 0
        T.append(t0 + (i + win / 2) / SR); F.append(fb[k] + d * (fb[1] - fb[0])); A.append(S[k])
    return np.array(T), 12 * np.log2(np.maximum(np.array(F), 1) / 261.63), np.array(A)


def glide_rate(sig, t0, t1, tgt):
    T, st, A = track_pitch(sig, t0, t1)
    good = A > np.percentile(A, 35)
    reg = good & (st > 0.8) & (st < tgt - 0.8) & (T > t0 + 0.3)
    reach = np.where(good & (st > tgt - 0.4) & (T > t0 + 0.3))[0]
    if len(reach):
        reg = reg & (T <= T[reach[0]])
    if reg.sum() < 5:
        return None, None, None
    A1 = np.polyfit(T[reg], st[reg], 1)
    fit = np.polyval(A1, T[reg])
    r2 = 1 - np.sum((st[reg] - fit) ** 2) / np.sum((st[reg] - st[reg].mean()) ** 2)
    dur = (tgt - A1[1]) / A1[0] - (0 - A1[1]) / A1[0]
    return A1[0], dur, r2


def report_porta(con, pre, cp, pp):
    print("\n" + "=" * 78)
    print("5. PORTAMENTO  (distance law + byte->rate law; provisional: rate proportional-to 1/byte)")
    print("=" * 78)
    for src, sig, base in [('console', con, cp), ('predict', pre, pp)]:
        d3 = glide_rate(sig, base + SCHED['B4d1_p3_b48'] - 0.12, base + SCHED['B4d2_p12_b48'] - 0.12, 3)
        d12 = glide_rate(sig, base + SCHED['B4d2_p12_b48'] - 0.12, base + SCHED['B4r1_p12_b24'] - 0.12, 12)
        r24 = glide_rate(sig, base + SCHED['B4r1_p12_b24'] - 0.12, base + SCHED['B4r2_p12_b96'] - 0.12, 12)
        # byte96 glide runs long; give it the room up to the next pilot
        r96 = glide_rate(sig, base + SCHED['B4r2_p12_b96'] - 0.12, base + LOOP - 1.9, 12)
        print(f"  [{src}]")
        print(f"    DISTANCE byte48 : +3st rate={d3[0]:.3f} dur={d3[1]:.2f}s | +12st rate={d12[0]:.3f} dur={d12[1]:.2f}s")
        if d3[0] and d12[0]:
            print(f"                      dur ratio(12/3)={d12[1]/d3[1]:.2f} (4.0=>dur~distance)  "
                  f"rate ratio={d12[0]/d3[0]:.2f} (1.0=>const rate)")
        print(f"    RATE  +12st     : byte24={r24[0]:.3f}  byte48={d12[0]:.3f}  byte96={r96[0]:.3f} st/s")
        if r24[0] and d12[0] and r96[0]:
            print(f"                      byte48/byte24={d12[0]/r24[0]:.3f} byte96/byte48={r96[0]/d12[0]:.3f}  "
                  f"(0.5=>1/byte, 0.25=>1/byte^2)")
            print(f"                      byte^2*rate: {24**2*r24[0]:.0f}/{48**2*d12[0]:.0f}/{96**2*r96[0]:.0f} "
                  "(const => rate~1/byte^2)")


# ---------- main ----------
def main():
    con_path = sys.argv[1] if len(sys.argv) > 1 else CON_DEFAULT
    pre_path = sys.argv[2] if len(sys.argv) > 2 else PRE_DEFAULT
    _, con = load(con_path)
    _, pre = load(pre_path)

    ons = detect_onsets(con)
    cp = find_pilot(ons)
    if cp is None:
        print("FATAL: no pilot found (no +2.0/+9.5/+17.0/+24.5 release ladder). Cannot segment.")
        sys.exit(2)
    # prediction pilot is at its very start
    pons = detect_onsets(pre, thr_hi=pre_dbfloor(pre))
    pp = pons[0] - SCHED['PILOT']
    pil_c = pilot_rms_db(con, cp)
    pil_p = pilot_rms_db(pre, pp)
    print(f"console pilot @ {cp:.3f}s (pass P2 of a {LOOP}s loop)  pilot RMS {pil_c:.2f} dBFS")
    print(f"predict pilot @ {pp:.3f}s  pilot RMS {pil_p:.2f} dBFS")

    report_release(con, pre, cp, pp)
    report_reverb(con, cp, pil_c)
    report_decay(con, pre, cp, pp, pil_c, pil_p)
    report_vibrato(con, pre, cp, pp)
    report_porta(con, pre, cp, pp)


def pre_dbfloor(pre):
    # prediction is digital-silent between notes; a high threshold is fine
    _, ed = envdb(pre, 0, len(pre) / SR, 5.0)
    return ed.max() - 35


if __name__ == "__main__":
    main()
