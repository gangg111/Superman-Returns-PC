"""tools/re/dump_asm.py on a tiny fake generated-code folder (no generated port code needed).

Input the tool expects: a folder with superman_returns_recomp.*.cpp files where each
function starts with a line DEFINE_REX_FUNC(sub_XXXXXXXX), the PPC instructions are
comment lines "\\t// <asm>", labels are "loc_XXXXXXXX:" at column 0 and calls look like
"sub_XXXXXXXX(ctx, base)" or "__imp__Name(ctx, base)"."""
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "re" / "dump_asm.py"
GENERATED = Path(__file__).resolve().parents[2] / "port" / "generated" / "default"

CPP0 = """DEFINE_REX_FUNC(sub_82000000) {
\t// li r3,0
loc_82000004:
\t// bl 0x82001000
\tsub_82001000(ctx, base);
\t__imp__XamFoo(ctx, base);
\tctx.r3.u64 = 0;
}

DEFINE_REX_FUNC(sub_82001000) {
\t// blr
\tsub_82009999(ctx, base);
}
"""
CPP1 = """DEFINE_REX_FUNC(sub_82002000) {
\t// nop
}
"""


def run(gen, *funcs):
    proc = subprocess.run([sys.executable, str(TOOL), str(gen), *funcs], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_dumps_only_requested_functions(tmp_path):
    (tmp_path / "superman_returns_recomp.0.cpp").write_text(CPP0, encoding="utf-8")
    (tmp_path / "superman_returns_recomp.1.cpp").write_text(CPP1, encoding="utf-8")
    (tmp_path / "other.cpp").write_text("DEFINE_REX_FUNC(sub_82002000) {\n\t// wrong file\n}\n")
    out = run(tmp_path, "sub_82000000", "sub_82002000")
    # źródło: wypisane ręcznie z wejścia CPP0/CPP1 (komentarz bez "\t// ", etykieta, wywołania jako "->")
    assert out == ("\n=== sub_82000000\n"
                   "   li r3,0\n"
                   "loc_82000004:\n"
                   "   bl 0x82001000\n"
                   "      -> sub_82001000\n"
                   "      -> __imp__XamFoo\n"
                   "\n=== sub_82002000\n"
                   "   nop\n")


def test_unknown_function_prints_nothing(tmp_path):
    (tmp_path / "superman_returns_recomp.0.cpp").write_text(CPP0, encoding="utf-8")
    assert run(tmp_path, "sub_8200ABCD") == ""  # źródło: brak takiej funkcji w wejściu


@pytest.mark.skipif(not list(GENERATED.glob("superman_returns_recomp.*.cpp")),
                    reason="brak wygenerowanego kodu port/generated/default")
def test_real_generated_code_has_header_line():
    first = next(GENERATED.glob("superman_returns_recomp.*.cpp"))
    import re
    m = re.search(r"DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)", first.read_text(encoding="utf-8", errors="replace"))
    assert m
    out = run(GENERATED, m.group(1))
    assert out.startswith(f"\n=== {m.group(1)}\n")  # źródło: format nagłówka z kodu wejściowego
