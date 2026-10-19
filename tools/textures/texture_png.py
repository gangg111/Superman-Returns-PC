"""Texture remaster pipeline, file side (docs/tekstury-remaster.md).

to-png <dump_dir> <out_dir>
    Game textures dumped by the renderer (sr_texture_dump_dir: <name>.dds +
    <name>.json) to RGBA PNG as the player sees them: the fetch swizzle from
    the JSON (D3D12 component mapping) is applied. Each PNG gets a JSON next
    to it with "kind": color, mono (one channel shown as grey) or normal
    (two-channel normal map: do not run it through an image upscaler).

to-dds <png_dir> <out_dir>
    Finished PNGs (any size, same <name>.png) to <name>.dds for
    sr_texture_replace_dir: R8G8B8A8_UNORM, DX10 header, full mip chain
    (2x2 box average). The renderer uploads them with the identity mapping.
"""
import argparse
import json
import pathlib
import struct
import sys

import numpy as np
from PIL import Image

DXGI_R8_UNORM = 61
DXGI_R8G8B8A8_UNORM = 28


def read_dds(path):
    """RGBA uint8 array (h, w, 4) of mip 0, before any swizzle."""
    data = pathlib.Path(path).read_bytes()
    width, height = struct.unpack_from("<2I", data, 12)[::-1]
    fourcc = data[84:88]
    fmt = struct.unpack_from("<I", data, 128)[0] if fourcc == b"DX10" else None
    if fmt == DXGI_R8_UNORM:
        plane = np.frombuffer(data, np.uint8, width * height, 148).reshape(height, width)
        rgba = np.zeros((height, width, 4), np.uint8)
        rgba[:, :, 0] = plane
        return rgba
    img = Image.open(path)
    img.load()
    return np.asarray(img.convert("RGBA"))


def apply_mapping(rgba, mapping):
    """D3D12 Shader4ComponentMapping: 3 bits per output channel, 0-3 = source
    channel, 4 = zero, 5 = one."""
    out = np.empty_like(rgba)
    for ch in range(4):
        src = (mapping >> (3 * ch)) & 7
        if src < 4:
            out[:, :, ch] = rgba[:, :, src]
        else:
            out[:, :, ch] = 255 if src == 5 else 0
    return out


def kind_of(meta):
    sources = [(meta["component_mapping"] >> (3 * ch)) & 7 for ch in range(4)]
    if sources[:3] == [0, 0, 0]:
        return "mono"
    if meta["dxgi_format"] in (83, 84) or sources[:3] == [0, 1, 1]:  # BC5 two-channel
        return "normal"
    return "color"


def to_png(dump_dir, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    count = {}
    for meta_path in sorted(dump_dir.glob("*.json")):
        meta = json.loads(meta_path.read_text())
        dds = meta_path.with_suffix(".dds")
        if not dds.exists():
            continue
        rgba = apply_mapping(read_dds(dds), meta["component_mapping"])
        Image.fromarray(rgba, "RGBA").save(out_dir / (meta["name"] + ".png"))
        meta["kind"] = kind_of(meta)
        (out_dir / (meta["name"] + ".json")).write_text(json.dumps(meta) + "\n")
        count[meta["kind"]] = count.get(meta["kind"], 0) + 1
    print("to-png:", count)


def mip_chain(rgba):
    levels = [rgba]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        a = levels[-1].astype(np.uint32)
        h, w = a.shape[:2]
        if h > 1:
            a = a[: h // 2 * 2]
            a = a[0::2] + a[1::2]
        else:
            a = a * 2
        if w > 1:
            a = a[:, : w // 2 * 2]
            a = a[:, 0::2] + a[:, 1::2]
        else:
            a = a * 2
        levels.append(((a + 2) // 4).astype(np.uint8))
    return levels


def write_dds(path, rgba):
    levels = mip_chain(rgba)
    h, w = rgba.shape[:2]
    header = [0] * 31
    header[0] = 124
    header[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000  # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|MIPMAPCOUNT
    header[2], header[3], header[4] = h, w, w * 4
    header[6] = len(levels)
    header[18], header[19] = 32, 0x4
    header[20] = struct.unpack("<I", b"DX10")[0]
    header[26] = 0x1000 | 0x400000 | 0x8  # TEXTURE|MIPMAP|COMPLEX
    with open(path, "wb") as f:
        f.write(b"DDS ")
        f.write(struct.pack("<31I", *header))
        f.write(struct.pack("<5I", DXGI_R8G8B8A8_UNORM, 3, 0, 1, 0))
        for level in levels:
            f.write(np.ascontiguousarray(level).tobytes())


def to_dds(png_dir, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for png in sorted(png_dir.glob("*.png")):
        rgba = np.asarray(Image.open(png).convert("RGBA"))
        write_dds(out_dir / (png.stem + ".dds"), rgba)
        n += 1
    print("to-dds:", n, "textures")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("command", choices=["to-png", "to-dds"])
    p.add_argument("src", type=pathlib.Path)
    p.add_argument("dst", type=pathlib.Path)
    a = p.parse_args(argv)
    (to_png if a.command == "to-png" else to_dds)(a.src, a.dst)


if __name__ == "__main__":
    sys.exit(main())
