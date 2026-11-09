"""Room panoramas for the night offices (patch 0036 srWindowRoom): indoor HDRIs (Poly Haven,
CC0) tone mapped to display values and stacked as horizontal bands of one RGBA8 DDS with a full
mip chain. Band k of N holds panorama k (equirectangular, 2:1); bands past the inputs stay black.
The shader picks a band per window and box-projects the room onto it (interior mapping).
Tone map: the median luminance to MID, then a soft shoulder x / (1 + x / KNEE) so the daylight
windows inside the rooms do not blow out, then gamma 2.2 (the values the old atlas held).
usage: python tools/textures/make_room_panos.py <out.dds> <band width> <a.hdr> [b.hdr ...]"""
import sys

import numpy as np

MID, KNEE = 0.22, 1.5
BANDS = 4


def tonemap(hdr):
    lum = hdr[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    x = hdr[..., :3] * (MID / max(float(np.median(lum)), 1e-6))
    x = x / (1.0 + x / KNEE)
    return np.clip(x, 0.0, 1.0) ** (1.0 / 2.2)


def resize_box(img, w, h):
    """Area resize by an integer factor or Lanczos-free bilinear fallback via cv2."""
    import cv2
    return cv2.resize(img.astype(np.float32), (w, h), interpolation=cv2.INTER_AREA).astype(np.float64)


def stack(panos, width):
    bh = width // 2
    out = np.zeros((bh * BANDS, width, 4))
    out[..., 3] = 1.0
    for k, p in enumerate(panos[:BANDS]):
        out[k * bh:(k + 1) * bh, :, :3] = resize_box(p, width, bh) if p.shape[1] != width else p[..., :3]
    return out


def mips(img):
    levels = [img]
    while max(levels[-1].shape[:2]) > 1:
        h, w = levels[-1].shape[:2]
        a = levels[-1]
        if h > 1:
            a = a.reshape(h // 2, 2, a.shape[1], 4).mean(1)
        if w > 1:
            a = a.reshape(a.shape[0], w // 2, 2, 4).mean(2)
        levels.append(a)
    return levels


def write_dds(path, levels):
    import struct
    h, w = levels[0].shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, h, w, w * 4, 0, len(levels))
    pf = struct.pack('<8I', 32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    caps = struct.pack('<5I', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + pf + caps)
        for lv in levels:
            f.write((np.clip(lv, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8).tobytes())


def main():
    import cv2
    out, width, srcs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    panos = []
    for s in srcs:
        hdr = cv2.imread(s, cv2.IMREAD_ANYDEPTH | cv2.IMREAD_COLOR)
        if hdr is None:
            sys.exit(f'cannot read {s}')
        panos.append(tonemap(hdr[..., ::-1].astype(np.float64)))
    write_dds(out, mips(stack(panos, width)))
    print(out, f'{width}x{width // 2 * BANDS}', len(panos), 'panoramas')


if __name__ == '__main__':
    main()
