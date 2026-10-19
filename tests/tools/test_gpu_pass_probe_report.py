"""tools/analysis/gpu_pass_probe_report.py on a tiny --sr_gpu_pass_probe_path CSV.

Expected input: CSV with header frame,index_base,index_length,vf0_addr,vf0_size,
c0_3_hash,c4_10_hash and optional rb_* state columns, one row per captured draw.
"""
import pytest

import gpu_pass_probe_report as probe

CSV = """\
frame,index_base,index_length,vf0_addr,vf0_size,c0_3_hash,c4_10_hash,rb_color_info
1,A000,36,V100,100,h1,m1,rc1
1,A000,36,V100,100,h2,m1,rc1
1,B000,12,V200,50,h3,m2,rc1
2,A000,36,V100,100,h1,m1,rc2
2,B000,12,V200,50,h1,m1,rc2
"""


def run(monkeypatch, tmp_path, capsys, text):
    path = tmp_path / "probe.csv"
    path.write_text(text, encoding="utf-8")
    monkeypatch.setattr("sys.argv", ["gpu_pass_probe_report.py", str(path)])
    probe.main()
    return capsys.readouterr().out.splitlines()


def test_report_on_builtin_csv(monkeypatch, tmp_path, capsys):
    lines = run(monkeypatch, tmp_path, capsys, CSV)
    # źródło: policzone ręcznie z 5 wierszy CSV powyżej
    assert lines == [
        "draws: 5 in 2 guest frame(s)",            # 5 wierszy, ramki {1,2}
        "vertex buffers: 2",                        # {V100, V200}
        "index ranges: 2",                          # {A000, B000}
        "geometry keys: 2",                         # GA=(A000,36,V100,100), GB=(B000,12,V200,50)
        "object constant sets (c0-c3): 3",          # {h1,h2,h3}
        "other constant sets (c4-c10): 2",          # {m1,m2}
        "rb_color_info: 2 state(s)",                # {rc1,rc2}; pozostałych rb_* brak w CSV
        "consecutive geometry runs: 4 (1 repeated calls)",  # GA GA | GB | GA | GB
        "material/geometry groups if reordering were valid: 3",  # (m1,GA),(m2,GB),(m1,GB)
        "top index ranges:",
        "  A000: 3 draws",                          # wiersze 1,2,4
        "  B000: 2 draws",                          # wiersze 3,5
    ]


def test_empty_csv_exits(monkeypatch, tmp_path, capsys):
    # źródło: sam nagłówek bez wierszy, narzędzie ma zakończyć z komunikatem
    with pytest.raises(SystemExit, match="no matching draws were captured"):
        run(monkeypatch, tmp_path, capsys, CSV.splitlines()[0] + "\n")
