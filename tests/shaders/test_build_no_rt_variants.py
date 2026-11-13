"""build_no_rt_variants: wejście to kontener DXIL (DXBC z częścią 'DXIL') albo
standalone HLSL z korpusu. program_version czyta słowo wersji programu, compile_no_rt
buduje wariant ps_6_0 bez RayQuery; pack_shaders zapisuje go jako etap 2."""
import struct
import subprocess
import sys
from pathlib import Path

import pytest

SHADERS = Path(__file__).resolve().parents[2] / "tools" / "shaders"
sys.path.insert(0, str(SHADERS))
import build_no_rt_variants as nrt  # noqa: E402
from srpaths import DXC  # noqa: E402


def container(parts):
    """Kontener DXBC złożony ręcznie: nagłówek 32 B, tablica przesunięć, części
    {fourcc, u32 rozmiar, dane} (układ z dokumentacji DXIL, DxilContainer.h)."""
    head = 32 + 4 * len(parts)
    offsets, body = [], b""
    for fourcc, data in parts:
        offsets.append(head + len(body))
        body += fourcc + struct.pack("<I", len(data)) + data
    total = head + len(body)
    return (b"DXBC" + bytes(16) + struct.pack("<III", 1, total, len(parts)) +
            struct.pack(f"<{len(parts)}I", *offsets) + body)


def test_program_version_of_hand_built_container():
    # źródło: policzone ręcznie, piksel 6.5 = (typ 0 << 16) | (6 << 4) | 5 = 0x65
    blob = container([(b"SFI0", bytes(8)), (b"DXIL", struct.pack("<I", 0x65) + bytes(12))])
    assert nrt.program_version(blob) == 0x65
    # źródło: wierzchołki 6.0 = (1 << 16) | 0x60
    blob = container([(b"DXIL", struct.pack("<I", 0x10060) + bytes(12))])
    assert nrt.program_version(blob) == 0x10060


def test_program_version_rejects_other_data():
    assert nrt.program_version(b"not a container") is None
    assert nrt.program_version(container([(b"SFI0", bytes(8))])) is None


def test_pack_stores_no_rt_variant_as_stage_2(tmp_path):
    h = "00000000000000AB"
    for name, data in ((f"{h}.ps.dxil", b"PS65"), (f"{h}.ps.nort.dxil", b"PS60"),
                       (f"{h}.vs.dxil", b"VS"), (f"{h}.ps.lib.dxil", b"LIB")):
        (tmp_path / name).write_bytes(data)
    out = tmp_path / "x.pak"
    subprocess.run([sys.executable, str(SHADERS / "pack_shaders.py"), str(tmp_path), str(out)],
                   check=True, capture_output=True)
    blob = out.read_bytes()
    magic, version, count = struct.unpack_from("<4sII", blob)
    # źródło: trzy wpisy (biblioteka .lib nie wchodzi), kolejność (hash, etap) jak w shader_pack.h
    assert (magic, count) == (b"CNSH", 3)
    rows = [struct.unpack_from("<QIII", blob, 12 + 20 * i) for i in range(count)]
    assert [(r[0], r[1]) for r in rows] == [(0xAB, 0), (0xAB, 1), (0xAB, 2)]
    assert blob[rows[2][2]:rows[2][2] + rows[2][3]] == b"PS60"


HLSL = """
#if __SHADER_TARGET_MAJOR > 6 || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 5)
RaytracingAccelerationStructure tlas : register(t0);
float vis(float3 p) {
  RayDesc r; r.Origin = p; r.TMin = 0; r.Direction = float3(0, 1, 0); r.TMax = 10;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
  q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, r); q.Proceed();
  return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0 : 1;
}
#else
float vis(float3 p) { return 1; }
#endif
float4 main(float4 pos : SV_Position) : SV_Target { return vis(pos.xyz).xxxx; }
"""


@pytest.mark.skipif(not DXC.exists(), reason="brak dxc w .tools/dxc (wymaga środowiska deweloperskiego)")
def test_compile_no_rt_gives_6_0_without_raytracing_flag(tmp_path):
    src = tmp_path / "s.hlsl"
    src.write_text(HLSL)
    out = tmp_path / "s.nort.dxil"
    rc, so, se = nrt.compile_no_rt(DXC, src, out)
    assert rc == 0, se + so
    blob = out.read_bytes()
    # źródło: dxc -T ps_6_0 daje wersję programu 0x60
    assert nrt.program_version(blob) == 0x60
    # źródło: flaga SFI0 0x100000 = Raytracing_Tier_1_1 (DxilShaderFlags), nie może jej być
    off = blob.find(b"SFI0")
    flags = struct.unpack_from("<Q", blob, off + 8)[0] if off >= 0 else 0
    assert not flags & 0x100000
