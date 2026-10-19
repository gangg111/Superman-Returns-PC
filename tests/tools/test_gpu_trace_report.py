"""tools/analysis/gpu_trace_report.py on a tiny --sr_renderer=trace CSV.

Expected input: CSV with columns frame,kind,accepted,vs_hash,ps_hash,rb_mode,rb_surface,
rb_color,rb_depth; kind is draw/skipped/copy/swap, accepted is "1"/"0" for draws.
"""
import csv

import pytest

import gpu_trace_report as gtr

HEADER = "frame,kind,accepted,vs_hash,ps_hash,rb_mode,rb_surface,rb_color,rb_depth\n"
ROWS = """\
0,draw,1,aa,bb,1,s1,c1,d1
0,draw,1,AA,BB,1,s1,c1,d1
0,draw,0,cc,dd,1,s1,c1,d1
0,swap,,,,,,,
1,draw,1,cc,dd,2,s2,c2,d2
1,copy,,,,,,,
1,skipped,,,,,,,
"""


def test_analyze_on_builtin_trace(tmp_path):
    path = tmp_path / "trace.csv"
    path.write_text(HEADER + ROWS, encoding="utf-8")
    frames, passes, accepted = gtr.analyze(path)
    # źródło: policzone ręcznie z ROWS
    assert {f: dict(c) for f, c in frames.items()} == {
        0: {"draw": 3, "swap": 1}, 1: {"draw": 1, "copy": 1, "skipped": 1}}
    assert dict(accepted) == {"1": 3, "0": 1}  # źródło: 3 draw z accepted=1, 1 z accepted=0
    # źródło: ręcznie, aa/AA scalone przez wielkie litery hashy, odrzucony draw pominięty
    assert dict(passes) == {("AA", "BB", "1", "s1", "c1", "d1"): 2,
                            ("CC", "DD", "2", "s2", "c2", "d2"): 1}


def test_main_writes_passes_csv(monkeypatch, tmp_path, capsys):
    trace = tmp_path / "trace.csv"
    trace.write_text(HEADER + ROWS, encoding="utf-8")
    out = tmp_path / "o" / "passes.csv"
    monkeypatch.setattr("sys.argv", ["gpu_trace_report.py", str(trace), str(out)])
    gtr.main()
    with out.open(newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    # źródło: policzone ręcznie, jak w test_analyze_on_builtin_trace
    assert rows == [["draws", "vs_hash", "ps_hash", "rb_mode", "rb_surface", "rb_color", "rb_depth"],
                    ["2", "AA", "BB", "1", "s1", "c1", "d1"],
                    ["1", "CC", "DD", "2", "s2", "c2", "d2"]]
    text = capsys.readouterr().out
    assert "frames=2 draws=4 skipped=1 copies=1 swaps=1" in text  # źródło: ręcznie z ROWS
    assert "draws accepted=3 rejected=1" in text  # źródło: ręcznie z ROWS


def test_missing_column_raises(tmp_path):
    path = tmp_path / "old.csv"
    path.write_text("frame,kind,accepted\n0,draw,1\n", encoding="utf-8")
    # źródło: brak kolumn vs_hash..rb_depth, narzędzie ma odmówić
    with pytest.raises(ValueError, match="missing required columns"):
        gtr.analyze(path)
