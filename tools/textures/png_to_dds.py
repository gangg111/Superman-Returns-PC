"""A square power-of-two picture (PNG, any mode) to an uncompressed RGBA8 DDS with a full mip
chain (box filter, linear values: for normal maps and masks, not colour textures in sRGB), the
format the texture loader reads (R8G8B8A8 legacy masks).
Normal maps (--normal): each mip's vectors are renormalised (xy from RG as 2v-1, z from B).
--size N: resize to N x N first (Lanczos). --match ref.dds: scale each colour channel so its
mean equals the reference texture's (a replacement keeps the game's tone and lighting).
usage: python tools/textures/png_to_dds.py <in.png|jpg> <out.dds> [--normal] [--size N] [--match ref.dds]"""
import struct
import sys

import numpy as np
from PIL import Image


def mips(rgba, normal=False):
    levels = [rgba]
    while levels[-1].shape[0] > 1:
        h = levels[-1].shape[0] // 2
        m = levels[-1].reshape(h, 2, h, 2, 4).mean((1, 3))
        if normal:
            v = m[..., :3] * 2.0 - 1.0
            v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
            m = np.concatenate([v * 0.5 + 0.5, m[..., 3:]], -1)
        levels.append(m)
    return levels


def write_dds(path, levels):
    size = levels[0].shape[0]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000  # caps, height, width, pixelformat, mipmapcount
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, size, size, size * 4, 0, len(levels))
    pf = struct.pack('<8I', 32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    caps = struct.pack('<5I', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + pf + caps)
        for lv in levels:
            f.write((np.clip(lv, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).tobytes())


def match_mean(rgba, ref):
    """Per-channel gain so the RGB means equal ref's (alpha untouched), clipped to 0..1."""
    out = rgba.copy()
    gain = ref[..., :3].reshape(-1, 3).mean(0) / np.maximum(rgba[..., :3].reshape(-1, 3).mean(0), 1e-6)
    out[..., :3] = np.clip(rgba[..., :3] * gain, 0.0, 1.0)
    return out


def load_rgba(path, size=None):
    im = Image.open(path)
    if size:
        im = im.resize((size, size), Image.LANCZOS)
    a = np.asarray(im.convert('RGBA')).astype(np.float64) / 255.0
    if im.mode in ('L', 'LA'):  # grey masks: the value in every channel, alpha = value too
        a[..., 3] = a[..., 0]
    h, w = a.shape[:2]
    if h != w or h & (h - 1):
        raise SystemExit(f'{path}: {w}x{h} is not square power-of-two')
    return a


def main():
    src, dst = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    normal = '--normal' in args
    size = int(args[args.index('--size') + 1]) if '--size' in args else None
    a = load_rgba(src, size)
    if '--match' in args:
        a = match_mean(a, load_rgba(args[args.index('--match') + 1]))
    write_dds(dst, mips(a, normal))
    print(dst, a.shape[0], 'normal' if normal else '')


if __name__ == '__main__':
    main()
