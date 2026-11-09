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
    # the recipe's edits; since OPTIONS (2026-11-08) also the game's three options
    # screens, whose B goes back to OPTIONS
    # since the key icons (2026-11-09, the user's "zamiast przycisków xboxa klawisze") also BUTTONICON
    # and SCRPRESSSTART ("PRESS ENTER" on keyboard and mouse, the user's request 2026-11-09)
    # and SCRMOVESLIST, SCRSTATISTICS (their scroll arrows clickable, the user's screenshots 2026-11-09)
    changed = {'MAIN', 'SCRCONTROLROOM', 'SCRPAUSEMENU', 'SCRGAMEPLAYOPT', 'SCRCONTROLLEROPT', 'SCRAUDIOOPT',
               'BUTTONICON', 'SCRPRESSSTART', 'SCRMOVESLIST', 'SCRSTATISTICS'}
    for e in orig.entries:
        if e['name'] not in changed:
            assert ui.data(ui.find(e['name'])) == orig.data(e), e['name']
    # source: the recipe adds SCRGRAPHICSOPT, SCRDISPLAYOPT, SCROPTIONSMENU, SCRKEYSOPT and ICONGRAPHICSOPT,
    # and a HINTKEY<code> icon for each of key_names.py's 111 keys plus HINTKEYMOUSE
    # (len(key_names.sdk_key_names()) printed 2026-11-09: 111), and SCRMOUSEOPT (the user's mouse
    # sensitivity slider, 2026-11-09)
    assert len(ui.entries) == len(orig.entries) + 5 + 111 + 1 + 1


def test_graphics_screen_rows(built):
    ui, _ = built
    m = movie(ui, 'SCRGRAPHICSOPT')
    assert [name for name, _ in m.header()['exports']] == \
        ['ScrGraphicsOpt', 'ScrGraphicsOpt', '__Packages.BistroBaseScreen', '__Packages.ScrGraphicsOpt']
    screen = m.characters()[2]
    rows = [it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    # eleven rows since the launcher's texture filter, FXAA, shadow maps and MSAA moved in
    # (2026-11-08, the user's request to drop the launcher); 30 apart from Gameplay Options'
    # top row (222.85, apt.py dump of the original) so the eleventh fits above the hints
    assert [r['name'] for r in rows] == ['Gfx%d' % i for i in range(11)]
    # source: 222.85 + 30 * i, by hand
    assert [round(r['pos'][1], 2) for r in rows] == [222.85, 252.85, 282.85, 312.85, 342.85, 372.85, 402.85,
                                                     432.85, 462.85, 492.85, 522.85]
    first = strings_pushed(m, rows[0]['events'][0][1])
    assert "'RT Shadows'" in first and "'On'" in first and "'Off'" in first
    # the tab order: one a row, top down
    tabs = [strings_pushed(m, r['events'][0][1]) for r in rows]
    assert [t[t.index("'m_tabIndex'") + 1] for t in tabs] == [str(i) for i in range(11)]


def test_graphics_screen_methods_use_the_natives(built):
    ui, _ = built
    m = movie(ui, 'SCRGRAPHICSOPT')
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    assert texts.count("'GetGraphicsOption'") == 11  # one a row
    assert texts.count("'SetGraphicsOption'") == 11
    assert "'InitGui'" in texts and "'HandleInput'" in texts and "'ExitScreen'" in texts
    # no row of Gameplay Options left behind
    assert not any(t in texts for t in ("'YInversion'", "'Vibration'", "'SetRumble'"))
    # the restart popup (the user's request, 2026-11-08), as Map's own: one per "*" row,
    # source: FXAA*, Shadow Maps*, MSAA* = 3, counted by hand in GRAPHICS_ROWS
    assert texts.count("'ShowPopup'") == 3
    # new BistroPopupParams(): no arguments, the class name (one PushData, apt.py prints both)
    assert texts.count("0, 'BistroPopupParams'") == 3
    # restart now or later (the user's request, 2026-11-08): the callback calls the native
    for t in ("'gPopupMgr'", "'Options_RestartNote'", "'Options_RestartNow'", "'Options_RestartLater'",
              "'srRestartShown'", "'selectedCallback'", "'RestartGame'"):
        assert t in texts
    # source: one callback a popup, each calling RestartGame once = 3
    assert texts.count("'RestartGame'") == 3


def test_display_screen(built):
    # Display Options (2026-11-08): the launcher's display settings, ids from 100
    # (graphics_menu.cpp kRows), 32.8 apart as Gameplay Options' rows (apt.py dump).
    ui, _ = built
    m = movie(ui, 'SCRDISPLAYOPT')
    assert [name for name, _ in m.header()['exports']] == \
        ['ScrDisplayOpt', 'ScrDisplayOpt', '__Packages.BistroBaseScreen', '__Packages.ScrDisplayOpt']
    screen = m.characters()[2]
    rows = [it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    # five rows: Skip Intro left the menu for A / Escape skipping one video, Keyboard/Mouse
    # for switching by the input last used (the user's requests, 2026-11-08/09)
    assert [r['name'] for r in rows] == ['Dsp%d' % i for i in range(5)]
    # source: 222.85 + 32.8 * i, by hand
    assert [round(r['pos'][1], 2) for r in rows] == [222.85, 255.65, 288.45, 321.25, 354.05]
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    assert texts.count("'GetGraphicsOption'") == 5 and texts.count("'SetGraphicsOption'") == 5
    assert "'Title_ScrDisplayOpt'" in texts and "'ScrDisplayOpt'" in texts
    assert "'ScrGraphicsOpt'" not in texts
    # source: every display row applies after a restart (all 5 labels end in "*")
    assert texts.count("'ShowPopup'") == 5


def test_main_sets_the_paths(built):
    ui, _ = built
    m = movie(ui, 'MAIN')
    items = m.frames(*m.header()['frames'])[0]
    actions = [it for it in items if it['type'] == 1]
    i = next(k for k, it in enumerate(actions)
             if "'screens/misc/ScrGameplayOpt'" in strings_pushed(m, it['code']))
    ours = strings_pushed(m, actions[i + 1]['code'])
    assert "'screens/misc/ScrGraphicsOpt'" in ours and "'artassets/MenuIcons/IconGraphicsOpt'" in ours
    assert "'screens/misc/ScrDisplayOpt'" in ours
    # the menu poll (2026-11-09): FlowMgr's state to SettingsAdapter.SetMenuActive every 100 ms
    # setInterval(_global, "srMenuPoll", 100): the args and the call each in one PushData (apt.py prints both)
    for t in ("'srMenuPoll'", "100, 'srMenuPoll'", "3, 'setInterval'", "'SetMenuActive'", "'CurInterfaceState'",
              "'IG'", "'PAUSEMODE_NONE'"):
        assert t in ours, t
    # the mouse poll (clickable menus, the user's request 2026-11-09): every 33 ms, the focus
    # manager's items hit-tested, the hit selected, a click pressed as A or RIGHT
    for t in ("33, 'srMousePoll'", "'GetMouseX'", "'GetMouseY'", "'TakeMenuClick'", "'PressPad'", "'selectItem'",
              "'m_arrItems'", "'_xscale'", "'GetIndex'", "'MenuMouseLog'"):
        assert t in ' | '.join(ours), t   # some share a PushData line (apt.py prints them together)
    # popups (the user's "Load game" could not be clicked, 2026-11-09): ScrGenPop's buttons sit in
    # its own mFM, reached through gPopupMgr.aPopupStack[top].popInstance (apt.py dumps of MAIN, SCRGENPOP)
    for t in ("'aPopupStack'", "'popInstance'", "'mFM'", "'gPopupMgr'"):
        assert t in ' | '.join(ours), t
    # every screen's own focus manager (the user's "nie da się kliknąć w load", 2026-11-09: the front
    # end's slots live in MAIN's SaveLoadSlots copy, apt.py dump of MAIN): CafeFW_FocusManager's
    # handleDefaultInputs and selectItem note the manager in _global.srActiveFM, the poll uses it
    for t in ("'srActiveFM'", "'srHDI'", "'srSel'", "'handleDefaultInputs'", "'CafeFW_FocusManager'", "'srWrapped'"):
        assert t in ' | '.join(ours), t
    # source: build_ui_mod.MOUSE_ITEMS = 16 items looked at, each read once from m_arrItems,
    # plus srActiveFM's first item once (still on the stage?)
    assert ours.count("'m_arrItems'") == 17


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
    # One OPTIONS button (the user's request, 2026-11-08: the menu ran into its frame).
    # The game's pause buttons: Gameplay (btn6) at y 268, Audio (btn8) 344, Quit (btn9)
    # 402 (apt.py dump of the original).
    ui, _ = built
    m = movie(ui, 'SCRPAUSEMENU')
    screen = m.characters()[2]
    rows = {it['name']: it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3}
    assert 'btn10' not in rows and 'btn11' not in rows
    # source: Quit keeps its 58 gap (402 - 344) below OPTIONS at 268: 326, by hand
    assert round(rows['btn9']['pos'][1], 2) == 326.0
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    for t in ("'btn6'", "'OPTIONS_MENU'", "'srHandleInput0'", "'Label_ScrOptionsMenu'", "'btn7'", "'btn8'",
              "'disable'", "'hide'"):
        assert t in texts


def test_options_menu(built):
    # OPTIONS: the pause menu's movie, six buttons 38 apart from Resume's 58 (apt.py dump),
    # each opening one options screen, B back to the pause menu.
    ui, _ = built
    m = movie(ui, 'SCROPTIONSMENU')
    assert 'ScrOptionsMenu' in [name for name, _ in m.header()['exports']]
    screen = m.characters()[2]
    rows = {it['name']: it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3}
    # source: 58 + 38 * i, by hand
    assert [round(rows['btn%d' % i]['pos'][1], 2) for i in range(1, 7)] == [58.0, 96.0, 134.0, 172.0, 210.0, 248.0]
    items = m.frames(*m.header()['frames'])[0]
    texts = []
    for it in items:
        if it['type'] == 8:
            texts += strings_pushed(m, it['code'])
    for t in ("'GAMEPLAY_OPT'", "'CONTROLLER_OPT'", "'AUDIO_OPT'", "'GRAPHICS_OPT'", "'DISPLAY_OPT'", "'KEYS_OPT'",
              "'PAUSEMENU'", "'OptionsSelectedItem'", "'Label_ScrKeysOpt'"):
        assert t in texts, t


def test_game_options_screens_back_to_options(built):
    # Gameplay, Controller and Audio Options: their HandleInput wrapped, B while paused to OPTIONS
    ui, _ = built
    for entry, cls in (('SCRGAMEPLAYOPT', 'ScrGameplayOpt'), ('SCRCONTROLLEROPT', 'ScrControllerOpt'),
                       ('SCRAUDIOOPT', 'ScrAudioOpt')):
        m = movie(ui, entry)
        items = m.frames(*m.header()['frames'])[0]
        # the wrapper is the init action right after the class's own (other classes follow it)
        inits = [strings_pushed(m, it['code']) for it in items if it['type'] == 8]
        ours = [t for t in inits if "'srHandleInput0'" in t]
        assert len(ours) == 1, entry
        for t in ("'%s'" % cls, "'OPTIONS_MENU'", "'TRIANGLE'"):
            assert t in ours[0], (entry, t)


def test_keys_screen(built):
    # Keys Mapping: eight rows a page, 32.8 apart as Gameplay Options' (apt.py dump),
    # the natives of graphics_menu.cpp, the poll through setInterval
    ui, _ = built
    m = movie(ui, 'SCRKEYSOPT')
    screen = m.characters()[2]
    rows = [it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    # and the page arrows' row beside the title (the user 2026-11-09: no arrows to turn 1/3)
    assert [r['name'] for r in rows] == ['Key%d' % i for i in range(8)] + ['srPageRow']
    items = m.frames(*m.header()['frames'])[0]
    ours = [it for it in items if it['type'] == 8 and it['sprite'] == 4][-1]
    texts = strings_pushed(m, ours['code'])
    for t in ("'GetKeyBind'", "'BeginKeyCapture'", "'KeyCaptureActive'", "'CancelKeyCapture'", "'setInterval'",
              "'clearInterval'", "'srPoll'", "'KEYS_PRESS'", "'KEYS_PAGE_ACTIONS'", "'KEYS_PAGE_STICKS'"):
        assert t in texts, t
    # source: 8 + 5 + 4 = 17 bindings listed, each read once: of key_capture.h's 24, the stick clicks
    # and d-pad down have no action in the game's control screens (the user, 2026-11-09: "sticki nadal są"),
    # and the four camera keys went too ("camera left i reszta kierunków też do usunięcia")
    assert texts.count("'GetKeyBind'") == 17


def test_texts(built):
    _, text = built
    xml = text.data(text.find('INTERFACETEXT')).decode('utf-8-sig')
    root = ET.fromstring(xml)
    values = {e.get('id'): e.get('value') for e in root.iter('uitext')}
    assert values['Label_ScrGraphicsOpt'] == 'GRAPHICS OPTIONS'
    assert values['GFX_OPT_SOFTNESS_LEVELS'].split('|') == ['Hard', 'Low', 'Medium', 'High', 'Ultra']
    assert values['GFX_OPT_DLSS_MODES'].split('|') == ['Off', 'Performance', 'Balanced', 'Quality', 'DLAA']
    # the order graphics_menu.cpp maps to sr_time_of_day -1, 0, 6, 9, 12, 15, 17, 21
    assert values['GFX_OPT_TIMES'].split('|') == \
        ['Game', 'Midnight', 'Dawn', 'Morning', 'Noon', 'Afternoon', 'Sunset', 'Night']
    # the order graphics_menu.cpp maps to sr_daynight_cycle_minutes 0, 10, 30, 60, 120, 240, 360
    assert values['GFX_OPT_CYCLES'].split('|') == ['Off', '10 min', '30 min', '60 min', '2 h', '4 h', '6 h']
    assert values['GFX_OPT_TIME_OF_DAY'] == 'Time of Day'
    # the orders graphics_menu.cpp maps to window_width x window_height, fullscreen true/false,
    # sr_native_fps_limit 30, 60, 120, 0 and sr_native_render_scale 0, 1, 1.25, 1.5, 2, 3, 4
    assert values['DSP_OPT_RESOLUTIONS'].split('|') == \
        ['960 x 540', '1280 x 720', '1600 x 900', '1920 x 1080', '2560 x 1440', '3840 x 2160']
    assert values['DSP_OPT_MODES'].split('|') == ['Fullscreen', 'Windowed']
    assert values['DSP_OPT_FPS_LIMITS'].split('|') == ['30', '60', '120', 'Unlimited']
    assert values['DSP_OPT_SCALES'].split('|') == ['Auto', '100%', '125%', '150%', '200%', '300%', '400%']
    # sr_native_anisotropic_filtering -1, 2, 4, 8, 16; shadow quality 1, 2, 4; MSAA 1, 4, 8
    assert values['GFX_OPT_FILTERS'].split('|') == ['Original', '2x', '4x', '8x', '16x']
    assert values['GFX_OPT_SHADOW_SIZES'].split('|') == ['Original', '2x', '4x']
    assert values['GFX_OPT_MSAA_LEVELS'].split('|') == ['Off', '4x', '8x']
    assert values['Label_ScrDisplayOpt'] == 'DISPLAY OPTIONS'
    assert values['Options_RestartNote'] == \
        'This option takes effect after a restart. Restart the game now? Unsaved progress will be lost.'
    assert (values['Options_RestartNow'], values['Options_RestartLater']) == ('RESTART NOW', 'LATER')
    assert values['Label_ScrAudioOpt'] == 'AUDIO OPTIONS'   # the game's own still there


def test_save_texts_speak_of_pc(built):
    # The load screen read "Please don't turn off your console or remove any Xbox 360 Memory
    # Units" (the user's screenshot, 2026-11-07); no save text may mention the console now,
    # while unrelated ones that say "console" (the Fortress console concept art) stay.
    _, text = built
    raw = text.data(text.find('INTERFACETEXT')).decode('utf-8-sig')
    # The game does not expand character references: an &#x27; showed on screen as "DON&#X27;T"
    # (dump of the load screen, 2026-11-07), so the raw text must hold the apostrophe itself.
    assert 'value="Loading saved game. Please don\'t close the game."' in raw
    assert '&#x27;' not in raw
    root = ET.fromstring(raw)
    values = {e.get('id'): e.get('value') for e in root.iter('uitext')}
    assert values['Load_Warning'] == "Loading saved game. Please don't close the game."
    assert values['Save_Warning'] == "Saving game. Please don't close the game."
    for key in ('Load_Warning', 'Save_Warning', 'SaveFailed_Body', 'LoadFailed_Body', 'SaveNoSpace_Body',
                'No_Device_Warning'):
        low = values[key].lower()
        assert 'xbox' not in low and 'console' not in low and 'memory unit' not in low, key
    assert values['ConceptArt_Name_243'] == 'Fortress Console'
    # nothing else of the game's changed: same number of texts as the original plus ours
    orig = apt_edit.Archive(open(os.path.join(DATA, 'text.AST'), 'rb').read())
    orig_root = ET.fromstring(orig.data(orig.find('INTERFACETEXT')).decode('utf-8-sig'))
    orig_values = {e.get('id'): e.get('value') for e in orig_root.iter('uitext')}
    changed = {k for k in orig_values if values.get(k) != orig_values[k]}
    assert changed == {'Load_Warning', 'Save_Warning', 'SaveFailed_Body', 'LoadFailed_Body', 'SaveNoSpace_Body',
                       'No_Device_Warning'}


def test_button_icon_shows_keys(built):
    # BUTTONICON: after the game's class init, SetIcon = _global.srSetIcon (apt.py dump of BUTTONICON:
    # SetIcon loads ../common/ButtonHints/<platform>/Hint<icon>.swf into mIconHolder)
    ui, _ = built
    m = movie(ui, 'BUTTONICON')
    items = m.frames(*m.header()['frames'])[0]
    inits = [it for it in items if it['type'] == 8 and it['sprite'] == 3]
    assert len(inits) == 2  # source: the game's own, then ours
    ours = ' | '.join(strings_pushed(m, inits[-1]['code']))
    for t in ("'srSetIcon'", "'SetIcon'", "'prototype'"):
        assert t in ours, t
    # MAIN defines it: KbmActive picks keys or pad, MenuHintKey the key (Enter/Escape for A/B in the
    # menus, the user 2026-11-09: "to powinien być ENTER, a back to ESC"), HintKey<code> its icon,
    # onEnterFrame reloads it when the input changes
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'srSetIcon'", "'KbmActive'", "'MenuHintKey'", "'/HintKey'", "'Mouse'", "'onEnterFrame'", "'toUpperCase'",
              "'createEmptyMovieClip'", "'SHOULDER1'", "'DPAD'", "'RSTICK'"):
        assert t in texts, t
    # an icon entry is a hint movie with a 128 x 128 texture (key_icons.py)
    import xtex
    inner = apt_edit.Archive(ui.data(ui.find('HINTKEY65')))
    tex = [inner.data(e) for e in inner.entries if e['flags'] == 0][0]
    assert xtex.size_of(tex)[:2] == (128, 128)


def test_icon_row_is_clickable(built):
    # the Fortress/Control Room icon row (the user's screenshot 2026-11-09, "nieklikalne"): IconMenu's
    # SetIcons/Next/Previous note it in _global.srIconMenu (apt.py dump of ICONMENU), a click beside every
    # item presses LEFT (3), RIGHT (2) or A (1) by its x
    ui, _ = built
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'srIconMenu'", "'srSetIcons'", "'srNext'", "'srPrev'", "'IconMenu'", "'_visible'"):
        assert t in texts, t


def test_press_enter(built):
    # SCRPRESSSTART: after the class init (sprite 61, apt.py dump: SetStartText sets mcStartText from
    # LOCALIZED_PRESS_START), SetStartText = _global.srSetStartText, the game's kept as srOrigSST
    ui, text = built
    m = movie(ui, 'SCRPRESSSTART')
    items = m.frames(*m.header()['frames'])[0]
    inits = [it for it in items if it['type'] == 8 and it['sprite'] == 61]
    assert len(inits) == 2  # source: the game's own, then ours
    ours = ' | '.join(strings_pushed(m, inits[-1]['code']))
    for t in ("'srSetStartText'", "'srOrigSST'", "'SetStartText'"):
        assert t in ours, t
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'srPressStart'", "'SUP_PRESS_ENTER'", "'mcStartText'"):
        assert t in texts, t
    # source: the user's words "press enter"
    xml = text.data(text.find('INTERFACETEXT')).decode('utf-8-sig')
    assert '<uitext id="SUP_PRESS_ENTER" value="PRESS ENTER"/>' in xml


def test_kept_clips_only_on_their_screen(built):
    # the user's crash on OPTIONS (2026-11-09, guest access violation right after the screen changed):
    # a clip kept in a _global variable is freed with its screen. The screen manager's loads and unloads
    # (BistroFW_ScreenManager in the apt.py dump of MAIN: ChangeScreenByLevel, LoadOverlayScreen,
    # UnloadOverlayScreen, ScreenLoaded) raise _global.srScreenGen; every kept clip carries <name>Gen
    ui, _ = built
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'srScreenGen'", "'BistroFW_ScreenManager'", "'srCSBL'", "'srLOS'", "'srUOS'", "'srSL'",
              "'srActiveFMGen'", "'srIconMenuGen'", "'srPressStartGen'"):
        assert t in texts, t


def test_mouse_options_screen(built):
    # MOUSE OPTIONS: Audio Options' movie (apt.py dump of SCRAUDIOOPT: sliders Music and SoundFX,
    # GetMusicVol/SetMusicVol), its Music slider through GetMouseSens/SetMouseSens, no SoundFX slider,
    # opened by OPTIONS' seventh button
    ui, text = built
    m = movie(ui, 'SCRMOUSEOPT')
    exports = [name for name, _ in m.header()['exports']]
    assert 'ScrMouseOpt' in exports and 'ScrAudioOpt' not in exports
    screen = m.characters()[2]
    names = [it['name'] for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    assert names == ['Music']  # source: SoundFX taken off
    texts = ' | '.join(t for f in m.frames(*m.header()['frames']) for it in f if it.get('code')
                       for t in strings_pushed(m, it['code']))
    for t in ("'GetMouseSens'", "'SetMouseSens'", "'MouseOpt_Sens_Slider'", "'Label_ScrMouseOpt'"):
        assert t in texts, t
    for t in ("'GetSFXVol'", "'SetSFXVol'", "'SetMusicVol'"):
        assert t not in texts, t
    xml = text.data(text.find('INTERFACETEXT')).decode('utf-8-sig')
    assert '<uitext id="MouseOpt_Sens_Slider" value="Mouse Sensitivity"/>' in xml


def test_smart_camera_follows_the_input(built):
    # Smart Camera (SCRGAMEPLAYOPT's AutoCenter row, Off/On, through SettingsAdapter.Get/SetAutoCenter,
    # apt.py dump) off for keyboard and mouse, the pad's own kept (the user, 2026-11-09)
    ui, _ = built
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'GetAutoCenter'", "'SetAutoCenter'", "'srCamMode'", "'srCamKbm'", "'srCamPad'"):
        assert t in texts, t


def test_smart_camera_set_before_the_game_takes_it(built):
    # the camera takes Smart Camera when FlowMgr.ChangeInterfaceState("IG") runs (a new game, a load, the
    # pause closed): setting it later does nothing (the user, 2026-11-09: "nadal kręci", then "działa")
    ui, _ = built
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'srCIS'", "'ChangeInterfaceState'", "'srSetAC'"):
        assert t in texts, t


def test_sliders_and_icon_arrows_answer_the_mouse(built):
    # source: the user's screenshots 2026-11-09: Audio and Mouse Options' sliders took no click on
    # their arrows or squares; the icon row's arrows both stayed dim (the game's CheckArrows never
    # lights an arrow again). MAIN's mouse poll then knows a Slider by mMaxValue and sets its value,
    # and IconMenu gets its own CheckArrows with both alphas.
    ui, _ = built
    main = movie(ui, 'MAIN')
    texts = ' | '.join(t for it in main.frames(*main.header()['frames'])[0] if it['type'] == 1
                       for t in strings_pushed(main, it['code']))
    for t in ("'mMaxValue'", "'mStepValue'", "'SetValue'", "'CheckArrows'", "'INACTIVE_ALPHA'", "'ACTIVE_ALPHA'"):
        assert t in texts, t
