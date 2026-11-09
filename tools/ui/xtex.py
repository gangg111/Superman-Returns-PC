"""The menus' textures (ui.AST icons and button hints): Xbox 360 DXT5, blocks
stored row by row (not tiled, although the format word has the tiled bit), 16-bit words byte-swapped, behind a 128-byte header (width, height, padded
width and height as big-endian u16 at 0..9, D3DFMT_DXT5 0x1A200154 at 14,
the data size / 256 at 24 and at 89 and 101; the rest is uninitialised).

decode(texture) -> RGBA rows; encode(rgba, header_from) -> texture bytes for
the same size as the texture whose header it borrows.
"""
import struct


def size_of(texture):
    w, h, _, pw, ph = struct.unpack_from(">5H", texture, 0)
    return w, h, pw, ph


def _swap16(b):
    out = bytearray(len(b))
    out[0::2], out[1::2] = b[1::2], b[0::2]
    return bytes(out)


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _decode_block(blk):
    a0, a1 = blk[0], blk[1]
    abits = int.from_bytes(blk[2:8], "little")
    alpha = [a0, a1] + ([((6 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else
                        [((4 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
    c0, c1 = struct.unpack_from("<HH", blk, 8)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    colors = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    cbits = int.from_bytes(blk[12:16], "little")
    return [colors[(cbits >> (2 * i)) & 3] + (alpha[(abits >> (3 * i)) & 7],) for i in range(16)]


def decode(texture):
    """RGBA rows (lists of (r, g, b, a)) of a header + data texture."""
    w, h, pw, ph = size_of(texture)
    data = _swap16(texture[128:])
    bw = pw // 4
    rows = [[None] * w for _ in range(h)]
    for by in range(h // 4):
        for bx in range(w // 4):
            at = (by * bw + bx) * 16
            px = _decode_block(data[at:at + 16])
            for i, p in enumerate(px):
                rows[by * 4 + i // 4][bx * 4 + i % 4] = p
    return rows


def _encode_block(px):
    """A DXT5 block (16 bytes, little-endian) for 16 RGBA pixels: alpha from
    its range, colour endpoints from the extremes along the luminance-weighted
    axis of the opaque pixels."""
    alphas = [p[3] for p in px]
    a0, a1 = max(alphas), min(alphas)
    if a0 == a1:
        aidx = [0] * 16
    else:
        pal = [a0, a1] + [((6 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
        aidx = [min(range(8), key=lambda k: abs(pal[k] - a)) for a in alphas]
    abits = 0
    for i, k in enumerate(aidx):
        abits |= k << (3 * i)
    seen = [p for p in px if p[3] >= 16] or px
    key = lambda p: 2 * p[0] + 4 * p[1] + p[2]
    hi, lo = max(seen, key=key), min(seen, key=key)

    def to565(p):
        return ((p[0] * 31 + 127) // 255) << 11 | ((p[1] * 63 + 127) // 255) << 5 | ((p[2] * 31 + 127) // 255)

    c0, c1 = to565(hi), to565(lo)
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        cidx = [0] * 16
    else:
        q0, q1 = _rgb565(c0), _rgb565(c1)
        pal = [q0, q1, tuple((2 * a + b) // 3 for a, b in zip(q0, q1)), tuple((a + 2 * b) // 3 for a, b in zip(q0, q1))]
        cidx = [min(range(4), key=lambda k: sum((pal[k][j] - p[j]) ** 2 for j in range(3))) for p in px]
    cbits = 0
    for i, k in enumerate(cidx):
        cbits |= k << (2 * i)
    return bytes([a0, a1]) + abits.to_bytes(6, "little") + struct.pack("<HH", c0, c1) + cbits.to_bytes(4, "little")


def encode(rows, header_from):
    """A texture (the header of `header_from`, same size) holding `rows`."""
    w, h, pw, ph = size_of(header_from)
    if len(rows) != h or len(rows[0]) != w:
        raise ValueError("rows are %dx%d, the header says %dx%d" % (len(rows[0]), len(rows), w, h))
    data = bytearray(len(header_from) - 128)
    bw = pw // 4
    for by in range(h // 4):
        for bx in range(w // 4):
            px = [rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)]
            at = (by * bw + bx) * 16
            data[at:at + 16] = _encode_block(px)
    return header_from[:128] + _swap16(bytes(data))
