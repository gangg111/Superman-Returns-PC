import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'textures'))
import make_window_map as wm  # noqa: E402


def blank(n=64):
    return np.zeros((n, n, 3))


def test_panes_split_by_a_mullion_are_one_window():
    rgb = blank()
    rgb[10:30, 10:20, 0] = 1.0   # left pane
    rgb[10:30, 21:31, 0] = 1.0   # right pane, one-texel mullion between
    rects = wm.make(rgb, [1, 0, 0])
    assert np.allclose(rects[15, 12], [10 / 64, 10 / 64, 31 / 64, 30 / 64])
    assert np.allclose(rects[15, 25], rects[15, 12])
    assert np.isnan(rects[40, 40]).all()


def test_windows_apart_by_a_wall_stay_two():
    rgb = blank()
    rgb[10:30, 5:20, 0] = 1.0
    rgb[10:30, 40:55, 0] = 1.0   # 20 texels of wall: far over the closing radius
    rects = wm.make(rgb, [1, 0, 0])
    assert not np.allclose(rects[15, 10], rects[15, 45])
    assert np.allclose(rects[15, 45], [40 / 64, 10 / 64, 55 / 64, 30 / 64])


def test_window_over_the_edge_is_one_rectangle_around_each_side():
    rgb = blank()
    rgb[10:30, 58:64, 0] = 1.0
    rgb[10:30, 0:6, 0] = 1.0     # the same window continues on the next tile
    rects = wm.make(rgb, [1, 0, 0])
    right, left = rects[15, 60], rects[15, 2]
    assert np.allclose(right, [58 / 64, 10 / 64, 70 / 64, 30 / 64])
    assert np.allclose(left, [58 / 64 - 1, 10 / 64, 70 / 64 - 1, 30 / 64])


def test_kenv_picks_the_glass_layer_and_encoding_round_trips():
    rgb = blank()
    rgb[..., 1] = 1.0            # a layer the constants do not weigh
    rgb[20:40, 20:40, 2] = 1.0
    rects = wm.make(rgb, [0, 0, 0.3])
    assert np.allclose(rects[30, 30], [20 / 64, 20 / 64, 40 / 64, 40 / 64])
    enc = wm.encode(rects)
    assert (enc[0, 0] == 0).all()
    back = enc[30, 30] / 255.0 * 2.0 - 0.5
    assert np.allclose(back, rects[30, 30], atol=2.0 / 255.0)
