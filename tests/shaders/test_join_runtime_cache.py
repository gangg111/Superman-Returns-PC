"""join_runtime_cache.py: wejście to (a) pliki .xsh ReXGlue: u32 LE 'XESH', u32 BE wersja
0x20201219, potem rekordy u64 LE hash xxh3 mikrokodu, u32 LE (liczba dwordów | typ<<31),
mikrokod big-endian; (b) korpus z manifest.json + raw/<hash>.<typ>.bin.
Wyjście: runtime_join.json z dopasowaniem exact/vfetch/fuzzy/none. Wszystko budujemy ręcznie."""
import json
import struct
import sys
from pathlib import Path

import pytest
import xxhash

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "shaders"))
import join_runtime_cache as jrc  # noqa: E402


def be(*words):
    return struct.pack(f">{len(words)}I", *words)


def xsh(entries, version=0x20201219):
    """entries: (typ 'vs'/'ps', mikrokod, hash albo None = poprawny xxh3)."""
    data = struct.pack("<I", 0x48534558) + struct.pack(">I", version)
    for kind, ucode, h in entries:
        h = xxhash.xxh3_64_intdigest(ucode) if h is None else h
        data += struct.pack("<QI", h, len(ucode) // 4 | (0x80000000 if kind == "ps" else 0)) + ucode
    return data


# Mikrokod z jedną instrukcją CF "ExecEnd" (opcode 2) wskazującą slot 1 (dwordy 3..5),
# count 1, sequence bit 0 = fetch. CF 48-bit: adres bity 0-11, count 12-14,
# sequence 16-27, opcode 44-47. Policzone ręcznie:
# cf = 1 | 1<<12 | 1<<16 | 2<<44 = 0x0000_2000_0001_1001
# w0 = 0x00011001, w1 = 0x00002000 (górna połowa cf) | (drugie CF = 0) << 16, w2 = 0
CF_FETCH = (0x00011001, 0x00002000, 0x00000000)
CF_ALU = (0x00001001, 0x00002000, 0x00000000)  # sequence = 0: slot ALU, nie fetch


def test_read_xsh_parses_type_count_and_hash(tmp_path):
    vs_ucode, ps_ucode = be(1, 2, 3), be(0xDEADBEEF)
    path = tmp_path / "454107ED.xsh"
    path.write_bytes(xsh([("vs", vs_ucode, None), ("ps", ps_ucode, None)]))
    out = jrc.read_xsh(path)
    # źródło: rekordy zapisane ręcznie wyżej (3 dwordy VS, 1 dword PS), hash z biblioteki xxhash
    assert [(e["type"], e["dwords"], e["ucode"]) for e in out] == [("vs", 3, vs_ucode), ("ps", 1, ps_ucode)]
    assert out[1]["ucode_data_hash"] == f"{xxhash.xxh3_64_intdigest(ps_ucode):016X}"  # źródło: xxhash


def test_read_xsh_rejects_bad_magic_and_stops_on_bad_hash(tmp_path):
    bad = tmp_path / "bad.xsh"
    bad.write_bytes(b"NOPE" + bytes(20))
    assert jrc.read_xsh(bad) == []  # źródło: magia inna niż 'XESH', policzone ręcznie
    broken = tmp_path / "broken.xsh"
    broken.write_bytes(xsh([("ps", be(7), None), ("ps", be(8), 0x1234), ("ps", be(9), None)]))
    # źródło: drugi rekord ma ręcznie wpisany zły hash, więc zostaje tylko pierwszy
    assert [e["ucode"] for e in jrc.read_xsh(broken)] == [be(7)]


def test_normalize_vfetch_blanks_only_rewritten_fetch_bits():
    fetch = (0xFFFFFFE0, 0x12345678, 0x9ABCDEF0)  # opcode (bity 0-4) = 0: vertex fetch
    assert jrc.normalize_vfetch(be(*CF_FETCH, *fetch)) == be(*CF_FETCH, 0x0007FFE0, 0, 0)
    # źródło: policzone ręcznie, 0xFFFFFFE0 & 0x7FFFF = 0x7FFE0, słowa 1 i 2 zerowane
    alu = be(*CF_ALU, *fetch)
    assert jrc.normalize_vfetch(alu) == alu  # źródło: slot ALU nie jest ruszany (sequence = 0)
    tfetch = be(*CF_FETCH, 0xFFFFFFE1, 1, 2)
    assert jrc.normalize_vfetch(tfetch) == tfetch  # źródło: opcode 1 to nie vfetch, bez zmian


def test_main_classifies_exact_vfetch_fuzzy_none(tmp_path, monkeypatch):
    corpus = tmp_path / "corpus"
    (corpus / "raw").mkdir(parents=True)
    vs_corpus = be(*CF_FETCH, 0xFFFFFFE0, 0x11111111, 0x22222222)
    ps_corpus = be(10, 20, 30, 40)
    shaders = []
    for chash, kind, ucode in (("00000000000000A1", "vs", vs_corpus), ("00000000000000B2", "ps", ps_corpus)):
        prefix = b"HDR!" * 3  # mikrokod leży w kontenerze od offsetu 12
        (corpus / "raw" / f"{chash}.{kind}.bin").write_bytes(prefix + ucode + b"tail")
        shaders.append({"container_hash": chash, "type": kind, "ucode_offset": 12, "ucode_size": len(ucode),
                        "ucode_hash": f"{xxhash.xxh3_64_intdigest(ucode):016X}"})
    (corpus / "manifest.json").write_text(json.dumps({"shaders": shaders}))
    runtime_vs = be(*CF_FETCH, 0xFFFFFFE0 ^ 0x01F00000, 0x99999999, 0x0)  # inny const_index/stride
    fuzzy_ps = be(10, 20, 30, 41)       # 1 z 4 dwordów różny: 1 <= max(1, 4 // 4)
    none_ps = be(10, 21, 31, 41)        # 3 z 4 różne
    path = tmp_path / "454107ED.xsh"
    path.write_bytes(xsh([("ps", ps_corpus, None), ("vs", runtime_vs, None), ("ps", fuzzy_ps, None), ("ps", none_ps, None)]))
    monkeypatch.setattr(sys, "argv", ["join_runtime_cache.py", "--corpus", str(corpus), str(path)])
    jrc.main()
    out = json.loads((corpus / "runtime_join.json").read_text())
    by_hash = {r["ucode_data_hash"]: r for r in out["runtime_shaders"]}

    def rec(ucode):
        return by_hash[f"{xxhash.xxh3_64_intdigest(ucode):016X}"]

    # źródło: klasyfikacja policzona ręcznie z definicji w docstringu narzędzia
    assert (rec(ps_corpus)["match"], rec(ps_corpus)["container_hashes"]) == ("exact", ["00000000000000B2"])
    assert (rec(runtime_vs)["match"], rec(runtime_vs)["container_hashes"]) == ("vfetch", ["00000000000000A1"])
    assert (rec(fuzzy_ps)["match"], rec(fuzzy_ps)["differing_dwords"]) == ("fuzzy", 1)
    assert (rec(none_ps)["match"], rec(none_ps)["container_hashes"]) == ("none", [])
    # źródło: zliczone ręcznie z czterech rekordów powyżej
    assert out["summary"] == {"ps_exact": 1, "vs_vfetch": 1, "ps_fuzzy": 1, "ps_none": 1}
