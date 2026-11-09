"""build_pipeline_db.py: wejście to pliki rekordów potoków renderera (pipeline_cache.cpp):
nagłówek 16 B ("CNPS", u32 wersja, u32 rozmiar rekordu, u32 zarezerwowane), potem rekordy
o stałym rozmiarze, zaczynające się od u64 LE klucza, hasha VS i hasha PS (0 = bez PS).
Katalog DXIL: pliki <HASH>.vs.dxil / <HASH>.ps.dxil. Wszystko budujemy ręcznie w teście."""
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "shaders"))
import build_pipeline_db as db  # noqa: E402

RECORD = 32  # rozmiar rekordu w teście: 3 x u64 + 8 bajtów reszty opisu


def record(key, vs, ps, tail=b"\xAA" * 8):
    return struct.pack("<QQQ", key, vs, ps) + tail


def write_db(path, records, version=1, size=RECORD, torn=b""):
    path.write_bytes(b"CNPS" + struct.pack("<III", version, size, 0) + b"".join(records) + torn)
    return path


@pytest.fixture
def dxil(tmp_path):
    d = tmp_path / "dxil"
    d.mkdir()
    for name in ("00000000000000A1.vs.dxil", "00000000000000B1.ps.dxil",
                 "00000000000000A2.vs.dxil", "00000000000000A2.vs.lib.dxil"):
        (d / name).write_bytes(b"x")
    return d


def test_merge_dedupes_and_drops_missing_shaders(tmp_path, dxil):
    a = write_db(tmp_path / "a.bin", [
        record(1, 0xA1, 0xB1),      # oba shadery są
        record(2, 0xA2, 0),         # bez PS, VS jest
        record(3, 0xA1, 0xB9),      # brak B9.ps
        record(1, 0xA1, 0xB1, b"\xBB" * 8),  # duplikat klucza 1
    ], torn=b"\x01\x02\x03")        # urwany ostatni rekord
    b = write_db(tmp_path / "b.bin", [
        record(4, 0xC3, 0),         # brak C3.vs
        record(5, 0xA2, 0xB1),
        record(2, 0xA2, 0),         # duplikat z a.bin
    ])
    out = tmp_path / "out.bin"
    assert db.main(["--dxil", str(dxil), "--out", str(out), str(a), str(b)]) == 0
    data = out.read_bytes()
    # źródło: policzone ręcznie - nagłówek bez zmian, zostają klucze 1 (pierwsze wystąpienie,
    # ogon AA), 2 i 5, w kolejności pierwszego pojawienia się
    assert data[:16] == b"CNPS" + struct.pack("<III", 1, RECORD, 0)
    body = data[16:]
    assert len(body) == 3 * RECORD
    assert body == record(1, 0xA1, 0xB1) + record(2, 0xA2, 0) + record(5, 0xA2, 0xB1)
    _, _, _, stats = db.merge([str(a), str(b)], db.available_shaders(str(dxil)))
    # źródło: policzone ręcznie - 4 + 3 przeczytane, duplikaty: klucz 1 i 2, bez shaderów: 3 i 4
    assert stats == {"read": 7, "duplicates": 2, "missing_shaders": 2}


def test_lowercase_hash_files_match(tmp_path):
    d = tmp_path / "dx"
    d.mkdir()
    (d / "00000000000000ab.vs.dxil").write_bytes(b"x")
    a = write_db(tmp_path / "a.bin", [record(9, 0xAB, 0)])
    _, _, kept, _ = db.merge([str(a)], db.available_shaders(str(d)))
    # źródło: policzone ręcznie - nazwa pliku małymi literami to ten sam hash 0xAB
    assert len(kept) == 1


def test_rejects_mixed_versions(tmp_path, dxil):
    a = write_db(tmp_path / "a.bin", [record(1, 0xA1, 0)], version=1)
    b = write_db(tmp_path / "b.bin", [record(2, 0xA1, 0)], version=2)
    with pytest.raises(ValueError):
        db.merge([str(a), str(b)], db.available_shaders(str(dxil)))


def test_rejects_not_a_pipeline_file(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(b"XXXX" + bytes(12))
    with pytest.raises(ValueError):
        db.read_records(str(p))
