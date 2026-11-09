"""Tests for tools/textures/make_moon.py.
Input: a synthetic picture (a grey disc of known centre and radius on white)."""
import os
import struct
import sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'textures'))
import make_moon  # noqa: E402


def picture():
    # źródło: wynik zadany z góry, koło o środku (120, 90) i promieniu 60 na białym tle
    yy, xx = np.mgrid[0:200, 0:260]
    img = np.full((200, 260, 3), 255, np.uint8)
    img[np.hypot(xx - 120, yy - 90) <= 60] = 128
    return img


def test_find_disc():
    cx, cy, r = make_moon.find_disc(picture())
    # źródło: koło narysowane w picture(): środek (120, 90), promień 60 (z dokładnością do piksela)
    assert abs(cx - 120) <= 1 and abs(cy - 90) <= 1 and abs(r - 60) <= 1


def test_rgba_alpha_and_premultiplied():
    rgba = make_moon.moon_rgba(picture(), 64)
    # źródło: środek tarczy ma alfę 1 i kolor koła 128/255; róg kwadratu leży poza tarczą: alfa 0
    assert rgba.shape == (64, 64, 4)
    assert abs(rgba[32, 32, 3] - 1.0) < 1e-6
    assert abs(rgba[32, 32, 0] - 128 / 255) < 0.02
    assert rgba[0, 0, 3] == 0.0 and rgba[0, 0, 0] == 0.0


def test_dds_mips(tmp_path):
    out = tmp_path / 'moon.dds'
    levels = make_moon.mips(make_moon.moon_rgba(picture(), 64))
    make_moon.write_dds(str(out), levels)
    data = out.read_bytes()
    # źródło: 64 -> 32 -> ... -> 1 to 7 poziomów; DDS: nagłówek 128 B, potem 4 B na teksel każdego poziomu
    assert [lv.shape[0] for lv in levels] == [64, 32, 16, 8, 4, 2, 1]
    assert data[:4] == b'DDS '
    assert struct.unpack('<I', data[28:32])[0] == 7
    assert struct.unpack('<I', data[88:92])[0] == 32 and struct.unpack('<I', data[92:96])[0] == 0xFF
    assert len(data) == 128 + 4 * sum(s * s for s in (64, 32, 16, 8, 4, 2, 1))
