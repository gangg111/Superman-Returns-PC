"""tools/audio/ea_ast.py on small hand-built inputs (no game files).

Inputs the tool expects:
  refpack_decompress: EA RefPack stream (0x10 0xFB, u24 BE size, commands)
  Bgfa: "BGFA" archive, LE header, TOC with variable-width fields
  parse_sabr / parse_snr: "rbas" record, property list, SNR header (BE bit fields)
  block_list / deblock: EAAC blocks (u8 flag + u24 BE size, u32 BE samples, streams)
"""
import struct
import zlib

import pytest
from pathlib import Path

import ea_ast

ROOT = Path(__file__).resolve().parents[2]
GAME_DATA = ROOT.parent / "SupermanReturnsPC" / "game" / "DATA"


def test_refpack_literals_and_overlapping_copy():
    src = bytes([0x10, 0xFB, 0x00, 0x00, 0x0A,   # RefPack, uncompressed size 10
                 0xE0]) + b"ABCD" + bytes([       # literal block: ((0 & 0x1F) << 2) + 4 = 4 bytes
                 0x0C, 0x03,                      # 2-byte cmd: 0 literals, copy ((0x0C>>2)&7)+3 = 6 from offset 3+1 = 4
                 0xFC])                           # end, 0 trailing literals
    # źródło: policzone ręcznie wg opisu formatu RefPack/QFS (copy nakładające się bajt po bajcie)
    assert ea_ast.refpack_decompress(src) == b"ABCDABCDAB"


def test_refpack_size_mismatch_is_rejected():
    src = bytes([0x10, 0xFB, 0x00, 0x00, 0x05, 0xE0]) + b"ABCD" + bytes([0xFC])
    with pytest.raises(ValueError):  # źródło: nagłówek mówi 5 bajtów, strumień daje 4
        ea_ast.refpack_decompress(src)


def make_bgfa():
    plain = b"A" * 100
    packed = zlib.compress(plain)
    data = bytearray(0x80)
    data[0x40:0x40 + 12] = b"hello world!"
    data[0x50:0x50 + len(packed)] = packed
    # widths: flag 1 byte, hash 4, offset 2 (<< shift 4), stored size 4, size 4, name 8; num = 1
    toc = b"\0" * 4
    toc += bytes([0x01]) + b"HSH0" + struct.pack("<HII", 0x40 >> 4, 12, 0) + b"first\0\0\0"
    toc += bytes([0x02]) + b"HSH1" + struct.pack("<HII", 0x50 >> 4, len(packed), len(plain)) + b"second\0\0"
    head = b"BGFA" + b"0003" + b"\0" * 4 + struct.pack("<IIII", 2, 0x80, 0, len(toc))
    head += b"\0" * 4 + bytes([0, 0, 4, 2, 4, 4, 4, 0]) + struct.pack("<II", 1, 8)
    data[:0x30] = head
    return bytes(data) + toc, plain


def test_bgfa_toc_and_read():
    data, plain = make_bgfa()
    arc = ea_ast.Bgfa("mem", data=data)
    assert arc.version == "0003"  # źródło: wpisane w nagłówek wyżej
    e0, e1 = arc.entries
    assert (e0.name, e0.flags, e0.hash, e0.offset) == ("first", 1, b"HSH0", 0x40)   # źródło: 4 << 4 = 0x40
    assert (e1.name, e1.flags, e1.hash, e1.offset) == ("second", 2, b"HSH1", 0x50)  # źródło: 5 << 4 = 0x50
    assert arc.toc_used == 50  # źródło: 4 + 2 * (1 + 4 + 2 + 4 + 4 + 8) policzone ręcznie
    assert arc.read(e0) == b"hello world!"  # źródło: bajty wpisane pod 0x40
    assert arc.read(e1) == plain            # źródło: tekst przed kompresją zlib
    assert set(arc.by_name()) == {"first", "second"}


def test_bgfa_rejects_other_magic():
    with pytest.raises(ValueError):
        ea_ast.Bgfa("mem", data=b"XXXX" + b"\0" * 0x40)


def test_parse_snr_bit_fields():
    # h1: version 0, codec 3 (bits 24-27), channels-1 = 1 (bits 18-23), rate 44100 = 0xAC44
    # h2: kind 1 (bits 30-31), loop 1 (bit 29), samples 1000; then loop start 500
    d = struct.pack(">III", 0x0304AC44, 0x600003E8, 500)
    s = ea_ast.parse_snr(d, 0)
    # źródło: pola zakodowane ręcznie wg układu SNR z vgmstream ea_eaac.c
    assert (s.codec, s.channels, s.rate) == (3, 2, 44100)
    assert (s.kind, s.loop, s.samples, s.loop_start, s.header_size) == (1, True, 1000, 500, 12)


def test_parse_snr_no_loop_and_bad_version():
    s = ea_ast.parse_snr(struct.pack(">II", 0x0300BB80, 0x00000010), 0)
    assert (s.channels, s.rate, s.loop, s.samples, s.header_size) == (1, 48000, False, 16, 8)  # źródło: 0xBB80 = 48000
    with pytest.raises(ValueError):
        ea_ast.parse_snr(struct.pack(">II", 0x1300BB80, 0), 0)  # źródło: wersja 1 w górnym półbajcie


def test_parse_sabr_props_and_snr_alignment(make_sabr):
    snr = struct.pack(">II", 0x0300AC44, 0x00000064)
    rec = make_sabr([("numc", 0, struct.pack("<I", 3)),
                     ("volm", 1, struct.pack("<i", 0x6400)),
                     ("name", 2, b"boom\0\0")], snr)
    r = ea_ast.parse_sabr(rec)
    # źródło: 0x6400 / 256 = 100.0; tekst do pierwszego NUL
    assert r.props == {"numc": 3, "volm": 100.0, "name": "boom"}
    assert r.snr_pos == 80  # źródło: 8 + 20 + 20 + 22 = 70, w górę do 16 = 80
    assert (r.snr.rate, r.snr.samples) == (44100, 100)
    assert not ea_ast.is_bank(rec)
    with pytest.raises(ValueError):
        ea_ast.parse_sabr(b"xxxx" + rec[4:])


def test_parse_bank(make_prop):
    lists = make_prop("numc", 0, struct.pack("<I", 1)) * 2           # two lists of 20 bytes
    names = b"A\0BB\0"
    d = b"rbas" + b"\0" * 4 + struct.pack("<III", 2, 0, 0)            # count 2 at 8, two u32
    d += struct.pack("<II", 0, 2) + struct.pack("<II", 0, 20)         # name offsets, list offsets
    d += struct.pack("<I", len(lists)) + lists + names
    assert ea_ast.is_bank(d)  # źródło: pod 8 jest licznik, nie "cmun"
    # źródło: nazwy i listy wpisane ręcznie wyżej
    assert ea_ast.parse_bank(d) == [("A", {"numc": 1}), ("BB", {"numc": 1})]


def test_block_list_and_deblock(make_block):
    buf = make_block(100, [b"\x01\x02\x03\x04", b"\xaa\xbb"]) + make_block(28, [b"\x05", b"\xcc"], last=True)
    buf += make_block(999, [b"\x00", b"\x00"])  # after the last-block flag: must be ignored
    assert ea_ast.block_list(buf, 0) == [100, 28]  # źródło: liczby próbek wpisane w bloki
    data0, total0 = ea_ast.deblock(buf, 0, 0)
    data1, total1 = ea_ast.deblock(buf, 0, 1)
    assert total0 == total1 == 128  # źródło: 100 + 28
    # źródło: każdy blok dopełniony 0xFF do pakietu 2048 B
    assert data0 == b"\x01\x02\x03\x04" + b"\xff" * 2044 + b"\x05" + b"\xff" * 2047
    assert data1 == b"\xaa\xbb" + b"\xff" * 2046 + b"\xcc" + b"\xff" * 2047


def test_xma2_riff_layout():
    riff = ea_ast.xma2_riff(b"0123456789", 2, 44100, 512)
    # źródło: RIFF/WAVE wg specyfikacji Microsoft; fmt = 18 B WAVEFORMATEX + 34 B XMA2
    assert riff[:4] == b"RIFF" and riff[8:16] == b"WAVEfmt "
    assert struct.unpack_from("<I", riff, 4)[0] == 4 + 8 + 52 + 8 + 10
    assert struct.unpack_from("<I", riff, 16)[0] == 52
    assert struct.unpack_from("<HHI", riff, 20) == (0x166, 2, 44100)  # źródło: WAVE_FORMAT_XMA2 = 0x166
    assert riff[-18:-10] == b"data" + struct.pack("<I", 10) and riff[-10:] == b"0123456789"


@pytest.mark.skipif(not (GAME_DATA / "sound.AST").exists(), reason="brak danych gry (sound.AST)")
def test_real_sound_archive_records():
    arc = ea_ast.Bgfa(str(GAME_DATA / "sound.AST"))
    assert arc.entries and arc.toc_used <= arc.toc_size
    singles = 0
    for e in arc.entries[:200]:
        try:
            body = arc.read(e)
        except NotImplementedError:
            continue
        if body[:4] == b"rbas" and not ea_ast.is_bank(body):
            snr = ea_ast.parse_sabr(body).snr
            assert snr.codec == 3 and snr.channels in (1, 2, 4, 6)  # źródło: EA-XMA w wersji X360
            singles += 1
    assert singles > 0
