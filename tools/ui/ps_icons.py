"""PlayStation button icons for the menus' button hints (DualShock 4, DualSense).

The icons come from assets/ps_buttons_zacksly.png, white on a transparent
background: a sheet the user generated with AI after "PlayStation 5 Button
Icons & Controls" by Zacksly (CC BY 4.0) as the reference (2026-11-11). Each icon the game's hints use (the Xbox
ButtonHints, HINT<name>) gets its PlayStation counterpart HINTPS<name>, loaded
instead while a PlayStation pad was used last (build_ui_mod.py srSetIcon):
  ../common/ButtonHints/<platform>/HintPsA.swf -> HINTPSA.
The plain arrows (UP, DOWN, LEFT, RIGHT) and QUESTIONMARK are not a pad's and
stay as they are.

The sheet is a grid: five rows (face buttons and shoulders, the d-pad, the left
stick, the right stick, Options/Create and the touch pad), each icon cropped
to its opaque pixels and fitted into the 128 x 128 hint texture.
"""
import os

from PIL import Image

SIZE = 128
MARGIN = 6  # transparent pixels kept around the icon in the texture
SHEET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "ps_buttons_zacksly.png")

# The sheet's rows (y from, y to) and columns (x from, x to), measured on the
# alpha of the 1774 x 887 sheet: a row or column ends where no pixel is opaque
# (the stick rows touch at y 510, their arrows almost meet).
ROWS = [(0, 170), (170, 326), (327, 510), (511, 690), (690, 886)]
COLUMNS = [
    [(61, 200), (269, 408), (480, 619), (688, 827), (910, 1035), (1101, 1250), (1305, 1432), (1498, 1658)],
    [(63, 202), (270, 414), (480, 624), (692, 832), (899, 1043)],
    [(68, 195), (235, 413), (430, 609), (642, 766), (807, 960), (1018, 1143), (1200, 1351), (1383, 1508),
     (1562, 1707)],
    [(68, 195), (235, 413), (430, 609), (642, 766), (807, 960), (1018, 1143), (1200, 1351), (1383, 1508),
     (1562, 1707)],
    [(89, 165), (273, 359), (423, 623), (657, 824), (865, 1034), (1074, 1238), (1286, 1456), (1497, 1695)],
]

# hint name (as the game's HINT<name>) -> (row, column) on the sheet
ICONS = {
    "A": (0, 0), "B": (0, 1), "X": (0, 2), "Y": (0, 3),  # cross, circle, square, triangle
    "RT": (0, 4), "SHOULDER4": (0, 4), "RB": (0, 5), "SHOULDER2": (0, 5),  # R2, R1
    "LT": (0, 6), "SHOULDER3": (0, 6), "LB": (0, 7), "SHOULDER1": (0, 7),  # L2, L1
    "DPAD": (1, 0), "DPAD_LEFT": (1, 1), "DPAD_UP": (1, 2), "DPAD_RIGHT": (1, 3), "DPAD_DOWN": (1, 4),
    # a stick: all four ways; ND (neutral) the stick alone; LR and UD its two ways
    "LSTICK": (2, 1), "STICK": (2, 1), "LSTICK_ND": (2, 0), "LSTICK_LR": (2, 2), "LSTICK_UD": (2, 3),
    "RSTICK": (3, 1), "RSTICK_ND": (3, 0), "RSTICK_LR": (3, 2), "RSTICK_UD": (3, 3),
    "START": (4, 0), "SELECT": (4, 1),  # Options, Create
}


def sheet():
    return Image.open(SHEET).convert("RGBA")


def cell(image, row, column):
    """The icon at (row, column): its cell cropped to the opaque pixels."""
    (y0, y1), (x0, x1) = ROWS[row], COLUMNS[row][column]
    part = image.crop((x0, y0, x1 + 1, y1 + 1))
    box = part.split()[3].point(lambda a: 255 if a > 8 else 0).getbbox()
    return part.crop(box) if box else part


def fit(icon):
    """`icon` scaled to fit SIZE - 2 * MARGIN, centred on a transparent SIZE x SIZE."""
    room = SIZE - 2 * MARGIN
    scale = min(room / icon.width, room / icon.height)
    w, h = max(1, round(icon.width * scale)), max(1, round(icon.height * scale))
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(icon.resize((w, h), Image.LANCZOS), ((SIZE - w) // 2, (SIZE - h) // 2))
    return out


def icons():
    """[(entry name, 128 x 128 RGBA image)], one per hint in ICONS."""
    image = sheet()
    return [("HINTPS" + name, fit(cell(image, row, column))) for name, (row, column) in ICONS.items()]
