"""Tests for tools/shaders/gen_shadow_regs.py.
Input: shader catalog entries ({type, container_hash, reflection.constants[{name, reg}]})."""
import json
import os
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
