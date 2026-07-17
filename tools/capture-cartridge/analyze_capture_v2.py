#!/usr/bin/env python3
"""
analyze_capture_v2.py -- offline analysis of a capture-cartridge v2 console
recording against its caesar_play prediction, extracting the measured constants
each probe was built to close.

A capture-cartridge v2 battery replaces a MiiPlaza BGM with a scripted probe
schedule (see tools/capture-cartridge/MANIFEST.md). Each pass opens with a PILOT
tone (prg23 key60 vel127, center) used as the per-channel normalisation
reference, then walks a sequence of probes. Track A probes pan / LPF / velocity /
voice-steal; track B probes release / decay / vibrato / portamento. The console
loops the battery forever; a line-in recording therefore holds 2+ passes with the
first one partial (recording started mid-playback).

This script:
  1. parses the MANIFEST probe schedule for the requested track,
  2. segments the recording into passes by cross-correlating its onset-strength
     envelope against the (single-pass) prediction, measuring the loop period,
  3. picks the cleanest complete pass (or a requested pass),
  4. fits an affine tick->time map for that pass (robust to the console clock),
  5. normalises each channel to the pilot tone,
  6. dispatches each probe to a per-section analyser and prints the measured
     value beside the player's current-law prediction, flagging disagreements.

It is a *measurement/tolerance* comparison, never a byte A/B: an analog line-in
capture has a ~-70 dBFS noise floor and can never be sample-identical to the dry
render. A disagreement between console and prediction is a FINDING, not a failure.

Dependencies: numpy, scipy, soundfile. (The sibling console_tolerance.py is
numpy-only; this tool uses scipy.signal.welch for the LPF spectra and soundfile
for float/PCM WAV I/O.)

Usage:
  python analyze_capture_v2.py --track A \
      --wav BATTERY_A_v2_console.wav \
      --prediction build/cartridge-v2/PREDICTION_battery_A.wav \
      --manifest  build/cartridge-v2/MANIFEST.md
"""
import argparse, re, sys
import numpy as np

try:
    import soundfile as sf
except ImportError:
    sys.stderr.write("ERROR: needs soundfile (pip install soundfile)\n"); sys.exit(2)
try:
    from scipy.signal import welch
except ImportError:
    sys.stderr.write("ERROR: needs scipy (pip install scipy)\n"); sys.exit(2)


# --------------------------------------------------------------------------- #
# MANIFEST parsing
# --------------------------------------------------------------------------- #
def parse_manifest(path, track):
    """Return [(tick_s, section, probe_text)] for the requested track's schedule."""
    want = f"## Track {track} probe schedule"
    rows, in_tbl = [], False
    with open(path, encoding="utf-8") as fh:
        lines = fh.readlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith(want):
            i += 1
            while i < len(lines) and not lines[i].startswith("## "):
                m = re.match(r"\s*\|\s*([\d.]+)\s*\|.*?\|\s*(\w+)\s*\|\s*(.+?)\s*\|",
                             lines[i])
                if m and m.group(1)[0].isdigit():
                    rows.append((float(m.group(1)), m.group(2), m.group(3).strip()))
                i += 1
            break
        i += 1
    return rows


# --------------------------------------------------------------------------- #
# Envelope / onset primitives
# --------------------------------------------------------------------------- #
class Recording:
    def __init__(self, path, hop=0.002, win=0.02):
        x, sr = sf.read(path, always_2d=True)
        self.sr = sr
        self.n_ch = x.shape[1]
        self.ch = [x[:, c] for c in range(x.shape[1])]
        self.mono = x.mean(axis=1)
        # a single work channel: prefer the loudest (a battery panned toward one
        # side reads best on the channel it pans into); dual-mono => any channel.
        rms = [np.sqrt(np.mean(c ** 2)) for c in self.ch]
        self.work = self.ch[int(np.argmax(rms))]
        self.dur = len(self.mono) / sr
        self._hop, self._win = hop, win
        self._env()

    def _env(self):
        h = int(self._hop * self.sr); w = int(self._win * self.sr)
        n = (len(self.work) - w) // h
        e = np.empty(n)
        for i in range(n):
            seg = self.work[i * h:i * h + w]
            e[i] = np.sqrt(np.mean(seg ** 2))
        self.env = e
        self.envdb = 20 * np.log10(e + 1e-12)

    def peak_in(self, tc, half=0.28):
        """(time, dBFS) of the energy peak within +-half seconds of tc."""
        h = self._hop
        i0 = max(0, int((tc - half) / h)); i1 = min(len(self.env), int((tc + half) / h))
        if i1 <= i0:
            return tc, -120.0
        k = i0 + int(np.argmax(self.env[i0:i1]))
        return k * h, self.envdb[k]

    def rms(self, ta, tb, chan=None):
        x = self.work if chan is None else self.ch[chan]
        a = max(0, int(ta * self.sr)); b = min(len(x), int(tb * self.sr))
        return np.sqrt(np.mean(x[a:b] ** 2)) if b > a else 0.0


def onset_env(x, sr, hop=256, nfft=1024):
    n = (len(x) - nfft) // hop
    win = np.hanning(nfft); prev = None; flux = np.empty(n)
    for i in range(n):
        mag = np.abs(np.fft.rfft(x[i * hop:i * hop + nfft] * win))
        flux[i] = 0.0 if prev is None else np.sum(np.maximum(mag - prev, 0))
        prev = mag
    return flux, hop


# --------------------------------------------------------------------------- #
# Segmentation: find pass pilots + loop period
# --------------------------------------------------------------------------- #
def segment(rec, pred_mono, pred_sr, nominal_period):
    """Return (pilots[s], period_s, corr_of_each_pilot) via onset-env xcorr."""
    fc, hop = onset_env(rec.work, rec.sr)
    fp, _ = onset_env(pred_mono, pred_sr)
    fcn = (fc - fc.mean()) / (fc.std() + 1e-9)
    fpn = (fp - fp.mean()) / (fp.std() + 1e-9)
    xcorr = np.correlate(fcn, fpn, mode="valid") / len(fpn)
    gmax = xcorr.max()
    md = int(20 * rec.sr / hop)  # >=20 s between distinct pilots
    peaks = []
    for o in np.argsort(xcorr)[::-1]:
        if xcorr[o] < 0.4 * gmax:
            break
        if all(abs(o - p) > md for p in peaks):
            peaks.append(int(o))
    peaks.sort()

    def refine(p):
        if 0 < p < len(xcorr) - 1:
            y0, y1, y2 = xcorr[p - 1], xcorr[p], xcorr[p + 1]
            den = y0 - 2 * y1 + y2
            return p + (0.5 * (y0 - y2) / den if den else 0.0)
        return p
    pilots = [refine(p) * hop / rec.sr for p in peaks]
    corrs = [xcorr[p] for p in peaks]
    # period: prefer the mean successive-pilot gap; fall back to onset-env autocorr
    if len(pilots) >= 2:
        period = float(np.mean(np.diff(pilots)))
    else:
        e = fc - fc.mean()
        ac = np.correlate(e, e, mode="full")[len(e) - 1:]
        lo = int((nominal_period - 3) / (hop / rec.sr)); hi = int((nominal_period + 3) / (hop / rec.sr))
        p = lo + int(np.argmax(ac[lo:hi]))
        period = p * hop / rec.sr
    return pilots, period, corrs


def fit_map(rec, sched, pilot_guess, clock0=0.999):
    """Least-squares affine map t_console = a + b*tick, snapping detectable
    (non-silent) probe ticks to local energy peaks; outlier-rejected."""
    ticks = [t for t, s, _ in sched if s in ("PILOT", "A1", "A2", "A3", "B1", "B3")]
    a, b = pilot_guess, clock0
    for _ in range(5):
        xs, ys = [], []
        for tk in ticks:
            pt, pdb = rec.peak_in(a + b * tk, 0.25)
            if pdb > -42 and abs(pt - (a + b * tk)) < 0.25:
                xs.append(tk); ys.append(pt)
        if len(xs) < 2:
            break
        xs = np.array(xs); ys = np.array(ys)
        A = np.vstack([np.ones_like(xs), xs]).T
        (a, b), *_ = np.linalg.lstsq(A, ys, rcond=None)
        # reject residual outliers
        res = ys - (a + b * xs)
        keep = np.abs(res) < max(0.06, 3 * np.std(res))
        if keep.sum() >= 2 and keep.sum() < len(xs):
            A = np.vstack([np.ones(keep.sum()), xs[keep]]).T
            (a, b), *_ = np.linalg.lstsq(A, ys[keep], rcond=None)
    return a, b, len(xs)


# --------------------------------------------------------------------------- #
# Pan-law helpers
# --------------------------------------------------------------------------- #
def eqpow_right(byte):      # equal-power cos/sin: R = sin(byte/127 * pi/2)
    return np.sin(byte / 127.0 * np.pi / 2)
def sqrt_right(byte):       # constant-power sqrt: R = sqrt(byte/127)
    return np.sqrt(byte / 127.0)
def db(x):
    return 20 * np.log10(np.abs(x) + 1e-12)


def peak_sum(rec, tc, half=0.28):
    """(time, dB) of the L+R energy peak within +-half s of tc. Unlike
    Recording.peak_in (which reads a single channel), this sums both channels so
    a hard-panned note is found regardless of which side it lands on -- required
    for the L/R-split analyser, whose whole point is that byte 0 and byte 127
    sit on OPPOSITE channels."""
    if rec.n_ch < 2:
        return rec.peak_in(tc, half)
    L, R = rec.ch[0], rec.ch[1]
    h = int(rec._hop * rec.sr); w = int(rec._win * rec.sr)
    i0 = max(0, int((tc - half) / rec._hop)); i1 = int((tc + half) / rec._hop)
    best = (tc, -120.0)
    for i in range(i0, i1):
        seg = L[i * h:i * h + w] ** 2 + R[i * h:i * h + w] ** 2
        d = 10 * np.log10(np.mean(seg) + 1e-12) if len(seg) else -120.0
        if d > best[1]:
            best = (i * h / rec.sr, d)
    return best


def analyze_pan_split(rec, a, b, sched, out):
    """TRUE-STEREO pan analyser: measure BOTH channels' RMS per pan byte and the
    inter-channel L/R split in dB, referenced to byte-64 centre (which removes any
    capture channel imbalance). The split MAGNITUDE discriminates the two candidate
    laws -- constant-power sqrt (byte32/96 -> ~4.7/4.9 dB) vs equal-power cos/sin
    (~7.6/7.9 dB) -- which a single channel (analyze_pan) cannot do. Sign convention:
    split = 20*log10(Lrms/Rrms), so + = LEFT louder. Requires >=2 channels."""
    if rec.n_ch < 2:
        out.append("  A1 PAN-SPLIT: recording is mono; L/R split not measurable.")
        return
    L, R = rec.ch[0], rec.ch[1]
    probes = [(t, int(re.search(r"byte (\d+)", p).group(1)))
              for t, s, p in sched if s == "A1"]

    def split_sqrt(byte):    # L=sqrt((127-b)/127), R=sqrt(b/127)  (task convention)
        return db(np.sqrt((127 - byte) / 127.0)) - db(np.sqrt(byte / 127.0))

    def split_cossin(byte):  # L=cos(b/127*pi/2), R=sin(b/127*pi/2)
        return db(np.cos(byte / 127.0 * np.pi / 2)) - db(np.sin(byte / 127.0 * np.pi / 2))

    # byte-64 reference removes the capture's own L/R channel imbalance
    t64 = next(t for t, by in probes if by == 64)
    pt64, _ = peak_sum(rec, a + b * t64, 0.28)
    ref = db(rec.rms(pt64 + 0.10, pt64 + 0.40, 0)) - db(rec.rms(pt64 + 0.10, pt64 + 0.40, 1))

    out.append("  A1 PAN-SPLIT  (TRUE STEREO: L/R split vs byte64, + = LEFT louder)")
    out.append(f"     byte64 channel-imbalance reference = {ref:+.3f} dB")
    out.append("     byte |  Lrms  |  Rrms  | relSplit | sqrt | cos/sin | verdict (by |split|)")
    disc = []
    for t, by in probes:
        pt, pdb = peak_sum(rec, a + b * t, 0.28)
        rL = rec.rms(pt + 0.10, pt + 0.40, 0); rR = rec.rms(pt + 0.10, pt + 0.40, 1)
        raw = db(rL) - db(rR); rel = raw - ref
        sq, cs = split_sqrt(by), split_cossin(by)
        if by in (0, 127):
            iso = abs(rel)
            out.append(f"     {by:4d} | {db(rL):+6.1f} | {db(rR):+6.1f} | {rel:+7.1f}  |"
                       f"  extreme pan: silent side {iso:.0f} dB down (both laws -> -inf; "
                       f"= capture isolation floor)")
            continue
        # discriminate by magnitude (direction may be inverted vs the player)
        v = "sqrt" if abs(abs(rel) - abs(sq)) < abs(abs(rel) - abs(cs)) else "cos/sin"
        disc.append((by, rel, sq, cs))
        out.append(f"     {by:4d} | {db(rL):+6.1f} | {db(rR):+6.1f} | {rel:+7.2f}  | "
                   f"{sq:+4.2f} | {cs:+5.2f}  | {v}")
    if disc:
        esq = np.mean([abs(abs(r) - abs(s)) for _, r, s, c in disc])
        ecs = np.mean([abs(abs(r) - abs(c)) for _, r, s, c in disc])
        law = "SQRT (constant-power)" if esq < ecs else "EQUAL-POWER (cos/sin)"
        out.append(f"     -> PAN LAW (split level): {law}  "
                   f"(mean |err| sqrt {esq:.2f} dB, cos/sin {ecs:.2f} dB)")
        # direction: sign of measured vs the task's L=sqrt((127-b)/127) mapping
        b32 = next((r for by, r, s, c in disc if by == 32), None)
        if b32 is not None:
            player_sign = "byte0->LEFT (increasing byte pans RIGHT)"
            meas_sign = ("byte0->LEFT (increasing byte pans RIGHT)" if b32 > 0
                         else "byte0->RIGHT (increasing byte pans LEFT)")
            note = "" if b32 > 0 else "  ** INVERTED vs the player's cos/sin mapping **"
            out.append(f"     -> PAN DIRECTION: console {meas_sign}; "
                       f"player renders {player_sign}.{note}")


# --------------------------------------------------------------------------- #
# Section analysers
# --------------------------------------------------------------------------- #
def analyze_pan(rec, a, b, sched, out):
    probes = [(t, int(re.search(r"byte (\d+)", p).group(1)))
              for t, s, p in sched if s == "A1"]
    out.append("  A1 PAN  (single-channel gain vs pan byte, rel byte64 center)")
    out.append("     byte | measured | cos/sin | sqrt   | verdict")
    ref_t = next(t for t, by in probes if by == 64)
    rt, _ = rec.peak_in(a + b * ref_t, 0.28)
    ref = rec.rms(rt + 0.10, rt + 0.40)
    rows = []
    for t, by in probes:
        pt, pdb = rec.peak_in(a + b * t, 0.28)
        if pdb < -45:
            out.append(f"     {by:4d} | SILENT (opposite-side pan not in recorded channel)")
            continue
        r = rec.rms(pt + 0.10, pt + 0.40)
        meas = db(r / ref)
        cs = db(eqpow_right(by) / eqpow_right(64))
        sq = db(sqrt_right(by) / sqrt_right(64))
        verdict = "sqrt" if abs(meas - sq) < abs(meas - cs) else "cos/sin"
        rows.append((by, meas, cs, sq))
        out.append(f"     {by:4d} | {meas:+7.2f} | {cs:+7.2f} | {sq:+6.2f} | {verdict}")
    # overall verdict from discriminator bytes (32, 96)
    disc = [r for r in rows if r[0] in (32, 96)]
    if disc:
        ecs = np.mean([abs(m - cs) for _, m, cs, sq in disc])
        esq = np.mean([abs(m - sq) for _, m, cs, sq in disc])
        law = "SQRT (constant-power)" if esq < ecs else "EQUAL-POWER (cos/sin)"
        out.append(f"     -> PAN LAW: {law}  "
                   f"(mean |err| cos/sin {ecs:.2f} dB, sqrt {esq:.2f} dB)")


def analyze_lpf(rec, a, b, sched, out):
    probes = [(t, int(re.search(r"byte (\d+)", p).group(1)))
              for t, s, p in sched if s == "A2"]
    # key of the LPF note (its .bcseq sig starts 0x48 = key 72); harmonic comb
    # is only used implicitly by the smoothed transfer function.
    def note_psd(t):
        pt, pdb = rec.peak_in(a + b * t, 0.28)
        seg = rec.work[int((pt + 0.05) * rec.sr):int((pt + 0.45) * rec.sr)]
        fr, P = welch(seg, fs=rec.sr, nperseg=4096, noverlap=3072, window="hann")
        return fr, P, pt, pdb
    D = {by: note_psd(t) for t, by in probes}
    open_byte = max(D)                    # byte 64 = open reference
    fr = D[open_byte][0]; Popen = D[open_byte][1]

    def logsmooth(H, frac=0.15):
        lf = np.log(fr + 1e-9); out_ = np.full_like(H, np.nan)
        for i in range(len(fr)):
            if fr[i] < 100 or fr[i] > 15500:
                continue
            out_[i] = np.median(H[np.abs(lf - lf[i]) < frac])
        return out_

    out.append("  A2 LPF  (1-pole vs 2-pole fit + smoothed -3 dB corner vs open)")
    out.append("     byte | corner(-3dB) | 1p RMSres | 2p RMSres | order | player-pred")
    corners = {}
    for by in sorted(D):
        fr_, P, pt, pdb = D[by]
        H = 10 * np.log10((P + 1e-20) / (Popen + 1e-20))
        Hs = logsmooth(H)
        band = ~np.isnan(Hs)
        ff, HH = fr[band], Hs[band]
        below = np.where(HH <= -3.0)[0]
        corner = ff[below[0]] if len(below) else float("inf")
        corners[by] = corner
        # 1p vs 2p model fit on the smoothed response
        m = (HH <= 1) & (HH >= -45)
        def fitorder(order):
            best = (None, 1e9)
            for fc in np.logspace(np.log10(150), np.log10(16000), 400):
                pr = -10 * np.log10(1 + (ff / fc) ** (2 * order))
                r = np.mean((pr[m] - HH[m]) ** 2) if m.sum() else 1e9
                if r < best[1]:
                    best = (fc, r)
            return best
        if by == open_byte:
            out.append(f"     {by:4d} | OPEN ~{corner:5.0f} Hz (flat transfer => filter bypassed)")
            continue
        (fc1, r1), (fc2, r2) = fitorder(1), fitorder(2)
        order = "1-pole" if r1 < r2 else "2-pole"
        pred = 4100 * 2 ** ((by - 48) * 187.5 / 1200)
        out.append(f"     {by:4d} | {corner:8.0f} Hz | {np.sqrt(r1):8.2f} | {np.sqrt(r2):8.2f} | "
                   f"{order} | {pred:6.0f} Hz")
    # cents/unit across the filtered bytes
    fbytes = sorted(k for k in corners if k != open_byte and np.isfinite(corners[k]))
    if len(fbytes) >= 2:
        lo, hi = fbytes[0], fbytes[-1]
        cpu = 1200 * np.log2(corners[hi] / corners[lo]) / (hi - lo)
        out.append(f"     -> slope {cpu:.1f} cents/unit (model assumes 187.5); "
                   f"byte48 corner {corners.get(48, float('nan')):.0f} Hz vs shipped 4100 Hz anchor")


def analyze_velocity(rec, a, b, sched, pilot_rms, out):
    probes = [(t, int(re.search(r"velocity (\d+)", p).group(1)))
              for t, s, p in sched if s == "A3"]
    out.append("  A3 VELOCITY  (note level rel pilot vs (vel/127)^2 law)")
    out.append("     vel | measured | (vel/127)^2 | residual")
    meas, law = [], []
    for t, v in probes:
        pt, pdb = rec.peak_in(a + b * t, 0.28)
        r = rec.rms(pt + 0.10, pt + 0.40)
        m = db(r / pilot_rms); l = db((v / 127.0) ** 2)
        meas.append(m); law.append(l)
        out.append(f"     {v:4d} | {m:+7.2f} | {l:+8.2f} | {m - l:+.2f}")
    resid = np.array(meas) - np.array(law)
    # the constant offset is the pilot/vel127 normalisation; report scatter about it
    off = np.median(resid)
    scatter = np.max(np.abs(resid - off))
    out.append(f"     -> squared-law {'CONFIRMED' if scatter < 0.5 else 'DEVIATES'}: "
               f"residual scatter {scatter:.2f} dB about a {off:+.2f} dB offset")


def analyze_steal(rec, a, b, sched, pilot_rms, pred_path, out):
    t = next(t for t, s, _ in sched if s == "A4")
    pt, pdb = rec.peak_in(a + b * t, 0.35)
    stack = rec.rms(pt + 0.15, pt + 0.85)
    out.append("  A4 STEAL  (30 stacked voices, 24-voice pool expected)")
    out.append(f"     stack rel pilot: {db(stack / pilot_rms):+.2f} dB")
    # compare against the prediction's steal region (the player's 24-voice render)
    try:
        pr, psr = sf.read(pred_path, always_2d=True)
        pm = pr.mean(axis=1)
        ppil = np.sqrt(np.mean(pm[int(0.15 * psr):int(0.75 * psr)] ** 2))
        # prediction plays at nominal ticks
        pstack = np.sqrt(np.mean(pm[int((t + 0.15) * psr):int((t + 0.85) * psr)] ** 2))
        d = db((stack / pilot_rms) / (pstack / ppil))
        out.append(f"     prediction (24-voice pool) rel pilot: {db(pstack / ppil):+.2f} dB")
        out.append(f"     -> console-vs-24voice delta {d:+.2f} dB "
                   f"({'consistent with 24 (not 30)' if d < 0.6 else 'exceeds 24-voice render'})")
    except Exception as e:
        out.append(f"     (prediction steal compare unavailable: {e})")
    out.append("     NOTE: exact surviving-voice count is not cleanly resolvable from a "
               "mono chromatic cluster (SE energy ~11 kHz, harmonic overlaps).")


# --------------------------------------------------------------------------- #
# Track-B analysers (best-effort; validated when the track-B capture lands)
# --------------------------------------------------------------------------- #
def analyze_release(rec, a, b, sched, out):
    probes = [(t, re.search(r"byte (\d+)", p).group(1)) for t, s, p in sched
              if s in ("B1", "B1r")]
    if not probes:
        return
    out.append("  B1 RELEASE  (note-off decay slope; best-effort tau grid-fit)")
    for t, byte in probes:
        pt, pdb = rec.peak_in(a + b * t, 0.4)
        # fit exponential decay over the tail after the note body
        seg = rec.envdb[int((pt + 0.3) / rec._hop):int((pt + 3.0) / rec._hop)]
        if len(seg) < 8:
            out.append(f"     byte {byte}: tail too short to fit"); continue
        tt = np.arange(len(seg)) * rec._hop
        m = seg > seg.max() - 40
        if m.sum() >= 4:
            slope = np.polyfit(tt[m], seg[m], 1)[0]  # dB/s
            out.append(f"     byte {byte}: decay {slope:+.1f} dB/s")


def analyze_vibrato(rec, a, b, sched, out):
    probes = [(t, p) for t, s, p in sched if s == "B3"]
    if not probes:
        return
    out.append("  B3 VIBRATO  (LFO rate from fine log-envelope spectrum)")
    for t, p in probes:
        pt, pdb = rec.peak_in(a + b * t, 0.3)
        seg = rec.work[int((pt + 0.2) * rec.sr):int((pt + 1.6) * rec.sr)]
        if len(seg) < rec.sr // 2:
            continue
        # amplitude envelope -> its modulation spectrum, 1..12 Hz
        h = int(0.002 * rec.sr)
        e = np.array([np.sqrt(np.mean(seg[k:k + h] ** 2)) for k in range(0, len(seg) - h, h)])
        e = np.log(e + 1e-9); e -= e.mean()
        sp = np.abs(np.fft.rfft(e * np.hanning(len(e))))
        frq = np.fft.rfftfreq(len(e), h / rec.sr)
        band = (frq >= 1) & (frq <= 12)
        if band.any():
            rate = frq[band][np.argmax(sp[band])]
            pred = re.search(r"([\d.]+) Hz", p)
            out.append(f"     {p[:40]:40s}: {rate:.2f} Hz"
                       + (f" (predicted {pred.group(1)})" if pred else ""))


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wav", required=True, help="console recording (line-in)")
    ap.add_argument("--prediction", required=True, help="caesar_play PREDICTION wav (one pass)")
    ap.add_argument("--manifest", required=True, help="capture-cartridge v2 MANIFEST.md")
    ap.add_argument("--track", default="A", choices=["A", "B"], help="which track's schedule")
    ap.add_argument("--nominal-period", type=float, default=36.06,
                    help="design pass length (s) for the tempo check")
    ap.add_argument("--pass", dest="which_pass", default="auto",
                    help="'auto' (cleanest complete pass), 'all', or a 0-based index")
    args = ap.parse_args()

    sched = parse_manifest(args.manifest, args.track)
    if not sched:
        sys.stderr.write(f"ERROR: no Track {args.track} schedule in {args.manifest}\n")
        sys.exit(2)

    rec = Recording(args.wav)
    pred, psr = sf.read(args.prediction, always_2d=True)
    pred_mono = pred.mean(axis=1)

    out = []
    out.append(f"=== capture-cartridge v2 analysis: Track {args.track} ===")
    out.append(f"recording: {args.wav}")
    out.append(f"  {rec.dur:.2f} s, {rec.sr} Hz, {rec.n_ch} ch, "
               f"peak {db(np.max(np.abs(rec.mono))):+.1f} dBFS, "
               f"floor ~{np.percentile(rec.envdb, 2):.0f} dBFS-RMS")

    # channel-configuration check (dual-mono defect detection)
    if rec.n_ch >= 2:
        d = rec.ch[0] - rec.ch[1]
        drms = db(np.sqrt(np.mean(d ** 2)))
        srms = db(np.sqrt(np.mean((rec.ch[0] + rec.ch[1]) ** 2)))
        if srms - drms > 40:
            out.append(f"  ** CHANNELS EFFECTIVELY MONO: L-R {drms:.0f} dBFS is "
                       f"{srms - drms:.0f} dB below L+R -> no stereo information. **")
            out.append(f"     Inter-channel L/R pan split (byte 32/96 discriminator) is "
                       f"NOT recoverable; the single recorded channel still yields the "
                       f"pan LAW via its gain-vs-byte curve.")

    # segmentation
    pilots, period, corrs = segment(rec, pred_mono, psr, args.nominal_period)
    out.append("")
    out.append(f"segmentation: {len(pilots)} pilot(s) at "
               + ", ".join(f"{p:.3f}s" for p in pilots))
    out.append(f"  loop period {period:.3f} s (nominal {args.nominal_period:.2f} s "
               f"=> {period / args.nominal_period:.4f}x)")

    # choose pass(es)
    if not pilots:
        sys.stderr.write("ERROR: no pilot detected -- recording looks wrong; stopping.\n")
        print("\n".join(out)); sys.exit(2)
    if args.which_pass == "all":
        chosen = list(range(len(pilots)))
    elif args.which_pass == "auto":
        chosen = [int(np.argmax(corrs))]
    else:
        chosen = [int(args.which_pass)]

    for ci in chosen:
        pilot = pilots[ci]
        a, b, nfit = fit_map(rec, sched, pilot)
        out.append("")
        out.append(f"--- PASS {ci} (pilot {pilot:.3f}s, xcorr {corrs[ci]:.2f}) ---")
        out.append(f"  tick->time map: t = {a:.4f} + {b:.5f}*tick  "
                   f"(within-pass clock {b:.4f}x, {nfit} anchors)")
        ppt, _ = rec.peak_in(a, 0.25)
        pilot_rms = rec.rms(ppt + 0.15, ppt + 0.75)
        out.append(f"  pilot normalisation: {db(pilot_rms):+.2f} dBFS-RMS")

        sections = {s for _, s, _ in sched}
        if "A1" in sections:
            analyze_pan(rec, a, b, sched, out)
            analyze_pan_split(rec, a, b, sched, out)
        if "A2" in sections:
            analyze_lpf(rec, a, b, sched, out)
        if "A3" in sections:
            analyze_velocity(rec, a, b, sched, pilot_rms, out)
        if "A4" in sections:
            analyze_steal(rec, a, b, sched, pilot_rms, args.prediction, out)
        if sections & {"B1", "B1r"}:
            analyze_release(rec, a, b, sched, out)
        if "B3" in sections:
            analyze_vibrato(rec, a, b, sched, out)

    print("\n".join(out))


if __name__ == "__main__":
    main()
