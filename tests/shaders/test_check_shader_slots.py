"""check_shader_slots: the HLSL's texture slots and the comparison with the runtime masks.
Every expected value is worked out by hand from the inputs."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'shaders'))
import check_shader_slots as check  # noqa: E402

HLSL = """#define L2_col_map_Texture2DDescriptorIndex SR_SHARED_UINT(8)
#define L2_col_map_Texture3DDescriptorIndex SR_SHARED_UINT(72)
#define L2_col_map_SamplerDescriptorIndex SR_SHARED_UINT(200)
#define L2_mat_map_Texture2DDescriptorIndex SR_SHARED_UINT(16)
#define s31_Texture2DDescriptorIndex SR_SHARED_UINT(124)
"""


def test_hlsl_slots_take_the_2d_index_offsets_only():
    # 2D offsets 8, 16, 124 -> slots 2, 4, 31; the 3D (72) and sampler (200) defines are not slots
    assert check.hlsl_slots(HLSL) == (1 << 2) | (1 << 4) | (1 << 31)


def test_hlsl_without_textures_is_zero():
    assert check.hlsl_slots('float4 main() { return 0; }') == 0


def run(tmp_path, masks, capsys):
    hlsl = tmp_path / 'hlsl'
    hlsl.mkdir()
    (hlsl / 'A.ps.hlsl').write_text(HLSL)
    (hlsl / 'B.vs.hlsl').write_text('')
    listing = tmp_path / 'masks.txt'
    listing.write_text(masks)
    sys.argv = ['check_shader_slots.py', str(listing), str(hlsl)]
    code = check.main()
    return code, capsys.readouterr().out


def test_an_exact_mask_passes(tmp_path, capsys):
    # A needs 0x80000014 (slots 2, 4, 31), B none
    code, out = run(tmp_path, 'A.ps.bin 80000014\nB.vs.bin 00000000\n', capsys)
    assert code == 0
    assert 'checked 2, unknown (all slots) 0, missing 0, extra slots 0' in out


def test_a_missing_slot_fails(tmp_path, capsys):
    # slot 4 (0x10) left out: 0x80000004 lacks 0x00000010
    code, out = run(tmp_path, 'A.ps.bin 80000004\nB.vs.bin 00000000\n', capsys)
    assert code == 1
    assert 'MISSING A.ps: mask 80000004 lacks 00000010' in out


def test_extra_slots_are_counted_and_all_slots_is_unknown(tmp_path, capsys):
    # A gets slot 0 too (one extra), B is "all slots" (unknown, not compared)
    code, out = run(tmp_path, 'A.ps.bin 80000015\nB.vs.bin FFFFFFFF\n', capsys)
    assert code == 0
    assert 'checked 2, unknown (all slots) 1, missing 0, extra slots 1' in out
