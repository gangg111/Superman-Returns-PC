"""Tests for tools/shaders/gen_camera_regs.py.
Input: catalog entries ({type, container_hash, reflection.constants[{name, reg,
rows, cols}]}) written here, and the real catalog when present (the city's
instanced shader 2C45BFFE76D77398 uses gViewProjectionXform from c0, read
from its translated HLSL: oPos.x = dot(gViewProjectionXform(0), ...))."""
import json
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
import gen_camera_regs  # noqa: E402

CATALOG = os.path.join(ROOT, 'artifacts', 'shaders', 'catalog.json')


def entry(kind, h, consts):
    return {'type': kind, 'container_hash': h,
            'reflection': {'constants': [{'name': n, 'reg': r, 'rows': 4, 'cols': 4} for n, r in consts]}}


def test_picks_vertex_shaders_with_the_camera():
    cat = [entry('vs', 'B0', [('gWorldXform', 0), ('gViewProjectionXform', 232)]),
           entry('vs', 'A0', [('gViewProjectionXform', 0)]),
           entry('ps', 'C0', [('gViewProjectionXform', 4)]),     # pixel shader: not ours
           entry('vs', 'D0', [('gWorldViewProjectionXform', 8)])]  # no camera of its own
    assert gen_camera_regs.camera_rows(cat) == [(0xA0, 0), (0xB0, 232)]


def test_render():
    text = gen_camera_regs.render([(0x2C45BFFE76D77398, 0)])
    assert text.splitlines()[-1] == '{0x2C45BFFE76D77398ull, 0},'


@pytest.mark.skipif(not os.path.exists(CATALOG), reason='catalog not built')
def test_real_catalog_has_the_city_shader():
    rows = dict(gen_camera_regs.camera_rows(json.load(open(CATALOG))['shaders']))
    assert rows[0x2C45BFFE76D77398] == 0
    assert rows[0xDF1EB50388CC2563] == 232
