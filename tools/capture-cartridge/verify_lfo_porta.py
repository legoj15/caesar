"""Independent blind cross-check of vibrato rate + portamento glide rate.

Measures from BATTERY_B_v2_stereo.wav only. Fits what the data shows; assumes no law.
Uncommitted verification tool.
"""
import sys
import numpy as np
import soundfile as sf

WAV = r"E:\GitHub\caesar\BATTERY_B_v2_stereo.wav"


def load():
    x, fs = sf.read(WAV)
    if x.ndim == 2:
        m = 0.5 * (x[:, 0] + x[:, 1])
    else:
        m = x
    return m.astype(np.float64), fs


def f0_fft(sig, fs, win, hop, fmin=150.0, fmax=1300.0):
    """Quadratic-interpolated FFT peak in [fmin,fmax]. Returns t, f0, mag."""
    w = np.hanning(win)
    n = (len(sig) - win) // hop
    freqs = np.fft.rfftfreq(win, 1.0 / fs)
    lo = np.searchsorted(freqs, fmin)
    hi = np.searchsorted(freqs, fmax)
    t = np.zeros(n)
    f0 = np.zeros(n)
    mag = np.zeros(n)
    for i in range(n):
        seg = sig[i * hop:i * hop + win] * w
        sp = np.abs(np.fft.rfft(seg))
        band = sp[lo:hi]
        k = np.argmax(band) + lo
        # parabolic interpolation
        if 1 <= k < len(sp) - 1:
            a, b, c = sp[k - 1], sp[k], sp[k + 1]
            denom = (a - 2 * b + c)
            delta = 0.5 * (a - c) / denom if denom != 0 else 0.0
        else:
            delta = 0.0
        f0[i] = (k + delta) * fs / win
        mag[i] = sp[k]
        t[i] = (i * hop + win / 2) / fs
    return t, f0, mag


def rms_env(sig, fs, hop=480, win=960):
    n = (len(sig) - win) // hop
    env = np.array([np.sqrt(np.mean(sig[i * hop:i * hop + win] ** 2)) for i in range(n)])
    t = (np.arange(n) * hop + win / 2) / fs
    return t, env


def coarse_timeline(m, fs, t0, t1, step=0.25):
    t, f0, mag = f0_fft(m, fs, win=2048, hop=256)
    te, env = rms_env(m, fs)
    envmax = env.max()
    print(f"\n=== timeline {t0:.1f}-{t1:.1f}s (f0 win=2048/hop256) ===")
    tt = t0
    while tt < t1:
        i = np.searchsorted(t, tt)
        j = np.searchsorted(te, tt)
        if i < len(t) and j < len(env):
            db = 20 * np.log10(env[j] / envmax + 1e-12)
            act = "*" if env[j] > 0.02 * envmax else " "
            print(f" {tt:7.2f}  {act} f0={f0[i]:7.1f}Hz  rms={db:6.1f}dB")
        tt += step


if __name__ == "__main__":
    m, fs = load()
    print("fs", fs, "dur", len(m) / fs)
    mode = sys.argv[1] if len(sys.argv) > 1 else "coarse"
    if mode == "coarse":
        a = float(sys.argv[2]); b = float(sys.argv[3])
        step = float(sys.argv[4]) if len(sys.argv) > 4 else 0.25
        coarse_timeline(m, fs, a, b, step)


def find_runs(m, fs):
    t, f0, mag = f0_fft(m, fs, win=2048, hop=256)
    te, env = rms_env(m, fs)
    envmax = env.max()
    # interpolate env onto f0 times
    envi = np.interp(t, te, env)
    active = envi > 0.03 * envmax
    # classify each active frame
    runs = []
    i = 0
    n = len(t)
    while i < n:
        if not active[i]:
            i += 1
            continue
        j = i
        while j < n and active[j]:
            j += 1
        seg_t = t[i:j]
        seg_f = f0[i:j]
        dur = seg_t[-1] - seg_t[0]
        runs.append((seg_t[0], seg_t[-1], dur, np.median(seg_f), seg_f[0], seg_f[-1], seg_f.min(), seg_f.max()))
        i = j
    print("\n=== active runs (win2048) ===")
    print(" start   end     dur    medf0   f0start  f0end   fmin    fmax")
    for r in runs:
        print(f" {r[0]:7.3f} {r[1]:7.3f} {r[2]:6.3f}  {r[3]:7.1f} {r[4]:7.1f} {r[5]:7.1f} {r[6]:7.1f} {r[7]:7.1f}")
    # period via envelope autocorrelation
    hop = 480
    e = env - env.mean()
    ac = np.correlate(e, e, 'full')[len(e)-1:]
    # search period 80-90s -> in frames
    lo = int(80 * fs / hop); hi = int(90 * fs / hop)
    kper = lo + np.argmax(ac[lo:hi])
    print(f"\nEnvelope autocorr period ~ {kper*hop/fs:.3f} s")


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "runs":
    m, fs = load()
    find_runs(m, fs)


from scipy.signal import butter, filtfilt, hilbert


def bandpass(sig, fs, f1, f2, order=4):
    b, a = butter(order, [f1 / (fs / 2), f2 / (fs / 2)], 'band')
    return filtfilt(b, a, sig)


def parab(sp, k):
    a, b, c = sp[k - 1], sp[k], sp[k + 1]
    d = (a - 2 * b + c)
    return 0.5 * (a - c) / d if d != 0 else 0.0


def vibrato_measure(m, fs, t0, t1, label):
    i0 = int(t0 * fs); i1 = int(t1 * fs)
    seg = m[i0:i1]
    # ---- method 1: FM demod via Hilbert on bandpassed fundamental ----
    bp = bandpass(seg, fs, 180, 360, 4)
    an = hilbert(bp)
    inst = np.diff(np.unwrap(np.angle(an))) * fs / (2 * np.pi)
    # trim edge transients of filtfilt/hilbert
    e = int(0.08 * fs)
    ifr = inst[e:-e]
    fc = np.mean(ifr)
    ac = ifr - fc
    N = len(ac)
    W = np.hanning(N)
    sp = np.abs(np.fft.rfft(ac * W))
    fr = np.fft.rfftfreq(N, 1.0 / fs)
    lo = np.searchsorted(fr, 1.0); hi = np.searchsorted(fr, 15.0)
    k = lo + np.argmax(sp[lo:hi])
    fm_demod = (k + parab(sp, k)) * fs / N
    depth_rms = np.std(ac)              # Hz rms
    depth_pk = depth_rms * np.sqrt(2)   # Hz peak (sine assumption)
    # ---- method 2: FM sideband spacing from audio magnitude spectrum ----
    W2 = np.hanning(len(seg))
    sp2 = np.abs(np.fft.rfft(seg * W2))
    fr2 = np.fft.rfftfreq(len(seg), 1.0 / fs)
    # carrier ~ fc; look at band fc-40..fc+40, find local peaks
    blo = np.searchsorted(fr2, fc - 45); bhi = np.searchsorted(fr2, fc + 45)
    band = sp2[blo:bhi]; bfr = fr2[blo:bhi]
    # peak-pick: local maxima above 8% of max
    pk = []
    thr = 0.06 * band.max()
    for j in range(2, len(band) - 2):
        if band[j] > thr and band[j] >= band[j-1] and band[j] >= band[j+1] and band[j] > band[j-2] and band[j] > band[j+2]:
            pk.append(bfr[j] + parab(band, j) * (fr2[1] - fr2[0]))
    spacing = np.median(np.diff(pk)) if len(pk) >= 2 else float('nan')
    print(f"  {label:14s} carrier={fc:6.2f}Hz  fm(demod)={fm_demod:5.3f}Hz  depth=+-{depth_pk:4.1f}Hz(={1200*np.log2((fc+depth_pk)/fc):5.1f}c)  sidebands={pk} spacing={spacing:.3f}")
    return fm_demod, spacing, fc, depth_pk


def porta_measure(m, fs, t0, t1, target_st, label):
    # fine f0 track restricted to plausible band for the glide target
    i0 = int(t0 * fs); i1 = int(t1 * fs)
    seg = m[i0:i1]
    fmax = 700.0 if target_st >= 12 else 420.0
    t, f0, mag = f0_fft(seg, fs, win=1024, hop=64, fmin=150.0, fmax=fmax)
    t = t + t0
    s = 12 * np.log2(f0 / 261.63)
    # restrict to rising region: between 0.5 st and (target-1) st, monotonic-ish
    hi_st = target_st - 1.0
    lo_st = 0.6
    mask = (s > lo_st) & (s < hi_st)
    if mask.sum() < 5:
        print(f"  {label}: insufficient rise")
        return None
    ts = t[mask]; ss = s[mask]
    # keep the first contiguous rising cluster (the glide, not later wobble)
    # sort by time, take from first in-band to where it plateaus (s stops increasing overall)
    order = np.argsort(ts)
    ts = ts[order]; ss = ss[order]
    # linear fit
    A = np.polyfit(ts, ss, 1)
    rate = A[0]
    resid = ss - np.polyval(A, ts)
    rms = np.sqrt(np.mean(resid ** 2))
    # duration to traverse full target distance at this rate
    print(f"  {label:16s} rate={rate:6.3f} st/s  fit N={len(ts)} span[{ts[0]-t0:.2f}-{ts[-1]-t0:.2f}s of win] rmsResid={rms:.3f}st  (dur for {target_st}st = {target_st/rate:.2f}s)")
    return rate


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "measure":
    m, fs = load()
    print("\n### VIBRATO (FM demod + sideband spacing) ###")
    # loop1: V48 22.512-26.539, V96 26.901-30.928 ; loop2: +85.60
    vib = {}
    for name, a, b in [("L1 V48", 22.90, 26.45), ("L1 V96", 27.30, 30.85),
                        ("L2 V48", 108.50, 112.05), ("L2 V96", 112.90, 116.45)]:
        fm, sb, fc, dp = vibrato_measure(m, fs, a, b, name)
        vib[name] = fm
    print("\n### PORTAMENTO (semitone slope) ###")
    por = {}
    for name, a, b, tgt in [
        ("L1 +3 @48", 31.70, 34.30, 3), ("L1 +12 @48", 35.20, 40.80, 12),
        ("L1 +12 @24", 41.60, 45.80, 12), ("L1 +12 @96", 46.60, 55.78, 12),
        ("L2 +3 @48", 117.30, 119.90, 3), ("L2 +12 @48", 120.80, 126.35, 12),
        ("L2 +12 @24", 127.20, 131.33, 12), ("L2 +12 @96", 132.20, 141.30, 12)]:
        r = porta_measure(m, fs, a, b, tgt, name)
        por[name] = r
    print("\n### SUMMARY ###")
    v48 = np.nanmean([vib["L1 V48"], vib["L2 V48"]])
    v96 = np.nanmean([vib["L1 V96"], vib["L2 V96"]])
    print(f" vibrato rate: setting48 = {v48:.3f} Hz  setting96 = {v96:.3f} Hz  ratio(96/48) = {v96/v48:.3f}")
    p24 = np.nanmean([por["L1 +12 @24"], por["L2 +12 @24"]])
    p48 = np.nanmean([por["L1 +12 @48"], por["L2 +12 @48"]])
    p96 = np.nanmean([por["L1 +12 @96"], por["L2 +12 @96"]])
    p3_48 = np.nanmean([por["L1 +3 @48"], por["L2 +3 @48"]])
    print(f" porta rate: @24 = {p24:.3f}  @48 = {p48:.3f}  @96 = {p96:.3f} st/s   (+3@48 = {p3_48:.3f} st/s)")
    print(f"   setting*rate:  24*{p24:.3f}={24*p24:.2f}  48*{p48:.3f}={48*p48:.2f}  96*{p96:.3f}={96*p96:.2f}")
    print(f"   setting^2*rate: 24^2*{p24:.3f}={24*24*p24:.1f}  48^2*{p48:.3f}={48*48*p48:.1f}  96^2*{p96:.3f}={96*96*p96:.1f}")


def vib_diag(m, fs, t0, t1, label):
    i0 = int(t0 * fs); i1 = int(t1 * fs)
    seg = m[i0:i1]
    bp = bandpass(seg, fs, 180, 360, 4)
    an = hilbert(bp)
    inst = np.diff(np.unwrap(np.angle(an))) * fs / (2 * np.pi)
    e = int(0.08 * fs)
    ifr = inst[e:-e]
    ac = ifr - np.mean(ifr)
    # autocorrelation period (decimate to ~960 Hz, FFT-based)
    dec = 50
    acd = ac[::dec]; fsd = fs / dec
    n = len(acd)
    F = np.fft.rfft(acd, 2 * n)
    c = np.fft.irfft(F * np.conj(F))[:n]
    c = c / c[0]
    kmin = int(fsd / 20.0); kmax = int(fsd / 2.0)   # 2..20 Hz
    kp = kmin + np.argmax(c[kmin:kmax])
    print(f"\n{label}: inst-freq autocorr period = {kp/fsd*1000:.1f} ms -> {fsd/kp:.3f} Hz  (peak r={c[kp]:.2f})")
    # inst-freq FFT top peaks
    N = len(ac); W = np.hanning(N)
    sp = np.abs(np.fft.rfft(ac * W)); fr = np.fft.rfftfreq(N, 1.0/fs)
    lo = np.searchsorted(fr, 1.0); hi = np.searchsorted(fr, 60.0)
    idx = lo + np.argsort(sp[lo:hi])[::-1][:6]
    print("  inst-freq FFT top peaks (Hz : rel-mag):", [(round(fr[k],2), round(sp[k]/sp[lo:hi].max(),2)) for k in idx])
    # audio spectrum near carrier: all local maxima with rel mag
    W2 = np.hanning(len(seg)); sp2 = np.abs(np.fft.rfft(seg*W2)); fr2 = np.fft.rfftfreq(len(seg),1.0/fs)
    fc = np.mean(ifr)
    blo = np.searchsorted(fr2, fc-70); bhi = np.searchsorted(fr2, fc+70)
    band = sp2[blo:bhi]; bfr = fr2[blo:bhi]; cmax = band.max()
    peaks = []
    for j in range(1, len(band)-1):
        if band[j] >= band[j-1] and band[j] > band[j+1] and band[j] > 0.03*cmax:
            peaks.append((round(bfr[j],2), round(band[j]/cmax,3)))
    print("  audio spectrum peaks near carrier (Hz:relmag):", peaks)
    dd = [round(peaks[k+1][0]-peaks[k][0],2) for k in range(len(peaks)-1)]
    print("   -> adjacent peak spacings:", dd)


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "vibdiag":
    m, fs = load()
    for name, a, b in [("L1 V48", 22.90, 26.45), ("L1 V96", 27.30, 30.85),
                       ("L2 V48", 108.50, 112.05), ("L2 V96", 112.90, 116.45)]:
        vib_diag(m, fs, a, b, name)


def vib_sideband(m, fs, t0, t1, label):
    """Robust modulation freq = half the spacing between +1 and -1 FM sidebands."""
    i0 = int(t0 * fs); i1 = int(t1 * fs)
    seg = m[i0:i1]
    W = np.hanning(len(seg))
    sp = np.abs(np.fft.rfft(seg * W)); fr = np.fft.rfftfreq(len(seg), 1.0 / fs)
    df = fr[1] - fr[0]
    # carrier: strongest bin in 250-273
    c0 = np.searchsorted(fr, 250); c1 = np.searchsorted(fr, 273)
    kc = c0 + np.argmax(sp[c0:c1]); fc = (kc + parab(sp, kc)) * df
    # search sidebands in windows on each side (expect within +-45 Hz)
    def peak_in(flo, fhi):
        a = np.searchsorted(fr, flo); b = np.searchsorted(fr, fhi)
        k = a + np.argmax(sp[a:b]); return (k + parab(sp, k)) * df, sp[k]
    fup, mup = peak_in(fc + 8, fc + 45)
    fdn, mdn = peak_in(fc - 45, fc - 8)
    fm_up = fup - fc; fm_dn = fc - fdn
    fm = 0.5 * (fm_up + fm_dn)
    print(f"  {label:8s} fc={fc:7.3f}  -1={fdn:7.3f}(+{mdn/sp[kc]:.2f}) +1={fup:7.3f}(+{mup/sp[kc]:.2f})  fm_dn={fm_dn:6.3f} fm_up={fm_up:6.3f}  -> fm={fm:6.3f} Hz")
    return fm


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "vibfinal":
    m, fs = load()
    res = {}
    for name, a, b in [("L1V48", 22.90, 26.45), ("L2V48", 108.50, 112.05),
                       ("L1V96", 27.30, 30.85), ("L2V96", 112.90, 116.45)]:
        res[name] = vib_sideband(m, fs, a, b, name)
    v48 = np.mean([res["L1V48"], res["L2V48"]])
    v96 = np.mean([res["L1V96"], res["L2V96"]])
    print(f"\n vibrato: setting48 = {v48:.3f} Hz   setting96 = {v96:.3f} Hz   ratio(96/48) = {v96/v48:.4f}")
    print(f" rate/setting: 48 -> {v48/48:.5f} Hz/unit   96 -> {v96/96:.5f} Hz/unit")


def porta_duration(m, fs, t0, t1, target_st, label):
    """Directly measure actual glide start->plateau duration (not rate-extrapolated)."""
    i0 = int(t0 * fs); i1 = int(t1 * fs)
    seg = m[i0:i1]
    fmax = 700.0 if target_st >= 12 else 420.0
    t, f0, mag = f0_fft(seg, fs, win=1024, hop=64, fmin=150.0, fmax=fmax)
    t = t + t0
    s = 12 * np.log2(f0 / 261.63)
    # smooth lightly
    from numpy import convolve
    k = np.ones(9) / 9
    ss = convolve(s, k, 'same')
    # t_start: last frame below 0.4 st before sustained climb above 1 st
    above1 = np.where(ss > 1.0)[0]
    if len(above1) == 0:
        print(f"  {label}: never rose >1 st (max {ss.max():.1f})"); return None
    i_first1 = above1[0]
    below = np.where(ss[:i_first1] < 0.4)[0]
    t_start = t[below[-1]] if len(below) else t[0]
    # t_end: first frame reaching within 0.4 of target and staying (for full glides)
    reach = target_st - 0.4
    hit = np.where(ss > reach)[0]
    if len(hit):
        t_end = t[hit[0]]; reached = ss.max()
        dur = t_end - t_start
        print(f"  {label:14s} glide start={t_start-t0:.2f}s end={t_end-t0:.2f}s  DURATION={dur:.3f}s  ({target_st}st -> {dur/target_st*1000:.0f} ms/st, {target_st/dur:.3f} st/s)  peak={reached:.1f}st")
        return dur
    else:
        print(f"  {label:14s} did not reach {target_st}st (max {ss.max():.1f}st) in window")
        return None


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "dur":
    m, fs = load()
    for name, a, b, tgt in [
        ("L1 +3 @48", 31.30, 34.60, 3), ("L2 +3 @48", 116.90, 120.20, 3),
        ("L1 +12 @48", 34.80, 41.20, 12), ("L2 +12 @48", 120.40, 126.70, 12),
        ("L1 +12 @24", 41.30, 46.20, 12), ("L2 +12 @24", 126.85, 131.70, 12),
        ("L1 +12 @96", 46.30, 55.90, 12), ("L2 +12 @96", 131.85, 141.40, 12)]:
        porta_duration(m, fs, a, b, tgt, name)
