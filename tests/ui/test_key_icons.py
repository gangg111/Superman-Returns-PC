"""key_icons: keyboard and mouse icons for the menus' button hints.

Input: SDK key names (key_names.sdk_key_names), for the archive entries the
game's ui.AST (HINTA, HINTLSTICK_LR)."""
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "ui"))
import key_icons  # noqa: E402

GAME_UI = os.path.join(os.path.dirname(__file__), "..", "..", "game", "DATA", "ui.AST")


def test_labels_follow_the_svg():
    # źródło: klawiatura_biala_wektor.svg usera (Esc, Hm, Prt/Scr, "!" nad "1")
    assert key_icons.label_of("Escape") == "Esc"
    assert key_icons.label_of("Home") == "Hm"
    assert key_icons.label_of("PrintScreen") == "Prt\nScr"
    assert key_icons.label_of("1") == "!\n1"
    assert key_icons.label_of("Numpad7") == "Num\n7"
    assert key_icons.label_of("Q") == "Q"


def test_key_image_white_face_black_outline_clear_corner():
    img = key_icons.key_image("Shift")
    assert img.size == (128, 128)  # źródło: tekstura 128 x 128 (nagłówek HINTLSTICK_LR)
    # źródło: geometria SVG przeskalowana 92 -> 128: róg poza klawiszem, obrys przy x = 128*7/92 ~ 10,
    # lico tuż pod górnym obrysem (y ~ 16) jest białe
    assert img.getpixel((1, 1))[3] == 0
    r, g, b, a = img.getpixel((11, 64))
    assert a == 255 and max(r, g, b) < 80
    r, g, b, a = img.getpixel((30, 18))
    assert a == 255 and min(r, g, b) > 220


def test_left_mouse_button_is_filled():
    img = key_icons.key_image("LMB")
    # źródło: rysunek myszy 52 x 76 jednostek na środku; lewy przycisk wypełniony #111, prawy jasny
    left = img.getpixel((52, 40))
    right = img.getpixel((76, 40))
    assert max(left[:3]) < 60 and min(right[:3]) > 200


@pytest.mark.skipif(not os.path.exists(GAME_UI), reason="needs the game's ui.AST")
def test_hint_entry_is_hinta_at_a_quarter_with_a_128_texture():
    import apt_edit
    import xtex
    ui = apt_edit.Archive(open(GAME_UI, "rb").read())
    inner = apt_edit.Archive(key_icons.hint_entry(ui, "HINTKEY65", key_icons.key_image("A")))
    by = {e["flags"]: inner.data(e) for e in inner.entries}
    assert xtex.size_of(by[0])[:2] == (128, 128)  # źródło: SIZE
    # źródło: własny skrót wpisu: młodsze 4 bajty FNV-1a 64 z "HINTKEY65"
    suffix = struct.pack("<I", apt_edit.fnv1a64("HINTKEY65") & 0xFFFFFFFF)
    assert all(e["hash"][4:] == suffix for e in inner.entries)
    m = apt_edit.MovieEdit(by[768], by[512])
    _, items = m.frame_items(m.root)
    place = [it for it in items if m.b.u32(it) == 3][0]
    # źródło: 128 jednostek x 0,25 = 32, rozmiar HINTA na scenie
    assert struct.unpack(">ff", bytes(m.b.b[place + 16:place + 20] + m.b.b[place + 28:place + 32])) == (0.25, 0.25)
    # the texture decodes back to the drawing (white face near the top-left)
    rows = xtex.decode(by[0])
    assert rows[1][1][3] < 40 and min(rows[18][30][:3]) > 200
