"""Keyboard and mouse button icons for the menus' button hints.

Each key the bindings know (key_names.sdk_key_names) gets an icon drawn like the
user's klawiatura_biala_wektor.svg: a white key with a black outline, a soft
shadow below, a lighter face inset and the label in bold Arial; mouse buttons
get a mouse with the button filled. The icons go into ui_sr.AST as the button
hint movies HINTKEY<code> (and HINTKEYMOUSE, the mouse moving: the camera), so
the game's ButtonIcon loads them as it loads HintA:
  ../common/ButtonHints/<platform>/HintKey65.swf -> HINTKEY65.

A hint movie is HINTA's (32 x 32 on the stage) with a 128 x 128 texture: the
geometry of HINTLSTICK_LR (a 128-unit quad) placed at a quarter scale.
"""
import os
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import apt_edit
import key_names
import xtex

SIZE = 128
SS = 4  # drawn 4x, reduced with a box filter (no ringing at the outline)
FONT = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", "arialbd.ttf")

# Labels as on the SVG's keys: one line, or the shifted symbol over the key's.
LABELS = {
    "Escape": "Esc", "Backspace": "Bksp", "Return": "Enter", "Space": "Space", "Tab": "Tab", "Delete": "Del", "Insert": "Ins",
    "Home": "Hm", "End": "End", "PageUp": "Pg\nUp", "PageDown": "Pg\nDn", "Shift": "Shift", "Control": "Ctrl",
    "Alt": "Alt", "CapsLock": "Caps\nLock", "NumLock": "Num\nLk", "ScrollLock": "Scr\nLk",
    "PrintScreen": "Prt\nScr", "Pause": "Pau\nBrk", "Backtick": "~\n`", "Minus": "_\n-", "Plus": "+\n=",
    "Comma": "<\n,", "Period": ">\n.", "Semicolon": ":\n;", "Slash": "?\n/", "Backslash": "|\n\\",
    "LBracket": "{\n[", "RBracket": "}\n]", "Quote": "\"\n'", "NumpadPlus": "Num\n+", "NumpadMinus": "Num\n-",
    "NumpadStar": "Num\n*", "NumpadSlash": "Num\n/",
    "1": "!\n1", "2": "@\n2", "3": "#\n3", "4": "$\n4", "5": "%\n5", "6": "^\n6", "7": "&\n7", "8": "*\n8",
    "9": "(\n9", "0": ")\n0",
}
ARROWS = {"Left": 180, "Right": 0, "Up": 90, "Down": 270}
MOUSE = {"LMB": "left", "RMB": "right", "MMB": "middle"}


def label_of(name):
    if name.startswith("Numpad") and name[6:].isdigit():
        return "Num\n" + name[6:]
    return LABELS.get(name, name)


def _font(px):
    return ImageFont.truetype(FONT, px)


def _text(d, cx, cy, text, px, max_w):
    f = _font(px)
    while d.textlength(text, font=f) > max_w and px > 8:
        px -= 2
        f = _font(px)
    d.text((cx, cy), text, font=f, fill=(17, 17, 17, 255), anchor="mm")


def _arrow(d, cx, cy, angle, length, head, width):
    import math
    ux, uy = math.cos(math.radians(angle)), -math.sin(math.radians(angle))
    px_, py_ = -uy, ux
    tip = (cx + ux * length / 2, cy + uy * length / 2)
    base = (tip[0] - ux * head, tip[1] - uy * head)
    tail = (cx - ux * length / 2, cy - uy * length / 2)
    d.line([tail, base], fill=(17, 17, 17, 255), width=width)
    d.polygon([tip, (base[0] + px_ * head * 0.6, base[1] + py_ * head * 0.6),
               (base[0] - px_ * head * 0.6, base[1] - py_ * head * 0.6)], fill=(17, 17, 17, 255))


def _key(draw_face):
    s = SIZE * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # the SVG's 78-unit key, its shadow 5 below at 35% grey
    k = lambda v: int(round(v * s / 92.0))
    x0, y0, x1, y1 = k(7), k(4), k(85), k(82)
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((x0 + k(1), y0 + k(5), x1 + k(1), y1 + k(5)), k(11),
                                             fill=(119, 119, 119, 90))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(k(2))))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((x0, y0, x1, y1), k(11), fill=(245, 245, 245, 255), outline=(17, 17, 17, 255), width=k(3.5))
    face = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    fd = ImageDraw.Draw(face)
    for i in range(y0 + k(6), y1 - k(9)):  # white to #e8e8e8, top to bottom
        t = (i - y0 - k(6)) / max(1, (y1 - k(9)) - (y0 + k(6)))
        v = int(255 - 23 * t)
        fd.line((x0 + k(6), i, x1 - k(6), i), fill=(v, v, v, 255))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle((x0 + k(6), y0 + k(6), x1 - k(6), y1 - k(9)), k(7), fill=255)
    img.paste(face, (0, 0), mask)
    d.rounded_rectangle((x0 + k(6), y0 + k(6), x1 - k(6), y1 - k(9)), k(7), outline=(208, 208, 208, 255),
                        width=k(1.5))
    draw_face(d, (x0 + x1) / 2, (y0 + k(6) + y1 - k(9)) / 2, x1 - x0 - k(16), k)
    return img.resize((SIZE, SIZE), Image.BOX)


def key_image(name):
    """The icon of a key (an SDK key name) as a 128 x 128 RGBA image."""
    if name in MOUSE:
        return mouse_image(MOUSE[name])

    def face(d, cx, cy, w, k):
        if name in ARROWS:
            _arrow(d, cx, cy, ARROWS[name], k(34), k(16), k(6))
            return
        lines = label_of(name).split("\n")
        if len(lines) == 2:
            _text(d, cx, cy - k(11), lines[0], k(24), w)
            _text(d, cx, cy + k(13), lines[1], k(24), w)
        else:
            _text(d, cx, cy, lines[0], k(34 if len(lines[0]) <= 2 else 26), w)

    return _key(face)


def mouse_image(button):
    """A mouse: button "left", "right" or "middle" filled, or "move" (the mouse
    with arrows around it, the camera)."""
    s = SIZE * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = lambda v: int(round(v * s / 92.0))
    small = button == "move"
    w, h = (k(36), k(50)) if small else (k(52), k(76))
    x0, y0 = (s - w) // 2, (s - h) // 2
    body = (x0, y0, x0 + w, y0 + h)
    d.rounded_rectangle(body, w // 2, fill=(245, 245, 245, 255), outline=(17, 17, 17, 255), width=k(3.5))
    split = y0 + int(h * 0.42)
    if button in ("left", "right"):
        mask = Image.new("L", (s, s), 0)
        ImageDraw.Draw(mask).rounded_rectangle(body, w // 2, fill=255)
        half = Image.new("L", (s, s), 0)
        hx = (x0, y0, x0 + w // 2, split) if button == "left" else (x0 + w // 2, y0, x0 + w, split)
        ImageDraw.Draw(half).rectangle(hx, fill=255)
        from PIL import ImageChops
        img.paste((17, 17, 17, 255), (0, 0), ImageChops.multiply(mask, half))
    d.line((x0 + k(2), split, x0 + w - k(2), split), fill=(17, 17, 17, 255), width=k(3))
    d.line((x0 + w // 2, y0 + k(2), x0 + w // 2, split), fill=(17, 17, 17, 255), width=k(3))
    wheel = (x0 + w // 2 - k(4), y0 + int(h * 0.12), x0 + w // 2 + k(4), y0 + int(h * 0.32))
    d.rounded_rectangle(wheel, k(4), fill=(17, 17, 17, 255) if button == "middle" else (245, 245, 245, 255),
                        outline=(17, 17, 17, 255), width=k(2.5))
    if small:
        c = s / 2
        for angle, (ax, ay) in ((0, (c + k(36), c)), (180, (c - k(36), c)), (90, (c, c - k(38))),
                                (270, (c, c + k(38)))):
            _arrow(d, ax, ay, angle, k(14), k(10), k(4))
    return img.resize((SIZE, SIZE), Image.BOX)


def icons():
    """[(entry name, image)]: every key the bindings know, and the moving mouse."""
    out = [("HINTKEY%d" % code, key_image(name)) for code, name in key_names.sdk_key_names()]
    out.append(("HINTKEYMOUSE", mouse_image("move")))
    return out


def rows_of(image):
    px = list(image.getdata())
    return [px[y * SIZE:(y + 1) * SIZE] for y in range(SIZE)]


def hint_entry(ui, name, image):
    """A hint movie (HINTA's) holding `image`, with its own hash suffix."""
    hint = apt_edit.Archive(ui.data(ui.find("HINTA")))
    big = apt_edit.Archive(ui.data(ui.find("HINTLSTICK_LR")))
    tex_of = lambda a, flags: [a.data(e) for e in a.entries if e["flags"] == flags][0]
    texture = xtex.encode(rows_of(image), tex_of(big, 0))
    data_e = [e for e in hint.entries if e["flags"] == 768][0]
    data = bytearray(hint.data(data_e))
    # the root's one placement: matrix a and d (at +16 and +28) to a quarter
    m = apt_edit.MovieEdit(bytes(data), tex_of(hint, 512))
    _, items = m.frame_items(m.root)
    place = [it for it in items if m.b.u32(it) == 3][0]
    m.b.setf(place + 16, 0.25)
    m.b.setf(place + 28, 0.25)
    # the shape's bounds (right, bottom at +24, +28): the 128-unit quad
    shape = m.character(2)
    m.b.setf(shape + 24, float(SIZE))
    m.b.setf(shape + 28, float(SIZE))
    new_data, new_const = m.write()
    # HINTLSTICK_LR's archive (its fields hold a 16 KB texture; HINTA's do not)
    # with HINTA's movie, scaled, and the new texture; its geometry stays
    by_flags = {e["flags"]: e["hash"] for e in big.entries}
    suffix = struct.pack("<I", apt_edit.fnv1a64(name) & 0xFFFFFFFF)
    return big.build(replace={by_flags[0]: texture, by_flags[768]: new_data, by_flags[512]: new_const},
                     suffix=suffix)


def entries(ui):
    """[(name, data, flags)] for ui_sr.AST."""
    flags = ui.find("HINTA")["flags"]
    return [(name, hint_entry(ui, name, image), flags) for name, image in icons()]
