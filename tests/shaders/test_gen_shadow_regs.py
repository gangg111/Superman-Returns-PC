"""Tests for tools/shaders/gen_shadow_regs.py.
Input: shader catalog entries ({type, container_hash, reflection.constants[{name, reg}]})."""
import json
import os
import re
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
import gen_shadow_regs  # noqa: E402

CATALOG = os.path.join(ROOT, 'artifacts', 'shaders', 'catalog.json')


def entry(kind, h, consts):
    return {'type': kind, 'container_hash': h,
            'reflection': {'constants': [{'name': n, 'reg': r} for n, r in consts]}}


def test_collect_small_catalog():
    shaders = [
        entry('vs', 'B0', [('shadowMapV', 6), ('shadowMapU', 5), ('shadowMapZ', 7), ('gWorldXform', 0)]),
        entry('vs', 'A0', [('shadowMapU', 19), ('shadowMapV', 20), ('shadowMapZ', 21)]),
        entry('vs', 'C0', [('shadowMapU', 1), ('shadowMapV', 2)]),          # incomplete: skipped
        entry('ps', 'D0', [('shadowMapU', 1), ('shadowMapV', 2), ('shadowMapZ', 3)]),  # pixel: skipped
    ]
    # źródło: policzone ręcznie z wejścia powyżej (sortowanie po hashu, kolejność U, V, Z)
    assert gen_shadow_regs.collect(shaders) == [(0xA0, 19, 20, 21), (0xB0, 5, 6, 7)]


def test_render_line_format():
    # źródło: format oczekiwany przez native_renderer.cpp (inicjalizator struktury C++)
    text = gen_shadow_regs.render([(0x019F34AB2939340D, 19, 20, 21)])
    assert text.splitlines()[-1] == '{0x019F34AB2939340Dull, 19, 20, 21},'


def test_collect_world_small_catalog():
    shaders = [
        {'type': 'vs', 'container_hash': 'B0', 'reflection': {'constants': [
            {'name': 'gWorldXform', 'reg': 4, 'rows': 4}, {'name': 'RwSkinMatrices', 'reg': 10, 'rows': 1}]}},
        {'type': 'vs', 'container_hash': 'A0', 'reflection': {'constants': [
            {'name': 'gWorldXform', 'reg': 0, 'rows': 4}]}},
        {'type': 'vs', 'container_hash': 'C0', 'reflection': {'constants': [
            {'name': 'gWorldXform', 'reg': 0, 'rows': 3}]}},   # not 4 rows: skipped
        {'type': 'vs', 'container_hash': 'D0', 'reflection': {'constants': [
            {'name': 'Mobj', 'reg': 0, 'rows': 4}]}},           # caster pass world matrix
    ]
    # źródło: policzone ręcznie z wejścia powyżej (A0 bez skinningu, B0 ze skinningiem, D0 = Mobj)
    assert gen_shadow_regs.collect_world(shaders) == [(0xA0, 0, 0), (0xB0, 4, 1), (0xD0, 0, 0)]


def test_collect_instanced_small_catalog():
    # Input: catalog entries + translated HLSL per hash (only the CONST_REL lines matter).
    shaders = [
        entry('vs', 'B0', [('instance_data', 4), ('instance_transforms', 100)]),
        entry('vs', 'A0', [('instance_data', 8), ('instance_transforms', 120)]),
        entry('vs', 'C0', [('instance_transforms', 100)]),                     # no instance_data: skipped
        entry('ps', 'D0', [('instance_data', 4), ('instance_transforms', 100)]),  # pixel: skipped
    ]
    src = {'A0': 'r0.y = r4.y + CONST_REL(120 + a0).y; r0.x = dot(r4.xz, CONST_REL(121 + a0).yx);',
           'B0': 'r1 = r2.zzzz * CONST_REL(102 + a0).yzxw; r1 = r1.zxyw + CONST_REL(103 + a0).xyzw;'}
    # źródło: policzone ręcznie z wejścia powyżej (A0 czyta tylko 120..121 -> 2, B0 czyta 103 -> 4)
    assert gen_shadow_regs.collect_instanced(shaders, src.__getitem__) == [(0xA0, 8, 120, 2), (0xB0, 4, 100, 4)]


def test_collect_albedo_small_catalog():
    def ps(h, samplers):
        return {'type': 'ps', 'container_hash': h, 'reflection': {'samplers': [
            {'name': n, 'reg': r, 'type': t} for n, r, t in samplers]}}
    shaders = [
        ps('B0', [('L0_col_map', 6, 'sampler2d'), ('L1_col_map', 3, 'sampler2d'), ('env_map', 1, 'samplercube')]),
        ps('A0', [('col_map', 0, 'sampler2d'), ('L1_col_map', 2, 'sampler2d')]),
        ps('C0', [('env_map', 1, 'samplercube'), ('lightRamp_map', 14, 'sampler2d')]),  # no colour map: skipped
        ps('E0', [('L3_col_map', 7, 'sampler2d'), ('L5_col_map', 2, 'sampler2d')]),  # no preferred: first by name
        ps('F0', [('col_map', 4, 'sampler3d')]),  # not 2D: skipped
        {'type': 'vs', 'container_hash': 'D0', 'reflection': {'samplers': [
            {'name': 'col_map', 'reg': 0, 'type': 'sampler2d'}]}},  # vertex: skipped
    ]
    # źródło: policzone ręcznie z wejścia powyżej (A0: col_map przed L1 -> 0; B0: L1 przed L0 -> 3;
    # E0: L3 < L5 alfabetycznie -> 7); HLSL bez pobrań tekstur -> UV nieznane (None)
    rows = gen_shadow_regs.collect_albedo(shaders, lambda h: '')
    assert rows == [(0xA0, 0, None), (0xB0, 3, None), (0xE0, 7, None)]


# Fragment shaped like the translated building shader 0D62162D76A532D1 (interpolant 6, freq c59,
# offset c60 plus a per-pixel parallax term that must be dropped).
UV_SNIPPET = '''
#define L1_UV_freq SR_LOAD_FLOAT4(Pixel, 944)
#define L1_UV_offs SR_LOAD_FLOAT4(Pixel, 960)
\tfloat4 c252 = asfloat(uint4(0x3E99999A, 0x40A00000, 0x3CA3D70A, 0x3D4CCCCD));
\tfloat4 r0 = iTexCoord0;
\tfloat4 r6 = iTexCoord6;
\tfloat4 r8 = iTexCoord1;
\t\tr0.x = dot(r8.xyz, r8.xyz);
\t\tr0.xy = r0.xx * r8.zz + L1_UV_offs.xy;
\t\tr6.xy = r6.xy * L1_UV_freq.xy + r0.xy;
\t\tr10.xyz = tfetch2D(L1_col_map_Texture2DDescriptorIndex, L1_col_map_SamplerDescriptorIndex, r6.xy, float2(0, 0)).xyz;
\tfloat4 r2 = iTexCoord2;
\t\tr2.zw = r2.yx * c252.yx;
\t\tr3.xyz = tfetch2D(L0_col_map_Texture2DDescriptorIndex, L0_col_map_SamplerDescriptorIndex, r2.wz, float2(0, 0)).xyz;
\t\tr4.xyz = tfetch2D(L2_col_map_Texture2DDescriptorIndex, L2_col_map_SamplerDescriptorIndex, r8.xy, float2(0, 0)).xyz;
'''


def test_analyze_albedo_uv_snippet():
    # źródło: policzone ręcznie z UV_SNIPPET: 944/16 = c59 (skala), 960/16 = c60 (przesunięcie),
    # składnik r0.xx * r8.zz zależy od piksela (paralaksa) i odpada
    assert gen_shadow_regs.analyze_albedo_uv(UV_SNIPPET, 'L1_col_map') == (
        6, 0, 1, (('c', 59, 0), ('c', 59, 1), ('c', 60, 0), ('c', 60, 1)))
    # r2.wz = (r2.x * c252.x, r2.y * c252.y): u = iTexCoord2.x * 0x3E99999A (0.3), v = .y * 5.0
    k, cu, cv, (fu, fv, ou, ov) = gen_shadow_regs.analyze_albedo_uv(UV_SNIPPET, 'L0_col_map')
    assert (k, cu, cv) == (2, 0, 1)
    assert fu == ('l', pytest.approx(0.3)) and fv == ('l', 5.0) and ou == ('l', 0.0) and ov == ('l', 0.0)
    # L2: r8 = iTexCoord1, ale r8 nie było przepisane -> uv = iTexCoord1.xy bez skali
    assert gen_shadow_regs.analyze_albedo_uv(UV_SNIPPET, 'L2_col_map') == (
        1, 0, 1, (('l', 1.0), ('l', 1.0), ('l', 0.0), ('l', 0.0)))
    assert gen_shadow_regs.analyze_albedo_uv(UV_SNIPPET, 'col_map') is None


# Shaped like 3730C6864F8CC9FE lines 1591-1915: mask sampled .zyx, normalised by the reciprocal of
# the channel sum, whose product permutes the components back.
TINT_SNIPPET = '''
\tr0.yzw = tfetch2D(L1_mat_map_Texture2DDescriptorIndex, L1_mat_map_SamplerDescriptorIndex, r1.xy, float2(0, 0)).zyx;
\t\tr13.w = saturate(dot(r0.ywz, c254.yyy));
\t\tps = select(abs(r13.w) < 1.17549435e-38, select((asuint(r13.w) >> 31) != 0, FLT_MIN, FLT_MAX), clamp(rcp(r13.w), FLT_MIN, FLT_MAX));
\t\tr0.x = ps;
\t\tr0.yzw = r0.xxx * r0.ywz;
\t\tr13.xyz = r0.www * L1_G_tint.xyz;
\t\tr13.xyz = r0.zzz * L1_R_tint.zxy + r13.zxy;
\t\tr13.xyz = r0.yyy * L1_B_tint.yzx + r13.zxy;
'''


def test_analyze_tint_blend_snippet():
    # źródło: policzone ręcznie z TINT_SNIPPET: po pobraniu r0.y = tex.z, r0.z = tex.y, r0.w = tex.x;
    # po normalizacji (r0.ywz) r0.y = z/s, r0.z = x/s, r0.w = y/s; G_tint * r0.w -> kanał 1,
    # R_tint * r0.z -> kanał 0, B_tint * r0.y -> kanał 2, wszystkie znormalizowane
    assert gen_shadow_regs.analyze_tint_blend(TINT_SNIPPET, 'L1_') == {
        'R_tint': (0, True), 'G_tint': (1, True), 'B_tint': (2, True)}
    # bez normalizacji: te same kanały, normalised = False
    raw = TINT_SNIPPET.replace('\t\tr0.yzw = r0.xxx * r0.ywz;\n', '\t\tr0.yzw = r0.ywz;\n')
    assert gen_shadow_regs.analyze_tint_blend(raw, 'L1_') == {
        'R_tint': (0, False), 'G_tint': (1, False), 'B_tint': (2, False)}
    assert gen_shadow_regs.analyze_tint_blend(TINT_SNIPPET, 'L0_') is None
    # zapis w bloku if (p0) dający nieznaną wartość (druga ścieżka gry) nie kasuje kanałów maski
    # (wzór z 3730C6864F8CC9FE, linie 1719-1743: r0.y = max(r13.z, r13.z) pod if (p0))
    pred = TINT_SNIPPET.replace('\t\tr0.x = ps;\n', '\t\tr0.x = ps;\n\tif (p0)\n\t{\n\t\tr0.y = max(r13.z, r13.z);\n\t}\n')
    assert gen_shadow_regs.analyze_tint_blend(pred, 'L1_') == {
        'R_tint': (0, True), 'G_tint': (1, True), 'B_tint': (2, True)}


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_collect_tint():
    from srpaths import ARTIFACTS
    rows = gen_shadow_regs.collect_tint(json.load(open(CATALOG))['shaders'],
                                        lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.ps.hlsl').read())
    row = next(r for r in rows if r[0] == 0x0D62162D76A532D1)
    consts = dict(re.findall(r'#define (L1_\w*tint) SR_LOAD_FLOAT4\(Pixel, (\d+)\)',
                             open(ARTIFACTS / 'hlsl' / '0D62162D76A532D1.ps.hlsl').read()))
    # źródło: artifacts/shaders/hlsl/0D62162D76A532D1.ps.hlsl: L1_mat_map slot 2 (catalog), UV jak dla
    # L1_col_map (r6.xy, linie 1407-1412, c59/c60), kolejność kanałów i normalizacja z linii 1480-1653,
    # rejestry odcieni = przesunięcia z #define / 16, L1_col_map slot 3
    expected_regs = tuple(int(consts[f'L1_{n}']) // 16 for n in ('R_tint', 'G_tint', 'B_tint', 'tint'))
    assert row == (0x0D62162D76A532D1, 2, (6, 0, 1, (('c', 59, 0), ('c', 59, 1), ('c', 60, 0), ('c', 60, 1))),
                   (0, 1, 2), True, expected_regs, 3)


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_palette_blend():
    from srpaths import ARTIFACTS
    r = gen_shadow_regs.analyze_palette_blend(open(ARTIFACTS / 'hlsl' / '8A6DD1D52752FAD6.ps.hlsl').read())
    # źródło: artifacts/shaders/hlsl/8A6DD1D52752FAD6.ps.hlsl, odczyt 2026-10-20: mat_map .zxy do r7.xyw,
    # R_tint * r7.y (kanał 0), G_tint * r7.w (1), B_tint * r7.x (2); dot(r7.xywz, gBuildingTint.zxyw)
    # -> R: x, G: y, B: z, reszta (r7.z): w; r0.x = max(r5.w), r5 = iTexCoord5, ps = c246.w (8) * r0.x
    assert r['channels'] == (0, 1, 2) and r['building'] == (0, 1, 2, 3) and r['index'] == (5, 3, 8.0)
    # Paleta przeliczona ręcznie z łańcucha select (linie po r11.w = floor(r0.x)) i literałów
    # c246 = (0.45, 0.4, 0.565, 8), c247.w = 0.6, c250 = (0.439, 0.435, 0.561, 0.757),
    # c251 = (0.384, 0.529, 0.604, 0.882), c252 = (0.722, 0.702, 0.537, -), c254 = (0.49, 0.612, 0.545, 0.592):
    # n=0: r12.zw = c250.wx, zamiana zw -> r15.xz = (0.757, 0.439), r15.y = c246.y; n=1: c247.ww; n=2: c254.wy
    # zamienione; n=3: r15.y = c246.z, r15.xz = c251.wy; n=4..7: c251, c250, c252, c254 .xyz
    expected = [(0.757, 0.4, 0.439), (0.6, 0.4, 0.6), (0.612, 0.4, 0.592), (0.882, 0.565, 0.529),
                (0.384, 0.529, 0.604), (0.439, 0.435, 0.561), (0.722, 0.702, 0.537), (0.49, 0.612, 0.545)]
    assert [tuple(round(v, 3) for v in c) for c in r['palette']] == expected


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_tint_blend():
    from srpaths import ARTIFACTS
    src = open(ARTIFACTS / 'hlsl' / '0D62162D76A532D1.ps.hlsl').read()
    # źródło: artifacts/shaders/hlsl/0D62162D76A532D1.ps.hlsl linie 1480-1653 (odczyt 2026-10-20):
    # r0.xyz = mat.zyx; r2.w = saturate(dot(r0.xzy, 1)); r0.w = rcp; r0.yzw = r0.www * r0.yzx
    # -> r0.y = y/s (G_tint), r0.z = x/s (R_tint), r0.w = z/s (B_tint, przez ps)
    assert gen_shadow_regs.analyze_tint_blend(src, 'L1_') == {
        'R_tint': (0, True), 'G_tint': (1, True), 'B_tint': (2, True)}


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_albedo():
    from srpaths import ARTIFACTS
    rows = gen_shadow_regs.collect_albedo(json.load(open(CATALOG))['shaders'],
                                          lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.ps.hlsl').read())
    # źródło: artifacts/shaders/hlsl/0D62162D76A532D1.ps.hlsl linie 807, 816-817, 1094, 1407, 1412
    # (L1_col_map slot 3; r6 = iTexCoord6; r6.xy = r6.xy * L1_UV_freq(c59).xy + r0.xy,
    # r0.xy = ... + L1_UV_offs(c60).xy; odczyt 2026-10-20)
    assert (0x0D62162D76A532D1, 3, (6, 0, 1, (('c', 59, 0), ('c', 59, 1), ('c', 60, 0), ('c', 60, 1)))) in rows


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_instanced():
    from srpaths import ARTIFACTS
    rows = gen_shadow_regs.collect_instanced(
        json.load(open(CATALOG))['shaders'], lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.vs.hlsl').read())
    # źródło: artifacts/shaders/hlsl/1C89B1A64AA3110B.vs.hlsl linie 979-994 (c100+a0, c101+a0, a0 = 2*instancja)
    assert (0x1C89B1A64AA3110B, 4, 100, 2) in rows
    # źródło: artifacts/shaders/hlsl/B27A827D7AA91D13.vs.hlsl linie 1047-1062 (c100..c103 + a0, a0 = 4*instancja)
    assert (0xB27A827D7AA91D13, 4, 100, 4) in rows


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='brak artifacts/shaders/catalog.json (lokalny korpus gry)')
def test_real_catalog_known_shader():
    rows = gen_shadow_regs.collect(json.load(open(CATALOG))['shaders'])
    # źródło: artifacts/shaders/hlsl/019F34AB2939340D.vs.hlsl, linie 611-613 (odczyt 2026-10-19)
    assert (0x019F34AB2939340D, 19, 20, 21) in rows
    # źródło: zliczenie stałych shadowMapU w katalogu (odczyt 2026-10-19)
    assert len(rows) == 24
