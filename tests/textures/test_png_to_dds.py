import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'textures'))
import png_to_dds as p2d  # noqa: E402


def test_normal_mips_stay_unit_length():
    # Two opposite tilts average to straight up: the box mean is shorter than 1, renormalised to (0,0,1).
    a = np.zeros((2, 2, 4))
    v1 = np.array([0.6, 0.0, 0.8]); v2 = np.array([-0.6, 0.0, 0.8])
    a[0, :, :3] = v1 * 0.5 + 0.5
    a[1, :, :3] = v2 * 0.5 + 0.5
    a[..., 3] = 1.0
    top = p2d.mips(a, normal=True)[-1][0, 0, :3] * 2 - 1
    assert np.allclose(top, [0.0, 0.0, 1.0], atol=1e-9)


def test_grey_mask_fills_alpha_and_dds_layout(tmp_path):
    src = tmp_path / 'm.png'
    Image.fromarray(np.full((4, 4), 128, np.uint8), 'L').save(src)
    a = p2d.load_rgba(str(src))
    assert np.allclose(a[..., 3], 128 / 255)
    out = tmp_path / 'm.dds'
    p2d.write_dds(str(out), p2d.mips(a))
    b = out.read_bytes()
    assert struct.unpack_from('<I', b, 28)[0] == 3  # 4, 2, 1
    assert len(b) == 128 + 4 * (16 + 4 + 1)
    assert b[128:132] == bytes([128, 128, 128, 128])


def test_rejects_non_power_of_two(tmp_path):
    src = tmp_path / 'x.png'
    Image.fromarray(np.zeros((6, 6, 3), np.uint8)).save(src)
    try:
        p2d.load_rgba(str(src))
    except SystemExit:
        return
    raise AssertionError('accepted 6x6')


def test_match_mean_gives_reference_means():
    rng = np.random.default_rng(1)
    src = rng.uniform(0.2, 0.4, (8, 8, 4))  # no channel reaches the 0..1 clip after the gain
    ref = np.zeros((4, 4, 4))
    ref[..., 0], ref[..., 1], ref[..., 2] = 0.6, 0.5, 0.4
    out = p2d.match_mean(src, ref)
    assert np.allclose(out[..., :3].reshape(-1, 3).mean(0), [0.6, 0.5, 0.4], atol=1e-9)
    assert np.array_equal(out[..., 3], src[..., 3])
