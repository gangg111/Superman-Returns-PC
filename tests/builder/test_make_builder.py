"""make_builder: the payload appended to the builder (format in builder/pack.h).

Input: [(payload path, bytes)]; the index is read back here as pack.h reads it, and the
LZMS data unpacked with the Windows Compression API as the builder does."""
import ctypes
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "builder"))
import make_builder  # noqa: E402


def unlzms(data, size):
    cab = ctypes.WinDLL("cabinet.dll")
    h = ctypes.c_void_p()
    assert cab.CreateDecompressor(5, None, ctypes.byref(h))
    out = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    assert cab.Decompress(h, data, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(size), ctypes.byref(got))
    cab.CloseDecompressor(h)
    return out.raw[:got.value]


def read_index(body, index, base=0):
    count, = struct.unpack_from("<I", index, 0)
    p, out = 4, {}
    for _ in range(count):
        n, = struct.unpack_from("<H", index, p)
        path = index[p + 2:p + 2 + n].decode()
        p += 2 + n
        method, off, packed, size = struct.unpack_from("<BQQQ", index, p)
        sha = index[p + 25:p + 57]
        p += 57
        raw = body[off - base:off - base + packed]
        data = unlzms(raw, size) if method else raw
        out[path] = (method, data, sha)
    return out


def test_pack_round_trip():
    big = b"superman " * 2000   # 18000 bytes, compresses
    small = b"tiny"            # below 4096: stored
    body, index = make_builder.pack([("files/a.bin", big), ("builder.txt", small)])
    got = read_index(body, index)
    # source: pack.h: method 1 LZMS for what shrinks, 0 stored; SHA-256 of the unpacked bytes
    assert got["files/a.bin"][0] == 1 and got["files/a.bin"][1] == big
    assert got["builder.txt"] == (0, small, hashlib.sha256(small).digest())
    assert len(body) < len(big)


def test_default_settings_name_no_machine():
    # source: the user's superman_returns.toml (2026-11-09) holds sr_native_gpu_luid and window sizes,
    # which belong to one machine; the builder's default must not
    assert "gpu_luid" not in make_builder.DEFAULT_TOML
    assert "sr_mouse_sensitivity = 15" in make_builder.DEFAULT_TOML


def test_offsets_count_from_the_exe_start():
    # source: pack.h reads each file at its offset in the .exe (the program comes first)
    data = b"x" * 10
    body, index = make_builder.pack([("a", data)], base=1000)
    off, = struct.unpack_from("<Q", index, 4 + 2 + 1 + 1)
    assert off == 1000
    assert read_index(body, index, base=1000)["a"][1] == data


def test_tiled_address_first_blocks():
    # source: worked by hand from Xenia's XenosTextureTiledAddress2D, 8-byte blocks (BC1):
    # (1,0) is the next block (8), (0,1) the odd row (bit 4: 16), (8,0) the next pipe (bit 6: 64)
    assert make_builder.tiled_address(0, 0, 1, 3) == 0
    assert make_builder.tiled_address(1, 0, 1, 3) == 8
    assert make_builder.tiled_address(0, 1, 1, 3) == 16
    assert make_builder.tiled_address(8, 0, 1, 3) == 64


def test_decode_bc1_block_as_pillow():
    # Input: one 4x4 BC1 texture as on the disc (tiled, 8-in-16 swapped): c0 = 0xF800 (red),
    # c1 = 0x001F (blue), first texel index 0, second index 2, the rest 0.
    # source: by hand, 5:6:5 widened by bit repeat (255) and the 2/3 point rounded down:
    # (2*255 + 0) // 3 = 170 red, (0 + 255) // 3 = 85 blue
    block = struct.pack("<HHI", 0xF800, 0x001F, 2 << 2)
    swapped = bytes(block[i ^ 1] for i in range(8))
    rgba = make_builder.decode_game_texture(swapped, 4, 4, 18)
    assert rgba[0:4] == bytes([255, 0, 0, 255])
    assert rgba[4:8] == bytes([170, 0, 85, 255])
    assert rgba[8:12] == bytes([255, 0, 0, 255])
