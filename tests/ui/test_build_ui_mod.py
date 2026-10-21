"""Tests for tools/ui/build_ui_mod.py.
Input: the game's ui.AST and text.AST (skipped without them). The built
archives are read back with apt.py and an XML parser, and checked against
what the recipe promises: the game's entries untouched, the new screen's
rows, the new paths in MAIN, the texts."""
import os
import sys
import xml.etree.ElementTree as ET

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ui'))
import apt  # noqa: E402
import apt_edit  # noqa: E402
import build_ui_mod  # noqa: E402

DATA = os.path.join(ROOT, 'game', 'DATA')
pytestmark = pytest.mark.skipif(not os.path.exists(os.path.join(DATA, 'ui.AST')),
                                reason='game data not present')


@pytest.fixture(scope='module')
def built():
    # Next to the repository's other build output, not the system temp folder:
    # the archive is 86 MB a run.
    out = os.path.join(ROOT, 'build', 'tests-ui')
    os.makedirs(out, exist_ok=True)
    build_ui_mod.build(DATA, out)
    ui = apt_edit.Archive(open(os.path.join(out, 'ui_sr.AST'), 'rb').read())
    text = apt_edit.Archive(open(os.path.join(out, 'text_sr.AST'), 'rb').read())
    return ui, text


def movie(archive, name):
    _, (_, d), (_, c) = build_ui_mod.movie_of(archive, name)
    return apt.Movie(d, c)


def strings_pushed(m, code_at):
    return [ins.text for ins in m.disassemble(code_at)]


def test_game_entries_untouched(built):
    ui, _ = built
    orig = apt_edit.Archive(open(os.path.join(DATA, 'ui.AST'), 'rb').read())
    changed = {'MAIN', 'SCRCONTROLROOM', 'SCRPAUSEMENU'}  # the recipe's edits
    for e in orig.entries:
        if e['name'] not in changed:
            assert ui.data(ui.find(e['name'])) == orig.data(e), e['name']
    assert len(ui.entries) == len(orig.entries) + 2


def test_graphics_screen_rows(built):
    ui, _ = built
    m = movie(ui, 'SCRGRAPHICSOPT')
    assert [name for name, _ in m.header()['exports']] == \
        ['ScrGraphicsOpt', 'ScrGraphicsOpt', '__Packages.BistroBaseScreen', '__Packages.ScrGraphicsOpt']
    screen = m.characters()[2]
    rows = [it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    assert [r['name'] for r in rows] == ['Gfx0', 'Gfx1', 'Gfx2', 'Gfx3']
    assert [round(r['pos'][1], 2) for r in rows] == [222.85, 255.65, 288.45, 321.25]
    first = strings_pushed(m, rows[0]['events'][0][1])
    assert "'RT Shadows'" in first and "'On'" in first and "'Off'" in first
    # the tab order: 0..3
    tabs = [strings_pushed(m, r['events'][0][1]) for r in rows]
    assert [t[t.index("'m_tabIndex'") + 1] for t in tabs] == ['0', '1', '2', '3']


def test_graphics_screen_methods_use_the_natives(built):
    ui, _ = built
    m = movie(ui, 'SCRGRAPHICSOPT')
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    assert texts.count("'GetGraphicsOption'") == 4
    assert texts.count("'SetGraphicsOption'") == 4
    assert "'InitGui'" in texts and "'HandleInput'" in texts and "'ExitScreen'" in texts
    # no row of Gameplay Options left behind
    assert not any(t in texts for t in ("'YInversion'", "'Vibration'", "'SetRumble'"))


def test_main_sets_the_paths(built):
    ui, _ = built
    m = movie(ui, 'MAIN')
    items = m.frames(*m.header()['frames'])[0]
    actions = [it for it in items if it['type'] == 1]
    i = next(k for k, it in enumerate(actions)
             if "'screens/misc/ScrGameplayOpt'" in strings_pushed(m, it['code']))
    ours = strings_pushed(m, actions[i + 1]['code'])
    assert "'screens/misc/ScrGraphicsOpt'" in ours and "'artassets/MenuIcons/IconGraphicsOpt'" in ours


def test_control_room_wraps_its_methods(built):
    ui, _ = built
    m = movie(ui, 'SCRCONTROLROOM')
    items = m.frames(*m.header()['frames'])[0]
    inits = [it for it in items if it['type'] == 8 and it['sprite'] == 5]
    assert len(inits) == 2
    texts = strings_pushed(m, inits[1]['code'])
    for t in ("'srInitGui0'", "'srHandleInput0'", "'GRAPHICS_OPT'", "'Label_ScrGraphicsOpt'"):
        assert t in texts


def test_pause_menu_button(built):
    # The game's pause buttons are 38 apart, Audio Options (btn8) at y 344 and
    # Quit (btn9) at 402 with tab 8 (apt.py dump of the original).
    ui, _ = built
    m = movie(ui, 'SCRPAUSEMENU')
    screen = m.characters()[2]
    rows = {it['name']: it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3}
    assert round(rows['btn10']['pos'][1], 2) == 382.0
    assert round(rows['btn9']['pos'][1], 2) == 440.0
    tab = lambda name: (lambda t: t[t.index("'m_tabIndex'") + 1])(strings_pushed(m, rows[name]['events'][0][1]))
    assert (tab('btn8'), tab('btn10'), tab('btn9')) == ('7', '8', '9')
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    for t in ("'btn10'", "'GRAPHICS_OPT'", "'srHandleInput0'", "'Label_ScrGraphicsOpt'"):
        assert t in texts


def test_texts(built):
    _, text = built
    xml = text.data(text.find('INTERFACETEXT')).decode('utf-8-sig')
    root = ET.fromstring(xml)
    values = {e.get('id'): e.get('value') for e in root.iter('uitext')}
    assert values['Label_ScrGraphicsOpt'] == 'GRAPHICS OPTIONS'
    assert values['GFX_OPT_SOFTNESS_LEVELS'].split('|') == ['Hard', 'Low', 'Medium', 'High', 'Ultra']
    assert values['Label_ScrAudioOpt'] == 'AUDIO OPTIONS'   # the game's own still there
