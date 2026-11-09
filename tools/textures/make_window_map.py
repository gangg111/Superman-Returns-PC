"""Window maps for the night rooms (patch 0036): for a building's material map (mat_map) and its
glass weights (the shader constant Kenv), every texel inside a window gets that window's
rectangle, so the shader can draw one room per window (interior mapping) instead of lighting
pieces of panes.

A window: the glass (weighted layers over their maximum, above GLASS) after a morphological
closing that bridges mullions (gaps up to CLOSE_FRAC of the texture), as a connected region on
the torus (the texture repeats, so a window across the edge stays one window). Its rectangle is
the region's bounding box in the region's own unwrapped coordinates.

Output: RGBA8 DDS, one level, same size as the mat_map: (u0, v0, u1, v1) of the texel's window in
texture units, stored as (x + 0.5) / 2 (range -0.5 .. 1.5 for windows over the edge); all zero
outside windows. The shader reads it with Load (no filtering: rectangles must not blend).
usage: python tools/textures/make_window_map.py <pairs.json> <texdump dir> <out dir>
  pairs.json: [[mat_map name, [kr, kg, kb]], ...] (several Kenv per map: the first one wins)"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

GLASS = 0.5
CLOSE_FRAC = 0.03


def glass_mask(rgb, kenv):
    k = np.maximum(np.asarray(kenv, np.float64), 0.0)
    return (rgb[..., :3] * k).sum(-1) / max(k.max(), 1e-3) > GLASS


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(m, (dy, dx), (0, 1))
    return out


def erode(m, r):
    return ~dilate(~m, r)


def close(m, r):
    return erode(dilate(m, r), r) | m


def components(m):
    """Labels of 4-connected regions on the torus (0 = background)."""
    h, w = m.shape
    lab = np.zeros((h, w), np.int32)
    n = 0
    for y0, x0 in zip(*np.nonzero(m)):
        if lab[y0, x0]:
            continue
        n += 1
        lab[y0, x0] = n
        stack = [(y0, x0)]
        while stack:
            y, x = stack.pop()
            for yy, xx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                yy %= h
                xx %= w
                if m[yy, xx] and not lab[yy, xx]:
                    lab[yy, xx] = n
                    stack.append((yy, xx))
    return lab, n


def unwrapped_span(idx, size):
    """Smallest interval (start, end) in texels covering the occupied indices on a circle."""
    occ = np.zeros(size, bool)
    occ[idx] = True
    if occ.all():
        return 0, size
    # Start right after the longest empty run.
    free = ~occ
    best_len, best_end, run = 0, 0, 0
    for i in range(2 * size):
        if free[i % size]:
            run += 1
            if run > best_len:
                best_len, best_end = run, i
        else:
            run = 0
    start = (best_end + 1) % size
    return start, start + size - best_len


def window_rects(m):
    """Per texel (u0, v0, u1, v1) in texture units, NaN outside windows."""
    h, w = m.shape
    lab, n = components(m)
    out = np.full((h, w, 4), np.nan)
    for i in range(1, n + 1):
        ys, xs = np.nonzero(lab == i)
        x0, x1 = unwrapped_span(xs, w)
        y0, y1 = unwrapped_span(ys, h)
        # Texels before the span start belong to the copy one tile further.
        u0, u1 = x0 / w, x1 / w
        v0, v1 = y0 / h, y1 / h
        out[ys, xs] = (u0, v0, u1, v1)
        # Texels whose wrapped index lies before the start are at +1 tile: shift so that the
        # shader's frac(uv) sees the rectangle around them.
        wrap_x = xs < x0
        wrap_y = ys < y0
        out[ys[wrap_x], xs[wrap_x], 0::2] -= 1.0
        out[ys[wrap_y], xs[wrap_y], 1::2] -= 1.0
    return out


def encode(rects):
    e = np.where(np.isnan(rects), 0.0, (rects + 0.5) / 2.0)
    return (np.clip(e, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def write_dds(path, rgba8):
    h, w = rgba8.shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, h, w, w * 4, 0, 1)
    pf = struct.pack('<8I', 32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    caps = struct.pack('<5I', 0x1000, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + pf + caps + rgba8.tobytes())


def make(mat_rgb, kenv):
    m = glass_mask(mat_rgb, kenv)
    r = max(1, int(round(CLOSE_FRAC * max(m.shape))))
    return window_rects(close(m, r))


def main():
    pairs, dump, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    done = set()
    for name, kenv in json.load(open(pairs)):
        if name in done:
            continue
        done.add(name)
        rgb = np.asarray(Image.open(os.path.join(dump, name + '.dds')).convert('RGB')).astype(np.float64) / 255.0
        rects = make(rgb, kenv)
        write_dds(os.path.join(out, name + '_win.dds'), encode(rects))
        n = len({tuple(r) for r in rects.reshape(-1, 4) if not np.isnan(r[0])})
        print(name, 'windows', n)


if __name__ == '__main__':
    main()
