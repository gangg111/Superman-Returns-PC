"""Tests for tools/ui/ps_icons.py.
Input: the sheet in tools/ui/assets (the user's PlayStation icons, 2026-11-11)
and, when present, the game's ui.AST. The expected values come from the sheet,
not from the code: a face button is an outer ring and a white disk with its
symbol cut out (the transparency read along the middle row, 2026-11-11), so the
cross's middle is transparent while the circle's dot and the square are white;
L1 is wider than tall, Options taller than wide; every pad hint the game ships
(its HINT<name> entries) has a PlayStation icon."""
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ui'))
import apt_edit  # noqa: E402
import ps_icons  # noqa: E402

UI = os.path.join(ROOT, 'game', 'DATA', 'ui.AST')


@pytest.fixture(scope='module')
def icons():
    return dict(ps_icons.icons())


def opaque_box(image):
    return image.split()[3].point(lambda a: 255 if a > 8 else 0).getbbox()


def test_names_and_size(icons):
    assert len(icons) == 28
    assert all(name.startswith('HINTPS') for name in icons)
    for name, image in icons.items():
        assert image.size == (128, 128) and image.mode == 'RGBA', name


def test_fitted_inside_the_margin(icons):
    # 6 transparent pixels on every side, and the icon reaches them on one axis
    # (116 pixels; the resampling softens up to 2 pixels at each edge below the
    # opacity threshold, so at least 112 are counted)
    for name, image in icons.items():
        x0, y0, x1, y1 = opaque_box(image)
        assert x0 >= 6 and y0 >= 6 and x1 <= 122 and y1 <= 122, name
        assert x1 - x0 >= 112 or y1 - y0 >= 112, name


def test_shapes_seen_on_the_sheet(icons):
    middle = (64, 64)
    assert icons['HINTPSA'].getpixel(middle)[3] < 60      # where the cross's cut-out strokes meet
    assert icons['HINTPSB'].getpixel(middle)[3] > 200     # the circle's white dot
    assert icons['HINTPSX'].getpixel(middle)[3] > 200     # the white square inside its cut-out outline
    for name in ('HINTPSA', 'HINTPSB', 'HINTPSX', 'HINTPSY'):
        assert icons[name].getpixel((64, 8))[3] > 200, name   # the outer ring's top
    x0, y0, x1, y1 = opaque_box(icons['HINTPSLB'])
    assert x1 - x0 > 1.4 * (y1 - y0)                       # L1: wide
    x0, y0, x1, y1 = opaque_box(icons['HINTPSSTART'])
    assert y1 - y0 > 1.5 * (x1 - x0)                       # Options: tall


def test_aliases_share_an_icon(icons):
    assert icons['HINTPSRT'].tobytes() == icons['HINTPSSHOULDER4'].tobytes()
    assert icons['HINTPSLB'].tobytes() == icons['HINTPSSHOULDER1'].tobytes()
    assert icons['HINTPSA'].tobytes() != icons['HINTPSB'].tobytes()
    assert icons['HINTPSLSTICK'].tobytes() != icons['HINTPSRSTICK'].tobytes()


@pytest.mark.skipif(not os.path.exists(UI), reason='game data not present')
def test_every_pad_hint_of_the_game(icons):
    ui = apt_edit.Archive(open(UI, 'rb').read())
    game = {e['name'][4:] for e in ui.entries
            if str(e.get('name', '')).startswith('HINT') and not str(e['name']).startswith('HINTKEY')}
    not_a_pads = {'UP', 'DOWN', 'LEFT', 'RIGHT', 'QUESTIONMARK'}
    assert len(game) == 31
    assert game - not_a_pads <= {name[6:] for name in icons}
