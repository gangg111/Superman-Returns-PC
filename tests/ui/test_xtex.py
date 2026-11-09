"""xtex: the menus' Xbox 360 DXT5 textures.

Input: a texture as stored in ui.AST (128-byte header, then DXT5 blocks row by
row with their 16-bit words byte-swapped)."""
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "ui"))
import xtex  # noqa: E402

GAME_UI = os.path.join(os.path.dirname(__file__), "..", "..", "game", "DATA", "ui.AST")


def header(w, h):
    # źródło: nagłówek HINTA z ui.AST (szerokość, wysokość, szerokość i wysokość z dopełnieniem)
    hd = bytearray(128)
    struct.pack_into(">5H", hd, 0, w, h, w, w, h)
    return bytes(hd)


def swapped(block):
    return bytes(b for i in range(0, 16, 2) for b in (block[i + 1], block[i]))


def test_decode_hand_made_blocks():
    # 8x4: two blocks. Block 0: alpha 255 everywhere, colour 0xF800 (red) for
    # every index 0; block 1: alpha 0, colour 0x001F (blue).
    red = bytes([255, 0]) + bytes(6) + struct.pack("<HH", 0xF800, 0x001F) + bytes(4)
    blue = bytes([0, 0]) + bytes(6) + struct.pack("<HH", 0x001F, 0xF800) + bytes(4)
    tex = header(8, 4) + swapped(red) + swapped(blue)
    rows = xtex.decode(tex)
    # źródło: policzone ręcznie (565: 31 -> 255, alfa a0 przy indeksie 0)
    assert rows[0][0] == (255, 0, 0, 255)
    assert rows[3][3] == (255, 0, 0, 255)
    assert rows[0][4] == (0, 0, 255, 0)
    assert rows[3][7] == (0, 0, 255, 0)


def test_encode_then_decode_two_colours():
    # left half white opaque, right half black transparent
    rows = [[(255, 255, 255, 255)] * 4 + [(0, 0, 0, 0)] * 4 for _ in range(4)]
    tex = xtex.encode(rows, header(8, 4) + bytes(32))
    assert len(tex) == 128 + 32  # źródło: 2 bloki po 16 B
    back = xtex.decode(tex)
    # źródło: policzone ręcznie (biel i czerń są dokładnie w 565)
    assert back[2][1] == (255, 255, 255, 255)
    assert back[2][6] == (0, 0, 0, 0)


@pytest.mark.skipif(not os.path.exists(GAME_UI), reason="needs the game's ui.AST")
def test_game_hint_a_is_green_with_a_dark_rim():
    sys.path.insert(0, os.path.dirname(xtex.__file__))
    import apt_edit
    a = apt_edit.Archive(open(GAME_UI, "rb").read())
    inner = apt_edit.Archive(a.data(a.find("HINTA")))
    tex = [inner.data(e) for e in inner.entries if e["flags"] == 0][0]
    rows = xtex.decode(tex)
    # źródło: obejrzany zrzut zdekodowanej ikony 2026-11-09 (zielone kółko z literą A, przezroczyste rogi)
    r, g, b, al = rows[6][16]
    assert g > 120 and g > r + 40 and al == 255
    assert rows[0][0][3] == 0
