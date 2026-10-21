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
  INTERFACETEXT   (text.AST) the screen's texts

  build_ui_mod.py <game DATA dir> [<out dir, default: the same>]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apt  # noqa: E402
import apt_edit  # noqa: E402

# The rows: (row name, label key, label, options key, options). Ids are the
# row's index; port/src/graphics_menu.cpp maps them to cvars in this order.
# A Toggle row's left arrow sits about 296 px right of its label (measured on
# the running screen), so a label longer than "Character Shadows" runs into it.
ROWS = [
    ("Gfx0", "GFX_OPT_RT_SHADOWS", "RT Shadows", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx1", "GFX_OPT_SHADOW_SOFTNESS", "Shadow Softness", "GFX_OPT_SOFTNESS_LEVELS",
     "Hard|Low|Medium|High|Ultra"),
    ("Gfx2", "GFX_OPT_CHARACTER_SHADOWS", "Character Shadows", "GFX_OPT_OFF_ON", "Off|On"),
    ("Gfx3", "GFX_OPT_RT_REFLECTIONS", "RT Reflections", "GFX_OPT_OFF_ON", "Off|On"),
]
TEXTS = [("Label_ScrGraphicsOpt", "GRAPHICS OPTIONS")] + \
    sorted({(r[1], r[2]) for r in ROWS} | {(r[3], r[4]) for r in ROWS})

SCREEN_PATH = "screens/misc/ScrGraphicsOpt"
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


def repack(inner, data, const, new_data, new_const):
    return inner.build(replace={data[0]: new_data, const[0]: new_const})


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
    code.set_member(("_global", "ICON"), "GRAPHICS_OPT", ICON_PATH)
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
    m = apt_edit.MovieEdit(data, const)
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    by_name = {m.placement_name(it): it for it in sitems if m.b.u32(it) == 3}
    audio, quit_ = by_name["btn8"], by_name["btn9"]
    code = m.code()
    y_audio = struct_f32(m, audio + 36)
    # Graphics Options: Audio's placement one row down, tab 8; Quit after it, tab 9.
    ours = m.place_copy(audio, "btn10", 37, struct_f32(m, audio + 32), y_audio + PAUSE_STEP,
                        button_code(code, 8))
    m.b.setf(quit_ + 36, struct_f32(m, quit_ + 36) + PAUSE_STEP)
    m.set_load_code(quit_, button_code(code, 9))
    m.set_frame_items(srec, sitems + [ours])

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
        code.call_method(this + ("btn10",), "SetWidth", [791])
        code.call_method(this + ("btn10",), "SetText", [lambda: code.call_method(
            (g, "LocalizationAdapter"), "GetLocalizedText", ["Label_ScrGraphicsOpt"], keep=True)])
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
        code.push("btn10")
        code.op("Equals2")
        code.op("Not")
        not_ours = code.branch_if_true()
        # as the game's own buttons: remembered, pressed, the open sound, the screen
        code.set_member((g,), "PauseSelectedItem", "btn10")
        code.call_method((("r", 6),), "handleInput", [("r", 3)])
        code.call_method((g,), "playSound", [lambda: code.path(g, "SOUND", "OPEN")])
        code.call_method((g, "FlowMgr"), "SwitchScreen", [lambda: code.path(g, "SCREEN", "GRAPHICS_OPT")])
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


# --- SCRGRAPHICSOPT --------------------------------------------------------

def make_graphics_screen(data, const):
    """Gameplay Options' movie made into Graphics Options."""
    m = apt_edit.MovieEdit(data, const)
    for old, new in (("ScrGameplayOpt", "ScrGraphicsOpt"), ("__Packages.ScrGameplayOpt", "__Packages.ScrGraphicsOpt")):
        m.rename_export(old, new)
        m.c.rename(old, new)
    m.c.rename("Label_ScrGameplayOpt", "Label_ScrGraphicsOpt")

    # The rows: copies of the game's top row (X-Axis Inversion) placement.
    screen = m.character(2)
    srec, sitems = m.frame_items(screen)
    rows = [it for it in sitems if m.b.u32(it) == 3]
    template = min(rows, key=lambda it: struct_f32(m, it + 36))
    kept = [it for it in sitems if m.b.u32(it) != 3]
    code = m.code()
    placed = []
    for i, (name, _, label, _, options) in enumerate(ROWS):
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
        placed.append(m.place_copy(template, name, 1 + 12 * i, ROW_X, ROW_FIRST_Y + ROW_STEP * i, at))
    m.set_frame_items(srec, kept + placed)

    # The class's methods, replaced after its init action (sprite 4).
    rec, items = m.frame_items(m.root)
    cls = find_init_action(m, items, 4)
    at = code.start()
    proto = ("_global", "ScrGraphicsOpt", "prototype")
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
        code.call_method((g,), "SetScreenTitle", [localized("Label_ScrGraphicsOpt")])
        paused()
        not_paused = code.branch_if_true()
        code.call_method((g,), "SetFortressDisplayWell", [True])
        code.land(not_paused)
        code.call_method((g,), "ShowHints", [localized("Generic_BACK"), "B"])
        for i, (name, key, _, options_key, _) in enumerate(ROWS):
            row = this + (name,)
            code.call_method(row, "SetLabel", [localized(key)])
            # SetData(GetLocalizedText(options).split("|"))
            code.call_method(row, "SetData", [lambda k=options_key: (
                code.push("|", 1), localized(k)(), code.push("split"), code.op("CallMethod"))])
            code.call_method(row, "SetIndex", [lambda i=i: code.call_method(
                (g, "SettingsAdapter"), "GetGraphicsOption", [i], keep=True)])
        code.end_method(body)

    def handle_input():
        body = code.begin_method(["keyCode", "controllerId", "ascii"], registers=6)

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
        # B: back to where the screen came from.
        key_is("TRIANGLE")
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
        for i, (name, *_) in enumerate(ROWS):
            code.call_method((g, "gFM"), "getSelectedItem", keep=True)
            code.push("_name")
            code.op("GetMember")
            code.push(name)
            code.op("Equals2")
            code.op("Not")
            skip = code.branch_if_true()
            code.call_method((g, "SettingsAdapter"), "SetGraphicsOption", [
                i, lambda n=name: code.call_method(this + (n,), "GetIndex", keep=True)])
            code.land(skip)
        code.land(no_change)
        code.push(False)
        code.op("Return")
        code.end_method(body)

    def exit_screen():
        body = code.begin_method()
        for name, *_ in ROWS:
            code.call_method(this + (name,), "OnExit")
        # As the game's screens do: the class unregistered and deleted.
        code.call_method(("Object",), "registerClass", ["ScrGraphicsOpt", None])
        code.push(("r", 2), "ScrGraphicsOpt")
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


def struct_f32(m, at):
    import struct
    return struct.unpack_from(">f", m.b.b, at)[0]


# --- texts -------------------------------------------------------------------

def edit_interface_text(xml):
    text = xml.decode("utf-8-sig")
    lines = "".join(f'\t<uitext id="{k}" value="{v}"/>\r\n' for k, v in TEXTS)
    block = "\r\n\t<!-- COMPONENT: GRAPHICS OPTIONS (Superman Returns PC) -->\r\n" + lines
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
    inner, d, c = movie_of(ui, "SCRGAMEPLAYOPT")
    add.append(("SCRGRAPHICSOPT", repack(inner, d, c, *make_graphics_screen(d[1], c[1])), ENTRY_FLAGS))
    add.append(("ICONGRAPHICSOPT", ui.data(ui.find("ICONCONTROLLEROPT")), ui.find("ICONCONTROLLEROPT")["flags"]))
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
