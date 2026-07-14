#!/usr/bin/env python3
"""
console_tolerance.py -- the suite stage-2 Phase IV (C11) console-tolerance net.

Ingests (a) a New 3DS line-in CAPTURE of a sequence and (b) a `caesar-play
--render` of the SAME sequence, and produces a pass/fail verdict against the
stage-2 proof criterion:

    the render matches the console within tolerance, EXCEPT the reverb tail.

The console has DSP reverb the dry player deliberately lacks (that is stage 3).
So every metric is computed over DRY-DOMINANT regions -- attack transients and
sustained note bodies -- and the reverb-attributable residual (the console
energy living where the dry render has already fallen silent) is REPORTED
SEPARATELY and never fails the verdict. That residual is stage 3's target.

Method precedent (all in this repo):
  * tools/surround-probe/tools/analyze_surround.py  -- the per-channel Welch-PSD
    MAGDEV metric (median-subtracted spectral reshaping, level-independent) and
    the hand-rolled numpy WAV reader / Welch estimator reused verbatim here.
  * docs/HISTORY.md 2026-07-08 (EMPTY_LANDSCAPE / BGM_MAIN_Mii_Only_One) -- the
    envelope-fit precedent: align at the -25 dB crossing, least-squares fit over
    the reliable dB window ABOVE the recording's noise floor.
  * docs/HISTORY.md 2026-07-11 (surround-probe split_run) -- the fixture-realism
    lesson: the synthetic validation passed but the real capture broke the
    segmenter because the fixture was UNREALISTIC. The --self-validate fixtures
    here therefore model the real capture chain: capture-rate resample, gain
    offset, a -85 dBFS noise floor, a lead-in, +-50 ppm clock drift, and a
    synthetic exponential reverb tail (so the reverb-exception path is exercised).

Only dependency is numpy (scipy is NOT installed in this environment -- every
DSP primitive below is hand-rolled on numpy.fft, exactly like the surround-probe).

Exit contract (identical to ab-verify / diag-goldens / play-goldens):
  0  the render matches the console within tolerance (reverb tail excepted)
  1  one or more metrics are out of tolerance
  2  harness error -- NOTHING was verified, never read as a pass

Usage:
  console_tolerance.py <console_capture.wav> <caesar_render.wav>
  console_tolerance.py --self-validate <render.wav> [--other <render2.wav>]
"""
import sys, os, struct, argparse

try:
    import numpy as np
except ImportError:
    sys.stderr.write("HARNESS ERROR: needs numpy:  python -m pip install numpy\n")
    sys.exit(2)

# --------------------------------------------------------------------------- #
# Tunable constants -- every tolerance is documented where it is used, and the
# metric/tolerance table is reproduced in README.md.
# --------------------------------------------------------------------------- #
HOP_MS          = 5.0        # envelope frame (matches analyze.py's 5 ms frames)
NFFT            = 16384      # Welch block (matches analyze_surround.py)
PSD_FLO         = 100.0      # Welch trusted band low  (Hz)
PSD_FHI         = 14000.0    # Welch trusted band high (Hz)
SNR_TRUST_DB    = 15.0       # a PSD bin is trusted if >= floor + this (surround)
FLOOR_GUARD_DB  = 6.0        # trusted-envelope lower bound = measured floor + this
PEAK_GUARD_DB   = 1.0        # trusted-envelope upper bound = render peak - this

# --- pass tolerances ---
ENV_RESIDUAL_TOL = 3.0       # median-subtracted envelope-shape residual RMS (dB), music
ENV_RESIDUAL_TOL_NOTE = 6.0  # ... for a SHORT single-note capture (reverb is a larger
                             # fraction of a short decay; the tail-decay diagnostic
                             # carries the release calibration instead)
NOTE_MAX_SEC     = 3.0       # a capture with < this much sounding content is a "note"
ENV_SLOPE_LO     = 0.85      # dB-domain envelope compression/expansion slope band
ENV_SLOPE_HI     = 1.15
REVERB_EXCLUDE_DB = 4.0      # within the trusted window, a frame where the CONSOLE
                             # sits > this above the render (after the global level
                             # offset) is reverb-dominated -> excluded from the dry
                             # envelope-shape residual and counted as reverb. A frame
                             # where the RENDER is louder (a wrong/padded envelope) is
                             # NOT excluded, so it still fails the shape residual.
ONSET_SLOPE_TOL  = 0.010     # |tempo slope - 1| : 1.0 % (frame-period / tempo)
ONSET_CORR_MIN   = 0.08      # onset-strength xcorr floor: below this the two onset
                             # trains do not correspond at all -> tempo inconclusive
                             # (dry-vs-reverb reshaping keeps a true match near ~0.2,
                             #  so this is a "no correspondence" floor, not a fit gate)
ONSET_MIN_COUNT  = 4         # below this, onset-timing is N/A (single-note SEs)
ONSET_MIN_SPAN_S = 1.0       # onsets must ALSO span this long: a single percussive
                             # note's clustered strength peaks are not a tempo
PSD_DIST_TOL     = 6.0       # per-channel MAGDEV over the sustained body (dB)
LOUD_MEDIAN_TOL  = 4.0       # |d(median-below-peak)| (dB)  -- EMPTY_LANDSCAPE precedent
LOUD_RANGE_TOL   = 6.0       # |d(dynamic range p95-p5)| (dB)

# --- fixture realism (the 2026-07-11 lesson: model the real capture chain) ---
FIX_CAPTURE_RATE = 192000    # line-in was 48k or 192k (CAPTURE-PROTOCOL); force resample
FIX_DRIFT_PPM    = 50.0      # +-50 ppm ADC-vs-console clock drift
FIX_LEADIN_MS    = 37.0      # a few ms of lead-in silence before the take
FIX_GAIN_DB      = -6.0      # line level vs full scale
FIX_NOISE_DBFS   = -85.0     # the surround-probe's measured line-in floor (HISTORY)
FIX_REVERB_TAU_S = 0.40      # synthetic reverb decay time constant
FIX_REVERB_SEND_DB = -20.0   # modest DSP reverb send (not a room)
FIX_REVERB_PREDELAY_MS = 18.0
FIX_SEED         = 0x0C11    # deterministic noise -> deterministic self-validate

# release/decay grid for the exposed-tail fit (the EMPTY_LANDSCAPE grid)
TAIL_GRID_S = [0.05, 0.1, 0.2, 0.35, 0.5, 0.8, 1.2, 2.0, 3.5, 5.0]


# ========================================================================== #
# WAV I/O  (reused from surround-probe/tools/analyze.py -- 16/24/32 int + float)
# ========================================================================== #
def read_wav(path):
    with open(path, "rb") as f:
        buf = f.read()
    if buf[:4] != b"RIFF" or buf[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(buf):
        cid = buf[pos:pos + 4]
        csz = struct.unpack("<I", buf[pos + 4:pos + 8])[0]
        body = buf[pos + 8:pos + 8 + csz]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and csz >= 40:  # WAVE_FORMAT_EXTENSIBLE
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif cid == b"data":
            data = body
        pos += 8 + csz + (csz & 1)
    if fmt is None or data is None:
        raise ValueError(f"{path}: missing fmt/data chunk")
    af, ch, sr, _, _, bits = fmt
    if af == 1:
        if bits == 16:   a = np.frombuffer(data, "<i2").astype(np.float64) / 32768.0
        elif bits == 32: a = np.frombuffer(data, "<i4").astype(np.float64) / 2147483648.0
        elif bits == 8:  a = (np.frombuffer(data, np.uint8).astype(np.float64) - 128) / 128.0
        elif bits == 24:
            u = np.frombuffer(data, np.uint8); u = u[:len(u) - (len(u) % 3)].reshape(-1, 3).astype(np.int32)
            v = u[:, 0] | (u[:, 1] << 8) | (u[:, 2] << 16)
            v = np.where(v >= (1 << 23), v - (1 << 24), v); a = v / 8388608.0
        else: raise ValueError(f"{path}: unsupported PCM bit depth {bits}")
    elif af == 3:
        if bits == 32:   a = np.frombuffer(data, "<f4").astype(np.float64)
        elif bits == 64: a = np.frombuffer(data, "<f8").copy()
        else: raise ValueError(f"{path}: unsupported float bit depth {bits}")
    else:
        raise ValueError(f"{path}: unsupported WAV format tag 0x{af:04X}")
    n = (a.size // ch) * ch
    return sr, a[:n].reshape(-1, ch)


# ========================================================================== #
# numpy DSP primitives (scipy-free)
# ========================================================================== #
def fft_resample(x, n_out):
    """Periodic FFT resample of a 1-D signal to n_out samples (scipy.signal.resample)."""
    n_in = x.size
    if n_in == n_out or n_in == 0:
        return x.copy()
    X = np.fft.rfft(x)
    n_out_bins = n_out // 2 + 1
    Y = np.zeros(n_out_bins, dtype=complex)
    k = min(X.size, n_out_bins)
    Y[:k] = X[:k]
    # split a copied Nyquist bin when downsampling to keep the signal real/scaled
    if n_out < n_in and (n_out % 2 == 0) and k > 0:
        Y[k - 1] *= 0.5
    y = np.fft.irfft(Y, n_out) * (float(n_out) / float(n_in))
    return y


def resample_wav(sr, wav, n_out):
    """Resample a stereo [N,ch] block to n_out frames, per channel."""
    out = np.zeros((n_out, wav.shape[1]))
    for c in range(wav.shape[1]):
        out[:, c] = fft_resample(wav[:, c], n_out)
    return out


def fft_convolve(x, h):
    """Full linear convolution of two 1-D signals via rfft (fast for long IRs)."""
    n = x.size + h.size - 1
    nfft = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(h, nfft), nfft)[:n]
    return y


def mono(wav):
    return wav.mean(axis=1)


def envelope_db(m, sr, hop_ms=HOP_MS):
    """Per-frame RMS envelope in dBFS, plus the hop in samples."""
    hop = max(1, int(round(hop_ms / 1000.0 * sr)))
    n = m.size // hop
    if n < 2:
        return np.array([-120.0]), hop
    e = np.sqrt(np.mean(np.square(m[:n * hop].reshape(n, hop)), axis=1))
    return 20.0 * np.log10(np.maximum(e, 1e-9)), hop


def smooth_db(x, win):
    """Moving-average smoothing of a dB envelope (edge-preserving)."""
    if win <= 1 or x.size < win:
        return x
    k = np.ones(win) / win
    return np.convolve(x, k, mode="same")


def onset_strength(edb):
    """Positive half-wave-rectified frame-to-frame dB rise = onset strength."""
    d = np.diff(edb, prepend=edb[:1])
    return np.maximum(d, 0.0)


def db(p):
    return 10.0 * np.log10(np.maximum(p, 1e-30))


def welch(x, sr, nfft=NFFT):
    """Per-channel Welch PSD (Hann, 50 % overlap). Returns freqs, PSD[bins]."""
    w = np.hanning(nfft); step = nfft // 2
    if x.size < nfft:
        x = np.pad(x, (0, nfft - x.size))
    nseg = 1 + (x.size - nfft) // step
    acc = np.zeros(nfft // 2 + 1)
    for i in range(nseg):
        seg = x[i * step:i * step + nfft] * w
        acc += np.abs(np.fft.rfft(seg)) ** 2
    acc /= max(nseg, 1)
    return np.fft.rfftfreq(nfft, 1.0 / sr), acc


def magdev(Sx, Sy, band, floor_pct=10.0):
    """Median-subtracted spectral reshaping RMS over trusted bins (dB).
    A pure level offset reads 0 (median removed); only spectral RESHAPING drives
    it up. This is analyze_surround.py's MAGDEV, per channel."""
    fl = np.percentile(Sx, floor_pct)
    m = band & (db(Sx) >= db(fl) + SNR_TRUST_DB) & (db(Sy) >= db(fl) + SNR_TRUST_DB)
    if int(m.sum()) < 8:
        return None
    H = 20.0 * np.log10(np.sqrt(np.maximum(Sy[m], 1e-30) / np.maximum(Sx[m], 1e-30)))
    H = H - np.median(H)
    return float(np.sqrt(np.mean(H ** 2)))


# ========================================================================== #
# Alignment  (onset lag + tempo/clock slope + level offset)
# ========================================================================== #
def integer_lag(env_c, env_r):
    """Integer frame lag (of C relative to R) maximising onset-strength xcorr."""
    a = onset_strength(env_c); b = onset_strength(env_r)
    a = a - a.mean(); b = b - b.mean()
    if a.size < 4 or b.size < 4:
        return 0
    xc = np.correlate(a, b, mode="full")
    return int(np.argmax(xc) - (b.size - 1))


def robust_onset_frames(edb, sr, hop):
    """Robust onset frame indices (an ABSOLUTE dB-rise / MAD threshold, not
    relative-to-max, so it is stable across the dry-vs-reverb reshaping)."""
    s = onset_strength(edb)
    if s.size == 0 or float(np.max(s)) <= 0:
        return np.array([], dtype=int)
    nz = s[s > 0]
    med = float(np.median(nz)); mad = float(np.median(np.abs(nz - med))) + 1e-9
    thr = max(3.0, med + 3.0 * mad)                 # >= 3 dB rise or 3-MAD above typical
    mg = max(2, int(round(0.040 * sr / hop)))
    out = []; last = -mg - 1
    for i in range(1, s.size - 1):
        if s[i] >= thr and s[i] >= s[i - 1] and s[i] > s[i + 1] and (i - last) >= mg:
            out.append(i); last = i
    return np.array(out, dtype=int)


def count_onsets(edb, sr, hop):
    return int(robust_onset_frames(edb, sr, hop).size)


def fft_xcorr_peak(a, b):
    """Best-lag normalized cross-correlation peak of two 1-D signals (FFT)."""
    a = a - a.mean(); b = b - b.mean()
    na = float(np.linalg.norm(a)); nb = float(np.linalg.norm(b))
    if na < 1e-9 or nb < 1e-9:
        return 0.0
    n = a.size + b.size - 1
    nfft = 1 << (n - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(a, nfft) * np.conj(np.fft.rfft(b, nfft)), nfft)
    return float(np.max(c)) / (na * nb)


def tempo_slope(env_c, env_r, hop, sr):
    """Tempo / clock ratio via a scale-search cross-correlation of the two
    onset-strength envelopes: for each candidate time-scale s the render's
    onset-strength is resampled by s and correlated with the capture's; the s
    that maximises the normalized peak is the tempo slope, and that peak is the
    confidence. Continuous (no discrete onset matching) so it is robust to the
    density/reverb mismatch that defeats peak-set matching on dense music."""
    ofr = robust_onset_frames(env_r, sr, hop)
    span = (ofr[-1] - ofr[0]) * hop / float(sr) if ofr.size >= 2 else 0.0
    if ofr.size < ONSET_MIN_COUNT or span < ONSET_MIN_SPAN_S:
        return None                                 # a single note is not a tempo
    oc = onset_strength(env_c); orr = onset_strength(env_r)
    def scan(grid, best):
        for s in grid:
            r = fft_resample(orr, max(2, int(round(orr.size * s))))
            pc = fft_xcorr_peak(oc, r)
            if pc > best[1]:
                best = (float(s), pc)
        return best
    best = scan(np.arange(0.90, 1.1001, 0.004), (1.0, -1.0))
    best = scan(np.arange(best[0] - 0.004, best[0] + 0.00401, 0.0005), best)
    return {"slope": best[0], "corr": best[1], "n_onsets": int(count_onsets(env_r, sr, hop))}


# ========================================================================== #
# The verdict
# ========================================================================== #
class Metric:
    def __init__(self, name, ok, value, tol, note="", applies=True):
        self.name, self.ok, self.value, self.tol, self.note, self.applies = \
            name, ok, value, tol, note, applies


def analyze_arrays(sr_c, wav_c, sr_r, wav_r, label=""):
    """Core comparison. Returns (metrics, diagnostics, reverb_report, sr)."""
    # 1) bring the capture onto the render's sample rate (the render is
    #    --rate matched; this handles a mismatched-rate capture + is the resample
    #    the fixture exercises).
    sr = sr_r
    if sr_c != sr_r:
        n_out = int(round(wav_c.shape[0] * sr_r / float(sr_c)))
        wav_c = resample_wav(sr_c, wav_c, n_out)
    if wav_c.shape[1] == 1: wav_c = np.repeat(wav_c, 2, axis=1)
    if wav_r.shape[1] == 1: wav_r = np.repeat(wav_r, 2, axis=1)

    mc, mr = mono(wav_c), mono(wav_r)
    env_c, hop = envelope_db(mc, sr)
    env_r, _   = envelope_db(mr, sr)

    # 2) alignment: integer onset lag, then the tempo/clock slope from onsets.
    lag = integer_lag(env_c, env_r)                 # frames (C relative to R)
    slope_fit = tempo_slope(env_c, env_r, hop, sr)

    # measured capture noise floor (5th pct of its dB envelope) and render peak
    floor_db = float(np.percentile(env_c, 5))
    floor_db = max(floor_db, FIX_NOISE_DBFS)        # never below the physical floor
    peak_r   = float(np.max(env_r))
    lo = floor_db + FLOOR_GUARD_DB
    hi = peak_r - PEAK_GUARD_DB

    # frame-aligned overlap of the two dB envelopes (shift C back by lag)
    def aligned_pair(a_c, a_r, lag):
        if lag >= 0:
            a_c2 = a_c[lag:]; a_r2 = a_r
        else:
            a_c2 = a_c; a_r2 = a_r[-lag:]
        n = min(a_c2.size, a_r2.size)
        return a_c2[:n], a_r2[:n]

    ec, er = aligned_pair(env_c, env_r, lag)

    # 3) drift/tempo-correct the RENDER envelope so envelope-fit measures SHAPE
    #    independent of tempo (tempo is the onset metric's job -> clean attribution).
    #    Only trust a CONFIDENT tempo estimate (high xcorr); otherwise leave the
    #    timeline alone so a spurious slope can never poison the envelope metric.
    slope = (slope_fit["slope"] if (slope_fit and slope_fit["corr"] >= ONSET_CORR_MIN
                                    and np.isfinite(slope_fit["slope"])) else 1.0)
    if abs(slope - 1.0) > 1e-4 and 0.5 < slope < 2.0:
        er = fft_resample(er, max(2, int(round(er.size * slope))))
        n = min(ec.size, er.size); ec, er = ec[:n], er[:n]

    # Tail-into-silence cut: exclude everything after the LAST SHARED onset. Past
    # the last note there is only a decay into silence, where the console carries
    # DSP reverb the dry render lacks (and, for a looping capture, where a slightly
    # longer render still has music the finite capture does not). This is the
    # "exclude decay tails into silence" the criterion calls for. A padded/wrong
    # envelope still fails on the many decays BEFORE the last onset.
    ofr_r = robust_onset_frames(er, sr, hop)
    ofr_c = robust_onset_frames(ec, sr, hop)
    hold = max(1, int(round(0.30 * sr / hop)))      # keep 300 ms past the last onset
    if ofr_r.size and ofr_c.size:
        tail_cut = min(int(ofr_r[-1]), int(ofr_c[-1])) + hold
    elif ofr_r.size:
        tail_cut = int(ofr_r[-1]) + hold
    else:
        tail_cut = er.size
    idx = np.arange(er.size)
    within = idx <= tail_cut

    # Fit the ADSHR-scale envelope, not 5 ms micro-structure: a light smoothing
    # keeps attack/decay/sustain/release shape (what the envelope constants govern)
    # while making the metric robust to reverb reshaping of a spiky fine envelope.
    sm = max(1, int(round(15.0 / HOP_MS)))
    ec = smooth_db(ec, sm); er = smooth_db(er, sm)

    # ---- METRIC 1: envelope fit over the trusted dB window (reverb excluded) ----
    # trusted = render above (floor+guard), below (peak-guard), and BEFORE the
    # tail-into-silence cut. The render-silent tail (where the console's reverb
    # lives) is below floor and past the cut -- that IS the reverb exception.
    trusted = (er >= lo) & (er <= hi) & within
    active_sec = float(np.sum(er >= lo)) * hop / sr
    is_note = active_sec < NOTE_MAX_SEC             # a short single-note capture
    env_tol = ENV_RESIDUAL_TOL_NOTE if is_note else ENV_RESIDUAL_TOL
    if int(trusted.sum()) >= 8:
        ect, ert = ec[trusted], er[trusted]
        diff = ect - ert
        level_off = float(np.median(diff))          # gain offset (render vs console)
        # down-weight/exclude reverb-dominated frames: where the console sits more
        # than REVERB_EXCLUDE_DB ABOVE the render (after the level offset), the tail
        # is reverb the dry player lacks -> excluded. Frames where the RENDER is
        # louder (a padded/wrong envelope) are kept, so they still fail.
        dry = (diff - level_off) <= REVERB_EXCLUDE_DB
        n_rev = int(trusted.sum()) - int(dry.sum())
        if int(dry.sum()) < 8:
            dry = np.ones_like(diff, dtype=bool)
        d = diff[dry]
        level_off = float(np.median(d))
        shape_res = float(np.sqrt(np.mean((d - level_off) ** 2)))
        A = np.vstack([ert[dry], np.ones(int(dry.sum()))]).T
        (eslope, _b), *_ = np.linalg.lstsq(A, ect[dry], rcond=None)
        eslope = float(eslope)
        env_ok = (shape_res <= env_tol) and (ENV_SLOPE_LO <= eslope <= ENV_SLOPE_HI)
        m_env = Metric("envelope-fit", env_ok, (shape_res, eslope),
                       (env_tol, ENV_SLOPE_LO, ENV_SLOPE_HI),
                       f"residual {shape_res:.2f} dB (<= {env_tol}{' note' if is_note else ''}), "
                       f"slope {eslope:.3f}, {n_rev} reverb frame(s) excluded")
    elif is_note:
        level_off = float("nan")
        m_env = Metric("envelope-fit", True, (float("nan"), float("nan")), env_tol,
                       "N/A (short note; see tail-decay diagnostic)", applies=False)
    else:
        level_off = float("nan")
        m_env = Metric("envelope-fit", False, (float("nan"), float("nan")), env_tol,
                       "too few trusted frames", applies=True)

    # ---- METRIC 2: onset-timing correlation (tempo / frame-period) ----
    # The scale-search SLOPE is the discriminator (tempo/clock ratio); the xcorr
    # is only a floor -- below it the onset trains do not correspond at all, so no
    # tempo can be asserted (inconclusive), and a genuinely different sequence
    # then fails on envelope/loudness instead. Above the floor, PASS iff the tempo
    # slope is within 1 %.
    if slope_fit is None:
        m_onset = Metric("onset-timing", True, (float("nan"), float("nan")),
                         (ONSET_SLOPE_TOL, ONSET_CORR_MIN), "N/A (too few onsets)", applies=False)
    else:
        sl, cc, non = slope_fit["slope"], slope_fit["corr"], slope_fit["n_onsets"]
        if cc < ONSET_CORR_MIN:
            m_onset = Metric("onset-timing", True, (sl, cc), (ONSET_SLOPE_TOL, ONSET_CORR_MIN),
                             f"N/A (no onset correspondence, xcorr {cc:.3f} < {ONSET_CORR_MIN})", applies=False)
        else:
            onset_ok = abs(sl - 1.0) <= ONSET_SLOPE_TOL
            m_onset = Metric("onset-timing", onset_ok, (sl, cc), (ONSET_SLOPE_TOL, ONSET_CORR_MIN),
                             f"slope {sl:.4f} (|.-1|<= {ONSET_SLOPE_TOL}), xcorr {cc:.3f}, onsets {non}")

    # ---- METRIC 3: per-channel Welch-PSD distance over the sustained body ----
    # body = the trusted-envelope span in samples (first..last trusted frame),
    # which excludes the trailing reverb tail. Per-channel MAGDEV, level-independent.
    body_ok = True; psd_vals = {}
    tr_idx = np.where(trusted)[0]
    if tr_idx.size >= 8:
        s0 = max(0, tr_idx[0] * hop)
        s1 = min(mc.size, mr.size, (tr_idx[-1] + 1) * hop)
        # shift capture window by lag samples to match the render body
        cs0 = s0 + lag * hop; cs1 = s1 + lag * hop
        cs0 = max(0, cs0); cs1 = min(wav_c.shape[0], cs1)
        band = None
        for ci, tag in ((0, "L"), (1, "R")):
            xr = wav_r[s0:s1, ci]
            xc = wav_c[cs0:cs1, ci]
            n = min(xr.size, xc.size)
            if n < NFFT:
                psd_vals[tag] = None; continue
            f, Sr = welch(xr[:n], sr)
            _, Sc = welch(xc[:n], sr)
            if band is None: band = (f >= PSD_FLO) & (f <= PSD_FHI)
            d = magdev(Sr, Sc, band)
            psd_vals[tag] = d
            if d is not None and d > PSD_DIST_TOL:
                body_ok = False
    vs = [v for v in psd_vals.values() if v is not None]
    if not vs:
        m_psd = Metric("psd-distance", True, psd_vals, PSD_DIST_TOL, "N/A (body too short)", applies=False)
    else:
        m_psd = Metric("psd-distance", body_ok, psd_vals, PSD_DIST_TOL,
                       " ".join(f"{k}:{('%.2f'%v) if v is not None else '--'}" for k, v in psd_vals.items()) + " dB")

    # ---- METRIC 4: loudness distribution (the EMPTY_LANDSCAPE precedent) ----
    # Measured over the SAME aligned trusted-body frames for both signals, on the
    # reverb-INSENSITIVE upper distribution: reverb fills valleys (lifts the low
    # percentiles), so a valley-depth "dynamic range" would always read the dry
    # render as more dynamic -- that IS the reverb exception, not a fault. So
    # median-below-peak = p95-p50 and the reported dynamic range = p95-p25 are
    # taken over the note bodies, where reverb barely moves them.
    if int(trusted.sum()) >= 8:
        ct, rt = ec[trusted], er[trusted]
        def lstat(a):
            p95, p50, p25 = np.percentile(a, [95, 50, 25])
            return float(p95 - p50), float(p95 - p25)
        mbp_c, dr_c = lstat(ct); mbp_r, dr_r = lstat(rt)
        d_mbp = abs(mbp_c - mbp_r); d_dr = abs(dr_c - dr_r)
        loud_ok = (d_mbp <= LOUD_MEDIAN_TOL) and (d_dr <= LOUD_RANGE_TOL)
        m_loud = Metric("loudness", loud_ok, (mbp_c, mbp_r, dr_c, dr_r), (LOUD_MEDIAN_TOL, LOUD_RANGE_TOL),
                        f"median-below-peak C {mbp_c:.1f}/R {mbp_r:.1f} (d {d_mbp:.1f}<= {LOUD_MEDIAN_TOL}); "
                        f"p95-p25 C {dr_c:.1f}/R {dr_r:.1f} (d {d_dr:.1f}<= {LOUD_RANGE_TOL}) dB")
    else:
        m_loud = Metric("loudness", True, (float("nan"),) * 4, (LOUD_MEDIAN_TOL, LOUD_RANGE_TOL),
                        "N/A (body too short)", applies=False)

    metrics = [m_env, m_onset, m_psd, m_loud]

    # ---- REVERB-ATTRIBUTABLE RESIDUAL (report only -- stage 3's target) ----
    # The reverb region is EVERY frame where the DRY render is silent (below
    # floor+guard) but the console still carries energy -- both the trailing tail
    # AND interior all-voices-off gaps (e.g. BGM_MAIN_Mii_Only_One's ~1.6 s gap
    # at ~51 s, the discriminating moment). That console energy is DSP reverb the
    # dry player lacks. Reported relative to the body energy; never fails.
    body_mask = er >= lo
    rev_mask = (er < lo) & (ec >= (floor_db + FLOOR_GUARD_DB))
    # ignore pre-roll silence (before the first body frame)
    if body_mask.any():
        first_body = int(np.where(body_mask)[0][0])
        pre = np.ones_like(rev_mask); pre[:first_body] = False
        rev_mask = rev_mask & pre
    cpow = 10.0 ** (ec / 10.0)
    body_e = float(np.sum(cpow[body_mask])) + 1e-30
    tail_e = float(np.sum(cpow[rev_mask])) + 1e-30
    reverb_db = 10.0 * np.log10(tail_e / body_e)
    tail_mask = rev_mask
    tail_frames = int(tail_mask.sum())
    reverb_report = {"reverb_db": reverb_db, "tail_seconds": tail_frames * hop / sr,
                     "floor_db": floor_db}

    # ---- per-flagged-unknown DIAGNOSTICS ----
    diags = diagnostics(sr, wav_c, wav_r, env_c, env_r, hop, lag, lo, hi, floor_db, level_off)

    return metrics, diags, reverb_report, sr


def diagnostics(sr, wav_c, wav_r, env_c, env_r, hop, lag, lo, hi, floor_db, level_off):
    """Targeted measurements that recalibrate a flagged constant from a capture."""
    d = {}

    # --- absolute output level (flagged: absolute output level) ---
    # level_off = median(C_db - R_db) over the trusted window (render vs console).
    d["output_level"] = {
        "render_vs_console_db": level_off,
        "note": "the render sits this many dB below(+)/above(-) the console over the "
                "trusted body; feeds the absolute-output-level calibration."}

    # --- pan curve (flagged: pan sqrt-poly vs equal-power) ---
    def lr_ratio_db(wav, s0, s1):
        s0 = max(0, s0); s1 = min(wav.shape[0], s1)
        if s1 - s0 < sr // 20: return None
        rL = np.sqrt(np.mean(wav[s0:s1, 0] ** 2)) + 1e-12
        rR = np.sqrt(np.mean(wav[s0:s1, 1] ** 2)) + 1e-12
        return 20.0 * np.log10(rL / rR)
    tr = env_r >= lo
    if tr.any():
        i0 = int(np.where(tr)[0][0]) * hop; i1 = (int(np.where(tr)[0][-1]) + 1) * hop
        pr = lr_ratio_db(wav_r, i0, i1)
        pc = lr_ratio_db(wav_c, i0 + lag * hop, i1 + lag * hop)
        d["pan"] = {"console_LR_db": pc, "render_LR_db": pr,
                    "note": "L/R RMS ratio; a non-zero console ratio on a panned note "
                            "discriminates the sqrt-poly vs equal-power pan law."}

    # --- vibrato/tremolo rate (flagged: LFO rate->Hz = 5/64) ---
    d["vibrato"] = measure_modulation(wav_c, sr, env_c, hop, lo)

    # --- exposed-tail decay time (flagged: envelope floor / release; decay-table) ---
    d["tail_decay"] = fit_tail_decay(env_c, env_r, hop, sr, floor_db)

    # --- interpolation filter proxy (flagged: linear vs polyphase) ---
    d["interpolation"] = hf_rolloff(wav_c, wav_r, sr, env_r, hop, lo, lag)

    # --- velocity / portamento: cross-capture, need specific repros ---
    d["velocity"] = {"note": "the (vel/127)^2 law needs >= 2 isolated-note captures at "
                             "known velocities; single-file mode cannot fit it."}
    d["portamento"] = {"note": "the time->duration mapping needs a portamento SE capture "
                               "(a monophonic glide); not present here."}
    return d


def measure_modulation(wav_c, sr, env_c, hop, lo):
    """Dominant 1-12 Hz modulation of the fine log-envelope = tremolo/vibrato rate."""
    m = mono(wav_c)
    body = env_c >= lo
    if not body.any():
        return {"rate_hz": None, "note": "no sustained body to measure modulation"}
    i0 = int(np.where(body)[0][0]) * hop; i1 = (int(np.where(body)[0][-1]) + 1) * hop
    seg = m[max(0, i0):min(m.size, i1)]
    if seg.size < sr // 2:
        return {"rate_hz": None, "note": "body too short (< 0.5 s) for a modulation FFT"}
    # fine amplitude envelope at ~1 ms, log domain, detrended
    fh = max(1, int(0.001 * sr)); n = seg.size // fh
    fe = np.sqrt(np.mean(np.square(seg[:n * fh].reshape(n, fh)), axis=1))
    le = np.log(np.maximum(fe, 1e-6)); le = le - np.mean(le)
    fs_env = sr / fh
    if le.size < 32:
        return {"rate_hz": None, "note": "modulation series too short"}
    w = np.hanning(le.size); L = np.abs(np.fft.rfft(le * w))
    fr = np.fft.rfftfreq(le.size, 1.0 / fs_env)
    bandm = (fr >= 1.0) & (fr <= 12.0)
    if not bandm.any() or L[bandm].size == 0:
        return {"rate_hz": None, "note": "no 1-12 Hz band"}
    kk = np.where(bandm)[0]; pk = kk[int(np.argmax(L[bandm]))]
    med = float(np.median(L[bandm])) + 1e-12
    snr = float(L[pk]) / med
    if snr < 4.0:
        return {"rate_hz": None, "snr": snr, "note": "no periodic 1-12 Hz modulation (SNR<4)"}
    hz = float(fr[pk])
    return {"rate_hz": hz, "snr": snr, "implied_rate_unit_hz": hz,
            "note": f"measured {hz:.2f} Hz modulation; caesar's kLfoRateHz=5/64 -> "
                    f"the commanded rate byte is {hz/(5.0/64.0):.1f} if this is the fundamental."}


def fit_tail_decay(env_c, env_r, hop, sr, floor_db):
    """Least-squares fit of the exposed final decay against an exp grid, aligned
    at the -25 dB crossing over the trusted window above the floor (the
    EMPTY_LANDSCAPE / BGM_MAIN_Mii_Only_One precedent). Reports console AND render
    best-fit tau; their gap is the reverb contribution (release+reverb are not
    separable from the tail alone -- cf. the 2026-07-08 dispute)."""
    def best_tau(edb):
        peak = float(np.max(edb))
        # exposed tail = from the last peak downward
        i = int(np.argmax(edb))
        # find the -25 dB crossing after the global peak, going down
        start = i
        for j in range(i, edb.size):
            if edb[j] <= peak - 25.0:
                start = j; break
        seg = edb[start:]
        lo = floor_db + FLOOR_GUARD_DB; hiw = peak - 25.0
        win = (seg <= hiw) & (seg >= lo)
        if int(win.sum()) < 6:
            return None, None
        t = np.arange(seg.size) / (sr / hop)        # seconds within the tail
        obs = seg[win]; tt = t[win]
        obs0 = obs - obs[0]                          # align at the -25 dB crossing
        best = (None, 1e30)
        for tau in TAIL_GRID_S:
            grid = -20.0 / np.log(10) * (tt - tt[0]) / tau  # exp decay slope in dB
            # least-squares intercept only (align at crossing => intercept ~0)
            r = float(np.sqrt(np.mean((obs0 - grid) ** 2)))
            if r < best[1]:
                best = (tau, r)
        return best
    tc, rc = best_tau(env_c)
    tr, rr = best_tau(env_r)
    return {"console_tau_s": tc, "console_rms_db": rc, "render_tau_s": tr, "render_rms_db": rr,
            "note": "exposed-tail decay fit (release+reverb, not separable). A console tail "
                    "much longer than the render tail is the DSP reverb -> stage 3."}


def hf_rolloff(wav_c, wav_r, sr, env_r, hop, lo, lag):
    """High-band (> 0.6 Nyquist) energy fraction, console vs render, over the body.
    A proxy for the interpolation filter (linear vs polyphase image rejection)."""
    tr = env_r >= lo
    if not tr.any():
        return {"console_hf_db": None, "render_hf_db": None, "note": "no body"}
    i0 = int(np.where(tr)[0][0]) * hop; i1 = (int(np.where(tr)[0][-1]) + 1) * hop
    def hf_frac_db(wav, s0, s1):
        s0 = max(0, s0); s1 = min(wav.shape[0], s1)
        x = mono(wav[s0:s1])
        if x.size < NFFT: return None
        f, S = welch(x, sr)
        ny = sr / 2.0
        hi = f >= 0.6 * ny; tot = f >= 0.0
        return 10.0 * np.log10((np.sum(S[hi]) + 1e-30) / (np.sum(S[tot]) + 1e-30))
    hc = hf_frac_db(wav_c, i0 + lag * hop, i1 + lag * hop)
    hr = hf_frac_db(wav_r, i0, i1)
    return {"console_hf_db": hc, "render_hf_db": hr,
            "note": "HF (>0.6*Nyquist) energy fraction; excess console HF vs the linear-"
                    "interpolated render indicates the console's sharper polyphase filter."}


# ========================================================================== #
# Reporting
# ========================================================================== #
def print_report(metrics, diags, reverb, sr, header):
    print(f"\n=== console-tolerance ({header}, {sr} Hz) ===\n")
    print("-- metrics (reverb tail excepted) --")
    verdict_fail = []
    for m in metrics:
        tag = "n/a " if not m.applies else ("PASS" if m.ok else "FAIL")
        print(f"  [{tag}] {m.name:14s} {m.note}")
        if m.applies and not m.ok:
            verdict_fail.append(m.name)
    print("\n-- reverb-attributable residual (report only; stage 3's target) --")
    print(f"  console tail energy vs body: {reverb['reverb_db']:+.1f} dB over "
          f"{reverb['tail_seconds']:.2f} s of exposed tail (measured floor {reverb['floor_db']:.1f} dBFS)")
    print("\n-- per-flagged-unknown diagnostics --")
    ol = diags["output_level"]; print(f"  output level : render {ol['render_vs_console_db']:+.2f} dB vs console")
    if "pan" in diags:
        p = diags["pan"]
        pc = f"{p['console_LR_db']:+.2f}" if p['console_LR_db'] is not None else "--"
        pr = f"{p['render_LR_db']:+.2f}" if p['render_LR_db'] is not None else "--"
        print(f"  pan (L/R dB) : console {pc}  render {pr}")
    vb = diags["vibrato"]
    print(f"  vibrato/trem : {('%.2f Hz (SNR %.1f)'%(vb['rate_hz'], vb.get('snr',0))) if vb.get('rate_hz') else vb['note']}")
    td = diags["tail_decay"]
    tcs = f"{td['console_tau_s']}" if td['console_tau_s'] is not None else "--"
    trs = f"{td['render_tau_s']}" if td['render_tau_s'] is not None else "--"
    print(f"  tail decay   : console tau {tcs} s  render tau {trs} s (grid-fit)")
    ip = diags["interpolation"]
    hc = f"{ip['console_hf_db']:.1f}" if ip['console_hf_db'] is not None else "--"
    hr = f"{ip['render_hf_db']:.1f}" if ip['render_hf_db'] is not None else "--"
    print(f"  interp (HF)  : console {hc} dB  render {hr} dB (HF energy fraction)")
    print(f"  velocity     : {diags['velocity']['note']}")
    print(f"  portamento   : {diags['portamento']['note']}")
    return verdict_fail


# ========================================================================== #
# Fixture synthesis for --self-validate  (models the real capture chain)
# ========================================================================== #
def make_capture_fixture(sr_r, wav_r, rng):
    """Apply the real capture chain to a dry render -> a realistic 'console'
    capture: capture-rate resample, gain, lead-in, +-50 ppm drift, -85 dBFS
    noise, and a synthetic exponential reverb tail (exercises the reverb path).
    THE 2026-07-11 LESSON: an idealized fixture is worthless -- reproduce the
    artifacts that break real analysis."""
    # 1) synthetic reverb tail (exp decay IR at a modest send, with pre-delay)
    tau_n = int(FIX_REVERB_TAU_S * sr_r)
    ir = np.exp(-np.arange(6 * tau_n) / max(1, tau_n))
    pre = int(FIX_REVERB_PREDELAY_MS / 1000.0 * sr_r)
    ir = np.concatenate([np.zeros(pre), ir])
    ir /= np.sqrt(np.sum(ir ** 2))                  # unit-energy IR
    send = 10.0 ** (FIX_REVERB_SEND_DB / 20.0)
    wet = np.zeros((wav_r.shape[0] + ir.size - 1, 2))
    for c in range(2):
        wet[:, c] = fft_convolve(wav_r[:, c], ir)
    dry = np.zeros_like(wet); dry[:wav_r.shape[0]] = wav_r
    y = dry + send * wet
    # 2) +-50 ppm clock drift + capture-rate resample (one combined resample)
    drift = 1.0 + FIX_DRIFT_PPM * 1e-6
    n_out = int(round(y.shape[0] * (FIX_CAPTURE_RATE / float(sr_r)) * drift))
    y = resample_wav(sr_r, y, n_out)
    # 3) gain offset
    y *= 10.0 ** (FIX_GAIN_DB / 20.0)
    # 4) lead-in silence
    lead = int(FIX_LEADIN_MS / 1000.0 * FIX_CAPTURE_RATE)
    y = np.vstack([np.zeros((lead, 2)), y])
    # 5) -85 dBFS noise floor
    y += rng.normal(0.0, 10.0 ** (FIX_NOISE_DBFS / 20.0), size=y.shape)
    return FIX_CAPTURE_RATE, y


def pad_release(wav_r, sr_r):
    """Negative control: extend every note's release/decay (a wrong envelope)
    WITHOUT adding a separate additive tail -- this holds the note body's own
    envelope up, so it lands in the trusted window (not the reverb-excluded
    tail) and must fail the envelope-fit metric."""
    out = wav_r.copy()
    a = np.exp(-1.0 / (0.6 * sr_r))                 # slow release-follower coeff
    for c in range(2):
        x = wav_r[:, c]
        env = np.abs(x)
        # asymmetric peak-hold with slow decay = a longer release envelope
        ext = np.empty_like(env); acc = 0.0
        for i in range(env.size):
            acc = env[i] if env[i] > acc else acc * a
            ext[i] = acc
        # smoothed base envelope for the ratio
        base = np.maximum(env, 1e-6)
        # light smoothing of base to avoid per-sample division noise
        k = max(1, int(0.005 * sr_r)); ker = np.ones(k) / k
        baseS = np.convolve(base, ker, mode="same")
        extS = np.convolve(ext, ker, mode="same")
        out[:, c] = x * (extS / np.maximum(baseS, 1e-6))
    m = np.max(np.abs(out)) + 1e-9
    if m > 1.0: out /= m
    return out


def retime(wav_r, sr_r, factor):
    """Negative control: time-scale the render (wrong tempo)."""
    n_out = int(round(wav_r.shape[0] * factor))
    return resample_wav(sr_r, wav_r, n_out)


def self_validate(render_path, other_path=None):
    """Prove the harness: the realistic fixture PASSES; three negative controls
    each FAIL with the expected metric named. Deterministic (seeded)."""
    print("== console-tolerance SELF-VALIDATE ==")
    print("   positive fixture must PASS; 3 negative controls must FAIL the named metric.\n")
    sr_r, wav_r = read_wav(render_path)
    if wav_r.shape[1] == 1: wav_r = np.repeat(wav_r, 2, axis=1)
    rng = np.random.default_rng(FIX_SEED)

    def run(sr_c, wav_c, sr_rr, wav_rr, header):
        metrics, diags, reverb, sr = analyze_arrays(sr_c, wav_c, sr_rr, wav_rr, header)
        fails = print_report(metrics, diags, reverb, sr, header)
        return fails

    failures = []

    # ---- POSITIVE: realistic capture of the correct render ----
    sr_c, cap = make_capture_fixture(sr_r, wav_r, rng)
    fails = run(sr_c, cap, sr_r, wav_r, "POSITIVE fixture")
    if fails:
        failures.append(f"positive fixture FAILED metrics {fails} (expected all pass)")
    else:
        print("\n   -> POSITIVE fixture PASSED (as required).")

    # ---- NEGATIVE 1: wrong tempo (render retimed 2%) -> onset-timing ----
    bad = retime(wav_r, sr_r, 1.02)
    fails = run(sr_c, cap, sr_r, bad, "NEGATIVE wrong-tempo (+2%)")
    if "onset-timing" in fails:
        print("\n   -> wrong-tempo correctly FAILED onset-timing.")
    else:
        failures.append(f"wrong-tempo did not fail onset-timing (failed {fails})")

    # ---- NEGATIVE 2: wrong envelope (release padded) -> envelope-fit ----
    bad = pad_release(wav_r, sr_r)
    fails = run(sr_c, cap, sr_r, bad, "NEGATIVE padded-release")
    if "envelope-fit" in fails:
        print("\n   -> padded-release correctly FAILED envelope-fit.")
    else:
        failures.append(f"padded-release did not fail envelope-fit (failed {fails})")

    # ---- NEGATIVE 3: different sequence -> onset-timing correlation ----
    # Use --other only when its length is comparable to the base; a wildly
    # different length would trim to the shorter and degenerate to all-N/A, so
    # fall back to a length-matched time-reversed surrogate (a self-contained
    # "different sequence" that cannot degenerate).
    use_other = None
    if other_path:
        sr_o, wav_o = read_wav(other_path)
        if wav_o.shape[1] == 1: wav_o = np.repeat(wav_o, 2, axis=1)
        ratio = (wav_o.shape[0] / sr_o) / max(1e-6, wav_r.shape[0] / sr_r)
        if 0.5 < ratio < 2.0:
            use_other = (sr_o, wav_o)
    if use_other:
        fails = run(sr_c, cap, use_other[0], use_other[1], "NEGATIVE different-sequence (--other)")
    else:
        fails = run(sr_c, cap, sr_r, wav_r[::-1].copy(), "NEGATIVE different-sequence (time-reversed surrogate)")
    if "onset-timing" in fails or "envelope-fit" in fails:
        print("\n   -> different-sequence correctly FAILED (onset-timing/envelope).")
    else:
        failures.append(f"different-sequence did not fail (failed {fails})")

    print("\n== SELF-VALIDATE RESULT ==")
    if failures:
        for f in failures:
            print(f"   FAIL: {f}")
        print("   SELF-VALIDATE FAILED -- do not trust this harness until fixed.")
        return 2
    print("   PASSED: realistic fixture passes; wrong-tempo -> onset-timing;")
    print("           padded-release -> envelope-fit; different-sequence -> fails.")
    return 0


# ========================================================================== #
# main / Stop-funnel
# ========================================================================== #
def main():
    ap = argparse.ArgumentParser(description="console-tolerance net (stage-2 C11)")
    ap.add_argument("a", nargs="?", help="console capture WAV (or render WAV with --self-validate)")
    ap.add_argument("b", nargs="?", help="caesar-play render WAV")
    ap.add_argument("--self-validate", action="store_true",
                    help="synthesize fixtures from a real render and prove the harness")
    ap.add_argument("--other", help="a different render for the different-sequence control")
    args = ap.parse_args()

    if args.self_validate:
        if not args.a:
            sys.stderr.write("HARNESS ERROR: --self-validate needs a render WAV\n")
            return 2
        return self_validate(args.a, args.other)

    if not args.a or not args.b:
        sys.stderr.write("HARNESS ERROR: need <console_capture.wav> <caesar_render.wav>\n")
        return 2

    sr_c, wav_c = read_wav(args.a)
    sr_r, wav_r = read_wav(args.b)
    metrics, diags, reverb, sr = analyze_arrays(sr_c, wav_c, sr_r, wav_r,
                                                f"{os.path.basename(args.a)} vs {os.path.basename(args.b)}")
    fails = print_report(metrics, diags, reverb, sr,
                         f"{os.path.basename(args.a)} vs {os.path.basename(args.b)}")
    print("\n-- VERDICT --")
    if fails:
        print(f"  FAIL: out of tolerance on {fails} (reverb tail excepted).")
        return 1
    print("  PASS: render matches the console within tolerance (reverb tail excepted).")
    return 0


if __name__ == "__main__":
    try:
        rc = main()
    except SystemExit:
        raise
    except BaseException as e:  # Stop-funnel: ANY error -> 2, never a silent pass
        sys.stderr.write(f"HARNESS ERROR: {type(e).__name__}: {e}\n")
        sys.stderr.write("Nothing was verified. Do NOT read this as a pass.\n")
        sys.exit(2)
    sys.exit(rc)
