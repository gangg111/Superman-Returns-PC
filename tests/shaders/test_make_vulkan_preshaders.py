"""make_vulkan_preshaders.serialize: wejście to lista (stage 0=VS/1=PS, kontener shadera gry,
wynik SVR3). Kontener zaczyna się nagłówkiem big-endian: flagi 0x102A1100|is_vertex,
rozmiar wirtualny, rozmiar fizyczny (suma = długość kontenera). Wyjście: biblioteka SRVKLIB v1:
'SRVKLIB\\0', u32 wersja 1, u32 liczba, u64 FNV-1a 64 ciała; potem wpisy <4I + bajty.
Pełna kompilacja (main) wymaga emitera, DXC i plików gry, więc test jest pominięty bez nich."""
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shaders"))
import make_vulkan_preshaders as mvp  # noqa: E402


def fnv1a64_reference(data):
    """Niezależna implementacja FNV-1a 64 według specyfikacji (offset basis i prime z RFC draft
    FNV, Fowler/Noll/Vo); sprawdzona na wektorze 'a' poniżej."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) % (1 << 64)
    return h


def container(vertex, payload=b"\x00" * 4):
    # 12 bajtów nagłówka + payload; vsize=8, psize=len-8, więc suma = długość
    total = 12 + len(payload)
    return struct.pack(">3I", 0x102A1100 | int(vertex), 8, total - 8) + payload


def test_reference_fnv_matches_published_vector():
    # źródło: oficjalny wektor testowy FNV-1a 64 dla "a" (isthe.com/chongo/tech/comp/fnv)
    assert fnv1a64_reference(b"a") == 0xAF63DC4C8601EC8C


def test_serialize_layout_by_hand():
    vs, ps = container(True), container(False, b"\x01\x02\x03\x04\x05\x06")
    entries = [(0, vs, b"RES-VS"), (1, ps, b"R")]
    body = struct.pack("<4I", 0, 16, 6, 0) + vs + b"RES-VS" + struct.pack("<4I", 1, 18, 1, 0) + ps + b"R"
    # źródło: format złożony ręcznie z docstringu (struct + własne FNV-1a)
    expected = b"SRVKLIB\x00" + struct.pack("<IIQ", 1, 2, fnv1a64_reference(body)) + body
    assert mvp.serialize(entries) == expected
    assert len(expected) == 24 + 16 + 16 + 6 + 16 + 18 + 1  # źródło: policzone ręcznie, 97 bajtów


@pytest.mark.parametrize("entries", [
    [],                                                        # pusta biblioteka
    [(1, container(True), b"x")],                              # kontener VS jako PS
    [(0, container(True)[:-1], b"x")],                         # suma rozmiarów != długość
    [(0, container(True), b"x"), (0, container(True), b"y")],  # duplikat kontenera
    [(2, container(False), b"x")],                             # zły stage
])
def test_serialize_rejects_invalid_entries(entries):
    with pytest.raises(ValueError):  # źródło: reguły z docstringu i komunikatów narzędzia
        mvp.serialize(entries)


@pytest.mark.skipif(not (ROOT / "build/vulkan-m2/emitter-build/XenosRecompCorpus.exe").is_file()
                    or not any((ROOT / "artifacts/shaders/raw").glob("*.vs.bin")),
                    reason="wymaga zbudowanego emitera Vulkan i kontenerów wyciągniętych z gry")
def test_compile_entry_real_shader(tmp_path):
    from compile_vulkan import Toolchain
    raw = sorted((ROOT / "artifacts/shaders/raw").glob("*.vs.bin"))[0]
    tools = Toolchain(ROOT / "build/vulkan-m2/emitter-build/XenosRecompCorpus.exe",
                      ROOT / "build/vulkan-m2/emitter-tree/src/XenosRecomp/shader_common.h",
                      ROOT / ".tools/dxc/bin/x64/dxc.exe")
    entry, error = mvp.compile_entry(raw, tmp_path, tools)
    assert error is None and entry[0] == 0 and entry[1] == raw.read_bytes()  # źródło: plik gry
