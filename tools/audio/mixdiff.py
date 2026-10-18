"""Compare two raw mix captures (--audio_dump_raw): 256 samples x 6 channels per
frame, big-endian float32, channel-sequential, 48 kHz.

The captures are aligned by cross-correlating their loudness envelopes (the
runs start at slightly different moments), then compared per 100 ms window and
per band (low < 300 Hz, mid 300-4000 Hz, high > 4000 Hz) on the 6-channel
energy. Reports the median and 95th percentile of the absolute dB difference
per band, and the windows with the largest differences.

usage: mixdiff.py <a.raw> <b.raw> [--from S] [--to S] [--max-shift S]
"""
import argparse
import sys

import numpy as np

RATE = 48000
FRAME = 256
CH = 6
WIN = RATE // 10  # 100 ms
BANDS = ((0, 300), (300, 4000), (4000, 24000))


def load(path):
    raw = np.fromfile(path, dtype=">f4")
    frames = raw.size // (FRAME * CH)
    pcm = raw[:frames * FRAME * CH].reshape(frames, CH, FRAME)
    return pcm.transpose(0, 2, 1).reshape(-1, CH).astype(np.float64)  # samples x channels


def band_energy(pcm):
    """dB energy per 100 ms window and band, summed over channels."""
    n = pcm.shape[0] // WIN
    seg = pcm[:n * WIN].reshape(n, WIN, CH)
    spec = np.abs(np.fft.rfft(seg * np.hanning(WIN)[None, :, None], axis=1)) ** 2
    freqs = np.fft.rfftfreq(WIN, 1 / RATE)
    out = np.empty((n, len(BANDS)))
    for i, (lo, hi) in enumerate(BANDS):
        mask = (freqs >= lo) & (freqs < hi)
        out[:, i] = 10 * np.log10(spec[:, mask, :].sum(axis=(1, 2)) + 1e-12)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="start", type=float, default=0)
    ap.add_argument("--to", dest="end", type=float, default=0)
    ap.add_argument("--max-shift", type=float, default=20)
    args = ap.parse_args()
    ea, eb = band_energy(load(args.a)), band_energy(load(args.b))
    # Align on the total-energy envelope.
    ta, tb = ea.max(axis=1), eb.max(axis=1)
    ta, tb = ta - ta.mean(), tb - tb.mean()
    max_shift = int(args.max_shift * 10)
    best, shift = -np.inf, 0
    for s in range(-max_shift, max_shift + 1):
        x = ta[max(0, s):]
        y = tb[max(0, -s):]
        n = min(len(x), len(y))
        if n < 100:
            continue
        c = np.dot(x[:n], y[:n]) / n
        if c > best:
            best, shift = c, s
    a = ea[max(0, shift):]
    b = eb[max(0, -shift):]
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    lo = int(args.start * 10)
    hi = int(args.end * 10) if args.end else n
    a, b = a[lo:hi], b[lo:hi]
    audible = (a.max(axis=1) > -60) | (b.max(axis=1) > -60)
    a, b = a[audible], b[audible]
    diff = np.abs(a - b)
    print(f"alignment shift: {shift / 10:+.1f} s, compared windows: {len(diff)} (100 ms, audible)")
    for i, (lo_hz, hi_hz) in enumerate(BANDS):
        print(f"band {lo_hz:5}-{hi_hz:5} Hz: median |dB diff| {np.median(diff[:, i]):5.2f}, "
              f"p95 {np.percentile(diff[:, i], 95):5.2f}, max {diff[:, i].max():5.2f}")
    worst = np.argsort(diff.max(axis=1))[-5:][::-1]
    print("largest windows (index in compared set, dB a vs b per band):")
    for w in worst:
        print(f"  #{w}: " + "  ".join(f"{a[w, i]:6.1f}/{b[w, i]:6.1f}" for i in range(len(BANDS))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
