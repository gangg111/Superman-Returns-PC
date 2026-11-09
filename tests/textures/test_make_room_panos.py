import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'textures'))
import make_room_panos as rp  # noqa: E402


def test_tonemap_puts_the_median_grey_at_the_hand_computed_value():
    hdr = np.full((4, 8, 3), 5.0)
    # median luminance 5 -> 0.22; shoulder 0.22 / (1 + 0.22 / 1.5); gamma 1 / 2.2
    want = (0.22 / (1 + 0.22 / 1.5)) ** (1 / 2.2)
    assert np.allclose(rp.tonemap(hdr), want)


def test_bands_hold_each_panorama_and_the_rest_is_black():
    a = np.full((4, 8, 3), 0.25)
    b = np.full((4, 8, 3), 0.75)
    s = rp.stack([a, b], 8)
    assert s.shape == (16, 8, 4)
    assert np.allclose(s[0:4, :, :3], 0.25)
    assert np.allclose(s[4:8, :, :3], 0.75)
    assert np.allclose(s[8:, :, :3], 0.0)
    assert np.allclose(s[..., 3], 1.0)


def test_mips_of_a_non_square_texture_halve_both_sides_down_to_one_texel():
    img = np.zeros((16, 8, 4))
    img[:8] = 1.0                      # top half white
    lv = rp.mips(img)
    assert [l.shape[:2] for l in lv] == [(16, 8), (8, 4), (4, 2), (2, 1), (1, 1)]
    assert np.allclose(lv[1][:4], 1.0) and np.allclose(lv[1][4:], 0.0)
    assert np.allclose(lv[-1], 0.5)


def test_dds_header_and_size(tmp_path):
    lv = rp.mips(np.zeros((16, 8, 4)))
    p = tmp_path / 'x.dds'
    rp.write_dds(str(p), lv)
    raw = p.read_bytes()
    h, w, mips = struct.unpack_from('<3I', raw, 12)[0], struct.unpack_from('<I', raw, 16)[0], struct.unpack_from('<I', raw, 28)[0]
    assert (h, w, mips) == (16, 8, 5)
    assert len(raw) == 128 + 4 * (16 * 8 + 8 * 4 + 4 * 2 + 2 * 1 + 1)
