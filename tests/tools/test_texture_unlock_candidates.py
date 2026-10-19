"""tools/analysis/texture_unlock_candidates.py on fake generated sources.

Expected input: a ReXGlue generated directory with superman_returns_recomp.*.cpp files whose
functions are written as "DEFINE_REX_FUNC(name) {" with one "// insn" comment per instruction
and calls as "ctx.lr = 0x...; sub_X(ctx, base);" (tail calls without ctx.lr).
"""
from pathlib import Path

import pytest

import texture_unlock_candidates as tuc

ROOT = Path(__file__).resolve().parents[2]


def func(name, ops, extra=""):
    body = "".join(f"\t// {op}\n" for op in ops)
    return f"DEFINE_REX_FUNC({name}) {{\n{body}{extra}}}\n\n"


def make_generated(gen, shape_override=None):
    gen.mkdir()
    shapes = {name: list(ops) for name, ops in tuc.EXPECTED.items()}
    shapes.update(shape_override or {})
    a = "".join(func(n, shapes.get(n, ["blr"]),
                     "\tsub_820F3C18(ctx, base);\n" if n == "sub_820FFCC8" else "")
                for n in list(tuc.TARGETS)[:6])
    b = "".join(func(n, shapes.get(n, ["blr"])) for n in list(tuc.TARGETS)[6:])
    b += func("sub_82000000", ["mflr r12", "bl 0x820f3c18", "b 0x820f4000"],
              "\tctx.lr = 0x82000008;\n\tsub_820F3C18(ctx, base);\n\tsub_820F4000(ctx, base);\n")
    (gen / "superman_returns_recomp.0.cpp").write_text(a, encoding="utf-8")
    (gen / "superman_returns_recomp.1.cpp").write_text(b, encoding="utf-8")


def test_collect_on_builtin_sources(tmp_path):
    make_generated(tmp_path / "gen")
    report = tuc.collect(tmp_path / "gen")
    # źródło: ręcznie, plik .0 zawiera 6 pierwszych celów, .1 pozostałe 5 i wywołującego
    assert len(report) == 11
    assert report["sub_820FFCC8"]["source"] == "superman_returns_recomp.0.cpp"
    assert report["sub_82480D80"]["source"] == "superman_returns_recomp.1.cpp"
    # źródło: lista instrukcji wpisana w teście (komentarze // w ciele funkcji)
    assert report["sub_820F3C18"]["instructions"] == ["blr"]
    # źródło: ręcznie, sub_82000000 woła przez ctx.lr=0x82000008, sub_820FFCC8 skacze bez lr
    assert report["sub_820F3C18"]["callers"] == [
        {"function": "sub_820FFCC8", "source": "superman_returns_recomp.0.cpp",
         "return_addresses": [], "tail_call": True},
        {"function": "sub_82000000", "source": "superman_returns_recomp.1.cpp",
         "return_addresses": ["0x82000008"], "tail_call": False},
    ]
    # źródło: ręcznie, wywołanie sub_820F4000 w ciele z ctx.lr tylko przed pierwszym call;
    # regex łączy lr wyłącznie z bezpośrednio następnym wywołaniem, więc tu brak adresu
    assert report["sub_820F4000"]["callers"] == [
        {"function": "sub_82000000", "source": "superman_returns_recomp.1.cpp",
         "return_addresses": [], "tail_call": True}]
    # źródło: `printf 'blr' | sha256sum` (coreutils, poza Pythonem)
    assert report["sub_820F3C18"]["instruction_comments_sha256"] == (
        "b5589f1b83f51aec96e4023acbb9921f0a9b85d82753e487d4e6d6faaae75f52")


def test_changed_wrapper_shape_raises(tmp_path):
    make_generated(tmp_path / "gen", {"sub_824707B0": ["lwz r11,0(r3)", "bctr "]})
    # źródło: kształt różny od EXPECTED, narzędzie ma odmówić
    with pytest.raises(ValueError, match="Wrapper shape changed: sub_824707B0"):
        tuc.collect(tmp_path / "gen")


def test_missing_target_raises(tmp_path):
    gen = tmp_path / "gen"
    gen.mkdir()
    (gen / "superman_returns_recomp.0.cpp").write_text(func("sub_82000000", ["blr"]))
    # źródło: żaden z 11 celów nie istnieje, pierwszy w kolejności TARGETS to sub_820F3C18
    with pytest.raises(ValueError, match="Missing generated entry: sub_820F3C18"):
        tuc.collect(gen)


@pytest.mark.skipif(not list((ROOT / "port/generated/default").glob("superman_returns_recomp.*.cpp")),
                    reason="brak wygenerowanych źródeł ReXGlue w port/generated/default")
def test_real_generated_sources():
    report = tuc.collect(ROOT / "port/generated/default")
    assert set(report) >= set(tuc.TARGETS)  # źródło: każdy cel musi zostać znaleziony
