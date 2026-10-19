"""verify_vulkan_preshaders.verify: wejście to bajty biblioteki SRVKLIB v1 (nagłówek 24 B:
magia, u32 wersja 1, u32 liczba, u64 FNV-1a 64 ciała; wpisy <4I stage/rozmiar kontenera/
rozmiar wyniku/0 + kontener + wynik SVR3). Wynik SVR3: <4I 'SVR3',1,1,stage, 48 B pól, tabele
lokacji wejść i wyjść (u32 liczba, potem u32 lokacja, u32 długość, tekst typu), u32 rozmiar
SPIR-V i SPIR-V. Bibliotekę składamy ręcznie; refleksję SPIR-V (runtime_vulkan_shader.ready,
testowana osobno) zastępujemy znanym wynikiem, bo sprawdzamy tu parser biblioteki."""
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shaders"))
import verify_vulkan_preshaders as vvp  # noqa: E402

SPIRV = struct.pack("<5I", 0x07230203, 0x10300, 0, 1, 0)


def fnv1a64_reference(data):
    # źródło: specyfikacja FNV-1a 64 (offset 0xCBF29CE484222325, prime 0x100000001B3)
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) % (1 << 64)
    return h


def svr3(stage):
    res = struct.pack("<4I", 0x33525653, 1, 1, stage) + bytes(48)          # do offsetu 64
    res += struct.pack("<I", 1) + struct.pack("<2I", 0, 9) + b"float32x4"   # 1 wejście
    res += struct.pack("<I", 0)                                             # 0 wyjść
    return res + struct.pack("<I", len(SPIRV)) + SPIRV


def container(vertex, payload=b"\xAA" * 4):
    return struct.pack(">3I", 0x102A1100 | int(vertex), 8, 4 + len(payload)) + payload


def library(entries, version=1, count=None, checksum=None, trailing=b""):
    body = b"".join(struct.pack("<4I", s, len(c), len(r), 0) + c + r for s, c, r in entries)
    head = struct.pack("<IIQ", version, len(entries) if count is None else count,
                       fnv1a64_reference(body) if checksum is None else checksum)
    return b"SRVKLIB\x00" + head + body + trailing


@pytest.fixture
def fake_ready(monkeypatch):
    calls = []

    def ready(stage, binary):
        calls.append((stage, binary))
        return svr3(0 if stage == "vs" else 1)
    monkeypatch.setattr(vvp, "ready", ready)
    return calls


def test_valid_hand_built_library(fake_ready):
    data = library([(0, container(True), svr3(0)), (1, container(False), svr3(1))])
    assert vvp.verify(data) == 2  # źródło: dwa wpisy złożone ręcznie
    # źródło: SPIR-V wycięty z wyniku musi trafić do refleksji z właściwym etapem
    assert fake_ready == [("vs", SPIRV), ("ps", SPIRV)]


@pytest.mark.parametrize("mutate", [
    lambda: b"SRVKLIX\x00" + library([(0, container(True), svr3(0))])[8:],      # zła magia
    lambda: library([(0, container(True), svr3(0))], version=2),               # wersja
    lambda: library([(0, container(True), svr3(0))], checksum=1),              # suma FNV
    lambda: library([(0, container(True), svr3(0))], count=2),                 # za duża liczba
    lambda: library([(0, container(True), svr3(0))], trailing=b"\x00"),        # bajty na końcu
    lambda: library([(1, container(True), svr3(1))]),                          # VS jako PS
    lambda: library([(0, container(True), svr3(0))] * 2),                      # duplikat
    lambda: library([(0, container(True), svr3(0)[:-1] + b"\x00\x00")]),      # rozmiar SPIR-V
    lambda: library([(0, container(True), svr3(1))]),                          # etap w wyniku
])
def test_corrupted_libraries_are_rejected(fake_ready, mutate):
    data = mutate()
    with pytest.raises(ValueError):  # źródło: każda mutacja łamie jedną regułę formatu z docstringu
        vvp.verify(data)


def test_refleksja_rozna_od_zapisanej_jest_odrzucana(monkeypatch):
    monkeypatch.setattr(vvp, "ready", lambda stage, binary: svr3(0)[:-4] + b"\x01\x00\x00\x00")
    with pytest.raises(ValueError, match="production reflection"):  # źródło: wynik różny od zapisanego
        vvp.verify(library([(0, container(True), svr3(0))]))


LIB = ROOT / "artifacts" / "shaders" / "superman_returns_vulkan.srvk"


@pytest.mark.skipif(not LIB.is_file(), reason="wymaga lokalnej biblioteki zbudowanej z plików gry")
def test_real_library_verifies():
    data = LIB.read_bytes()
    # źródło: liczba wpisów odczytana wprost z nagłówka struct'em, niezależnie od verify
    assert vvp.verify(data) == struct.unpack_from("<I", data, 12)[0]
