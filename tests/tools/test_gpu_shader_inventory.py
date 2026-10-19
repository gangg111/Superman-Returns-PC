"""tools/analysis/gpu_shader_inventory.py on a synthetic --dump_shaders directory.

Expected input: a directory tree holding shader_<16 hex>.ucode.<vert|frag> disassembly,
optional shader_<hash>.ucode.bin.<stage> microcode and host variants shader_<hash>_*.<stage>.
"""
import csv

import gpu_shader_inventory as inv


def make_dump(root):
    (root / "sub").mkdir(parents=True)
    (root / "shader_00000000000000AB.ucode.vert").write_bytes(b"x" * 10)
    (root / "shader_00000000000000AB.ucode.bin.vert").write_bytes(b"x" * 32)
    (root / "shader_00000000000000AB_spirv.vert").write_bytes(b"s")
    (root / "shader_00000000000000AB_dxbc.vert").write_bytes(b"d")
    (root / "shader_00000000000000AB_spirv.frag").write_bytes(b"f")  # other stage, not counted
    (root / "shader_nothex.ucode.vert").write_bytes(b"bad")          # bad name, skipped
    (root / "sub" / "shader_00000000000000cd.ucode.frag").write_bytes(b"y" * 7)


EXPECTED = [
    # źródło: policzone ręcznie z plików w make_dump; sortowanie (stage, hash): pixel < vertex
    {"stage": "pixel", "hash": "00000000000000CD", "ucode_bytes": 0,  # brak .ucode.bin.frag
     "disassembly_bytes": 7, "host_variants": 0},
    {"stage": "vertex", "hash": "00000000000000AB", "ucode_bytes": 32,
     "disassembly_bytes": 10, "host_variants": 2},  # _spirv.vert i _dxbc.vert
]


def test_inventory_on_builtin_dump(tmp_path):
    make_dump(tmp_path / "dump")
    assert inv.inventory(tmp_path / "dump") == EXPECTED


def test_main_writes_csv(monkeypatch, tmp_path, capsys):
    make_dump(tmp_path / "dump")
    out_csv = tmp_path / "out" / "inv.csv"
    monkeypatch.setattr("sys.argv", ["gpu_shader_inventory.py", str(tmp_path / "dump"), str(out_csv)])
    inv.main()
    with out_csv.open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    # źródło: EXPECTED powyżej (ręcznie), CSV przechowuje liczby jako tekst
    assert rows == [{k: str(v) for k, v in row.items()} for row in EXPECTED]
    # źródło: policzone ręcznie, 1 vertex + 1 pixel
    assert capsys.readouterr().out.startswith("2 shaders (1 vertex, 1 pixel) -> ")
