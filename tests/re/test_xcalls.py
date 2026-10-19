"""tools/re/xcalls.py on a tiny fake generated-code folder.

Input the tool expects: superman_returns_recomp.*.cpp files with DEFINE_REX_FUNC(sub_X)
headers and calls "sub_XXXXXXXX(ctx, base)"; arguments: folder, from_lo, from_hi,
to_lo, to_hi (hex, half-open ranges)."""
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "re" / "xcalls.py"
GENERATED = Path(__file__).resolve().parents[2] / "port" / "generated" / "default"


def func(addr, *callees):
    body = "".join(f"\tsub_{c:08X}(ctx, base);\n" for c in callees)
    return f"DEFINE_REX_FUNC(sub_{addr:08X}) {{\n{body}}}\n\n"


def run(gen, *ranges):
    proc = subprocess.run([sys.executable, str(TOOL), str(gen), *ranges], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_calls_between_ranges(tmp_path):
    text = func(0x82000000, 0x82001000, 0x82001004, 0x82003000)       # 0x82003000: outside "to"
    text += "".join(func(0x82000000 + 0x10 * i, 0x82001000) for i in range(1, 10))
    text += func(0x82001000, 0x82001004)                               # caller outside "from" (hi is exclusive)
    (tmp_path / "superman_returns_recomp.0.cpp").write_text(text, encoding="utf-8")
    out = run(tmp_path, "82000000", "82001000", "82001000", "82002000")
    # źródło: wypisane ręcznie: 10 wołających sub_82001000 (pierwsze 8 + "(+2)"), 1 woła sub_82001004
    assert out == ("sub_82001000 <- 82000000 82000010 82000020 82000030 82000040 82000050 82000060 82000070 (+2)\n"
                   "sub_82001004 <- 82000000\n")


def test_no_calls_in_range(tmp_path):
    (tmp_path / "superman_returns_recomp.0.cpp").write_text(func(0x82000000, 0x82005000), encoding="utf-8")
    assert run(tmp_path, "82000000", "82001000", "82001000", "82002000") == ""  # źródło: callee poza zakresem


@pytest.mark.skipif(not list(GENERATED.glob("superman_returns_recomp.*.cpp")),
                    reason="brak wygenerowanego kodu port/generated/default")
def test_real_generated_code_runs():
    out = run(GENERATED, "82000000", "84000000", "82000000", "84000000")
    assert out.startswith("sub_82")  # źródło: obraz gry zaczyna się od 0x82000000
