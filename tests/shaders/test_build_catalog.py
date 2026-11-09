"""build_catalog.shader_profile: wejście to typ shadera ("vs"/"ps") i tekst HLSL z XenosRecomp.
Shadery śledzące promienie (RayQuery) muszą dostać model 6.5, pozostałe 6.0."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "shaders"))
import build_catalog as bc  # noqa: E402


def test_shadow_receiver_pixel_shader_gets_6_5():
    # źródło: reguła z docs/cienie-rt-projekt.md (odbiorcy cienia = stała shadowMapOffsetScales)
    assert bc.shader_profile("ps", "#define shadowMapOffsetScales SR_LOAD(Pixel, 0)") == "ps_6_5"


def test_water_pixel_shader_gets_6_5():
    # źródło: shadery wody 2EFEE77C / F3E35AF5 mają stałą gWaterSpec (odczyt HLSL 2026-11-04)
    assert bc.shader_profile("ps", "#define gWaterSpec SR_LOAD_FLOAT4(Pixel, 272)") == "ps_6_5"


def test_other_shaders_stay_on_6_0():
    # źródło: domyślne profile katalogu (vs_6_0 / ps_6_0)
    assert bc.shader_profile("ps", "#define darkColor SR_LOAD_FLOAT4(Pixel, 0)") == "ps_6_0"
    # stała wody w shaderze wierzchołków niczego nie zmienia: RayQuery tylko w pikselach
    assert bc.shader_profile("vs", "#define gWaterSpec 0") == "vs_6_0"
