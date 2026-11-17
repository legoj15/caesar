# Remove the A120 music bed from the CD around the first hit: FIR fit on clean music, tracked gain through the duck.
import sys, numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, fftconvolve
cd_p, a120_p, out_p = sys.argv[1:4]
sr, cd = wavfile.read(cd_p); cd = cd.astype(np.float64) / (32768 if cd.dtype == np.int16 else 1)
s2, m = wavfile.read(a120_p); m = m.astype(np.float64) / 32768
m = resample_poly(m, 147, 80, axis=0)                      # 24 kHz -> 44.1 kHz
A, B = int(186.0 * sr), int(198.2 * sr)
# coarse lag: where does CD[190..192] sit in the music?
tpl = cd[int(190 * sr):int(192 * sr)].mean(axis=1); guess = int((190 - 0.6731) * sr)
win = m[guess - 4000: guess + len(tpl) + 4000].mean(axis=1)
lag = int(np.argmax(fftconvolve(win, tpl[::-1], mode='valid'))) - 4000
off = int(190 * sr) - (guess + lag)                        # cd index = music index + off
print('music sits %.4f s into the CD timeline' % (off / sr))
taps = 257; half = taps // 2
res = cd.copy(); fitA, fitB = int(188.0 * sr), int(197.30 * sr); preds = np.zeros((B - A, 2))
for c in (0, 1):
    src = m[:, c]
    def cols(a, b):
        return np.stack([src[a - off + k - half: b - off + k - half] for k in range(taps)], axis=1)
    X = cols(fitA, fitB)[::3]; y = cd[fitA:fitB, c][::3]
    h, *_ = np.linalg.lstsq(X, y, rcond=None)
    pred = cols(A, B) @ h
    preds[:, c] = pred
    e = cd[fitA:fitB, c] - pred[fitA - A: fitB - A]
    print('ch %s: music cancels to %.1f dB on clean stretch' % ('LR'[c], 10 * np.log10(np.mean(e ** 2) / np.mean(cd[fitA:fitB, c] ** 2))))
    blk = int(0.04 * sr); g = []
    for q in range(A, B - blk, blk):
        p = pred[q - A: q - A + blk]; r = cd[q:q + blk, c]
        g.append(np.dot(r, p) / (np.dot(p, p) + 1e-12))
    g = np.array(g); tt = (A + (np.arange(len(g)) + 0.5) * blk) / sr
    if c == 0:
        print('music gain through the hit (CD time: gain):', ' '.join('%.2f:%.2f' % (t, v) for t, v in zip(tt, g) if 197.25 < t < 198.0))
    # gain model: 1 before the duck, block median afterwards, 0 after the cut
    gm = np.ones(len(g)); duck = tt > 197.385
    gs = np.array([np.median(g[max(0, i - 2): i + 3]) for i in range(len(g))])
    gm[duck] = np.clip(gs[duck], 0, 1)
    gi = np.interp(np.arange(A, B), tt * sr, gm)
    res[A:B, c] = cd[A:B, c] - gi * pred
wavfile.write(out_p, sr, res.astype(np.float32))
np.save(out_p + '.musicpred.npy', preds)   # music as it sits on the CD, 186.0..198.2 s
