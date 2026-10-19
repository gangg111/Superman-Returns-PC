"""tools/textures/texture_png.py on tiny hand-made files (no game data).

Input the tool expects: for to-png, <name>.dds as the renderer dumps it
(DX10 header, tightly packed mip 0 first) and <name>.json with name,
dxgi_format, component_mapping; for to-dds, <name>.png in any mode."""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "textures"))
import texture_png  # noqa: E402


def write_dx10(path, dxgi, w, h, payload):
    """DDS as the renderer's WriteDds writes it (one mip)."""
    header = [0] * 31
    header[0], header[1], header[2], header[3], header[6] = 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, h, w, 1
    header[18], header[19], header[20], header[26] = 32, 0x4, struct.unpack("<I", b"DX10")[0], 0x1000
    path.write_bytes(b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", dxgi, 3, 0, 1, 0) + payload)


def test_r8_mono_mapping_to_png(tmp_path):
    dump, out = tmp_path / "dump", tmp_path / "png"
    dump.mkdir()
    # R8 2x2: 10, 20 / 30, 40; mapping RRR1 (0x1A00 as dumped by the game).
    write_dx10(dump / "t.dds", 61, 2, 2, bytes([10, 20, 30, 40]))
    (dump / "t.json").write_text(json.dumps({"name": "t", "dxgi_format": 61, "component_mapping": 0x1A00}))
    texture_png.to_png(dump, out)
    px = np.asarray(Image.open(out / "t.png"))
    # source: by hand, RRR1 = (v, v, v, 255)
    assert px[0, 0].tolist() == [10, 10, 10, 255]
    assert px[1, 1].tolist() == [40, 40, 40, 255]
    assert json.loads((out / "t.json").read_text())["kind"] == "mono"


def test_bgra_swap_mapping(tmp_path):
    dump, out = tmp_path / "dump", tmp_path / "png"
    dump.mkdir()
    # RGBA8 1x1 stored (1, 2, 3, 4); mapping 0x160A = out (B, G, R, A).
    write_dx10(dump / "c.dds", 28, 1, 1, bytes([1, 2, 3, 4]))
    (dump / "c.json").write_text(json.dumps({"name": "c", "dxgi_format": 28, "component_mapping": 0x160A}))
    texture_png.to_png(dump, out)
    # source: by hand from the mapping bits (2, 1, 0, 3)
    assert np.asarray(Image.open(out / "c.png"))[0, 0].tolist() == [3, 2, 1, 4]
    assert json.loads((out / "c.json").read_text())["kind"] == "color"


def test_bc5_is_normal_kind():
    # source: BC5 (dxgi 83) with RGGG mapping 0x1248 is a two-channel normal map
    assert texture_png.kind_of({"dxgi_format": 83, "component_mapping": 0x1248}) == "normal"


def test_to_dds_mips_and_roundtrip(tmp_path):
    src, out = tmp_path / "png", tmp_path / "dds"
    src.mkdir()
    rgba = np.array([[[0, 0, 1, 255], [100, 0, 0, 255]],
                     [[0, 200, 0, 255], [0, 0, 40, 255]]], np.uint8)
    Image.fromarray(rgba, "RGBA").save(src / "n.png")
    texture_png.to_dds(src, out)
    data = (out / "n.dds").read_bytes()
    # header: 2x2, mip count 2 (2x2 and 1x1), DXGI 28
    assert struct.unpack_from("<2I", data, 12) == (2, 2)
    assert struct.unpack_from("<I", data, 28)[0] == 2
    assert struct.unpack_from("<I", data, 128)[0] == 28
    level0 = np.frombuffer(data, np.uint8, 16, 148).reshape(2, 2, 4)
    assert (level0 == rgba).all()
    # source: by hand, rounded average of the four pixels:
    # R (0+100+0+0)/4 = 25, G 200/4 = 50, B (1+40)/4 = 10.25 -> 10, A 255
    level1 = np.frombuffer(data, np.uint8, 4, 148 + 16)
    assert level1.tolist() == [25, 50, 10, 255]
    # independent decoder (Pillow) reads mip 0 back unchanged
    assert (np.asarray(Image.open(out / "n.dds").convert("RGBA")) == rgba).all()
