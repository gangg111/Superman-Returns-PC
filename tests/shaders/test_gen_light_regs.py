"""Tests for tools/shaders/gen_light_regs.py.
Input: shader catalog entries ({type, container_hash, reflection.constants[{name, reg}]})."""
import json
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
import gen_light_regs  # noqa: E402

CATALOG = os.path.join(ROOT, 'artifacts', 'shaders', 'catalog.json')


def entry(kind, h, consts):
    return {'type': kind, 'container_hash': h,
            'reflection': {'constants': [{'name': n, 'reg': r} for n, r in consts]}}


def test_collect_small_catalog():
    shaders = [
        entry('ps', 'B0', [('gKey1', 5), ('gLightPos', 3), ('gShadCol', 14), ('L0_tint', 2), ('time', 1)]),
        entry('ps', 'A0', [('TgKey1', 1), ('TgMid4', 4), ('fogColor', 7)]),       # water copies, fog
        entry('vs', 'C0', [('gLightPos', 230)]),
        entry('ps', 'D0', [('L0_KdL', 0), ('time', 1)]),                           # no lighting: skipped
        entry('ps', '19F2721057EAECAD', [('L0_tint', 0), ('gOverBright', 3)]),     # the sky backdrop
    ]
    # źródło: policzone ręcznie z wejścia i tabeli ról w docstringu narzędzia
    # (0 słońce, 1 światło kluczowe, 2 otoczenie, 3 mgła/niebo; L0_tint tylko w tle nieba)
    assert gen_light_regs.collect(shaders) == [
        (0xA0, 1, [(1, 1), (4, 2), (7, 3)]),
        (0xB0, 1, [(3, 0), (5, 1), (14, 2)]),
        (0xC0, 0, [(230, 0)]),
        (0x19F2721057EAECAD, 1, [(0, 3), (3, 2)]),
    ]


def test_exposure_role():
    shaders = [entry('ps', 'F0', [('exposure', 0), ('bloomIntensity', 1), ('toneMapContribution', 2)])]
    # źródło: tabela ról w docstringu (6 = exposure); pozostałe stałe tonemappingu bez roli
    assert gen_light_regs.collect(shaders) == [(0xF0, 1, [(0, 6)])]


def test_output_tint_shaders():
    def ps(h, consts, samplers=()):
        e = entry('ps', h, consts)
        e['reflection']['samplers'] = [{'name': s} for s in samplers]
        return e
    shaders = [
        ps('E1', [('TopFogColor', 2), ('gContinentLight', 0)]),               # sky dome
        ps('E2', [('gHorizFogCol_KFog', 0), ('gLightPos', 3)]),               # lit: no tint
        ps('E3', [('gHorizFogCol_KFog', 1)]),                                  # backdrop
        ps('E4', [('gWaterSpec', 5)]),                                         # water
        ps('E5', [('dBloom', 0)], ['flareSampler']),                           # the sun's flare
        ps('E7', [('tileScaleOffset', 0)], ['colorMap', 'depthMap']),          # soft particles
        ps('E8', [('tileScaleOffset', 0)], ['colorMap']),                      # no depth: not one
        entry('vs', 'E6', [('TopFogColor', 2)]),                               # vertex: no tint
    ]
    # źródło: reguła z docstringu narzędzia i z shader_recompiler.cpp (srDayNightTintShader),
    # wynik policzony ręcznie; rejestr odcienia c200, rola 4 niebo/tło, 5 flara, 7 woda
    assert gen_light_regs.collect(shaders) == [
        (0xE1, 1, [(0, 3), (2, 3), (200, 4)]),
        (0xE2, 1, [(0, 3), (3, 0)]),
        (0xE3, 1, [(1, 3), (200, 4)]),
        (0xE4, 1, [(200, 7)]),
        (0xE5, 1, [(200, 5)]),
        (0xE6, 0, [(2, 3)]),
        (0xE7, 1, [(200, 8)]),
    ]


def test_window_light_role():
    def ps(h, consts, samplers):
        e = entry('ps', h, consts)
        e['reflection']['samplers'] = [{'name': s} for s in samplers]
        return e
    shaders = [
        ps('A1', [('Kenv', 5), ('gKey1', 34), ('gBuildingTint', 28)], ['mat_map', 'col_map']),  # building
        ps('A2', [('L0_Kenv', 5)], ['L1_mat_map']),                        # layered: not one
        ps('A3', [('Kenv', 5)], ['col_map']),                              # no material map
        ps('A4', [('Kenv', 5)], ['mat_map', 'gFreeze_map']),               # a prop (boat): not one
    ]
    # źródło: reguła z docstringu narzędzia i z shader_recompiler.cpp (srWindowShader:
    # stałe Kenv i gBuildingTint, sampler mat_map); rejestr okien c201, rola 9; gKey1 rola 1
    assert gen_light_regs.collect(shaders) == [(0xA1, 1, [(34, 1), (201, 9)])]


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='no shader catalog')
def test_superman_shader_matches_its_hlsl():
    # źródło: definicje w artifacts/shaders/hlsl/3EF6A4A3C740ADB1.ps.hlsl odczytane niezależnie:
    # gKeyLightPosition c3, gLightPos c45, gSupesKey1-3 c48-50, gSupesMid4 c51, gSupesFil5-7 c52-54,
    # gSupes_rim_col c58; gSupesLightPos c46 (parametr, nie pozycja) nie może trafić do tabeli
    rows = gen_light_regs.collect(json.load(open(CATALOG))['shaders'])
    row = next(r for r in rows if r[0] == 0x3EF6A4A3C740ADB1)
    assert row[2] == [(3, 0), (45, 0), (48, 1), (49, 1), (50, 1), (51, 2), (52, 2), (53, 2), (54, 2), (58, 1)]


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='no shader catalog')
def test_building_shader_matches_its_hlsl():
    # źródło: definicje stałych w artifacts/shaders/hlsl/E6C740EAC16B70D4.ps.hlsl, odczytane
    # niezależnie (SR_LOAD_FLOAT4(Pixel, 16 * rejestr)): gLightPos c3, gKey1-3 c5-7, gMid4 c8,
    # gFil5-7 c9-11, gShadCol c14
    rows = gen_light_regs.collect(json.load(open(CATALOG))['shaders'])
    row = next(r for r in rows if r[0] == 0xE6C740EAC16B70D4)
    assert row[1] == 1
    assert row[2] == [(3, 0), (5, 1), (6, 1), (7, 1), (8, 2), (9, 2), (10, 2), (11, 2), (14, 2)]
