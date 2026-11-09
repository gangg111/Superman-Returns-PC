"""Builds the port's own front-end archives: DATA/ui_sr.AST and DATA/text_sr.AST,
the game's ui.AST and text.AST with a Graphics Options screen added. The game's
files are read, never written; the port's BigFile::Open hook
(port/src/graphics_menu.cpp) opens <name>_sr.AST in place of <name>.AST when
it is there.

What changes (apt.py dumps every movie named here):
  MAIN            _global.SCREEN.GRAPHICS_OPT and _global.ICON.GRAPHICS_OPT,
                  set by an action after the frame's own that sets the others
  SCRPAUSEMENU    a Graphics Options button after Audio Options (Quit one row
                  down): InitGui runs the game's own, then labels it;
                  HandleInput takes A on it and hands every other input to the
                  game's own
  SCRCONTROLROOM  the same for its icon menu: a fourth icon after Save/Load,
                  Audio and Controller (the game reaches this screen only from
                  a Fortress icon its Fortress never shows, so this is for
                  completeness)
  SCRGRAPHICSOPT  new: Gameplay Options' movie (its Toggle rows, its layout)
                  with the class renamed, our rows in place of its three and
                  its methods replaced; values through the natives
                  SettingsAdapter.GetGraphicsOption(id) / SetGraphicsOption(id, v)
  ICONGRAPHICSOPT new: the icon (artassets/MenuIcons/IconGraphicsOpt)
  BUTTONICON      SetIcon replaced: with keyboard and mouse in use the icon
                  of the key bound to the pad button (edit_button_icon)
  HINTKEY<code>   new: the keys' and the mouse's icons (key_icons.py)
  INTERFACETEXT   (text.AST) the screen's texts

  build_ui_mod.py <game DATA dir> [<out dir, default: the same>]
"""
import argparse
import html
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apt  # noqa: E402
import apt_edit  # noqa: E402
import key_icons  # noqa: E402

# The rows: (row name, label key, label, options key, options). A row's id is
# its screen's id base plus its index; port/src/graphics_menu.cpp maps the ids
# (0.. Graphics Options, 100.. Display Options) to cvars in this order.
# A Toggle row's left arrow sits about 296 px right of its label (measured on
# the running screen), so a label longer than "Character Shadows" runs into it.
# "*": the renderer or the window reads the option at start (applies after a restart).
GRAPHICS_ROWS = [
    ("Gfx0", "GFX_OPT_RT_SHADOWS", "RT Shadows", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx1", "GFX_OPT_SHADOW_SOFTNESS", "Shadow Softness", "GFX_OPT_SOFTNESS_LEVELS",
     "Hard|Low|Medium|High|Ultra"),
    ("Gfx2", "GFX_OPT_CHARACTER_SHADOWS", "Character Shadows", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx3", "GFX_OPT_RT_REFLECTIONS", "RT Reflections", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx4", "GFX_OPT_DLSS", "NVIDIA DLSS", "GFX_OPT_DLSS_MODES", "Off|Performance|Balanced|Quality|DLAA"),
    ("Gfx5", "GFX_OPT_TIME_OF_DAY", "Time of Day", "GFX_OPT_TIMES",
     "Game|Midnight|Dawn|Morning|Noon|Afternoon|Sunset|Night"),
    ("Gfx6", "GFX_OPT_DAY_CYCLE", "Day Cycle", "GFX_OPT_CYCLES", "Off|10 min|30 min|60 min|2 h|4 h|6 h"),
    ("Gfx7", "GFX_OPT_TEXTURE_FILTER", "Texture Filter", "GFX_OPT_FILTERS", "Original|2x|4x|8x|16x"),
    ("Gfx8", "GFX_OPT_FXAA", "FXAA*", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx9", "GFX_OPT_SHADOW_MAPS", "Shadow Maps*", "GFX_OPT_SHADOW_SIZES", "Original|2x|4x"),
    ("Gfx10", "GFX_OPT_MSAA", "MSAA*", "GFX_OPT_MSAA_LEVELS", "Off|4x|8x"),
]
DISPLAY_ROWS = [
    ("Dsp0", "DSP_OPT_RESOLUTION", "Resolution*", "DSP_OPT_RESOLUTIONS",
     "960 x 540|1280 x 720|1600 x 900|1920 x 1080|2560 x 1440|3840 x 2160"),
    ("Dsp1", "DSP_OPT_MODE", "Display Mode*", "DSP_OPT_MODES", "Fullscreen|Windowed"),
    ("Dsp2", "DSP_OPT_VSYNC", "VSync*", "GFX_OPT_OFF_ON", "Off|On"),
    ("Dsp3", "DSP_OPT_FPS", "FPS Limit*", "DSP_OPT_FPS_LIMITS", "30|60|120|Unlimited"),
    ("Dsp4", "DSP_OPT_RENDER_SCALE", "Render Scale*", "DSP_OPT_SCALES", "Auto|100%|125%|150%|200%|300%|400%"),
]
ROWS = GRAPHICS_ROWS + DISPLAY_ROWS
# The screens: (class, its label key = pause menu button, its title key, title,
# row id base, rows, row step).
SCREENS = [
    ("ScrGraphicsOpt", "Label_ScrGraphicsOpt", "Title_ScrGraphicsOpt", "GRAPHICS OPTIONS (* RESTART)", 0,
     GRAPHICS_ROWS, 30.0),
    ("ScrDisplayOpt", "Label_ScrDisplayOpt", "Title_ScrDisplayOpt", "DISPLAY OPTIONS (* RESTART)", 100,
     DISPLAY_ROWS, 32.8),
]
TEXTS = [("Label_ScrGraphicsOpt", "GRAPHICS OPTIONS"), ("Label_ScrDisplayOpt", "DISPLAY OPTIONS")] + \
    [(t, v) for _, _, t, v, *_ in SCREENS] + \
    [("Options_RestartNote", "This option takes effect after a restart. Restart the game now? "
                             "Unsaved progress will be lost."),
     ("Options_RestartNow", "RESTART NOW"), ("Options_RestartLater", "LATER"),
     ("SUP_PRESS_ENTER", "PRESS ENTER")] + \
    sorted({(r[1], r[2]) for r in ROWS} | {(r[3], r[4]) for r in ROWS})


def key_texts():
    """OPTIONS and Keys Mapping: the titles, the rows' labels and every key name
    the bindings know (KEYNAME_<code>, from the SDK's table, key_names.py)."""
    import key_names
    mouse = {"LMB": "Left Mouse", "RMB": "Right Mouse", "MMB": "Middle Mouse"}
    out = [("Label_ScrOptionsMenu", "OPTIONS"), ("Label_ScrKeysOpt", "KEYS MAPPING"),
           ("Label_ScrMouseOpt", "MOUSE OPTIONS"), ("KEYS_DEFAULTS", "DEFAULTS"), ("MouseOpt_Sens_Slider", "Mouse Sensitivity"),
           ("KEYS_PRESS", "PRESS A KEY..."), ("KEYNAME_NONE", "-"),
           ("KEYMOD_SHIFT", "Shift+"), ("KEYMOD_CTRL", "Ctrl+"), ("KEYMOD_ALT", "Alt+")]
    for title_key, title, page_rows in KEY_PAGES:
        out.append((title_key, title))
        out += [(key, label) for _, key, label in page_rows]
    out += [("KEYNAME_%d" % code, mouse.get(name, name)) for code, name in key_names.sdk_key_names()]
    return out

SCREEN_PATH = "screens/misc/ScrGraphicsOpt"
DISPLAY_SCREEN_PATH = "screens/misc/ScrDisplayOpt"
OPTIONS_SCREEN_PATH = "screens/misc/ScrOptionsMenu"
KEYS_SCREEN_PATH = "screens/misc/ScrKeysOpt"
MOUSE_SCREEN_PATH = "screens/misc/ScrMouseOpt"
# The mouse poll: the focus manager's first items looked at, and their parents up to the stage.
MOUSE_ITEMS, MOUSE_DEPTH = 16, 6
# The mouse poll's wait around a screen change (polls of 33 ms): until the new
# screen is loaded (BLOCKED, any large count), then SETTLE more (~0.66 s).
SCREEN_BLOCKED, SCREEN_SETTLE = 100000, 20
# An IconMenu's row (SCRFORTRESS: mIconMenu at y 547.8, its icons and arrows
# drawn about 40..120 below; the selected icon ~90 wide, at the row's x):
# a click this far below its origin, within ICON_ROW_HALF of x is the icon.
ICON_ROW_TOP, ICON_ROW_BOTTOM, ICON_ROW_HALF = 20, 140, 50
# A Slider (components/Slider, Audio and Mouse Options) in its own units: mSection1..10
# 17 apart from 489.8, the mirrored left arrow just before them, the right one after
# (on screen 2026-11-09: left arrow 474..485, squares 490..660, right arrow 668..680).
# In tenths of a unit: the code pushes whole numbers only.
SLIDER_FIRST, SLIDER_SECTION, SLIDER_SECTIONS = 4898, 170, 10
SLIDER_LEFT_END, SLIDER_RIGHT_END = 4550, 7000
# A click this near (stage units, both axes) an arrow's origin is on the arrow: the
# scroll arrows of Moves List and Statistics, the page arrows of Keys Mapping.
ARROW_HIT = 22
# Keys Mapping's page row: at the title's height (rows' units: the first row's text
# at stage y 274, the title at 154), the title centred at the row's x 495 (stage 640),
# about 14 units a character (the user's screenshot 2026-11-09: 27 characters, 384 wide).
KEY_TITLE_Y, KEY_TITLE_CENTER, KEY_TITLE_CHAR = 103, 495, 14.2
ICON_PATH = "artassets/MenuIcons/IconGraphicsOpt"
# Gameplay Options' rows: 32.8 apart from the top one (X-Axis Inversion).
ROW_X, ROW_FIRST_Y, ROW_STEP = -394.0, 222.85, 32.8

ENTRY_FLAGS = 272  # as the game's screens have them in ui.AST


def movie_of(archive, name):
    """The (inner archive, data, constants) of a ui.AST entry."""
    inner = apt_edit.Archive(archive.data(archive.find(name)))
    data = const = None
    for e in inner.entries:
        d = inner.data(e)
        if d.startswith(apt.MAGIC_DATA):
            data = (e["hash"], d)
        elif d.startswith(apt.MAGIC_CONST):
            const = (e["hash"], d)
    return inner, data, const


def repack(inner, data, const, new_data, new_const, name=None):
    """The movie's archive with its data and constants replaced. A new screen
    copied from a game's (`name` given) gets its own hash suffix: the game
    keeps a loaded movie's files by hash, so OPTIONS (a copy of the pause menu,
    opened while the pause menu is loaded) got the pause menu's files and the
    loader relocated them a second time (the crash entering OPTIONS,
    sub_826ADD40, 2026-11-09)."""
    suffix = None if name is None else struct.pack("<I", apt_edit.fnv1a64(name) & 0xFFFFFFFF)
    return inner.build(replace={data[0]: new_data, const[0]: new_const}, suffix=suffix)


def find_action(m, frame_items, holding):
    """The action item of a frame whose code pushes the string `holding`."""
    movie = apt.Movie(bytes(m.b.b), m.c.write())
    for it in frame_items:
        if m.b.u32(it) != 1:
            continue
        for ins in movie.disassemble(m.b.u32(it + 4)):
            if holding in ins.text:
                return it
    raise LookupError(holding)


def find_init_action(m, frame_items, sprite):
    for it in frame_items:
        if m.b.u32(it) == 8 and m.b.u32(it + 4) == sprite:
            return it
    raise LookupError(sprite)


# --- MAIN ------------------------------------------------------------------

def edit_main(data, const):
    m = apt_edit.MovieEdit(data, const)
    rec, items = m.frame_items(m.root)
    after = find_action(m, items, "'screens/misc/ScrGameplayOpt'")
    code = m.code()
    at = code.start()
    code.set_member(("_global", "SCREEN"), "GRAPHICS_OPT", SCREEN_PATH)
    code.set_member(("_global", "SCREEN"), "DISPLAY_OPT", DISPLAY_SCREEN_PATH)
    code.set_member(("_global", "SCREEN"), "OPTIONS_MENU", OPTIONS_SCREEN_PATH)
    code.set_member(("_global", "SCREEN"), "KEYS_OPT", KEYS_SCREEN_PATH)
    code.set_member(("_global", "SCREEN"), "MOUSE_OPT", MOUSE_SCREEN_PATH)
    code.set_member(("_global", "ICON"), "GRAPHICS_OPT", ICON_PATH)

    # Every 100 ms: is a menu open? (FlowMgr.CurInterfaceState "IG" is the game,
    # "FE" the front end; PauseMode not PAUSEMODE_NONE a pause menu.) The mouse
    # moves the cursor in menus and the camera in the game (rawinput_kbm_driver).
    # A movie clip kept in a variable is not safe after its screen goes: Apt
    # frees it and the reference reads freed memory (the user's crash on
    # OPTIONS, 2026-11-09). Every kept clip (srActiveFM's items, srIconMenu,
    # srPressStart) is noted with _global.srScreenGen, which the screen
    # manager's loads and unloads raise, and used only while it is the same.
    code.set_member(("_global",), "srScreenGen", 0)
    # The mouse poll walks gFM's items, which still name the old screen's
    # clips while a screen changes (freed: the crash entering OPTIONS with the
    # mouse in use). It waits _global.srSettle polls: from a screen change or
    # load until SCREEN_SETTLE polls after the new screen is loaded.
    code.set_member(("_global",), "srSettle", 0)

    def same_screen(note):
        """Branch (to land) taken when _global.<note> was kept on another screen."""
        code.path(("r", 2), note + "Gen")
        code.path(("r", 2), "srScreenGen")
        code.op("Equals2")
        code.op("Not")
        return code.branch_if_true()

    def menu_poll():
        body = code.begin_method(registers=4)
        g = ("r", 2)
        code.path(g, "SettingsAdapter")
        code.push(None)
        code.op("Equals2")
        missing = code.branch_if_true()
        code.path(g, "FlowMgr")
        code.push(None)
        code.op("Equals2")
        no_flow = code.branch_if_true()
        code.call_method((g, "SettingsAdapter"), "SetMenuActive", [lambda: (
            code.path(g, "FlowMgr", "CurInterfaceState"), code.push("IG"), code.op("Equals2"), code.op("Not"),
            code.path(g, "FlowMgr", "PauseMode"), code.path(g, "FlowMgr", "PAUSEMODE_NONE"), code.op("Equals2"),
            code.op("Not"), code.op("Or"))])
        code.land(no_flow)
        code.land(missing)
        # Once each class is there (several movies carry their own copy; the
        # first defined wins): CafeFW_FocusManager notes the manager taking
        # input or a selection (_global.srActiveFM, for the mouse poll), and
        # ButtonIcon's SetIcon becomes _global.srSetIcon (keys' icons).
        code.path(g, "BistroFW_ScreenManager", "prototype")
        code.set_register(3)
        code.push(("r", 3), None)
        code.op("Equals2")
        no_sm = code.branch_if_true()
        code.path(("r", 3), "srWrapped")
        sm_wrapped = code.branch_if_true()
        for method, saved, settle, raise_only in (
                ("ChangeScreenByLevel", "srCSBL", SCREEN_BLOCKED, True), ("LoadOverlayScreen", "srLOS", SCREEN_BLOCKED, True),
                ("UnloadOverlayScreen", "srUOS", SCREEN_SETTLE, True), ("ScreenLoaded", "srSL", SCREEN_SETTLE, False)):
            code.set_member((("r", 3),), saved, lambda m=method: code.path(("r", 3), m))
            code.set_member((("r", 3),), method, lambda s_=saved, v=settle, r=raise_only: counting(s_, v, r))
        code.set_member((("r", 3),), "srWrapped", True)
        code.land(sm_wrapped)
        code.land(no_sm)
        code.path(g, "CafeFW_FocusManager", "prototype")
        code.set_register(3)
        code.push(("r", 3), None)
        code.op("Equals2")
        no_fm = code.branch_if_true()
        code.path(("r", 3), "srWrapped")
        wrapped = code.branch_if_true()
        for method, saved in (("handleDefaultInputs", "srHDI"), ("selectItem", "srSel")):
            code.set_member((("r", 3),), saved, lambda m=method: code.path(("r", 3), m))
            code.set_member((("r", 3),), method, lambda s_=saved: noting(s_))
        code.set_member((("r", 3),), "srWrapped", True)
        code.land(wrapped)
        code.land(no_fm)
        # Smart Camera (Gameplay Options' AutoCenter: SettingsAdapter.Get/SetAutoCenter,
        # 0 Off 1 On) per input: off for keyboard and mouse (A/D would turn the camera,
        # the user 2026-11-09), the game's own for a pad. Kept every poll, as loading a
        # save sets the game's value again: with keyboard and mouse a value the game set
        # is the pad's and the camera goes back off. A change by hand (Gameplay Options
        # calls SetAutoCenter, wrapped once here) is kept for the input in use.
        sa = (g, "SettingsAdapter")
        code.path(g, "SettingsAdapter", "srSetAC")
        code.push(None)
        code.op("Equals2")
        code.op("Not")
        ac_wrapped = code.branch_if_true()
        code.path(g, "SettingsAdapter", "SetAutoCenter")
        code.push(None)
        code.op("Equals2")
        ac_missing = code.branch_if_true()
        code.set_member(sa, "srSetAC", lambda: code.path(g, "SettingsAdapter", "SetAutoCenter"))
        code.set_member(sa, "SetAutoCenter", set_auto_center)
        # log: -8, whether the wrap held (srSetAC there), the value now
        code.call_method(sa, "MenuMouseLog", [-8, lambda: (code.path(g, "SettingsAdapter", "srSetAC"), code.push(None),
                         code.op("Equals2"), code.op("Not")), lambda: code.call_method(sa, "GetAutoCenter", keep=True),
                         0, 0, 0])
        code.land(ac_missing)
        code.land(ac_wrapped)
        # FlowMgr.ChangeInterfaceState("IG") (a new game, a load, the pause closed)
        # takes the settings into the game: with keyboard and mouse Smart Camera off
        # first (the value set later in the game is not taken; the user 2026-11-09).
        # Not on the front end's screen of icons (FORTRESS, METROPOLIS: an IconMenu
        # kept on this screen): its METROPOLIS calls ChangeInterfaceState and any
        # wrap of it crashed the game there (2026-11-09, "Call to invalid ... function
        # at guest address 0x20"), so the game's own is put back while it is shown
        code.path(g, "FlowMgr", "CurInterfaceState")
        code.push("FE")
        code.op("Equals2")
        code.op("Not")
        not_fe = code.branch_if_true()
        code.path(g, "srIconMenu")
        code.push(None)
        code.op("Equals2")
        no_icons = code.branch_if_true()
        code.path(g, "srIconMenuGen")
        code.path(g, "srScreenGen")
        code.op("Equals2")
        code.op("Not")
        icons_gone = code.branch_if_true()
        code.path(g, "FlowMgr", "srCIS")
        code.push(None)
        code.op("Equals2")
        not_wrapped = code.branch_if_true()
        code.set_member((g, "FlowMgr"), "ChangeInterfaceState", lambda: code.path(g, "FlowMgr", "srCIS"))
        code.set_member((g, "FlowMgr"), "srCIS", lambda: code.push(None))
        code.land(not_wrapped)
        icons_shown = code.branch()
        code.land(not_fe)
        code.land(no_icons)
        code.land(icons_gone)
        code.path(g, "FlowMgr", "srCIS")
        code.push(None)
        code.op("Equals2")
        code.op("Not")
        cis_wrapped = code.branch_if_true()
        code.path(g, "FlowMgr", "ChangeInterfaceState")
        code.push(None)
        code.op("Equals2")
        cis_missing = code.branch_if_true()
        code.set_member((g, "FlowMgr"), "srCIS", lambda: code.path(g, "FlowMgr", "ChangeInterfaceState"))
        code.set_member((g, "FlowMgr"), "ChangeInterfaceState", change_interface_state)
        code.land(cis_missing)
        code.land(cis_wrapped)
        code.land(icons_shown)
        code.path(g, "SettingsAdapter", "srSetAC")
        code.push(None)
        code.op("Equals2")
        no_ac = code.branch_if_true()
        code.path(g, "srCamKbm")
        code.push(None)
        code.op("Equals2")
        code.op("Not")
        kbm_known = code.branch_if_true()
        code.set_member((g,), "srCamKbm", 0)
        code.land(kbm_known)
        code.call_method(sa, "KbmActive", keep=True)
        code.op("Not")
        cam_pad = code.branch_if_true()
        # keyboard and mouse: a value the game set is the pad's; ours back
        code.call_method(sa, "GetAutoCenter", keep=True)
        code.path(g, "srCamKbm")
        code.op("Equals2")
        cam_kept = code.branch_if_true()
        code.set_member((g,), "srCamPad", lambda: code.call_method(sa, "GetAutoCenter", keep=True))
        code.call_method(sa, "srSetAC", [lambda: code.path(g, "srCamKbm")])
        # log: -7, the game's value now (after ours), ours, the pad's
        code.call_method(sa, "MenuMouseLog", [-7, lambda: code.call_method(sa, "GetAutoCenter", keep=True),
                         lambda: code.path(g, "srCamKbm"), lambda: code.path(g, "srCamPad"), 0, 0])
        code.land(cam_kept)
        # entering the game (a new game, a load, the pause closed): set again for
        # 30 polls (3 s), as the camera takes the value when it is made, which a
        # new game does after the menus already set it (the user 2026-11-09: "nie
        # pomogło, jak robię nową grę"; Gameplay Options works as it sets it in the game)
        code.path(g, "FlowMgr", "CurInterfaceState")
        code.push("IG")
        code.op("Equals2")
        code.path(g, "FlowMgr", "PauseMode")
        code.path(g, "FlowMgr", "PAUSEMODE_NONE")
        code.op("Equals2")
        code.op("And")
        code.set_register(3)
        code.push(("r", 3))
        code.op("Not")
        not_ig = code.branch_if_true()
        code.path(g, "srWasIG")
        burst_on = code.branch_if_true()
        code.set_member((g,), "srCamBurst", 30)
        code.land(burst_on)
        code.land(not_ig)
        code.set_member((g,), "srWasIG", ("r", 3))
        code.push(0)
        code.path(g, "srCamBurst")
        code.op("LessThan2")
        code.op("Not")
        no_burst = code.branch_if_true()
        code.set_member((g,), "srCamBurst", lambda: (code.path(g, "srCamBurst"), code.push(1), code.op("Subtract")))
        code.call_method(sa, "srSetAC", [lambda: code.path(g, "srCamKbm")])
        code.land(no_burst)
        code.set_member((g,), "srCamMode", 1)
        cam_done = code.branch()
        code.land(cam_pad)
        # a pad: back to its own value once when it takes over, then follow the game
        code.path(g, "srCamMode")
        code.push(1)
        code.op("Equals2")
        code.op("Not")
        was_pad = code.branch_if_true()
        code.path(g, "srCamPad")
        code.push(None)
        code.op("Equals2")
        no_pad_value = code.branch_if_true()
        code.call_method(sa, "srSetAC", [lambda: code.path(g, "srCamPad")])
        code.land(no_pad_value)
        code.land(was_pad)
        code.set_member((g,), "srCamPad", lambda: code.call_method(sa, "GetAutoCenter", keep=True))
        code.set_member((g,), "srCamMode", 0)
        code.land(cam_done)
        code.land(no_ac)
        # the Press Start screen's text follows the input in use
        other_screen = same_screen("srPressStart")
        code.path(g, "srPressStart")
        code.set_register(3)
        code.push(("r", 3), None)
        code.op("Equals2")
        no_start = code.branch_if_true()
        code.path(("r", 3), "_parent")
        code.push(None)
        code.op("Equals2")
        start_gone = code.branch_if_true()
        code.call_method((g, "SettingsAdapter"), "KbmActive", keep=True)
        code.path(("r", 3), "srKbm")
        code.op("Equals2")
        start_same = code.branch_if_true()
        code.call_method((("r", 3),), "SetStartText")
        code.land(start_same)
        code.land(start_gone)
        code.land(no_start)
        code.land(other_screen)
        # IconMenu (Fortress, Control Room: icons in a row, LEFT/RIGHT turn
        # it): the one set up or turned last is _global.srIconMenu
        code.path(g, "IconMenu", "prototype")
        code.set_register(3)
        code.push(("r", 3), None)
        code.op("Equals2")
        no_menu = code.branch_if_true()
        code.path(("r", 3), "srWrapped")
        menu_wrapped = code.branch_if_true()
        for method, saved in (("SetIcons", "srSetIcons"), ("Next", "srNext"), ("Previous", "srPrev")):
            code.set_member((("r", 3),), saved, lambda m=method: code.path(("r", 3), m))
            code.set_member((("r", 3),), method, lambda s_=saved: noting(s_, "srIconMenu"))
        # the game's CheckArrows dims the left arrow on the first icon and the right
        # one on the last but never lights them again, so with two icons (METROPOLIS,
        # FORTRESS) both stay dim after one turn each way (the user 2026-11-09)
        code.set_member((("r", 3),), "CheckArrows", check_arrows)
        code.set_member((("r", 3),), "srWrapped", True)
        code.land(menu_wrapped)
        code.land(no_menu)
        code.path(g, "ButtonIcon", "prototype")
        code.set_register(3)
        code.push(("r", 3), None)
        code.op("Equals2")
        no_icon = code.branch_if_true()
        code.path(("r", 3), "SetIcon")
        code.path(g, "srSetIcon")
        code.op("Equals2")
        same = code.branch_if_true()
        code.set_member((("r", 3),), "SetIcon", lambda: code.path(g, "srSetIcon"))
        code.land(same)
        code.land(no_icon)
        code.end_method(body)

    def check_arrows():
        # function () { each arrow IconMenu.INACTIVE_ALPHA at its end, else ACTIVE_ALPHA }
        body = code.begin_method(registers=3)
        last = lambda: (code.path(("r", 1), "mVisibleIcons"), code.push(1), code.op("Subtract"))
        for arrow, end in (("mLeftArrow", lambda: code.push(0)), ("mRightArrow", last)):
            code.path(("r", 1), "mSelectedIndex")
            end()
            code.op("Equals2")
            code.op("Not")
            active = code.branch_if_true()
            code.set_member((("r", 1), arrow), "_alpha", lambda: code.path(("r", 2), "IconMenu", "INACTIVE_ALPHA"))
            done = code.branch()
            code.land(active)
            code.set_member((("r", 1), arrow), "_alpha", lambda: code.path(("r", 2), "IconMenu", "ACTIVE_ALPHA"))
            code.land(done)
        code.end_method(body)

    def change_interface_state():
        # function (state, b, c) { if (state == "IG" && keyboard and mouse) Smart Camera
        # off; return this.srCIS(state, b, c); }
        body = code.begin_method(params=("state", "b", "c"), registers=6)
        sa_ = (("r", 2), "SettingsAdapter")
        code.push(("r", 3), "IG")
        code.op("Equals2")
        code.op("Not")
        not_ig = code.branch_if_true()
        code.call_method(sa_, "KbmActive", keep=True)
        code.op("Not")
        pad = code.branch_if_true()
        code.path(("r", 2), "SettingsAdapter", "srSetAC")
        code.push(None)
        code.op("Equals2")
        no_ac = code.branch_if_true()
        code.call_method(sa_, "srSetAC", [lambda: code.path(("r", 2), "srCamKbm")])
        code.call_method(sa_, "MenuMouseLog", [-6, lambda: code.call_method(sa_, "GetAutoCenter", keep=True),
                         0, 0, 0, 0])
        code.land(no_ac)
        code.land(pad)
        code.land(not_ig)
        code.call_method((("r", 1),), "srCIS", [("r", 3), ("r", 4), ("r", 5)], keep=True)
        code.op("Return")
        code.end_method(body)

    def set_auto_center():
        # SettingsAdapter.SetAutoCenter(v) by hand: kept for the input in use, then the game's
        body = code.begin_method(params=("v",), registers=4)
        code.call_method((("r", 2), "SettingsAdapter"), "KbmActive", keep=True)
        code.op("Not")
        pad = code.branch_if_true()
        code.set_member((("r", 2),), "srCamKbm", ("r", 3))
        done = code.branch()
        code.land(pad)
        code.set_member((("r", 2),), "srCamPad", ("r", 3))
        code.land(done)
        code.call_method((("r", 2), "SettingsAdapter"), "srSetAC", [("r", 3)])
        code.end_method(body)

    def noting(saved, note="srActiveFM"):
        # function (a, b) { _global.<note> = this; return this.<saved>(a, b); }
        body = code.begin_method(params=("a", "b"), registers=5)
        code.set_member((("r", 2),), note, ("r", 1))
        code.set_member((("r", 2),), note + "Gen", lambda: code.path(("r", 2), "srScreenGen"))
        code.call_method((("r", 1),), saved, [("r", 3), ("r", 4)], keep=True)
        code.op("Return")
        code.end_method(body)

    def counting(saved, settle, raise_only):
        # function (a, b, c) { _global.srScreenGen++; srSettle = settle (or the
        # larger of the two); return this.<saved>(a, b, c); }
        body = code.begin_method(params=("a", "b", "c"), registers=6)
        # only a load, an unload or a change frees clips: ScreenLoaded comes
        # after the new screen's InitGui kept its own (the icon row did not
        # answer the mouse, 2026-11-09), so it leaves the generation as it is
        if raise_only:
            code.set_member((("r", 2),), "srScreenGen", lambda: (
                code.path(("r", 2), "srScreenGen"), code.push(1), code.op("Add2")))
        keep = None
        if raise_only:
            code.push(settle)
            code.path(("r", 2), "srSettle")
            code.op("LessThan2")  # settle < srSettle: keep the larger
            keep = code.branch_if_true()
        code.set_member((("r", 2),), "srSettle", settle)
        if keep is not None:
            code.land(keep)
        code.call_method((("r", 1),), saved, [("r", 3), ("r", 4), ("r", 5)], keep=True)
        code.op("Return")
        code.end_method(body)

    code.set_member(("_global",), "srMenuPoll", menu_poll)
    # Moves List's and Statistics' InitGui (edit_scroll_screen): the screen noted for
    # the mouse poll's scroll arrows, then the game's own
    code.set_member(("_global",), "srNoteScroll", lambda: noting("srOrigInit", "srScrollScreen"))
    define_set_icon(code)

    # Every 33 ms: the mouse in the menus. The item under the cursor (the focus
    # manager's items, enabled and visible, their rectangle on the stage from
    # _x/_y and _width/_height through their parents' positions and scales) is
    # selected when the cursor moves or clicks; a left click then asks the game
    # for A, or RIGHT on an option row (one with GetIndex): the press reaches the
    # screen as a pad's would (SettingsAdapter.PressPad, rawinput_kbm_driver).
    def icon_row():
        """A click beside every item, on the icon row (_global.srIconMenu, still
        on the stage and shown): on the selected icon A, left of it LEFT, right
        of it RIGHT. The row's origin is the selected icon's centre in x; the
        icons and arrows lie ICON_ROW_TOP..ICON_ROW_BOTTOM below it. Uses
        r5..r7, r9..r11."""
        g_main = ("r", 2)
        ends = [same_screen("srIconMenu")]
        code.path(g_main, "srIconMenu")
        code.set_register(5)
        code.push(("r", 5), None)
        code.op("Equals2")
        ends.append(code.branch_if_true())
        code.path(("r", 5), "_parent")
        code.push(None)
        code.op("Equals2")
        ends.append(code.branch_if_true())
        code.path(("r", 5), "_visible")
        code.op("Not")
        ends.append(code.branch_if_true())
        code.path(("r", 5), "_x")
        code.set_register(6)
        code.path(("r", 5), "_y")
        code.set_register(7)
        code.path(("r", 5), "_parent")
        code.set_register(9)
        tops = []
        for _ in range(MOUSE_DEPTH):
            code.push(("r", 9), None)
            code.op("Equals2")
            tops.append(code.branch_if_true())
            for pos, scale, at in (("_x", "_xscale", 6), ("_y", "_yscale", 7)):
                code.push(("r", at))
                code.path(("r", 9), scale)
                code.op("Multiply")
                code.push(100)
                code.op("Divide")
                code.path(("r", 9), pos)
                code.op("Add2")
                code.set_register(at)
            code.path(("r", 9), "_parent")
            code.set_register(9)
        for t in tops:
            code.land(t)
        code.call_method((g_main, "SettingsAdapter"), "MenuMouseLog", [("r", 3), ("r", 4), ("r", 6), ("r", 7), -3, 0])
        # r10 = x - row x, r11 = y - row y
        code.push(("r", 3), ("r", 6))
        code.op("Subtract")
        code.set_register(10)
        code.push(("r", 4), ("r", 7))
        code.op("Subtract")
        code.set_register(11)
        for test in ((11, ICON_ROW_TOP, True), (11, ICON_ROW_BOTTOM, False)):
            reg, limit, below = test
            # outside: dy < top, or bottom < dy
            if below:
                code.push(("r", reg), limit)
            else:
                code.push(limit, ("r", reg))
            code.op("LessThan2")
            ends.append(code.branch_if_true())
        for limit, pad in ((-ICON_ROW_HALF, 3), (ICON_ROW_HALF, None)):
            if pad == 3:
                code.push(("r", 10))  # dx < 0 - half: LEFT
                code.push(0, ICON_ROW_HALF)
                code.op("Subtract")
                code.op("LessThan2")
                code.op("Not")
                other = code.branch_if_true()
                code.call_method((g_main, "SettingsAdapter"), "PressPad", [3])
                ends.append(code.branch())
                code.land(other)
            else:
                code.push(limit, ("r", 10))  # half < dx: RIGHT, else A
                code.op("LessThan2")
                code.op("Not")
                other = code.branch_if_true()
                code.call_method((g_main, "SettingsAdapter"), "PressPad", [2])
                ends.append(code.branch())
                code.land(other)
                code.call_method((g_main, "SettingsAdapter"), "PressPad", [1])
        for e in ends:
            code.land(e)

    def slider_click():
        """A click on a Slider (r8, its rectangle r13..r16): its left arrow LEFT,
        its right arrow RIGHT, a square that square's value (the slider set a step
        short of it, then one arrow, so the screen takes the value as from a pad).
        Uses r5 (x in tenths of the slider's units), r6 (the value asked), r7 (the value now)."""
        it = ("r", 8)
        sa_ = (("r", 2), "SettingsAdapter")
        code.push(("r", 3), ("r", 13))
        code.op("Subtract")
        code.path(it, "_width")
        code.op("Multiply")
        code.push(10)
        code.op("Multiply")
        code.push(("r", 15))
        code.op("Divide")
        code.set_register(5)
        ends = []
        # left of the squares: the left arrow (not the label further left)
        code.push(("r", 5), SLIDER_FIRST)
        code.op("LessThan2")
        code.op("Not")
        not_left = code.branch_if_true()
        code.push(SLIDER_LEFT_END, ("r", 5))
        code.op("LessThan2")
        code.op("Not")
        ends.append(code.branch_if_true())
        code.call_method(sa_, "PressPad", [3])
        ends.append(code.branch())
        code.land(not_left)
        last = SLIDER_FIRST + SLIDER_SECTION * SLIDER_SECTIONS
        code.push(("r", 5), last)
        code.op("LessThan2")
        on_squares = code.branch_if_true()
        code.push(("r", 5), SLIDER_RIGHT_END)
        code.op("LessThan2")
        code.op("Not")
        ends.append(code.branch_if_true())
        code.call_method(sa_, "PressPad", [2])
        ends.append(code.branch())
        code.land(on_squares)
        # the square under the cursor: k = 1..10, value min + k * step (at most max)
        for k in range(1, SLIDER_SECTIONS + 1):
            code.push(("r", 5), SLIDER_FIRST + SLIDER_SECTION * (k - 1))
            code.op("LessThan2")
            before = code.branch_if_true()
            code.path(it, "mMinValue")
            code.push(k)
            code.path(it, "mStepValue")
            code.op("Multiply")
            code.op("Add2")
            code.set_register(6)
            code.land(before)
        code.path(it, "mMaxValue")
        code.push(("r", 6))
        code.op("LessThan2")
        code.op("Not")
        in_range = code.branch_if_true()
        code.path(it, "mMaxValue")
        code.set_register(6)
        code.land(in_range)
        code.call_method((it,), "GetValue", keep=True)
        code.set_register(7)
        for press, toward in ((2, "Subtract"), (3, "Add2")):
            # RIGHT when now < asked (set a step below), LEFT when asked < now (a step above)
            if press == 2:
                code.push(("r", 7), ("r", 6))
            else:
                code.push(("r", 6), ("r", 7))
            code.op("LessThan2")
            code.op("Not")
            other = code.branch_if_true()
            code.call_method((it,), "SetValue", [lambda t=toward: (
                code.push(("r", 6)), code.path(it, "mStepValue"), code.op(t))])
            code.call_method(sa_, "PressPad", [press])
            ends.append(code.branch())
            code.land(other)
        for e in ends:
            code.land(e)

    def near_clip(get_clip):
        """Branches (to land) taken unless the cursor (r3, r4) is within ARROW_HIT of
        the clip's origin on the stage and the clip is shown. Uses r5..r7, r9."""
        skips = []
        get_clip()
        code.set_register(5)
        code.push(("r", 5), None)
        code.op("Equals2")
        skips.append(code.branch_if_true())
        code.path(("r", 5), "_visible")
        code.op("Not")
        skips.append(code.branch_if_true())
        code.path(("r", 5), "_x")
        code.set_register(6)
        code.path(("r", 5), "_y")
        code.set_register(7)
        code.path(("r", 5), "_parent")
        code.set_register(9)
        tops = []
        for _ in range(MOUSE_DEPTH + 2):
            code.push(("r", 9), None)
            code.op("Equals2")
            tops.append(code.branch_if_true())
            for pos, scale, at in (("_x", "_xscale", 6), ("_y", "_yscale", 7)):
                code.push(("r", at))
                code.path(("r", 9), scale)
                code.op("Multiply")
                code.push(100)
                code.op("Divide")
                code.path(("r", 9), pos)
                code.op("Add2")
                code.set_register(at)
            code.path(("r", 9), "_parent")
            code.set_register(9)
        for t in tops:
            code.land(t)
        for mouse, origin in ((3, 6), (4, 7)):
            for a, b in ((mouse, origin), (origin, mouse)):
                code.push(("r", a), ("r", b))
                code.op("Subtract")
                code.push(ARROW_HIT)
                code.op("LessThan2")
                code.op("Not")
                skips.append(code.branch_if_true())
        return skips

    def arrow_clicks():
        """A click on nothing: the scroll arrows of the screen noted as srScrollScreen
        (Moves List, Statistics: its HandleInput(UP or DOWN), as from a pad) and the
        page arrows of Keys Mapping (srPageRow: LEFT or RIGHT)."""
        g_main = ("r", 2)
        other = same_screen("srScrollScreen")
        code.path(g_main, "srScrollScreen")
        code.push(None)
        code.op("Equals2")
        none = code.branch_if_true()
        for arrow, key in (("mUpArrow", "UP"), ("mDownArrow", "DOWN")):
            skips = near_clip(lambda a=arrow: code.path(g_main, "srScrollScreen", a))
            code.call_method((g_main, "srScrollScreen"), "HandleInput",
                             [lambda k=key: code.path(g_main, "INPUTCODE", k)])
            for s in skips:
                code.land(s)
        code.land(none)
        code.land(other)
        other = same_screen("srPageRow")
        code.path(g_main, "srPageRow")
        code.push(None)
        code.op("Equals2")
        none = code.branch_if_true()
        for arrow, press in (("mLeftArrow", 3), ("mRightArrow", 2)):
            skips = near_clip(lambda a=arrow: code.path(g_main, "srPageRow", a))
            code.call_method((g_main, "SettingsAdapter"), "PressPad", [press])
            for s in skips:
                code.land(s)
        code.land(none)
        code.land(other)

    def option_click():
        """A click on an option row (a Toggle, it has GetIndex; r8, its rectangle
        r13..r16): between its label and the middle of its value (halfway between
        mLeftArrow and mRightArrow) LEFT, else RIGHT (the label too, as before; the
        user 2026-11-09: the left arrow of Controller Options went right).
        Uses r5 (x in the row's units), r6 (the middle)."""
        it = ("r", 8)
        sa_ = (("r", 2), "SettingsAdapter")
        ends = []
        code.path(it, "mLeftArrow")
        code.push(None)
        code.op("Equals2")
        code.path(it, "mRightArrow")
        code.push(None)
        code.op("Equals2")
        code.op("Or")
        no_arrows = code.branch_if_true()
        code.push(("r", 3), ("r", 13))
        code.op("Subtract")
        code.path(it, "_width")
        code.op("Multiply")
        code.push(("r", 15))
        code.op("Divide")
        code.set_register(5)
        code.path(it, "mLeftArrow", "_x")
        code.path(it, "mRightArrow", "_x")
        code.op("Add2")
        code.push(2)
        code.op("Divide")
        code.set_register(6)
        # left of the left arrow by more than its width: the label, RIGHT
        code.push(("r", 5))
        code.path(it, "mLeftArrow", "_x")
        code.path(it, "mLeftArrow", "_width")
        code.op("Subtract")
        code.op("LessThan2")
        label = code.branch_if_true()
        code.push(("r", 5), ("r", 6))
        code.op("LessThan2")
        code.op("Not")
        right = code.branch_if_true()
        code.call_method(sa_, "PressPad", [3])
        ends.append(code.branch())
        code.land(right)
        code.land(label)
        code.land(no_arrows)
        code.call_method(sa_, "PressPad", [2])
        for e in ends:
            code.land(e)

    def mouse_poll():
        # r3 x, r4 y, r5 item, r6/r7 its stage x/y, r8 the hit, r9 a parent,
        # r10/r11 the scale, r12 click, r13..r16 the hit's rectangle, r17 the
        # focus manager, r18 the top popup, r19 srActiveFM's first item
        body = code.begin_method(registers=20)
        g = ("r", 2)
        sa = (g, "SettingsAdapter")
        code.path(g, "SettingsAdapter")
        code.push(None)
        code.op("Equals2")
        no_sa = code.branch_if_true()
        code.path(g, "gFM")
        code.push(None)
        code.op("Equals2")
        no_fm = code.branch_if_true()
        # a screen changing: wait (srSettle polls)
        code.push(0)
        code.path(g, "srSettle")
        code.op("LessThan2")
        code.op("Not")
        settled = code.branch_if_true()
        code.set_member((g,), "srSettle", lambda: (code.path(g, "srSettle"), code.push(1), code.op("Subtract")))
        waiting = code.branch()
        code.land(settled)
        code.call_method(sa, "GetMouseX", keep=True)
        code.set_register(3)
        code.push(("r", 3), 0)
        code.op("LessThan2")
        outside = code.branch_if_true()
        code.call_method(sa, "GetMouseY", keep=True)
        code.set_register(4)
        code.push(None)
        code.set_register(8)
        code.call_method(sa, "TakeMenuClick", keep=True)
        code.set_register(12)
        # the focus manager: the top popup's own (ScrGenPop's mFM, its buttons
        # PopupBtn0..4 are not in gFM; gPopupMgr.aPopupStack[top].popInstance is
        # the popup screen's path), else the screen's gFM
        code.path(g, "gFM")
        code.set_register(17)
        # the focus manager that last took input or a selection (every screen,
        # list and popup has its own: Save/Load's slots, MAIN's front end),
        # while its first item is still on the stage
        other_fm_screen = same_screen("srActiveFM")
        code.path(g, "srActiveFM", "m_arrItems")
        code.push(0)
        code.op("GetMember")
        code.set_register(19)
        code.push(("r", 19), None)
        code.op("Equals2")
        no_item = code.branch_if_true()
        code.path(("r", 19), "_parent")
        code.push(None)
        code.op("Equals2")
        gone = code.branch_if_true()
        code.path(g, "srActiveFM")
        code.set_register(17)
        code.land(gone)
        code.land(no_item)
        code.land(other_fm_screen)
        code.path(g, "gPopupMgr")
        code.push(None)
        code.op("Equals2")
        no_popups = code.branch_if_true()
        code.push(0)
        code.path(g, "gPopupMgr", "aPopupStack", "length")
        code.op("LessThan2")
        code.op("Not")
        empty = code.branch_if_true()
        code.path(g, "gPopupMgr", "aPopupStack")
        code.path(g, "gPopupMgr", "aPopupStack", "length")
        code.push(1)
        code.op("Subtract")
        code.op("GetMember")
        code.push("popInstance")
        code.op("GetMember")
        code.op("GetVariable")
        code.set_register(18)
        code.push(("r", 18), None)
        code.op("Equals2")
        no_instance = code.branch_if_true()
        code.path(("r", 18), "mFM")
        code.push(None)
        code.op("Equals2")
        no_fm2 = code.branch_if_true()
        code.path(("r", 18), "mFM")
        code.set_register(17)
        code.land(no_fm2)
        code.land(no_instance)
        code.land(empty)
        code.land(no_popups)
        for i in range(MOUSE_ITEMS):
            code.path(("r", 17), "m_arrItems")
            code.push(i)
            code.op("GetMember")
            code.set_register(5)
            skips = []
            code.push(("r", 5), None)
            code.op("Equals2")
            skips.append(code.branch_if_true())
            for method in ("isEnabled", "isVisible"):
                code.call_method((("r", 5),), method, keep=True)
                code.op("Not")
                skips.append(code.branch_if_true())
            code.path(("r", 5), "_x")
            code.set_register(6)
            code.path(("r", 5), "_y")
            code.set_register(7)
            code.push(1)
            code.set_register(10)
            code.push(1)
            code.set_register(11)
            code.path(("r", 5), "_parent")
            code.set_register(9)
            ends = []
            for _ in range(MOUSE_DEPTH):
                code.push(("r", 9), None)
                code.op("Equals2")
                ends.append(code.branch_if_true())
                for pos, scale, at, s in (("_x", "_xscale", 6, 10), ("_y", "_yscale", 7, 11)):
                    # at = parent.pos + at * parent.scale / 100; s = s * parent.scale / 100
                    code.push(("r", at))
                    code.path(("r", 9), scale)
                    code.op("Multiply")
                    code.push(100)
                    code.op("Divide")
                    code.path(("r", 9), pos)
                    code.op("Add2")
                    code.set_register(at)
                    code.push(("r", s))
                    code.path(("r", 9), scale)
                    code.op("Multiply")
                    code.push(100)
                    code.op("Divide")
                    code.set_register(s)
                code.path(("r", 9), "_parent")
                code.set_register(9)
            for e in ends:
                code.land(e)
            # a click: every item's rectangle to the log (calibration)
            code.push(("r", 12))
            code.op("Not")
            no_log = code.branch_if_true()
            code.call_method(sa, "MenuMouseLog", [i, -2, ("r", 6), ("r", 7), lambda: (
                code.path(("r", 5), "_width"), code.push(("r", 10)), code.op("Multiply")), lambda: (
                code.path(("r", 5), "_height"), code.push(("r", 11)), code.op("Multiply"))])
            code.land(no_log)
            # outside: x < left, left + width * sx < x, y < top, top + height * sy < y
            code.push(("r", 3), ("r", 6))
            code.op("LessThan2")
            skips.append(code.branch_if_true())
            code.push(("r", 6))
            code.path(("r", 5), "_width")
            code.push(("r", 10))
            code.op("Multiply")
            code.op("Add2")
            code.push(("r", 3))
            code.op("LessThan2")
            skips.append(code.branch_if_true())
            code.push(("r", 4), ("r", 7))
            code.op("LessThan2")
            skips.append(code.branch_if_true())
            code.push(("r", 7))
            code.path(("r", 5), "_height")
            code.push(("r", 11))
            code.op("Multiply")
            code.op("Add2")
            code.push(("r", 4))
            code.op("LessThan2")
            skips.append(code.branch_if_true())
            code.push(("r", 5))
            code.set_register(8)
            code.push(("r", 6))
            code.set_register(13)
            code.push(("r", 7))
            code.set_register(14)
            code.path(("r", 5), "_width")
            code.push(("r", 10))
            code.op("Multiply")
            code.set_register(15)
            code.path(("r", 5), "_height")
            code.push(("r", 11))
            code.op("Multiply")
            code.set_register(16)
            for s_ in skips:
                code.land(s_)
        code.push(("r", 8), None)
        code.op("Equals2")
        no_hit = code.branch_if_true()
        # moved or clicked: the item under the cursor becomes the selected one
        code.push(("r", 3))
        code.path(g, "srMouseX")
        code.op("Equals2")
        code.push(("r", 4))
        code.path(g, "srMouseY")
        code.op("Equals2")
        code.op("And")
        code.push(("r", 12))
        code.op("Not")
        code.op("And")
        still = code.branch_if_true()
        code.call_method((("r", 17),), "getSelectedItem", keep=True)
        code.push(("r", 8))
        code.op("Equals2")
        same = code.branch_if_true()
        code.call_method((("r", 17),), "selectItem", [("r", 8)])
        code.land(same)
        code.land(still)
        code.push(("r", 12))
        code.op("Not")
        no_click = code.branch_if_true()
        code.call_method(sa, "MenuMouseLog", [("r", 3), ("r", 4), ("r", 13), ("r", 14), ("r", 15), ("r", 16)])
        # a Slider (it has mMaxValue): arrows and squares (slider_click)
        code.push(("r", 8), "mMaxValue")
        code.op("GetMember")
        code.push(None)
        code.op("Equals2")
        not_slider = code.branch_if_true()
        slider_click()
        slider_done = code.branch()
        code.land(not_slider)
        code.push(("r", 8), "GetIndex")
        code.op("GetMember")
        code.push(None)
        code.op("Equals2")
        button = code.branch_if_true()
        # a Keys Mapping row (srKeyRow): A, which waits for a key
        code.path(("r", 8), "srKeyRow")
        code.op("Not")
        not_key = code.branch_if_true()
        code.call_method(sa, "PressPad", [1])
        key_done = code.branch()
        code.land(not_key)
        option_click()
        code.land(key_done)
        clicked = code.branch()
        code.land(button)
        code.call_method(sa, "PressPad", [1])
        code.land(clicked)
        code.land(slider_done)
        code.land(no_click)
        missed = code.branch()
        code.land(no_hit)
        # a click on nothing: logged with the first item's rectangle, for calibration
        code.push(("r", 12))
        code.op("Not")
        quiet = code.branch_if_true()
        code.call_method(sa, "MenuMouseLog", [("r", 3), ("r", 4), -1, -1, 0, 0])
        icon_row()
        arrow_clicks()
        code.land(quiet)
        code.land(missed)
        code.land(outside)
        code.set_member((g,), "srMouseX", ("r", 3))
        code.set_member((g,), "srMouseY", ("r", 4))
        code.land(waiting)
        code.land(no_fm)
        code.land(no_sa)
        code.end_method(body)

    code.set_member(("_global",), "srMousePoll", mouse_poll)
    code.set_member(("_global",), "srMousePollId", lambda: (
        code.push(33, "srMousePoll"), code.path("_global"), code.push(3, "setInterval"), code.op("CallFunction")))
    # setInterval(object, method, ms), as MAIN's FlowMgr starts its own (ShowPopupDelayed)
    code.set_member(("_global",), "srMenuPollId", lambda: (
        code.push(100, "srMenuPoll"), code.path("_global"), code.push(3, "setInterval"), code.op("CallFunction")))
    code.end()
    code.finish()
    items.insert(items.index(after) + 1, m.action(at))
    m.set_frame_items(rec, items)
    return m.write()


# --- SCRCONTROLROOM --------------------------------------------------------

def edit_control_room(data, const):
    m = apt_edit.MovieEdit(data, const)
    rec, items = m.frame_items(m.root)
    # The class's own init action (sprite 5, ScrControlRoom); ours after it.
    cls = find_init_action(m, items, 5)
    code = m.code()
    at = code.start()
    proto = ("_global", "ScrControlRoom", "prototype")
    # if (proto.srInitGui0 == undefined) { wrap }
    code.path(*proto, "srInitGui0")
    code.push(None)
    code.op("Equals2")
    code.op("Not")
    done = code.branch_if_true()
    code.set_member(proto, "srInitGui0", lambda: code.path(*proto, "InitGui"))
    code.set_member(proto, "srHandleInput0", lambda: code.path(*proto, "HandleInput"))

    def init_gui():
        body = code.begin_method()
        this = (("r", 1),)
        g = ("r", 2)
        code.call_method(this, "srInitGui0")
        # this.mIcons.push(ICON.GRAPHICS_OPT); this.mTitles.push(label)
        code.call_method(this + ("mIcons",), "push", [lambda: code.path(g, "ICON", "GRAPHICS_OPT")])
        code.call_method(this + ("mTitles",), "push", ["Label_ScrGraphicsOpt"])
        # the selection as the game's own InitGui restores it, all four icons
        # in view (as Fortress shows its four), then the icons again
        code.call_method(this + ("mIconMenu",), "SetSelectedPosition", [lambda: (
            code.path(g, "ControlRoomSelectedIcon"), code.push(1), code.op("Add2"))])
        code.call_method(this + ("mIconMenu",), "SetVisibleIcons", [4])
        code.call_method(this + ("mIconMenu",), "SetIcons", [lambda: code.path(("r", 1), "mIcons")])
        code.call_method(this, "SetTitle")
        code.end_method(body)

    def handle_input():
        # this r1, _global r2, keyCode r3, controllerId r4, ascii r5, the icon index r6
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=7)
        g = ("r", 2)
        # if (keyCode == INPUTCODE.CROSS && mIcons[index] == ICON.GRAPHICS_OPT)
        code.push(("r", 3))
        code.path(g, "INPUTCODE", "CROSS")
        code.op("Equals2")
        code.op("Not")
        other = code.branch_if_true()
        code.call_method((("r", 1), "mIconMenu"), "GetSelectedIndex", keep=True)
        code.set_register(6)
        code.path(("r", 1), "mIcons")
        code.push(("r", 6))
        code.op("GetMember")
        code.path(g, "ICON", "GRAPHICS_OPT")
        code.op("Equals2")
        code.op("Not")
        not_ours = code.branch_if_true()
        code.set_member((g,), "ControlRoomSelectedIcon", ("r", 6))
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
        code.call_method((g, "FlowMgr"), "SwitchScreen", [lambda: code.path(g, "SCREEN", "GRAPHICS_OPT")])
        code.push(False)
        code.op("Return")
        code.land(not_ours)
        code.land(other)
        # return this.srHandleInput0(keyCode, controllerId, ascii)
        code.call_method((("r", 1),), "srHandleInput0", [("r", 3), ("r", 4), ("r", 5)], keep=True)
        code.op("Return")
        code.end_method(body)

    code.set_member(proto, "InitGui", init_gui)
    code.set_member(proto, "HandleInput", handle_input)
    code.land(done)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(5, at))
    m.set_frame_items(rec, items)
    return m.write()


# --- SCRPAUSEMENU ------------------------------------------------------------

# The pause menu's buttons are 38 apart, Quit set off below them; ours goes
# after Audio Options (btn8) and Quit one row further down.
PAUSE_STEP = 38.0


def button_code(code, tab, first=False):
    """A BasicButton's load event, as the pause menu's own set theirs."""
    at = code.start()
    code.set_variable("mButtonText", "")
    code.set_variable("m_bEnabled", True)
    code.set_variable("m_refFM", "_global.gFM")
    code.set_variable("m_initiallySelected", first)
    code.set_variable("m_tabIndex", tab)
    code.set_variable("m_bVisible", True)
    code.end()
    code.finish()
    return at


def edit_pause_menu(data, const):
    """Gameplay Options (btn6) becomes OPTIONS, the port's options screen with all
    the options screens in it; Controller (btn7) and Audio (btn8) are hidden and
    disabled (the focus manager skips them, MAIN's FocusManager checks isEnabled
    and isVisible), Quit (btn9) moves up to keep its gap below OPTIONS (the menu
    no longer ran into its frame, the user's screenshot 2026-11-08)."""
    m = apt_edit.MovieEdit(data, const)
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    by_name = {m.placement_name(it): it for it in sitems if m.b.u32(it) == 3}
    options, audio, quit_ = by_name["btn6"], by_name["btn8"], by_name["btn9"]
    # Quit sits 58 below Audio (402 - 344): the same below OPTIONS
    m.b.setf(quit_ + 36, struct_f32(m, options + 36) + struct_f32(m, quit_ + 36) - struct_f32(m, audio + 36))
    code = m.code()

    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 4)
    at = code.start()
    proto = ("_global", "ScrPauseMenu", "prototype")
    this = (("r", 1),)
    g = ("r", 2)
    code.path(*proto, "srInitGui0")
    code.push(None)
    code.op("Equals2")
    code.op("Not")
    done = code.branch_if_true()
    code.set_member(proto, "srInitGui0", lambda: code.path(*proto, "InitGui"))
    code.set_member(proto, "srHandleInput0", lambda: code.path(*proto, "HandleInput"))

    def init_gui():
        body = code.begin_method()
        code.call_method(this, "srInitGui0")
        code.call_method(this + ("btn6",), "SetText", [lambda: code.call_method(
            (g, "LocalizationAdapter"), "GetLocalizedText", ["Label_ScrOptionsMenu"], keep=True)])
        for button in ("btn7", "btn8"):
            code.call_method(this + (button,), "disable")
            code.call_method(this + (button,), "hide")
        code.end_method(body)

    def handle_input():
        # this r1, _global r2, keyCode r3, controllerId r4, ascii r5, the button r6
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=7)
        code.push(("r", 3))
        code.path(g, "INPUTCODE", "CROSS")
        code.op("Equals2")
        code.op("Not")
        other = code.branch_if_true()
        code.call_method((g, "gFM"), "getSelectedItem", keep=True)
        code.set_register(6)
        code.push(("r", 6), "_name")
        code.op("GetMember")
        code.push("btn6")
        code.op("Equals2")
        code.op("Not")
        not_ours = code.branch_if_true()
        # as the game's own buttons: remembered, pressed, the open sound, the screen
        code.set_member((g,), "PauseSelectedItem", "btn6")
        code.call_method((("r", 6),), "handleInput", [("r", 3)])
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
        code.call_method((g, "FlowMgr"), "SwitchScreen", [lambda: code.path(g, "SCREEN", "OPTIONS_MENU")])
        code.push(False)
        code.op("Return")
        code.land(not_ours)
        code.land(other)
        code.call_method(this, "srHandleInput0", [("r", 3), ("r", 4), ("r", 5)], keep=True)
        code.op("Return")
        code.end_method(body)

    code.set_member(proto, "InitGui", init_gui)
    code.set_member(proto, "HandleInput", handle_input)
    code.land(done)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(4, at))
    m.set_frame_items(rec, items)
    return m.write()


# --- SCROPTIONSMENU -------------------------------------------------------------

# The OPTIONS screen's buttons: (the pause menu's button, label key, screen).
OPTIONS_BUTTONS = [
    ("btn1", "Label_ScrGameplayOpt", "GAMEPLAY_OPT"),
    ("btn2", "Label_ScrControllerOpt", "CONTROLLER_OPT"),
    ("btn3", "Label_ScrAudioOpt", "AUDIO_OPT"),
    ("btn4", "Label_ScrGraphicsOpt", "GRAPHICS_OPT"),
    ("btn5", "Label_ScrDisplayOpt", "DISPLAY_OPT"),
    ("btn6", "Label_ScrKeysOpt", "KEYS_OPT"),
    ("btn7", "Label_ScrMouseOpt", "MOUSE_OPT"),
]


def find_class_init(m, frame_items, cls_name):
    """The init action (type 8) whose code defines `cls_name`'s prototype."""
    movie = apt.Movie(bytes(m.b.b), m.c.write())
    for it in frame_items:
        if m.b.u32(it) != 8:
            continue
        texts = [ins.text for ins in movie.disassemble(m.b.u32(it + 8))]
        if "'%s'" % cls_name in texts and "'prototype'" in texts:
            return it
    raise LookupError(cls_name)


def back_to_options(code, g):
    """In a screen's HandleInput: B while paused goes to OPTIONS (the game's
    screens went back to the pause menu). Leaves the stack as it was."""
    code.path(g, "FlowMgr", "PauseMode")
    code.path(g, "FlowMgr", "PAUSEMODE_MENU")
    code.op("Equals2")
    code.op("Not")
    not_paused = code.branch_if_true()
    code.push(("r", 3))
    code.path(g, "INPUTCODE", "TRIANGLE")
    code.op("Equals2")
    code.op("Not")
    not_back = code.branch_if_true()
    code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])
    code.call_method((g, "FlowMgr"), "SwitchScreen", [lambda: code.path(g, "SCREEN", "OPTIONS_MENU")])
    code.push(False)
    code.op("Return")
    code.land(not_back)
    code.land(not_paused)


def make_options_menu(data, const):
    """The pause menu's movie made into OPTIONS: its first six buttons open the
    options screens, the rest hidden."""
    m = apt_edit.MovieEdit(data, const)
    for old, new in (("ScrPauseMenu", "ScrOptionsMenu"), ("__Packages.ScrPauseMenu", "__Packages.ScrOptionsMenu")):
        m.rename_export(old, new)
        m.c.rename(old, new)
    # Evenly spaced from Resume's place (the pause menu has a gap after Save/Load).
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    by_name = {m.placement_name(it): it for it in sitems if m.b.u32(it) == 3}
    top = struct_f32(m, by_name["btn1"] + 36)
    for i, (button, *_) in enumerate(OPTIONS_BUTTONS):
        m.b.setf(by_name[button] + 36, top + PAUSE_STEP * i)
    code = m.code()
    rec, items = m.frame_items(m.root)
    cls = find_class_init(m, items, "ScrOptionsMenu")
    at = code.start()
    proto = ("_global", "ScrOptionsMenu", "prototype")
    this = (("r", 1),)
    g = ("r", 2)

    def localized(key):
        return lambda: code.call_method((g, "LocalizationAdapter"), "GetLocalizedText", [key], keep=True)

    def paused():
        code.path(g, "FlowMgr", "PauseMode")
        code.path(g, "FlowMgr", "PAUSEMODE_MENU")
        code.op("Equals2")

    def init_gui():
        body = code.begin_method(registers=4)
        code.call_method((g,), "SetScreenTitle", [localized("Label_ScrOptionsMenu")])
        paused()
        not_paused = code.branch_if_true()
        code.call_method((g,), "SetFortressDisplayWell", [True])
        code.land(not_paused)
        code.call_method(this, "ShowHints")
        for button, key, _ in OPTIONS_BUTTONS:
            code.call_method(this + (button,), "SetWidth", [791])
            code.call_method(this + (button,), "SetText", [localized(key)])
        for button in ("btn8", "btn9"):
            code.call_method(this + (button,), "disable")
            code.call_method(this + (button,), "hide")
        # the button the screen was left from, else the first
        code.path(g, "OptionsSelectedItem")
        code.push(None)
        code.op("Equals2")
        code.op("Not")
        known = code.branch_if_true()
        code.set_member((g,), "OptionsSelectedItem", "btn1")
        code.land(known)
        code.call_method((g, "gFM"), "selectItem", [lambda: (
            code.push(("r", 1)), code.path(g, "OptionsSelectedItem"), code.op("GetMember"))])
        code.end_method(body)

    def handle_input():
        # this r1, _global r2, keyCode r3, controllerId r4, ascii r5, the button r6
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=7)
        paused()
        code.op("Not")
        not_paused = code.branch_if_true()
        code.push(("r", 3))
        code.path(g, "INPUTCODE", "START")
        code.op("Equals2")
        code.op("Not")
        not_start = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])
        code.call_method((g, "FlowMgr"), "ChangeInterfaceState", ["IG"])
        code.push(False)
        code.op("Return")
        code.land(not_start)
        code.land(not_paused)
        # B: back to the pause menu (or the Control Room outside a pause)
        code.push(("r", 3))
        code.path(g, "INPUTCODE", "TRIANGLE")
        code.op("Equals2")
        code.op("Not")
        not_back = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])

        def back_screen():
            paused()
            code.op("Not")
            to_room = code.branch_if_true()
            code.path(g, "SCREEN", "PAUSEMENU")
            joined = code.branch()
            code.land(to_room)
            code.path(g, "SCREEN", "CONTROL_ROOM")
            code.land(joined)

        code.call_method((g, "FlowMgr"), "SwitchScreen", [back_screen])
        code.push(False)
        code.op("Return")
        code.land(not_back)
        # A: the selected button's screen
        code.push(("r", 3))
        code.path(g, "INPUTCODE", "CROSS")
        code.op("Equals2")
        code.op("Not")
        not_cross = code.branch_if_true()
        code.call_method((g, "gFM"), "getSelectedItem", keep=True)
        code.set_register(6)
        for button, _, screen_name in OPTIONS_BUTTONS:
            code.push(("r", 6), "_name")
            code.op("GetMember")
            code.push(button)
            code.op("Equals2")
            code.op("Not")
            skip = code.branch_if_true()
            code.set_member((g,), "OptionsSelectedItem", button)
            code.call_method((("r", 6),), "handleInput", [("r", 3)])
            code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
            code.call_method((g, "FlowMgr"), "SwitchScreen", [lambda sc=screen_name: code.path(g, "SCREEN", sc)])
            code.push(False)
            code.op("Return")
            code.land(skip)
        code.land(not_cross)
        code.call_method((g, "gFM"), "handleDefaultInputs", [("r", 3)])
        code.push(False)
        code.op("Return")
        code.end_method(body)

    code.set_member(proto, "InitGui", init_gui)
    code.set_member(proto, "HandleInput", handle_input)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(m.b.u32(cls + 4), at))
    m.set_frame_items(rec, items)
    return m.write()


def make_mouse_screen(data, const):
    """Audio Options (already made to go back to OPTIONS) made into MOUSE
    OPTIONS: its Music slider (0..10 by 1) is Mouse Sensitivity through the
    natives GetMouseSens/SetMouseSens (graphics_menu.cpp, sr_mouse_sensitivity);
    the Sound FX slider is taken off the screen and its natives renamed to
    nothing, so the screen never sets the volume."""
    m = apt_edit.MovieEdit(data, const)
    for old, new in (("ScrAudioOpt", "ScrMouseOpt"), ("__Packages.ScrAudioOpt", "__Packages.ScrMouseOpt")):
        m.rename_export(old, new)
        m.c.rename(old, new)
    for old, new in (("Label_ScrAudioOpt", "Label_ScrMouseOpt"), ("AudioOpt_Music_Slider", "MouseOpt_Sens_Slider"),
                     ("GetMusicVol", "GetMouseSens"), ("SetMusicVol", "SetMouseSens"),
                     ("GetSFXVol", "srNoSFX"), ("SetSFXVol", "srNoSFX")):
        m.c.rename(old, new)
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    m.set_frame_items(srec, [it for it in sitems if m.placement_name(it) != "SoundFX"])
    return m.write()


def edit_game_options_screen(data, const, cls_name):
    """Gameplay, Controller or Audio Options: B while paused back to OPTIONS
    (their own HandleInput only plays the close sound and switches to the pause
    menu on B, nothing to save: apt.py dumps 2026-11-08)."""
    m = apt_edit.MovieEdit(data, const)
    code = m.code()
    rec, items = m.frame_items(m.root)
    cls = find_class_init(m, items, cls_name)
    at = code.start()
    proto = ("_global", cls_name, "prototype")
    g = ("r", 2)
    code.path(*proto, "srHandleInput0")
    code.push(None)
    code.op("Equals2")
    code.op("Not")
    done = code.branch_if_true()
    code.set_member(proto, "srHandleInput0", lambda: code.path(*proto, "HandleInput"))

    def handle_input():
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=6)
        back_to_options(code, g)
        code.call_method((("r", 1),), "srHandleInput0", [("r", 3), ("r", 4), ("r", 5)], keep=True)
        code.op("Return")
        code.end_method(body)

    code.set_member(proto, "HandleInput", handle_input)
    code.land(done)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(m.b.u32(cls + 4), at))
    m.set_frame_items(rec, items)
    return m.write()


# --- SCRGRAPHICSOPT --------------------------------------------------------

def make_options_screen(data, const, cls_name, label_key, title_key, id_base, screen_rows, row_step):
    """Gameplay Options' movie made into one of the port's options screens."""
    m = apt_edit.MovieEdit(data, const)
    for old, new in (("ScrGameplayOpt", cls_name), ("__Packages.ScrGameplayOpt", "__Packages." + cls_name)):
        m.rename_export(old, new)
        m.c.rename(old, new)
    m.c.rename("Label_ScrGameplayOpt", label_key)

    # The rows: copies of the game's top row (X-Axis Inversion) placement.
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    rows = [it for it in sitems if m.b.u32(it) == 3]
    template = min(rows, key=lambda it: struct_f32(m, it + 36))
    kept = [it for it in sitems if m.b.u32(it) != 3]
    code = m.code()
    placed = []
    for i, (name, _, label, _, options) in enumerate(screen_rows):
        at = code.start()
        code.set_variable("mArrowWidth", 200)
        code.set_variable("mLabel", label)
        code.set_variable("mOptionsArray", lambda o=options: code.array(o.split("|")))
        code.set_variable("isWrapping", True)
        code.set_variable("m_bEnabled", True)
        code.set_variable("m_refFM", "_global.gFM")
        code.set_variable("m_initiallySelected", i == 0)
        code.set_variable("m_tabIndex", i)
        code.set_variable("m_bVisible", True)
        code.end()
        code.finish()
        placed.append(m.place_copy(template, name, 1 + 12 * i, ROW_X, ROW_FIRST_Y + row_step * i, at))
    m.set_frame_items(srec, kept + placed)

    # The class's methods, replaced after its init action (sprite 4).
    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 4)
    at = code.start()
    proto = ("_global", cls_name, "prototype")
    this = (("r", 1),)
    g = ("r", 2)

    def localized(key):
        return lambda: code.call_method((g, "LocalizationAdapter"), "GetLocalizedText", [key], keep=True)

    def paused():
        code.path(g, "FlowMgr", "PauseMode")
        code.path(g, "FlowMgr", "PAUSEMODE_MENU")
        code.op("Equals2")

    def init_gui():
        body = code.begin_method()
        # As Audio Options sets itself up: on _global, the title, the well, the hints.
        code.call_method((g,), "SetScreenTitle", [localized(title_key)])
        paused()
        not_paused = code.branch_if_true()
        code.call_method((g,), "SetFortressDisplayWell", [True])
        code.land(not_paused)
        code.call_method((g,), "ShowHints", [localized("Generic_BACK"), "B"])
        for i, (name, key, _, options_key, _) in enumerate(screen_rows):
            row = this + (name,)
            code.call_method(row, "SetLabel", [localized(key)])
            # SetData(GetLocalizedText(options).split("|"))
            code.call_method(row, "SetData", [lambda k=options_key: (
                code.push("|", 1), localized(k)(), code.push("split"), code.op("CallMethod"))])
            code.call_method(row, "SetIndex", [lambda i=i: code.call_method(
                (g, "SettingsAdapter"), "GetGraphicsOption", [id_base + i], keep=True)])
        code.end_method(body)

    def handle_input():
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=7)

        def key_is(name):
            code.push(("r", 3))
            code.path(g, "INPUTCODE", name)
            code.op("Equals2")

        # START in the pause menu: back to the game, as Audio Options does.
        paused()
        code.op("Not")
        not_paused = code.branch_if_true()
        key_is("START")
        code.op("Not")
        not_start = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])
        code.call_method((g, "FlowMgr"), "ChangeInterfaceState", ["IG"])
        code.land(not_start)
        code.land(not_paused)
        def restart_selected():
            # selectedCallback(popupInst, button), as the pause menu's quitCallBack:
            # button 0 = "Restart now" -> SettingsAdapter.RestartGame().
            body = code.begin_method(["popupInst", "button"], registers=5)
            code.push(("r", 4), 0)
            code.op("Equals2")
            code.op("Not")
            later = code.branch_if_true()
            code.call_method((g, "SettingsAdapter"), "RestartGame")
            code.land(later)
            code.end_method(body)

        def restart_note():
            # The first change of a "*" row while the screen is open: the game's own
            # two-button popup (as the pause menu's quit question: BistroPopupParams,
            # gPopupMgr.ShowPopup, selectedCallback): restart now or later.
            code.path(("r", 1), "srRestartShown")
            shown = code.branch_if_true()
            code.set_member((("r", 1),), "srRestartShown", True)
            code.push(0, "BistroPopupParams")
            code.op("NewObject")
            code.set_register(6)
            params = (("r", 6),)
            code.set_member(params, "body", localized("Options_RestartNote"))
            code.set_member(params, "title", "")
            code.set_member(params, "numButtons", 2)
            # "Later" selected: a stray A does not end the game
            code.set_member(params, "defaultButton", 1)
            code.set_member(params, "buttonLabels", lambda: code.array(
                [localized("Options_RestartNow"), localized("Options_RestartLater")]))
            code.set_member(params, "context", ("r", 1))
            code.set_member(params, "selectedCallback", restart_selected)
            code.set_member(params, "readyCallBack", None)
            code.set_member(params, "canCancel", False)
            code.set_member(params, "closeOnPress", True)
            code.call_method((g, "gPopupMgr"), "ShowPopup", [("r", 6)])
            code.land(shown)

        # B: back to where the screen came from.
        key_is("TRIANGLE")
        code.op("Not")
        not_back = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])

        def back_screen():
            paused()
            code.op("Not")
            to_room = code.branch_if_true()
            code.path(g, "SCREEN", "OPTIONS_MENU")
            joined = code.branch()
            code.land(to_room)
            code.path(g, "SCREEN", "CONTROL_ROOM")
            code.land(joined)

        code.call_method((g, "FlowMgr"), "SwitchScreen", [back_screen])
        code.land(not_back)
        code.call_method((g, "gFM"), "handleDefaultInputs", [("r", 3)])
        # LEFT/RIGHT: the row's new value, at once.
        key_is("LEFT")
        code.op("PushDuplicate")
        either = code.branch_if_true()
        code.op("Pop")
        key_is("RIGHT")
        code.land(either)
        code.op("Not")
        no_change = code.branch_if_true()
        for i, (name, _, label, *_) in enumerate(screen_rows):
            code.call_method((g, "gFM"), "getSelectedItem", keep=True)
            code.push("_name")
            code.op("GetMember")
            code.push(name)
            code.op("Equals2")
            code.op("Not")
            skip = code.branch_if_true()
            code.call_method((g, "SettingsAdapter"), "SetGraphicsOption", [
                id_base + i, lambda n=name: code.call_method(this + (n,), "GetIndex", keep=True)])
            if label.endswith("*"):
                restart_note()
            code.land(skip)
        code.land(no_change)
        code.push(False)
        code.op("Return")
        code.end_method(body)

    def exit_screen():
        body = code.begin_method()
        for name, *_ in screen_rows:
            code.call_method(this + (name,), "OnExit")
        # As the game's screens do: the class unregistered and deleted.
        code.call_method(("Object",), "registerClass", [cls_name, None])
        code.push(("r", 2), cls_name)
        code.op("Delete")
        code.op("Pop")
        code.end_method(body)

    code.set_member(proto, "InitGui", init_gui)
    code.set_member(proto, "HandleInput", handle_input)
    code.set_member(proto, "ExitScreen", exit_screen)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(4, at))
    m.set_frame_items(rec, items)
    return m.write()


# --- SCRKEYSOPT ------------------------------------------------------------------

# Keys Mapping: pages of eight rows, (title key, title, [(binding index, label key,
# label)]). The indices are port/src/input/key_capture.cpp's kKeyBindCvars.
# Rows named by what they do: the game's own Controller Options, configuration 1
# (INTERFACETEXT CntrlOpt_Ground/Air_Config1_*; SCRCONTROLLEROPT lists the face
# buttons top to bottom Action4 Y, Action2 B, Action3 X, Action1 A; Shoulder1..4
# LB RB LT RT), ground first, flying after the slash. The d-pad's powers from
# the HUD's DPADPOWERS: 'up' red (Heat Vision), 'right' cyan (Freeze Breath),
# 'left' yellow-green (Superbreath), no 'down' (X-Ray left the game). The stick
# clicks and d-pad down do nothing in the game's control screens, so they are
# not listed, nor the camera keys (the mouse turns the camera; the user,
# 2026-11-09). A page shorter than 8 hides its other rows. At most ~17 characters:
# a longer label reaches the Toggle's arrow.
KEY_PAGES = [
    ("KEYS_PAGE_ACTIONS", "KEYS MAPPING: ACTIONS (1/3)", [
        (0, "KEY_A", "Pick Up / Throw"), (1, "KEY_B", "Block / Drop"), (2, "KEY_X", "Action"),
        (3, "KEY_Y", "Take Off / Land"), (4, "KEY_LB", "Reset Cam / Hover"), (5, "KEY_RB", "Superspeed"),
        (6, "KEY_LT", "Target Lock"), (7, "KEY_RT", "Superpower")]),
    ("KEYS_PAGE_BUTTONS", "KEYS MAPPING: MENUS AND POWERS (2/3)", [
        (8, "KEY_START", "Pause"), (9, "KEY_BACK", "Map"), (15, "KEY_DPAD_UP", "Heat Vision"),
        (17, "KEY_DPAD_LEFT", "Superbreath"), (18, "KEY_DPAD_RIGHT", "Freeze Breath")]),
    ("KEYS_PAGE_STICKS", "KEYS MAPPING: MOVE (3/3)", [
        (10, "KEY_MOVE_UP", "Move Forward"), (11, "KEY_MOVE_DOWN", "Move Back"), (12, "KEY_MOVE_LEFT", "Move Left"),
        (13, "KEY_MOVE_RIGHT", "Move Right")]),
]
KEY_ROWS = ["Key%d" % i for i in range(8)]
# A key's icon in its row: centred between the Toggle's arrows (TOGGLE: right
# arrow at 597.8, the left one mArrowWidth 200 before it, ~12 wide), on the
# row's highlight bar (y -2.1, 32 high).
KEY_ICON_X, KEY_ICON_Y = 482, 0

# --- BUTTONICON: keys instead of the pad's buttons --------------------------
#
# The game's hints (ButtonIcon.SetIcon(icon)) load ../common/ButtonHints/
# <platform>/Hint<icon>.swf. While the keyboard and mouse are in use
# (SettingsAdapter.KbmActive) the icon of the key bound to that pad button is
# loaded instead (HintKey<code>, key_icons.py); a stick or the d-pad shows its
# keys side by side, the right stick the mouse. Each icon checks every frame
# whether the input changed and loads itself again.

# icon (upper case) -> the binding (key_capture.cpp kKeyBindCvars)
HINT_KEYS = {"A": 0, "B": 1, "X": 2, "Y": 3, "SHOULDER1": 4, "LB": 4, "SHOULDER2": 5, "RB": 5,
             "SHOULDER3": 6, "LT": 6, "SHOULDER4": 7, "RT": 7, "START": 8, "SELECT": 9, "BACK": 9,
             "DPAD_UP": 15, "UP": 15, "DPAD_DOWN": 16, "DOWN": 16, "DPAD_LEFT": 17, "LEFT": 17,
             "DPAD_RIGHT": 18, "RIGHT": 18}
# several keys: (bindings, (x, y) of each, scale %), in the 32-unit icon
CROSS = ((12.8, 3), (12.8, 16), (0, 16), (25.6, 16)), 40
HINT_KEY_GROUPS = {
    "DPAD": ((15, 16, 17, 18),) + CROSS,
    "LSTICK": ((10, 11, 12, 13),) + CROSS, "STICK": ((10, 11, 12, 13),) + CROSS,
    "LSTICK_ND": ((10, 11, 12, 13),) + CROSS,
    "LSTICK_UD": ((10, 11), ((8, 0), (8, 16)), 50),
    "LSTICK_LR": ((12, 13), ((0, 8), (16, 8)), 50),
}
HINT_MOUSE = ("RSTICK", "RSTICK_UD", "RSTICK_LR", "RSTICK_ND")


def define_set_icon(code):
    """_global.srSetIcon, ButtonIcon's new SetIcon (set in MAIN, which every
    other movie follows; BUTTONICON's own archive has no room for it)."""
    g = ("r", 2)
    this = (("r", 1),)

    def key_path(code_reg):
        # mFilePath + mPlatform + "/HintKey" + code + ".swf"
        code.path(("r", 1), "mFilePath")
        code.path(("r", 1), "mPlatform")
        code.op("Add2")
        code.push("/HintKey")
        code.op("Add2")
        code.push(code_reg)
        code.op("Add2")
        code.push(".swf")
        code.op("Add2")

    def bound_key(binding):
        # MenuHintKey(binding) -> r6: the bound key, or in the menus Enter for A
        # and Escape for B (rawinput_kbm_driver presses them so)
        code.call_method((g, "SettingsAdapter"), "MenuHintKey", [binding], keep=True)
        code.set_register(6)

    def set_icon():
        # r3 icon, r4 its name in upper case, r5 -, r6 a key code, r7 the
        # holder, r8 a child, r9 keyboard and mouse in use
        body = code.begin_method(params=("icon",), registers=10)
        code.set_member(this, "srIcon", ("r", 3))
        code.call_method((g, "SettingsAdapter"), "KbmActive", keep=True)
        code.set_register(9)
        code.set_member(this, "srKbm", hint_state)
        code.path(("r", 1), "mIconHolder")
        code.set_register(7)
        for j in range(4):
            code.call_method((("r", 7), "k%d" % j), "removeMovieClip")
        to_pad = []
        done = []
        code.push(("r", 9))
        code.op("Not")
        to_pad.append(code.branch_if_true())
        code.call_method((("r", 3),), "toUpperCase", keep=True)
        code.set_register(4)
        # one key
        for name, binding in HINT_KEYS.items():
            code.push(("r", 4), name)
            code.op("Equals2")
            code.op("Not")
            other = code.branch_if_true()
            bound_key(binding)
            code.push(("r", 6), 0)
            code.op("Equals2")
            to_pad.append(code.branch_if_true())
            code.call_method((("r", 7),), "loadMovie", [lambda: key_path(("r", 6))])
            done.append(code.branch())
            code.land(other)
        # the mouse (the camera)
        for name in HINT_MOUSE:
            code.push(("r", 4), name)
            code.op("Equals2")
            code.op("Not")
            other = code.branch_if_true()
            code.call_method((("r", 7),), "loadMovie", [lambda: key_path("Mouse")])
            done.append(code.branch())
            code.land(other)
        # several keys, each in a child of the holder
        for name, (bindings, places, scale) in HINT_KEY_GROUPS.items():
            code.push(("r", 4), name)
            code.op("Equals2")
            code.op("Not")
            other = code.branch_if_true()
            code.call_method((("r", 7),), "unloadMovie")
            for j, (binding, (x, y)) in enumerate(zip(bindings, places)):
                bound_key(binding)
                code.push(("r", 6), 0)
                code.op("Equals2")
                unbound = code.branch_if_true()
                code.call_method((("r", 7),), "createEmptyMovieClip", ["k%d" % j, 10 + j], keep=True)
                code.set_register(8)
                for prop, v in (("_x", x), ("_y", y), ("_xscale", scale), ("_yscale", scale)):
                    code.set_member((("r", 8),), prop, lambda v=v: code.push(v) if isinstance(v, int) else
                                    (code.push(int(round(v * 10))), code.push(10), code.op("Divide")))
                code.call_method((("r", 8),), "loadMovie", [lambda: key_path(("r", 6))])
                code.land(unbound)
            done.append(code.branch())
            code.land(other)
        # the pad's icon (as the game's SetIcon)
        for b in to_pad:
            code.land(b)
        code.call_method((("r", 7),), "loadMovie", [lambda: (
            code.path(("r", 1), "mFilePath"), code.path(("r", 1), "mPlatform"), code.op("Add2"),
            code.push("/Hint"), code.op("Add2"), code.push(("r", 3)), code.op("Add2"),
            code.push(".swf"), code.op("Add2"))])
        for b in done:
            code.land(b)
        code.set_member(this, "onEnterFrame", enter_frame)
        code.end_method(body)

    def hint_state():
        # KbmActive() * 256 + MenuHintKey(0): changes with the input in use and
        # with a menu opening or closing (A's key: Enter in the menus); a pause
        # menu's hints are made in the frame START opens it, before MAIN's poll
        # reports the menu, so each icon looks again every frame
        code.call_method((g, "SettingsAdapter"), "KbmActive", keep=True)
        code.push(256)
        code.op("Multiply")
        code.call_method((g, "SettingsAdapter"), "MenuHintKey", [0], keep=True)
        code.op("Add2")

    def enter_frame():
        body = code.begin_method(registers=3)
        hint_state()
        code.path(("r", 1), "srKbm")
        code.op("Equals2")
        same = code.branch_if_true()
        code.call_method(this, "SetIcon", [lambda: code.path(("r", 1), "srIcon")])
        code.land(same)
        code.end_method(body)

    code.set_member(("_global",), "srSetIcon", set_icon)

    def set_start_text():
        # the game's SetStartText (layout and "PRESS START"), then "PRESS ENTER"
        # while keyboard and mouse are in use; the screen is remembered so the
        # menu poll can set it again when the input changes
        body = code.begin_method(registers=4)
        code.set_member((g,), "srPressStart", ("r", 1))
        code.set_member((g,), "srPressStartGen", lambda: code.path(g, "srScreenGen"))
        code.call_method(this, "srOrigSST")
        code.call_method((g, "SettingsAdapter"), "KbmActive", keep=True)
        code.set_register(3)
        code.set_member(this, "srKbm", ("r", 3))
        code.push(("r", 3))
        code.op("Not")
        pad = code.branch_if_true()
        code.call_method((("r", 1), "mcStartText"), "SetText", [lambda: code.call_method(
            (g, "LocalizationAdapter"), "GetLocalizedText", ["SUP_PRESS_ENTER"], keep=True)])
        code.land(pad)
        code.end_method(body)

    code.set_member(("_global",), "srSetStartText", set_start_text)


def edit_press_start(data, const):
    """After the class init (sprite 61, ScrPressStart): SetStartText becomes
    MAIN's _global.srSetStartText (PRESS ENTER with keyboard and mouse), the
    game's kept as srOrigSST."""
    m = apt_edit.MovieEdit(data, const)
    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 61)
    code = m.code()
    at = code.start()
    proto = ("_global", "ScrPressStart", "prototype")
    code.path("_global", "srSetStartText")
    code.push(None)
    code.op("Equals2")
    missing = code.branch_if_true()
    code.set_member(proto, "srOrigSST", lambda: code.path(*proto, "SetStartText"))
    code.set_member(proto, "SetStartText", lambda: code.path("_global", "srSetStartText"))
    code.land(missing)
    code.end()
    code.finish()
    new = m.init_action(61, at)
    i = items.index(cls)
    m.set_frame_items(rec, items[:i + 1] + [new] + items[i + 1:])
    return m.write()


def edit_scroll_screen(data, const, sprite, cls_name):
    """After the screen's class init (Moves List, Statistics): InitGui becomes MAIN's
    _global.srNoteScroll (the screen noted for the mouse poll's arrows), the game's
    kept as srOrigInit."""
    m = apt_edit.MovieEdit(data, const)
    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, sprite)
    code = m.code()
    at = code.start()
    proto = ("_global", cls_name, "prototype")
    code.path("_global", "srNoteScroll")
    code.push(None)
    code.op("Equals2")
    missing = code.branch_if_true()
    code.set_member(proto, "srOrigInit", lambda: code.path(*proto, "InitGui"))
    code.set_member(proto, "InitGui", lambda: code.path("_global", "srNoteScroll"))
    code.land(missing)
    code.end()
    code.finish()
    new = m.init_action(sprite, at)
    i = items.index(cls)
    m.set_frame_items(rec, items[:i + 1] + [new] + items[i + 1:])
    return m.write()


def edit_button_icon(data, const):
    """After the game's class init (which defines SetIcon):
    ButtonIcon.prototype.SetIcon = _global.srSetIcon, when MAIN has set it."""
    m = apt_edit.MovieEdit(data, const)
    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 3)
    code = m.code()
    at = code.start()
    code.path("_global", "srSetIcon")
    code.push(None)
    code.op("Equals2")
    missing = code.branch_if_true()
    code.set_member(("_global", "ButtonIcon", "prototype"), "SetIcon", lambda: code.path("_global", "srSetIcon"))
    code.land(missing)
    code.end()
    code.finish()
    new = m.init_action(3, at)
    i = items.index(cls)
    m.set_frame_items(rec, items[:i + 1] + [new] + items[i + 1:])
    return m.write()


def make_keys_screen(data, const):
    """Gameplay Options' movie made into Keys Mapping: eight rows a page, LEFT and
    RIGHT turn the page, A on a row waits for a key or mouse button
    (SettingsAdapter.BeginKeyCapture, polled every 50 ms through setInterval as
    MAIN's FlowMgr polls its popups), B while waiting cancels."""
    m = apt_edit.MovieEdit(data, const)
    for old, new in (("ScrGameplayOpt", "ScrKeysOpt"), ("__Packages.ScrGameplayOpt", "__Packages.ScrKeysOpt")):
        m.rename_export(old, new)
        m.c.rename(old, new)
    m.c.rename("Label_ScrGameplayOpt", "Label_ScrKeysOpt")
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    rows = [it for it in sitems if m.b.u32(it) == 3]
    template = min(rows, key=lambda it: struct_f32(m, it + 36))
    kept = [it for it in sitems if m.b.u32(it) != 3]
    code = m.code()
    placed = []
    for i, name in enumerate(KEY_ROWS):
        at = code.start()
        code.set_variable("mArrowWidth", 200)
        code.set_variable("mLabel", "")
        code.set_variable("mOptionsArray", lambda: code.array([""]))
        code.set_variable("isWrapping", True)
        code.set_variable("m_bEnabled", True)
        code.set_variable("m_refFM", "_global.gFM")
        code.set_variable("m_initiallySelected", i == 0)
        code.set_variable("m_tabIndex", i)
        code.set_variable("m_bVisible", True)
        code.set_variable("srKeyRow", True)  # a click on it is A (the mouse poll)
        code.end()
        code.finish()
        placed.append(m.place_copy(template, name, 1 + 12 * i, ROW_X, ROW_FIRST_Y + ROW_STEP * i, at))
    # the page arrows beside the title: one more Toggle, out of the focus manager
    # (m_tabIndex -1), only its two arrows shown (init_gui, refresh)
    at = code.start()
    code.set_variable("mArrowWidth", 200)
    code.set_variable("mLabel", "")
    code.set_variable("mOptionsArray", lambda: code.array([""]))
    code.set_variable("isWrapping", True)
    code.set_variable("m_bEnabled", True)
    code.set_variable("m_refFM", "_global.gFM")
    code.set_variable("m_initiallySelected", False)
    code.set_variable("m_tabIndex", -1)
    code.set_variable("m_bVisible", True)
    code.end()
    code.finish()
    placed.append(m.place_copy(template, "srPageRow", 1 + 12 * len(KEY_ROWS), ROW_X, KEY_TITLE_Y, at))
    m.set_frame_items(srec, kept + placed)

    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 4)
    at = code.start()
    proto = ("_global", "ScrKeysOpt", "prototype")
    this = (("r", 1),)
    g = ("r", 2)

    def localized(key):
        return lambda: code.call_method((g, "LocalizationAdapter"), "GetLocalizedText", [key], keep=True)

    def paused():
        code.path(g, "FlowMgr", "PauseMode")
        code.path(g, "FlowMgr", "PAUSEMODE_MENU")
        code.op("Equals2")

    def call_function(name, args):
        for a in reversed(args):
            code._arg(a)
        code.push(len(args))
        code.push(name)
        code.op("CallFunction")

    def key_text(binding):
        """Push the binding's first key as text: [Shift+][Ctrl+][Alt+]name, or
        KEYNAME_NONE. Uses r3 (the code) and r4 (the text)."""
        code.call_method((g, "SettingsAdapter"), "GetKeyBind", [binding], keep=True)
        code.set_register(3)
        code.push("")
        code.set_register(4)
        for bit, key in ((0x100, "KEYMOD_SHIFT"), (0x200, "KEYMOD_CTRL"), (0x400, "KEYMOD_ALT")):
            code.push(("r", 3), bit)
            code.op("BitwiseAnd")
            code.op("Not")
            skip = code.branch_if_true()
            code.push(("r", 4))
            localized(key)()
            code.op("Add2")
            code.set_register(4)
            code.land(skip)
        code.push(("r", 3), 255)
        code.op("BitwiseAnd")
        code.op("Not")
        # name: KEYNAME_<code & 255>, or KEYNAME_NONE when the code is 0
        zero = code.branch_if_true()
        code.push(("r", 4))
        code.call_method((g, "LocalizationAdapter"), "GetLocalizedText", [lambda: (
            code.push("KEYNAME_"), code.push(("r", 3), 255), code.op("BitwiseAnd"), code.op("Add2"))], keep=True)
        code.op("Add2")
        joined = code.branch()
        code.land(zero)
        localized("KEYNAME_NONE")()
        code.land(joined)

    def key_icon(row):
        """The key's icon (key_icons.py) in the row's value, between its arrows,
        when the binding is one key without Shift/Ctrl/Alt; the text stays for
        the others (and is emptied for the icon). Right after key_text: r3
        still holds the binding's code; r4 is reused for the icon's clip."""
        code.call_method(this + (row, "srKey"), "removeMovieClip")
        code.push(("r", 3), 0xF00)
        code.op("BitwiseAnd")
        no_icon = code.branch_if_true()
        code.push(("r", 3), 0)
        code.op("Equals2")
        none = code.branch_if_true()
        code.call_method(this + (row,), "SetData", [lambda: code.array([""])])
        code.call_method(this + (row,), "SetIndex", [0])
        code.call_method(this + (row,), "createEmptyMovieClip", ["srKey", 100], keep=True)
        code.set_register(4)
        code.set_member((("r", 4),), "_x", KEY_ICON_X)
        code.set_member((("r", 4),), "_y", KEY_ICON_Y)
        code.call_method((("r", 4),), "loadMovie", [lambda: (
            code.push("../common/ButtonHints/"),
            code.call_method((g, "GlobalAdapter"), "GetPlatform", keep=True), code.op("Add2"),
            code.push("/HintKey"), code.op("Add2"), code.push(("r", 3)), code.op("Add2"),
            code.push(".swf"), code.op("Add2"))])
        code.land(none)
        code.land(no_icon)

    def refresh():
        body = code.begin_method(registers=5)
        for p, (title_key, _, page_rows) in enumerate(KEY_PAGES):
            code.path(("r", 1), "srPage")
            code.push(p)
            code.op("Equals2")
            code.op("Not")
            other = code.branch_if_true()
            code.call_method((g,), "SetScreenTitle", [localized(title_key)])
            # the page arrows just outside this page's title
            half = int(len(KEY_PAGES[p][1]) * KEY_TITLE_CHAR / 2)
            code.set_member(this + ("srPageRow", "mLeftArrow"), "_x", KEY_TITLE_CENTER - half - 24)
            code.set_member(this + ("srPageRow", "mRightArrow"), "_x", KEY_TITLE_CENTER + half + 12)
            code.set_member(this, "srIds", lambda ids=[b for b, *_ in page_rows]: code.array(ids))
            for row, (binding, label_key, _) in zip(KEY_ROWS, page_rows):
                code.call_method(this + (row,), "show")
                code.call_method(this + (row,), "enable")
                code.call_method(this + (row,), "SetLabel", [localized(label_key)])
                code.call_method(this + (row,), "SetData", [lambda b=binding: code.array([lambda: key_text(b)])])
                code.call_method(this + (row,), "SetIndex", [0])
                key_icon(row)
            for row in KEY_ROWS[len(page_rows):]:
                code.call_method(this + (row, "srKey"), "removeMovieClip")
                code.call_method(this + (row,), "disable")
                code.call_method(this + (row,), "hide")
            # a hidden row selected (the page turned from a lower row): the first
            code.call_method((g, "gFM"), "getSelectedItem", keep=True)
            code.set_register(3)
            code.call_method((("r", 3),), "isVisible", keep=True)
            shown = code.branch_if_true()
            code.call_method((g, "gFM"), "selectItem", [lambda: code.path(("r", 1), KEY_ROWS[0])])
            code.land(shown)
            code.land(other)
        code.end_method(body)

    def poll():
        body = code.begin_method(registers=3)
        code.call_method((g, "SettingsAdapter"), "KeyCaptureActive", keep=True)
        waiting = code.branch_if_true()
        call_function("clearInterval", [lambda: code.path(("r", 1), "srInterval")])
        code.op("Pop")
        code.set_member(this, "srCapturing", False)
        code.call_method(this, "srRefresh")
        code.land(waiting)
        code.end_method(body)

    def init_gui():
        body = code.begin_method(registers=3)
        paused()
        not_paused = code.branch_if_true()
        code.call_method((g,), "SetFortressDisplayWell", [True])
        code.land(not_paused)
        # Y (the game's INPUTCODE.CIRCLE, as Controller Options' "Y" hint) restores the defaults
        code.call_method((g,), "ShowHints", [localized("Generic_BACK"), "B", localized("KEYS_DEFAULTS"), "Y"])
        code.path(g, "KeysPage")
        code.push(None)
        code.op("Equals2")
        code.op("Not")
        known = code.branch_if_true()
        code.set_member((g,), "KeysPage", 0)
        code.land(known)
        code.set_member(this, "srPage", lambda: code.path(g, "KeysPage"))
        code.set_member(this, "srCapturing", False)
        # the rows' arrows never shown (LEFT/RIGHT turn the page, not a row's value;
        # the user 2026-11-09), the page row's always: their Show and Hide swapped
        for row in KEY_ROWS:
            for arrow in ("mLeftArrow", "mRightArrow"):
                code.set_member(this + (row, arrow), "Show", lambda r=row, a=arrow: code.path(("r", 1), r, a, "Hide"))
                code.call_method(this + (row, arrow), "Hide")
        for arrow in ("mLeftArrow", "mRightArrow"):
            code.set_member(this + ("srPageRow", arrow), "Hide", lambda a=arrow: code.path(("r", 1), "srPageRow", a, "Show"))
            code.call_method(this + ("srPageRow", arrow), "Show")
        code.set_member((g,), "srPageRow", lambda: code.path(("r", 1), "srPageRow"))
        code.set_member((g,), "srPageRowGen", lambda: code.path(g, "srScreenGen"))
        code.call_method(this, "srRefresh")
        code.end_method(body)

    def handle_input():
        # this r1, _global r2, keyCode r3, controllerId r4, ascii r5, the row index r6, scratch r7
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=8)

        def key_is(name):
            code.push(("r", 3))
            code.path(g, "INPUTCODE", name)
            code.op("Equals2")

        # waiting for a key: B cancels, the rest is ignored
        code.path(("r", 1), "srCapturing")
        code.op("Not")
        not_waiting = code.branch_if_true()
        key_is("TRIANGLE")
        code.op("Not")
        keep_waiting = code.branch_if_true()
        code.call_method((g, "SettingsAdapter"), "CancelKeyCapture")
        code.land(keep_waiting)
        code.push(False)
        code.op("Return")
        code.land(not_waiting)
        # START in the pause menu: back to the game
        paused()
        code.op("Not")
        not_paused = code.branch_if_true()
        key_is("START")
        code.op("Not")
        not_start = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])
        code.call_method((g, "FlowMgr"), "ChangeInterfaceState", ["IG"])
        code.push(False)
        code.op("Return")
        code.land(not_start)
        code.land(not_paused)
        # B: back to OPTIONS (or the Control Room outside a pause)
        key_is("TRIANGLE")
        code.op("Not")
        not_back = code.branch_if_true()
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])

        def back_screen():
            paused()
            code.op("Not")
            to_room = code.branch_if_true()
            code.path(g, "SCREEN", "OPTIONS_MENU")
            joined = code.branch()
            code.land(to_room)
            code.path(g, "SCREEN", "CONTROL_ROOM")
            code.land(joined)

        code.call_method((g, "FlowMgr"), "SwitchScreen", [back_screen])
        code.push(False)
        code.op("Return")
        code.land(not_back)
        # Y: every binding back to its default (key_capture.cpp kDefaultKeyBinds)
        key_is("CIRCLE")
        code.op("Not")
        not_reset = code.branch_if_true()
        code.call_method((g, "SettingsAdapter"), "ResetKeyBinds")
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "CLOSE")])
        code.call_method(this, "srRefresh")
        code.push(False)
        code.op("Return")
        code.land(not_reset)
        # LEFT / RIGHT: the previous / next page
        for key, step in (("LEFT", len(KEY_PAGES) - 1), ("RIGHT", 1)):
            key_is(key)
            code.op("Not")
            other = code.branch_if_true()
            code.set_member((g,), "KeysPage", lambda st=step: (
                code.path(("r", 1), "srPage"), code.push(st), code.op("Add2"), code.push(len(KEY_PAGES)),
                code.op("Modulo")))
            code.set_member(this, "srPage", lambda: code.path(g, "KeysPage"))
            code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
            code.call_method(this, "srRefresh")
            code.push(False)
            code.op("Return")
            code.land(other)
        # A on a row: wait for a key for its binding
        key_is("CROSS")
        code.op("Not")
        not_cross = code.branch_if_true()
        for i, row in enumerate(KEY_ROWS):
            code.call_method((g, "gFM"), "getSelectedItem", keep=True)
            code.push("_name")
            code.op("GetMember")
            code.push(row)
            code.op("Equals2")
            code.op("Not")
            skip = code.branch_if_true()
            code.call_method((g, "SettingsAdapter"), "BeginKeyCapture", [lambda i=i: (
                code.path(("r", 1), "srIds"), code.push(i), code.op("GetMember"))])
            code.call_method(this + (row,), "SetData", [lambda: code.array([localized("KEYS_PRESS")])])
            code.call_method(this + (row,), "SetIndex", [0])
            code.set_member(this, "srCapturing", True)
            code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
            code.set_member(this, "srInterval", lambda: call_function("setInterval", [("r", 1), "srPoll", 50]))
            code.push(False)
            code.op("Return")
            code.land(skip)
        code.land(not_cross)
        code.call_method((g, "gFM"), "handleDefaultInputs", [("r", 3)])
        code.push(False)
        code.op("Return")
        code.end_method(body)

    def exit_screen():
        body = code.begin_method(registers=3)
        code.call_method((g, "SettingsAdapter"), "CancelKeyCapture")
        code.path(("r", 1), "srInterval")
        code.push(None)
        code.op("Equals2")
        no_interval = code.branch_if_true()
        call_function("clearInterval", [lambda: code.path(("r", 1), "srInterval")])
        code.op("Pop")
        code.land(no_interval)
        for name in KEY_ROWS:
            code.call_method(this + (name,), "OnExit")
        code.call_method(("Object",), "registerClass", ["ScrKeysOpt", None])
        code.push(("r", 2), "ScrKeysOpt")
        code.op("Delete")
        code.op("Pop")
        code.end_method(body)

    code.set_member(proto, "srRefresh", refresh)
    code.set_member(proto, "srPoll", poll)
    code.set_member(proto, "InitGui", init_gui)
    code.set_member(proto, "HandleInput", handle_input)
    code.set_member(proto, "ExitScreen", exit_screen)
    code.end()
    code.finish()
    items.insert(items.index(cls) + 1, m.init_action(4, at))
    m.set_frame_items(rec, items)
    return m.write()


def struct_f32(m, at):
    import struct
    return struct.unpack_from(">f", m.b.b, at)[0]


# --- texts -------------------------------------------------------------------

# The save screens speak of the console and its memory units; on PC the saves are files.
PC_TEXTS = {
    "Load_Warning": "Loading saved game. Please don't close the game.",
    "Save_Warning": "Saving game. Please don't close the game.",
    "SaveFailed_Body": "The save file could not be written. Save failed!",
    "LoadFailed_Body": "The save file could not be read. Load failed!",
    "SaveNoSpace_Body": "There is not enough free disk space to save the game.",
    "No_Device_Warning": "NO SAVE FOLDER IS AVAILABLE. GAMES CANNOT BE SAVED.",
}


def edit_interface_text(xml):
    text = xml.decode("utf-8-sig")
    for key, value in PC_TEXTS.items():
        text, n = re.subn(rf'(<uitext id="{re.escape(key)}" value=")[^"]*(")',
                          # Apostrophes stay as they are (the game shows an &#x27; literally,
                          # as its own texts do); only what would break the attribute is escaped.
                          lambda m: m.group(1) + html.escape(value, quote=False).replace('"', "&quot;")
                          + m.group(2), text)
        if n != 1:
            raise SystemExit(f"INTERFACETEXT: {key} found {n} times")
    lines = "".join(f'\t<uitext id="{k}" value="{html.escape(v)}"/>\r\n' for k, v in TEXTS + key_texts())
    block = "\r\n\t<!-- COMPONENT: GRAPHICS AND DISPLAY OPTIONS (Superman Returns PC) -->\r\n" + lines
    end = text.rindex("</uilocalizedtextlist>")
    return ("﻿" + text[:end] + block + text[end:]).encode("utf-8")


def build(data_dir, out_dir):
    ui = apt_edit.Archive(open(os.path.join(data_dir, "ui.AST"), "rb").read())
    replace, add = {}, []

    inner, d, c = movie_of(ui, "MAIN")
    replace["MAIN"] = repack(inner, d, c, *edit_main(d[1], c[1]))
    inner, d, c = movie_of(ui, "SCRCONTROLROOM")
    replace["SCRCONTROLROOM"] = repack(inner, d, c, *edit_control_room(d[1], c[1]))
    inner, d, c = movie_of(ui, "SCRPAUSEMENU")
    replace["SCRPAUSEMENU"] = repack(inner, d, c, *edit_pause_menu(d[1], c[1]))
    inner, d, c = movie_of(ui, "SCRPAUSEMENU")
    add.append(("SCROPTIONSMENU", repack(inner, d, c, *make_options_menu(d[1], c[1]), name="SCROPTIONSMENU"),
                ENTRY_FLAGS))
    for entry, cls_name in (("SCRGAMEPLAYOPT", "ScrGameplayOpt"), ("SCRCONTROLLEROPT", "ScrControllerOpt"),
                            ("SCRAUDIOOPT", "ScrAudioOpt")):
        inner, d, c = movie_of(ui, entry)
        replace[entry] = repack(inner, d, c, *edit_game_options_screen(d[1], c[1], cls_name))
    inner, d, c = movie_of(ui, "SCRAUDIOOPT")
    audio_data, audio_const = edit_game_options_screen(d[1], c[1], "ScrAudioOpt")
    add.append(("SCRMOUSEOPT", repack(inner, d, c, *make_mouse_screen(audio_data, audio_const), name="SCRMOUSEOPT"),
                ENTRY_FLAGS))
    inner, d, c = movie_of(ui, "SCRGAMEPLAYOPT")
    add.append(("SCRKEYSOPT", repack(inner, d, c, *make_keys_screen(d[1], c[1]), name="SCRKEYSOPT"), ENTRY_FLAGS))
    for cls_name, label_key, title_key, _, id_base, screen_rows, row_step in SCREENS:
        inner, d, c = movie_of(ui, "SCRGAMEPLAYOPT")
        add.append((cls_name.upper(), repack(inner, d, c, name=cls_name.upper(), *make_options_screen(
            d[1], c[1], cls_name, label_key, title_key, id_base, screen_rows, row_step)), ENTRY_FLAGS))
    add.append(("ICONGRAPHICSOPT", ui.data(ui.find("ICONCONTROLLEROPT")), ui.find("ICONCONTROLLEROPT")["flags"]))
    inner, d, c = movie_of(ui, "SCRPRESSSTART")
    replace["SCRPRESSSTART"] = repack(inner, d, c, *edit_press_start(d[1], c[1]))
    inner, d, c = movie_of(ui, "BUTTONICON")
    replace["BUTTONICON"] = repack(inner, d, c, *edit_button_icon(d[1], c[1]))
    for entry, sprite, cls_name in (("SCRMOVESLIST", 14, "ScrMovesList"), ("SCRSTATISTICS", 26, "ScrStatistics")):
        inner, d, c = movie_of(ui, entry)
        replace[entry] = repack(inner, d, c, *edit_scroll_screen(d[1], c[1], sprite, cls_name))
    add += key_icons.entries(ui)
    with open(os.path.join(out_dir, "ui_sr.AST"), "wb") as f:
        f.write(ui.build(replace=replace, add=add))

    text = apt_edit.Archive(open(os.path.join(data_dir, "text.AST"), "rb").read())
    xml = text.data(text.find("INTERFACETEXT"))
    with open(os.path.join(out_dir, "text_sr.AST"), "wb") as f:
        f.write(text.build(replace={"INTERFACETEXT": edit_interface_text(xml)}))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data_dir")
    ap.add_argument("out_dir", nargs="?")
    a = ap.parse_args()
    build(a.data_dir, a.out_dir or a.data_dir)


if __name__ == "__main__":
    main()
