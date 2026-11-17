# Rebuild the P10 transition from the 14 notes of Uprising sequence 286 (shared archive), fitted jointly with the
# A120 music bed against the raw CD. Note times: engine frames of 160/32728 s after the first note.
import sys, numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
cd_p, pred_p, s14_p, s39_p, s35_p, outdir = sys.argv[1:7]
sr, cd = wavfile.read(cd_p); cd = cd.astype(np.float64) / 32768
pred = np.load(pred_p); PA = int(186.0 * sr)                   # music as it sits on the CD, from 186.0 s
def load(p):
    ss, x = wavfile.read(p); x = x.astype(np.float64) / 32768
    return ss, x, resample_poly(x, 8, 1)
S = {14: load(s14_p), 39: load(s39_p), 35: load(s35_p)}
FR = 160 / 32728.0
def copy(w, semis, t_on):
    ss, x, xu = S[w]; s = 2 ** (semis / 12.0); n = int(len(x) / ss / s * sr)
    i0 = int(np.floor(t_on * sr)); fr = t_on * sr - i0
    pos = (np.arange(n) - fr) * s * ss / sr * 8
    m = (pos >= 0) & (pos < len(xu) - 1)
    y = np.zeros(n); y[m] = np.interp(pos[m], np.arange(len(xu)), xu)
    return i0, y
# (label, wave slot, key-60 semitones, tick, engine frames after first note, velocity, dominant channel, slow attack)
N = [('thud',        14,  0,   0,   0, 127, 0, 0), ('pair L',      39, -3,   6,  20, 127, 0, 0), ('pair R',      39, -1,   6,  20, 127, 1, 0),
     ('thud echo 1', 14,  0,  26,  88,  64, 0, 0), ('pair echo1 L', 39, -1, 28,  95,  70, 0, 0), ('pair echo1 R', 39, -3, 28,  95,  70, 1, 0),
     ('thud echo 2', 14,  0,  52, 177,  25, 0, 0), ('pair echo2 L', 39, -1, 53, 180,  40, 0, 0), ('pair echo2 R', 39, -3, 53, 180,  40, 1, 0),
     ('boom C',      35,  0,  64, 218, 100, 0, 0), ('boom L',      35, -7,  64, 218, 115, 0, 0), ('boom R',      35, -8,  64, 218, 115, 1, 0),
     ('boom2 L',     35, -5, 142, 484,  95, 0, 1), ('boom2 R',     35, -7, 142, 484,  95, 1, 1)]
A, B = int(197.30 * sr), int(203.6 * sr)
T0 = 197.40155
def ncc(u, v): return float(np.dot(u, v) / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-12))
# 1) each strong note: refine its own start within +-0.3 ms of the frame grid; weak notes stay on the grid
times = []
for label, w, semis, tick, frames, vel, ch, slow in N:
    t = T0 + frames * FR
    if vel >= 70:
        best = (-2, t)
        for dt in np.arange(-0.0003, 0.000301, 0.000005):
            i0, y = copy(w, semis, t + dt); n = min(len(y), int(0.25 * sr)); k = int(0.12 * sr) if slow else 0
            v = ncc(cd[i0 + k:i0 + n, ch], y[k:n])
            if v > best[0]: best = (v, t + dt)
        t = best[1]
    times.append(t)
# 2) attack shape of the slow notes, measured per engine frame on the CD (no music there)
env = []
for f in range(70):
    num = den = 0.0
    for (label, w, semis, tick, frames, vel, ch, slow), t in zip(N, times):
        if not slow: continue
        i0, y = copy(w, semis, t); a = int(f * FR * sr); b = int((f + 1) * FR * sr)
        num += np.dot(cd[i0 + a:i0 + b, ch], y[a:b]); den += np.dot(y[a:b], y[a:b])
    env.append(num / (den + 1e-12))
env = np.array(env); env /= np.median(env[45:]); env = np.clip(np.maximum.accumulate(env), 0, 1)
print('attack of the slow notes, level per engine frame (4.889 ms), dB:', ' '.join('%.0f' % (20 * np.log10(e + 1e-4)) for e in env[:40]))
def column(i, t):
    label, w, semis, tick, frames, vel, ch, slow = N[i]
    i0, y = copy(w, semis, t)
    if slow:
        g = np.interp(np.arange(len(y)) / sr, (np.arange(len(env)) + 0.5) * FR, env, left=0.0, right=1.0); y = y * g
    col = np.zeros(B - A); a = i0 - A; col[a:a + len(y)] = y[:max(0, min(len(y), B - A - a))]
    return col
X = np.stack([column(i, t) for i, t in enumerate(times)], axis=1)
# music: gain 1 before 197.36, free gain per 20 ms block from 197.36 to 197.96, nothing after
mA, mB = int(197.36 * sr), int(197.96 * sr); blk = int(0.02 * sr); nb = (mB - mA) // blk
def music_cols(c):
    M = np.zeros((B - A, nb))
    for q in range(nb):
        a = mA + q * blk; M[a - A:a - A + blk, q] = pred[a - PA:a - PA + blk, c]
    return M
fixed = np.zeros((B - A, 2)); fixed[:mA - A] = pred[A - PA:mA - PA]
def solve(X):
    G = []; R = np.zeros((B - A, 2)); MG = []
    for c in (0, 1):
        D = np.concatenate([X, music_cols(c)], axis=1); y = cd[A:B, c] - fixed[:, c]
        g = np.linalg.lstsq(D, y, rcond=None)[0]; G.append(g[:X.shape[1]]); MG.append(g[X.shape[1]:]); R[:, c] = y - D @ g
    return np.array(G), np.array(MG), R
G, MG, R = solve(X)
# 3) fine timing: nudge each strong note by a few microseconds while everything else stays put
for rnd in range(2):
    for i, (label, w, semis, tick, frames, vel, ch, slow) in enumerate(N):
        if vel < 70: continue
        base = R + np.outer(X[:, i], G[:, i]); best = (np.inf, 0.0, None)
        for dt in np.arange(-14e-6, 14.1e-6, 2e-6):
            col = column(i, times[i] + dt); e = 0.0
            for c in (0, 1):
                g = np.dot(base[:, c], col) / (np.dot(col, col) + 1e-12); d = np.diff(base[:, c] - g * col); e += np.dot(d, d)
            if e < best[0]: best = (e, dt, col)
        times[i] += best[1]; X[:, i] = best[2]
    G, MG, R = solve(X)
print('music gain per 20 ms block from CD 197.36 (L):', ' '.join('%.2f' % v for v in MG[0]))
print('music gain per 20 ms block from CD 197.36 (R):', ' '.join('%.2f' % v for v in MG[1]))
print('note            wave key-60 tick  frames  vel  note-on CD    after 1st    off grid    L dB    R dB    vel^2 dB')
for (label, w, semis, tick, frames, vel, ch, slow), t, gl, gr in zip(N, times, G[0], G[1]):
    d = lambda g: ('%+6.1f' % (20 * np.log10(abs(g)))) + ('*' if g < 0 else ' ') if abs(g) > 1e-4 else '  -inf '
    print('%-14s  %2d   %+3d   %3d   %4d   %3d  %10.5f  %+9.5f  %+7.3f ms  %s %s   %6.1f' % (label, w, semis, tick, frames, vel, t, t - times[0], (t - times[0] - frames * FR) * 1000, d(gl), d(gr), 40 * np.log10(vel / 127.0)))
print('(* = negative gain, i.e. polarity-inverted in that channel)')
sfx = np.zeros_like(cd); mus = np.zeros_like(cd)
for c in (0, 1):
    sfx[A:B, c] = X @ G[c]; mus[A:B, c] = fixed[:, c] + music_cols(c) @ MG[c]
tgt = cd - mus
print('how far the rebuild cancels the music-free CD (dB):')
for a, b in ((197.40, 197.50), (197.50, 197.86), (197.87, 198.28), (198.28, 198.46), (198.47, 199.76), (199.77, 200.07), (200.07, 201.0), (201.0, 203.2)):
    s = slice(int(a * sr), int(b * sr))
    print('  %.2f-%.2f  %6.1f' % (a, b, 10 * np.log10(np.mean((tgt[s] - sfx[s]) ** 2) / np.mean(tgt[s] ** 2))))
a, b = int(197.2 * sr), int(204.4 * sr)
def w(nm, y): wavfile.write(outdir + '/' + nm, sr, np.clip(y * 32767, -32768, 32767).astype(np.int16))
w('CD_transition_rebuilt_v2_seq286_14_notes.wav', sfx[a:b])
w('CD_transition_music_removed_v2.wav', tgt[a:b])
w('CD_transition_leftover_after_rebuild_v2.wav', (tgt - sfx)[a:b])
