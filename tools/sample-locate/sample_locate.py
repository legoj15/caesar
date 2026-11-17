#!/usr/bin/env python3
"""Locate a known sound-bank sample inside a reference recording by sample-exact matched filtering.

A game's sequenced sound plays a bank sample as notes at semitone steps from the
sample's native rate. Slide the raw sample, read at a trial speed, over a lossless
reference and the normalised cross-correlation (NCC) peaks where, at what speed,
in which stereo channel and at what level the sample sounds. Five subcommands
share one set of helpers: `speeds` (fine speed grid, short window), `semis`
(exact semitone grid, long window), `pursue` (peel every copy off both channels,
with level, pan and envelope), `fit` (fit one copy on one channel) and `all`
(try every WAV in a directory against one short stretch). Python 3, numpy and
scipy only; mono or stereo WAV, int16 or float.

Examples (paths are placeholders):

  speeds   python -I sample_locate.py speeds --ref cd.wav --sample sfx.wav --t0 197.33 --t1 197.52 --speed-lo 0.3 --speed-hi 3.2 --step 0.0005 --window-ms 120 --chan mid --peaks 6
  semis    python -I sample_locate.py semis --ref cd.wav --sample sfx.wav --t0 197.2 --t1 204.5 --semi-lo -24 --semi-hi 24 --window-ms 300 --chan L --cents-step 100 --peaks 14
  pursue   python -I sample_locate.py pursue --ref cd.wav --sample sfx.wav --t0 197.2 --t1 204.4 --semi-lo -19 --semi-hi 19 --window-ms 250 --threshold 0.42 --max-iter 24 --out residual.wav --report-shift 0.670 --tsv copies.tsv
  fit      python -I sample_locate.py fit --ref cd.wav --sample sfx.wav --chan L --t 198.4681 --cents -700 --duration 3.5 --out residual.wav
  all      python -I sample_locate.py all --ref cd.wav --wav-dir waves/ --t0 197.45 --t1 197.66 --semi-lo -19 --semi-hi 19 --window-ms 80 --out ranked.tsv --names-tsv occurrences.tsv

Times in the output are template-onset times (the first sample above 10% of the
sample's peak); the note-on is earlier by onset_samples / (native_rate * speed).
Failures a user can fix (missing file, reference stretch shorter than the window)
print one line to stderr and exit with status 2.
"""
import argparse
import glob
import os
import sys
import warnings

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, fftconvolve, medfilt, resample_poly, sosfiltfilt

UP_SCAN = 8              # oversampling of the sample for speeds, semis, pursue and fit
UP_ALL = 4               # oversampling for the directory scan
HP_HZ = 300.0            # zero-phase high-pass used by pursue, fit and all
EDGE_GUARD = 4000        # context samples kept around a scan window so the high-pass settles
REFINE_GUARD = 3000      # context samples around a copy while refining it
BLOCK_S = 0.05           # gain-fit block length, seconds
PEAK_SUPPRESS_SPEEDS_S = 0.03   # speeds: time suppressed around each listed peak
PEAK_SUPPRESS_SEMIS_S = 0.04    # semis: same
PEAK_SUPPRESS_CENTS = 50.0      # semis: pitch rows this close to a listed peak are blanked with it
PEAK_SUPPRESS_SPEED_RATIO = 1.03  # speeds: same, as a speed ratio (about 50 cents)
CHANNELS = ('mid', 'side', 'L', 'R')


class ToolError(Exception):
    """A failure the user can fix; printed as one line, exit status 2."""


# ---- shared helpers --------------------------------------------------------

def to_float(data):
    """WAV array to float64 in [-1, 1]; floats pass through, integers are scaled by their range."""
    if data.dtype == np.int16:
        return data.astype(np.float64) / 32768
    if np.issubdtype(data.dtype, np.floating):
        return data.astype(np.float64)
    if data.dtype == np.int32:
        return data.astype(np.float64) / 2147483648
    if data.dtype == np.uint8:
        return (data.astype(np.float64) - 128) / 128
    raise ToolError('unsupported WAV sample type %s' % data.dtype)


def load_wav(path):
    """Returns (rate, float64 array shaped (frames,) or (frames, channels))."""
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', wavfile.WavFileWarning)
            rate, data = wavfile.read(path)
    except Exception as err:
        raise ToolError('cannot read %s: %s' % (path, err))
    return rate, to_float(data)


def load_sample(path):
    """A sound-bank sample as (rate, mono float64)."""
    rate, x = load_wav(path)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if x.size == 0 or np.abs(x).max() == 0:
        raise ToolError('%s is empty or silent' % path)
    return rate, x


def onset_index(x):
    """Index of the first sample above 10% of the peak."""
    return int(np.argmax(np.abs(x) > 0.1 * np.abs(x).max()))


class Sample:
    """A mono sample oversampled once, so any playback rate can be read by interpolation."""

    def __init__(self, x, rate, up, keep=None):
        self.rate = rate
        self.length = len(x)
        self.onset = onset_index(x)
        self.up = up
        self.xu = resample_poly(x if keep is None else x[:keep], up, 1)
        self.index = np.arange(len(self.xu))


def cents_to_speed(cents):
    return 2 ** (cents / 1200)


def read_template(smp, speed, n, ref_rate, offset_ms=0.0, frac=0.0):
    """The sample played at `speed` from its onset, n frames at the reference rate.

    offset_ms starts the template that much playback time after the onset
    (offset_ms * speed of sample time); frac shifts the start by a
    fraction of a reference frame. Returns (template, ok) where ok marks frames
    that fall inside the sample; the rest are zero.
    """
    pos = (smp.onset + offset_ms / 1000 * smp.rate * speed
           + (np.arange(n) - frac) * speed * smp.rate / ref_rate) * smp.up
    ok = (pos >= 0) & (pos < len(smp.xu) - 1)
    tpl = np.zeros(n)
    tpl[ok] = np.interp(pos[ok], smp.index, smp.xu)
    return tpl, ok


def highpass_sos(rate, hz=HP_HZ):
    if not 0 < hz < rate / 2:
        raise ToolError('high-pass cutoff %g Hz must be above 0 and below half the reference rate (%g Hz)' % (hz, rate / 2))
    return butter(4, hz / (rate / 2), 'hp', output='sos')


def highpassed_window(sos, sig, a, b):
    """Zero-phase high-pass of sig[a:b], filtered with EDGE_GUARD samples of context each side."""
    return sosfiltfilt(sos, sig[a - EDGE_GUARD: b + EDGE_GUARD])[EDGE_GUARD:-EDGE_GUARD]


def local_norm(seg, n):
    """Running RMS-style norm of seg over n-sample windows (floor avoids divide by zero)."""
    return np.sqrt(np.maximum(fftconvolve(seg ** 2, np.ones(n), mode='valid'), 1e-12))


def correlate(seg, tpl):
    """Raw sliding correlation of tpl against seg, one value per start position."""
    return fftconvolve(seg, tpl[::-1], mode='valid')


def ncc(u, v):
    """Normalised correlation of two equal-length vectors."""
    return float(np.dot(u, v) / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-12))


def channel_signal(ref, chan):
    if ref.ndim == 1:
        if chan != 'mid':
            raise ToolError('reference is mono; only --chan mid is available')
        return ref
    return {'mid': lambda: ref.mean(axis=1), 'side': lambda: (ref[:, 0] - ref[:, 1]) / 2,
            'L': lambda: ref[:, 0], 'R': lambda: ref[:, 1]}[chan]()


def require_stereo(ref, what):
    if ref.ndim < 2 or ref.shape[1] < 2:
        raise ToolError('%s needs a stereo reference' % what)


def stretch_samples(t0, t1, rate, total, window_ms, guard=0):
    """Frame bounds (a, b) and window length n for the reference stretch.

    Fails when the stretch cannot hold one window. With guard, the stretch is
    also kept EDGE_GUARD frames inside the reference so the high-pass can settle.
    """
    if t0 < 0:
        raise ToolError('--t0 must be >= 0')
    n = int(window_ms / 1000 * rate)
    a = int(t0 * rate)
    b = min(int(t1 * rate), total - guard)
    if guard and a < guard:
        raise ToolError('--t0 must be at least %.3f s so the high-pass has context before the stretch' % (guard / rate))
    if b - a < n:
        msg = ('reference stretch t1 - t0 = %.3f s is shorter than the %.0f ms window; '
               'widen --t0/--t1 or lower --window-ms' % (t1 - t0, window_ms))
        if int(t1 * rate) > b:
            msg += ' (only %.3f s of the stretch lies inside the %.3f s reference)' % (max(b - a, 0) / rate, total / rate)
        raise ToolError(msg)
    return a, b, n


def pick_peaks(A, count, suppress_frames, suppress_rows, floor=None):
    """Greedy strongest peaks of |matrix| (pitch rows x time).

    Each pick blanks +-suppress_frames in time, but only on the pitch rows within +-suppress_rows of it, so a
    second copy that starts at the same moment at another pitch (a chord in one channel) is still listed.
    Yields (row, col). Stops early below `floor` or when nothing is left.
    """
    B = A.copy()
    for _ in range(count):
        i, j = np.unravel_index(np.argmax(B), B.shape)
        if B[i, j] == 0 or (floor is not None and B[i, j] < floor):
            return
        yield i, j
        B[max(0, i - suppress_rows): i + suppress_rows + 1,
          max(0, j - suppress_frames): min(B.shape[1], j + suppress_frames + 1)] = 0


# ---- speeds ----------------------------------------------------------------

def cmd_speeds(args):
    sr, ref = load_wav(args.ref)
    ch = channel_signal(ref, args.chan)
    a, b, n = stretch_samples(args.t0, args.t1, sr, len(ch), args.window_ms)
    seg = ch[a:b]
    rate, x = load_sample(args.sample)
    smp = Sample(x, rate, UP_SCAN)
    loc = local_norm(seg, n)
    speeds = np.exp(np.arange(np.log(args.speed_lo), np.log(args.speed_hi), args.step))
    rows = []
    for s in speeds:
        tpl, ok = read_template(smp, s, n, sr, args.tpl_offset_ms)
        if not ok[-1]:
            continue            # the sample ends before the window does at this speed
        tpl -= tpl.mean()
        rows.append((s, correlate(seg, tpl) / (loc * np.sqrt(np.sum(tpl ** 2)))))
    if not rows:
        raise ToolError('the sample is shorter than the window at every speed in range')
    S = np.array([r[0] for r in rows])
    M = np.array([r[1] for r in rows])          # speeds x time
    A = np.abs(M)
    print('grid', M.shape, 'speeds %.4f..%.4f' % (S[0], S[-1]), 'window %.0f ms' % args.window_ms, 'chan', args.chan)
    out = [(j / sr + args.t0 - args.tpl_offset_ms / 1000, S[i], M[i, j])
           for i, j in pick_peaks(A, args.peaks, int(PEAK_SUPPRESS_SPEEDS_S * sr),
                                  int(np.log(PEAK_SUPPRESS_SPEED_RATIO) / args.step), floor=0.05)]
    print('  time(s)   speed   semitones   ncc(signed)')
    for t, s, v in sorted(out):
        print('%9.4f  %6.4f  %+7.2f   %+.3f' % (t, s, 12 * np.log2(s), v))
    print('noise floor: median |ncc| %.3f, 99.9th pct %.3f' % (np.median(A), np.percentile(A, 99.9)))


# ---- semis -----------------------------------------------------------------

def cmd_semis(args):
    sr, ref = load_wav(args.ref)
    ch = channel_signal(ref, args.chan)
    a, b, n = stretch_samples(args.t0, args.t1, sr, len(ch), args.window_ms)
    seg = ch[a:b]
    rate, x = load_sample(args.sample)
    smp = Sample(x, rate, UP_SCAN)
    loc = local_norm(seg, n)
    cents = np.arange(args.semi_lo * 100, args.semi_hi * 100 + 0.5, args.cents_step)
    rows = []
    for c in cents:
        tpl, _ = read_template(smp, cents_to_speed(c), n, sr, args.tpl_offset_ms)
        rows.append(correlate(seg, tpl) / (loc * np.sqrt(np.sum(tpl ** 2) + 1e-12)))
    M = np.array(rows)
    A = np.abs(M)
    print('grid', M.shape, 'window %.0f ms' % args.window_ms, 'chan', args.chan, 'sample onset at %d' % smp.onset)
    out = [(j / sr + args.t0 - args.tpl_offset_ms / 1000, cents[i], M[i, j])
           for i, j in pick_peaks(A, args.peaks, int(PEAK_SUPPRESS_SEMIS_S * sr),
                                  int(PEAK_SUPPRESS_CENTS / args.cents_step))]
    p999, p99999 = np.percentile(A, 99.9), np.percentile(A, 99.999)
    print('  time(s)   cents    speed    ncc(signed)')
    for t, c, v in sorted(out):
        print('%9.4f  %+6.0f   %6.4f   %+.3f %s' % (t, c, cents_to_speed(c), v, '<<<' if abs(v) > 1.5 * p99999 else ''))
    print('noise floor: median |ncc| %.3f, 99.9th pct %.3f, 99.999th %.3f  (<<< = above 1.5 x the 99.999th)'
          % (np.median(A), p999, p99999))


# ---- copy fitting shared by pursue and fit ---------------------------------

def refine_copy(y, i0, sos, smp, ref_rate, cents0, probe_n, span, step, frac_step):
    """Refine one rough hit: cents in cents0 +- span, whole-frame shift within +-3 ms, then a sub-frame fraction.

    Returns (cents, i0 shifted, frac, ncc before the fraction search, ncc after it).
    """
    pad = int(0.003 * ref_rate)
    G = REFINE_GUARD
    segw = sosfiltfilt(sos, y[i0 - pad - G: i0 + probe_n + pad + G])
    best = (0.0, cents0, 0)
    for c in np.arange(cents0 - span, cents0 + span + 0.01, step):
        t = sosfiltfilt(sos, read_template(smp, cents_to_speed(c), probe_n, ref_rate)[0])
        for d in range(-pad, pad + 1):
            w = ncc(segw[G + pad + d: G + pad + d + probe_n], t)
            if abs(w) > abs(best[0]):
                best = (w, c, d)
    w, c, d = best
    i0 += d
    seg = sosfiltfilt(sos, y[i0 - G: i0 + probe_n + G])[G:G + probe_n]

    def score(f):
        return ncc(seg, sosfiltfilt(sos, read_template(smp, cents_to_speed(c), probe_n, ref_rate, frac=f)[0]))

    fr = max(np.arange(-1, 1.01, frac_step), key=lambda f: abs(score(f)))
    return c, i0, fr, w, score(fr)


def block_gains(y, i0, T, blk):
    """Least-squares gain of template T per blk-frame block of y starting at i0, and the fraction explained."""
    g, ex = [], []
    for q in range(0, len(T) - blk + 1, blk):
        t = T[q:q + blk]
        r = y[i0 + q: i0 + q + blk]
        gg = np.dot(r, t) / (np.dot(t, t) + 1e-12)
        g.append(gg)
        ex.append(1 - np.sum((r - gg * t) ** 2) / (np.sum(r ** 2) + 1e-12))
    return np.array(g), np.array(ex)


def subtraction_curve(g, ex, full, blk):
    """Per-frame gain to subtract a copy: median-smoothed block gains while it is heard, zero from two blocks after.

    Returns (gain per frame, level, heard-for seconds). Smoothing keeps unrelated energy from being chased block by block.
    """
    good = np.where(ex > 0.25)[0]
    last = (good.max() + 1) * BLOCK_S if len(good) else 0.0
    lvl = np.median(g[ex > 0.4]) if np.any(ex > 0.4) else np.median(g[:6])
    gs = medfilt(g, 5)
    gs[:2] = g[:2]
    gs[int(last / BLOCK_S) + 2:] = 0.0
    gi = np.interp(np.arange(full), (np.arange(len(g)) + 0.5) * blk, gs, left=gs[0], right=gs[-1])
    return gi, lvl, last


# ---- pursue ----------------------------------------------------------------

def strongest_copy(res, a, b, sos, T, Tn, n, semis):
    """(ncc, channel, semitone, frame offset) of the best template match over both channels of the residual."""
    best = None
    for chn in (0, 1):
        seg = highpassed_window(sos, res[:, chn], a, b)
        loc = local_norm(seg, n)
        floor = np.median(loc) * 0.25                      # ignore near-silent stretches
        for ki, k in enumerate(semis):
            v = correlate(seg, T[ki]) / (np.maximum(loc, floor) * Tn[ki])
            j = int(np.argmax(np.abs(v)))
            if best is None or abs(v[j]) > abs(best[0]):
                best = (v[j], chn, k, j)
    return best


def copy_columns(chn, t_ref, cents, lvl, g0, last, w, shift):
    return ('LR'[chn], t_ref, t_ref - shift, cents, cents / 100, cents_to_speed(cents),
            20 * np.log10(abs(lvl) + 1e-9), 20 * np.log10(abs(g0) + 1e-9), last, w)


PURSUE_HEADER = '  ch  ref time   shifted   cents  semis  speed    level dB  first50ms dB  heard for  ncc'
PURSUE_ROW = '  %s  %9.4f  %9.4f  %+6.0f  %+5.1f  %6.4f  %7.1f   %7.1f      %5.2f s   %+.2f'
PURSUE_TSV_HEADER = 'channel\tref_time_s\tshifted_time_s\tcents\tspeed\tlevel_db\tfirst50ms_db\theard_for_s\tncc\n'
PURSUE_TSV_ROW = '%s\t%.5f\t%.5f\t%.1f\t%.5f\t%.2f\t%.2f\t%.2f\t%.3f\n'


def cmd_pursue(args):
    sr, ref = load_wav(args.ref)
    require_stereo(ref, 'pursue')
    rate, x = load_sample(args.sample)
    smp = Sample(x, rate, UP_SCAN)
    sos = highpass_sos(sr)
    a, b, n = stretch_samples(args.t0, args.t1, sr, len(ref), args.window_ms, guard=EDGE_GUARD)
    semis = np.arange(int(args.semi_lo), int(args.semi_hi) + 1)
    T = [sosfiltfilt(sos, read_template(smp, cents_to_speed(100.0 * k), n, sr)[0]) for k in semis]
    Tn = [np.sqrt(np.sum(t ** 2)) for t in T]
    res = ref.copy()
    found = []
    blk = int(BLOCK_S * sr)
    pad, G, probe_n = int(0.003 * sr), REFINE_GUARD, int(0.4 * sr)
    print(PURSUE_HEADER)
    for _ in range(args.max_iter):
        v, chn, k, j = strongest_copy(res, a, b, sos, T, Tn, n, semis)
        if abs(v) < args.threshold:
            print('stop: best remaining ncc %+.3f (ch %s, %+d st, %.4f s)' % (v, 'LR'[chn], k, (a + j) / sr))
            break
        i0 = a + j
        if i0 + probe_n + pad + G > len(res):
            print('stop: strongest remaining copy (ch %s, %+d st, %.4f s) is too close to the end of the reference to fit'
                  % ('LR'[chn], k, i0 / sr))
            break
        y = res[:, chn]
        c, i0, fr, w, _ = refine_copy(y, i0, sos, smp, sr, 100.0 * k, probe_n, 20, 1.0, 0.1)
        full = min(int((smp.length - smp.onset) / smp.rate / cents_to_speed(c) * sr), len(res) - i0)
        Tf = read_template(smp, cents_to_speed(c), full, sr, frac=fr)[0]
        g, ex = block_gains(y, i0, Tf, blk)
        if len(g) == 0:
            print('stop: the copy at %.4f s is shorter than one %d ms block' % (i0 / sr, int(BLOCK_S * 1000)))
            break
        gi, lvl, last = subtraction_curve(g, ex, full, blk)
        res[i0:i0 + full, chn] -= gi * Tf
        found.append(copy_columns(chn, (i0 + fr) / sr, c, lvl, g[0], last, w, args.report_shift))
        print(PURSUE_ROW % found[-1])
    else:
        print('stop: reached --max-iter %d' % args.max_iter)
    wavfile.write(args.out, sr, res.astype(np.float32))
    if args.tsv:
        with open(args.tsv, 'w', encoding='utf-8') as fo:
            fo.write(PURSUE_TSV_HEADER)
            for row in found:
                fo.write(PURSUE_TSV_ROW % (row[0], row[1], row[2], row[3], row[5], row[6], row[7], row[8], row[9]))


# ---- fit -------------------------------------------------------------------

def cmd_fit(args):
    chn = {'L': 0, 'R': 1}[args.chan]
    sr, ref = load_wav(args.in_path or args.ref)
    require_stereo(ref, 'fit')
    rate, x = load_sample(args.sample)
    smp = Sample(x, rate, UP_SCAN)
    sos = highpass_sos(sr, args.hp)
    y = ref[:, chn]
    i0 = int(round(args.t * sr))
    n = int(min(args.duration, 0.5) * sr)
    pad, G = int(0.003 * sr), REFINE_GUARD
    if i0 - pad - G < 0 or i0 + n + pad + G > len(y):
        raise ToolError('--t %.4f is too close to an edge of the reference to fit (needs %.3f s of context each side)'
                        % (args.t, (pad + G) / sr))
    cents, i0, fr, _, v = refine_copy(y, i0, sos, smp, sr, args.cents, n, 15, 0.5, 0.05)
    full = (smp.length - smp.onset) / smp.rate / cents_to_speed(cents)
    n = min(int(min(args.duration, full) * sr), len(y) - i0)
    T = read_template(smp, cents_to_speed(cents), n, sr, frac=fr)[0]
    blk = int(BLOCK_S * sr)
    gains, exs = block_gains(y, i0, T, blk)
    if len(gains) == 0:
        raise ToolError('--duration leaves less than one %d ms block to fit' % int(BLOCK_S * 1000))
    gd = 20 * np.log10(np.abs(gains) + 1e-9)
    print('%s  t=%.5f s  cents=%+7.1f  speed %.5f  ncc(first %.0f ms, hp %d)=%+.3f  full length at this speed %.2f s'
          % (args.chan, (i0 + fr) / sr, cents, cents_to_speed(cents), min(args.duration, 0.5) * 1000, args.hp, v, full))
    if not args.quiet:
        print('   gain dB per 50 ms :', ' '.join('%5.1f' % g for g in gd))
        print('   explained per 50ms:', ' '.join('%5.2f' % max(e, 0) for e in exs))
    if args.out:
        centers = (np.arange(len(gains)) + 0.5) * blk
        gi = np.interp(np.arange(n), centers, gains, left=gains[0], right=gains[-1])
        res = ref.copy()
        res[i0:i0 + n, chn] -= gi * T
        wavfile.write(args.out, sr, res.astype(np.float32))


# ---- all -------------------------------------------------------------------

def read_names(path):
    """Optional occurrence table: tab-separated, wave name then a description (kept to 70 characters)."""
    names = {}
    if path:
        try:
            with open(path, encoding='utf-8', errors='replace') as fh:
                for line in fh:
                    p = line.rstrip('\n').split('\t')
                    if len(p) > 1:
                        names[p[0]] = p[1][:70]
        except OSError as err:
            raise ToolError('cannot read %s: %s' % (path, err))
    return names


ALL_TSV_HEADER = 'ncc\twave\trate\tlen_s\tsemis\tch\tref_time\twhere'
ALL_TSV_ROW = '%+.3f\t%s\t%d\t%.2f\t%+d\t%s\t%.4f\t%s'


def best_match_for_wave(path, args, sr, sos, segs, locs, n, semis):
    """Best (ncc, semitone, channel, frame) of one WAV over every semitone and both channels, or None to skip it."""
    try:
        ss, x = load_sample(path)
    except ToolError:
        return None
    if len(x) < 0.03 * ss or np.abs(x).max() < 1e-3:
        return None
    on = onset_index(x)
    need = int(on + (args.window_ms / 1000) * ss * 2 ** (args.semi_hi / 12) + 64)
    smp = Sample(x, ss, UP_ALL, keep=min(len(x), need))
    best = None
    for k in semis:
        tpl, ok = read_template(smp, 2 ** (k / 12.0), n, sr)
        if ok.sum() < 0.5 * n:
            continue
        t = sosfiltfilt(sos, tpl)
        tn = np.sqrt(np.sum(t ** 2))
        if tn < 1e-9:
            continue
        for c in (0, 1):
            v = correlate(segs[c], t) / (locs[c] * tn)
            j = int(np.argmax(np.abs(v)))
            if best is None or abs(v[j]) > abs(best[0]):
                best = (v[j], k, c, j)
    if best is None:
        return None             # shorter than the window at every semitone in range: nothing was scored
    return ss, len(x) / ss, best


def cmd_all(args):
    sr, ref = load_wav(args.ref)
    require_stereo(ref, 'all')
    names = read_names(args.names_tsv)
    a, b, n = stretch_samples(args.t0, args.t1, sr, len(ref), args.window_ms, guard=EDGE_GUARD)
    sos = highpass_sos(sr)
    segs = [highpassed_window(sos, ref[:, c], a, b) for c in (0, 1)]
    locs = [local_norm(s, n) for s in segs]
    semis = np.arange(int(args.semi_lo), int(args.semi_hi) + 1)
    files = sorted(glob.glob(os.path.join(args.wav_dir, '*.wav')))
    if not files:
        raise ToolError('no .wav files in %s' % args.wav_dir)
    rows = []
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        for path in files:
            hit = best_match_for_wave(path, args, sr, sos, segs, locs, n, semis)
            if hit is None:
                continue
            ss, length_s, best = hit
            name = os.path.basename(path).split('.')[0]
            rows.append((abs(best[0]), best[0], name, ss, length_s, best[1], 'LR'[best[2]], (a + best[3]) / sr,
                         names.get(name, '')))
    rows.sort(reverse=True)
    lines = [ALL_TSV_HEADER] + [ALL_TSV_ROW % r[1:] for r in rows]
    with open(args.out, 'w', encoding='utf-8') as fo:
        fo.write('\n'.join(lines) + '\n')
    print('waves tried', len(rows))
    print('\n'.join(lines[:args.top + 1]))


# ---- command line ----------------------------------------------------------

def add_common(sp, window_default, window_help=None):
    sp.add_argument('--ref', required=True, help='reference recording (stereo WAV, int16 or float)')
    sp.add_argument('--t0', type=float, required=True, help='start of the reference stretch, seconds')
    sp.add_argument('--t1', type=float, required=True, help='end of the reference stretch, seconds')
    sp.add_argument('--window-ms', type=float, default=window_default,
                    help=window_help or 'template window length in ms (default %(default)s)')


def add_sample(sp):
    sp.add_argument('--sample', required=True, help='sound-bank sample (WAV; stereo is averaged to mono)')


def add_semitone_range(sp):
    sp.add_argument('--semi-lo', type=float, default=-24, help='lowest semitone offset (default %(default)s)')
    sp.add_argument('--semi-hi', type=float, default=24, help='highest semitone offset (default %(default)s)')


def build_parser():
    p = argparse.ArgumentParser(prog='sample_locate', description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True, metavar='{speeds,semis,pursue,fit,all}')

    sp = sub.add_parser('speeds', help='fine log-spaced speed grid x time, short window, one channel')
    add_common(sp, 60)
    add_sample(sp)
    sp.add_argument('--speed-lo', type=float, default=0.25, help='lowest playback speed (default %(default)s)')
    sp.add_argument('--speed-hi', type=float, default=4.0, help='highest playback speed (default %(default)s)')
    sp.add_argument('--step', type=float, default=0.001,
                    help='log-speed step as a fraction, 0.001 = 0.1%% (default %(default)s)')
    sp.add_argument('--chan', choices=CHANNELS, default='mid', help='channel to scan (default %(default)s)')
    sp.add_argument('--tpl-offset-ms', type=float, default=0.0,
                    help='skip this far into the sample before matching, ms (default %(default)s)')
    sp.add_argument('--peaks', type=int, default=14, help='peaks to list (default %(default)s)')
    sp.set_defaults(func=cmd_speeds)

    sp = sub.add_parser('semis', help='exact semitone (or cents-step) grid x time, long window, one channel')
    add_common(sp, 250)
    add_sample(sp)
    add_semitone_range(sp)
    sp.add_argument('--chan', choices=CHANNELS, required=True,
                    help='channel to scan; scan L and R separately, not mid, to see hard-panned chords')
    sp.add_argument('--cents-step', type=float, default=100.0, help='grid step in cents (default %(default)s)')
    sp.add_argument('--tpl-offset-ms', type=float, default=0.0,
                    help='skip this far into the sample before matching, ms (default %(default)s)')
    sp.add_argument('--peaks', type=int, default=16, help='peaks to list (default %(default)s)')
    sp.set_defaults(func=cmd_semis)

    sp = sub.add_parser('pursue', help='matching pursuit: find, refine, fit and subtract every copy, both channels')
    add_common(sp, 250)
    add_sample(sp)
    add_semitone_range(sp)
    sp.add_argument('--threshold', type=float, default=0.42,
                    help='stop when the best remaining ncc is below this (default %(default)s)')
    sp.add_argument('--max-iter', type=int, default=24, help='most copies to peel (default %(default)s)')
    sp.add_argument('--out', required=True, help='residual WAV after subtraction (float32)')
    sp.add_argument('--report-shift', type=float, default=0.0,
                    help='seconds subtracted from the reference time for a second time column (default %(default)s)')
    sp.add_argument('--tsv', help='also write the found copies as a TSV')
    sp.set_defaults(func=cmd_pursue)

    sp = sub.add_parser('fit', help='fit one copy on one channel, print gain and explained fraction per 50 ms')
    sp.add_argument('--ref', required=True, help='reference recording (stereo WAV, int16 or float)')
    sp.add_argument('--in', dest='in_path', help='read audio from this WAV (e.g. a pursue residual) instead of --ref')
    add_sample(sp)
    sp.add_argument('--chan', choices=('L', 'R'), required=True, help='channel to fit')
    sp.add_argument('--t', type=float, required=True, help='approximate reference time of the copy, seconds')
    sp.add_argument('--cents', type=float, required=True, help='approximate pitch of the copy, cents from native')
    sp.add_argument('--duration', type=float, required=True, help='seconds of the copy to fit (the first 0.5 s drive the refinement)')
    sp.add_argument('--hp', type=float, default=300.0, help='high-pass cutoff in Hz (default %(default)s)')
    sp.add_argument('--out', help='write the reference with this copy subtracted (float32 WAV)')
    sp.add_argument('--quiet', action='store_true', help='print only the summary line')
    sp.set_defaults(func=cmd_fit)

    sp = sub.add_parser('all', help='try every WAV in a directory at every semitone against one short stretch')
    add_common(sp, 80)
    sp.add_argument('--wav-dir', required=True, help='directory of candidate WAV samples')
    add_semitone_range(sp)
    sp.add_argument('--out', required=True, help='ranked result TSV')
    sp.add_argument('--names-tsv', help='optional occurrence table: wave name <tab> description')
    sp.add_argument('--top', type=int, default=25, help='rows echoed to the terminal (default %(default)s)')
    sp.set_defaults(func=cmd_all)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        for name, value in vars(args).items():
            if isinstance(value, float) and not np.isfinite(value):
                raise ToolError('--%s must be a finite number' % name.replace('_', '-'))
        for name in ('window_ms', 'cents_step', 'duration', 'step', 'peaks', 'max_iter', 'speed_lo', 'speed_hi'):
            value = getattr(args, name, None)
            if value is not None and value <= 0:
                raise ToolError('--%s must be greater than 0' % name.replace('_', '-'))
        for lo, hi in (('semi_lo', 'semi_hi'), ('speed_lo', 'speed_hi'), ('t0', 't1')):
            a, b = getattr(args, lo, None), getattr(args, hi, None)
            if a is None or b is None:
                continue
            if lo == 'semi_lo':
                if a > b:       # equal bounds are a legitimate single-pitch scan
                    raise ToolError('--semi-lo must not be above --semi-hi')
            elif a >= b:
                raise ToolError('--%s must be below --%s' % (lo.replace('_', '-'), hi.replace('_', '-')))
        args.func(args)
    except ToolError as err:
        print('sample_locate: error: %s' % err, file=sys.stderr)
        return 2
    except OSError as err:
        print('sample_locate: error: cannot read or write a file: %s' % err, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
