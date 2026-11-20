"""check_shader_constants: the float constant registers the HLSL reads and the comparison with the
runtime's sets. Every expected value is worked out by hand from the inputs."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'shaders'))
import check_shader_constants as check  # noqa: E402

DEFINES = """#define gLightPos SR_LOAD_FLOAT4(Vertex, 3680)
#define unused SR_LOAD_FLOAT4(Vertex, 48)
#define xform(INDEX) select((INDEX) < 156, SR_LOAD_FLOAT4(Vertex, (100 + min(INDEX, 155)) * 16), 0.0)
#define CONST_REL(INDEX) select((uint)(INDEX) < 256, SR_LOAD_FLOAT4(Vertex, min((uint)(INDEX), 255) * 16), 0.0)
float4 helper() { return c7; }
"""


def body(text):
    return DEFINES + 'void srVsMain(\n' + text + '\n}\n'


def test_named_array_unnamed_and_literal_registers():
    # gLightPos 3680/16 = 230; xform(2) = 100 + 2; c5 read; c254 is a literal, c7 is outside the body;
    # CONST_REL(210) is a fixed register; `unused` is defined but never read
    text = body('float4 c254 = asfloat(uint4(0,0,0,0));\n'
                'r0 = gLightPos + xform(2) + c5 + c254 + CONST_REL(210);  // c9 in a comment')
    assert check.hlsl_registers(text) == ({230, 102, 5, 210}, False)


def test_addressed_reads_are_relative():
    assert check.hlsl_registers(body('r0 = CONST_REL(100 + a0);'))[1] is True
    assert check.hlsl_registers(body('r0 = xform(aL);'))[1] is True
    assert check.hlsl_registers(body('r0 = SR_LOAD_FLOAT4(Pixel, 16 * i);'))[1] is True


def test_direct_load_in_the_body():
    # SR_LOAD_FLOAT4(Pixel, 3216) = register 3216 / 16 = 201
    assert check.hlsl_registers(body('r0 = SR_LOAD_FLOAT4(Pixel, 3216);')) == ({201}, False)


def test_parse_use():
    assert check.parse_use('all') is None
    # bits 0, 1 and 65: 2**65 + 3
    assert check.parse_use(format(2 ** 65 + 3, '064X')) == {0, 1, 65}


def run(tmp_path, listing, hlsl_files, capsys):
    hlsl = tmp_path / 'hlsl'
    hlsl.mkdir()
    for name, text in hlsl_files.items():
        (hlsl / name).write_text(text)
    (tmp_path / 'use.txt').write_text(listing)
    sys.argv = ['check_shader_constants.py', str(tmp_path / 'use.txt'), str(hlsl)]
    return check.main(), capsys.readouterr().out


def test_main_passes_a_superset_and_counts(tmp_path, capsys):
    # A reads 5 and 102; its set is {5, 102, 103} (3 kept); B is all (256 kept); 259 of 512
    sets = format(1 << 5 | 1 << 102 | 1 << 103, '064X')
    code, out = run(tmp_path, f'A.vs.bin {sets}\nB.ps.bin all\n',
                    {'A.vs.hlsl': body('r0 = c5 + xform(2);'), 'B.ps.hlsl': body('r0 = CONST_REL(a0);')}, capsys)
    assert code == 0
    assert 'checked 2, all 1, missing 0, relative but not all 0, registers kept 259 of 512' in out


def test_main_fails_a_missing_register_and_relative_without_all(tmp_path, capsys):
    only5 = format(1 << 5, '064X')
    code, out = run(tmp_path, f'A.vs.bin {only5}\nB.ps.bin {only5}\n',
                    {'A.vs.hlsl': body('r0 = c5 + xform(2);'), 'B.ps.hlsl': body('r0 = CONST_REL(a0) + c5;')},
                    capsys)
    assert code == 1
    assert 'MISSING A.vs.bin: [102]' in out
    assert 'RELATIVE B.ps.bin' in out
