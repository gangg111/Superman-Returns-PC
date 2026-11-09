"""make_cursor.cursor_pixels: input a PIL RGBA image (the cut-out shield on a
transparent background); output (size, BGRA bytes top-down, hotspot x, y): the
shield turned, cropped, in the top-left of a square, scaled to `size`, the
hotspot on its opaque pixel nearest the top-left corner (the tip)."""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'ui'))
import make_cursor  # noqa: E402


def test_crop_square_scale_bgra():
    # a 4x2 opaque red block in a 10x10 transparent image, not turned, scaled to 8
    im = Image.new('RGBA', (10, 10), (0, 0, 0, 0))
    for x in range(3, 7):
        for y in range(5, 7):
            im.putpixel((x, y), (200, 10, 20, 255))
    size, bgra, hx, hy = make_cursor.cursor_pixels(im, 8, rotate=0)
    # source: by hand, the 4x2 block in the top of a 4x4 square -> 8x8, rows 0..3 opaque;
    # the tip (the opaque pixel nearest the corner) is (0, 0)
    assert (size, hx, hy) == (8, 0, 0)
    assert len(bgra) == 8 * 8 * 4
    alpha = [bgra[i * 4 + 3] for i in range(64)]
    assert alpha[0] == 255 and alpha[3 * 8 + 3] == 255
    assert alpha[7 * 8] == 0                  # bottom-left: transparent
    # source: BGRA order of (200, 10, 20): blue 20 first, red 200 third (BOX keeps a flat colour)
    mid = (2 * 8 + 3) * 4
    assert (bgra[mid], bgra[mid + 2]) == (20, 200)


def test_tip_turned_to_the_upper_left():
    # a downward triangle (the shield's shape: wide top, tip at the bottom centre)
    im = Image.new('RGBA', (40, 40), (0, 0, 0, 0))
    for y in range(40):
        half = (39 - y) // 2
        for x in range(20 - half, 20 + half + 1):
            im.putpixel((x, y), (255, 0, 0, 255))
    size, bgra, hx, hy = make_cursor.cursor_pixels(im, 32, rotate=-135)
    # source: by hand, turned 135 degrees clockwise the tip points up and left, so the
    # hotspot is within a few pixels of the corner and on an opaque pixel
    assert hx + hy <= 4
    assert bgra[(hy * size + hx) * 4 + 3] > 128
    # source: by hand, the wide side now runs from the upper right to the lower left
    # (a diagonal), so the body lies right of and below the tip: the opaque pixels'
    # centre is well inside, and the lower-right corner, beyond the wide side, is empty
    opaque = [(i % size, i // size) for i in range(size * size) if bgra[i * 4 + 3] > 128]
    cx = sum(x for x, _ in opaque) / len(opaque)
    cy = sum(y for _, y in opaque) / len(opaque)
    assert cx > size / 4 and cy > size / 4
    assert bgra[((size - 1) * size + size - 1) * 4 + 3] == 0
