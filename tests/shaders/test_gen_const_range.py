"""gen_const_range.registers: wejście to tekst HLSL przetłumaczonego shadera (XenosRecomp), wynik to
liczba rejestrów stałych float, które shader może przeczytać (0..n-1), albo 256 przy indeksie
liczonym w czasie działania."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "shaders"))
import gen_const_range as g  # noqa: E402

DEF = "#define CONST_REL(INDEX) select((uint)(INDEX) < 256, SR_LOAD_FLOAT4(Pixel, min((uint)(INDEX), 255) * 16), 0.0)\n"


def test_literal_indices():
    src = DEF + "int a0 = 0;\nr0 = CONST_REL(3);\nr1 = CONST_REL(17);\n"
    # źródło: policzone ręcznie, najwyższy rejestr 17 -> rejestry 0..17 = 18
    assert g.registers(src) == 18


def test_named_constant_macro_uses():
    src = DEF + ("#define gWorldXform(INDEX) CONST_REL(40 + (INDEX))\n"
                 "a = gWorldXform(0); b = gWorldXform(3);\n")
    # źródło: policzone ręcznie, 40 + 3 = 43 -> 44 rejestry
    assert g.registers(src) == 44


def test_a0_never_computed_counts_as_zero_offset():
    src = DEF + "int a0 = 0;\n{ int aluAddress = a0; r = CONST_REL(229 + aluAddress); }\n"
    # źródło: a0 tylko zainicjowane zerem, więc czytany jest rejestr 229 -> 230
    assert g.registers(src) == 230


def test_a0_from_mova_reads_whole_bank():
    src = DEF + "int a0 = 0;\na0 = (int)clamp(floor(r0.x + 0.5), -256.0, 255.0);\nr = CONST_REL(229 + a0);\n"
    # źródło: indeks z rejestru w czasie działania -> cały bank (256)
    assert g.registers(src) == 256


def test_named_constant_with_register_index_reads_whole_bank():
    src = DEF + "#define bones(INDEX) CONST_REL(10 + (INDEX))\na = bones(r1.x);\n"
    # źródło: indeks nieliteralny -> cały bank (256)
    assert g.registers(src) == 256


def test_no_constants():
    # źródło: shader bez odczytu stałych -> 0 rejestrów
    assert g.registers(DEF + "oC0 = float4(1, 0, 0, 1);\n") == 0
