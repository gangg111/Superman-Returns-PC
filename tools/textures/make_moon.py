"""The sky's moon texture (patch 0034 srSkyBodies) from a picture of the full moon on a plain
background: the disc found, cropped, an anti-aliased circular alpha, 512x512 with a full mip
chain, premultiplied by alpha, written as an uncompressed RGBA8 DDS (R8G8B8A8, the texture
loader's legacy masks).
usage: python tools/textures/make_moon.py <picture> <out.dds> [size]"""
import struct
import sys

import numpy as np
from PIL import Image


def find_disc(rgb):
    """Centre and radius of the disc: the pixels that differ from the corner colour."""
    bg = np.median(np.concatenate([rgb[:8, :8].reshape(-1, 3), rgb[-8:, -8:].reshape(-1, 3)]), 0)
    mask = np.abs(rgb.astype(np.float32) - bg).max(-1) > 24
    ys, xs = np.nonzero(mask)
    cx, cy = (xs.min() + xs.max()) / 2.0, (ys.min() + ys.max()) / 2.0
    r = ((xs.max() - xs.min()) + (ys.max() - ys.min())) / 4.0
    return cx, cy, r


def moon_rgba(rgb, size):
    cx, cy, r = find_disc(rgb)
    r *= 0.985  # inside the rim, where the background bleeds into the edge pixels
    im = Image.fromarray(rgb).crop((int(cx - r), int(cy - r), int(cx + r), int(cy + r)))
    im = im.resize((size, size), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32) / 255.0
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) + 0.5
    d = np.hypot(xx - size / 2.0, yy - size / 2.0) / (size / 2.0)
    alpha = np.clip((1.0 - d) * size / 2.0, 0.0, 1.0)  # one-pixel soft edge
    return np.concatenate([a * alpha[..., None], alpha[..., None]], -1)  # premultiplied


def mips(rgba):
    levels = [rgba]
    while levels[-1].shape[0] > 1:
        h = levels[-1].shape[0] // 2
        levels.append(levels[-1].reshape(h, 2, h, 2, 4).mean((1, 3)))
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


def main():
    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    rgb = np.asarray(Image.open(src).convert('RGB'))
    write_dds(dst, mips(moon_rgba(rgb, size)))
    print(dst, size)


if __name__ == '__main__':
    main()
