"""tools/re/peek.py on a tiny fake PE image built in the test.

Input the tool expects: a PE file (e_lfanew at 0x3C, section table after the
optional header) whose section RVAs are relative to the guest base 0x82000000,
then addresses "XXXXXXXX[:nwords]" (hex, default 8 words). Words are big-endian."""
import struct
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "re" / "peek.py"
IMAGE = Path(__file__).resolve().parents[2] / "logs" / "default_image.bin"


def make_pe():
    d = bytearray(0x300)
    struct.pack_into("<I", d, 0x3C, 0x40)                       # e_lfanew
    d[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", d, 0x46, 1)                          # 1 section
    struct.pack_into("<H", d, 0x54, 0)                          # no optional header
    d[0x58:0x60] = b".data\0\0\0"                               # section header at 0x40 + 24
    # VirtualSize 0x200, RVA 0x1000, SizeOfRawData 0x100, PointerToRawData 0x200
    struct.pack_into("<IIII", d, 0x60, 0x200, 0x1000, 0x100, 0x200)
    struct.pack_into(">III", d, 0x200, 0x82001010, 0x82001020, 0x12345678)
    d[0x210:0x216] = b"Hello\0"
    struct.pack_into(">I", d, 0x220, 0x82001010)                # pointer to the string pointer
    return bytes(d)


def run(image, *args):
    proc = subprocess.run([sys.executable, str(TOOL), str(image), *args], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_words_strings_bss_and_outside(tmp_path):
    img = tmp_path / "fake.exe"
    img.write_bytes(make_pe())
    out = run(img, "82001000:3", "820011FC:2", "81000000")
    # źródło: policzone ręcznie: 0x82001000 = baza + RVA 0x1000 -> plik 0x200; 0x820011FC leży
    # za SizeOfRawData (0x1FC >= 0x100) -> bss; 0x82001200 = koniec VirtualSize -> poza obrazem
    assert out == ("== 82001000\n"
                   '  +  0: 82001010  "Hello"\n'
                   '  +  4: 82001020  -> [82001010] "Hello"\n'
                   "  +  8: 12345678\n"
                   "== 820011FC\n"
                   "  +  0: bss\n"
                   "  (not in image)\n"
                   "== 81000000\n"
                   "  (not in image)\n")


@pytest.mark.skipif(not IMAGE.exists(), reason="brak zrzutu obrazu logs/default_image.bin (SR_DUMP_IMAGE)")
def test_real_image_known_constant():
    # 0x82000000 is the PE header, outside every section ("not in image" is correct);
    # 0x820013C0 holds the float 1.0 the audio code loads with lis/lfs.
    out = run(IMAGE, "820013C0:1")
    # źródło: docs/natywny-dzwiek/spec-pitch-resampler.md, tabela stałych (0x820013C0 = 0x3F800000)
    assert out.startswith("== 820013C0\n  +  0: 3F800000")
