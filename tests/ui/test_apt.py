"""Tests for tools/ui/apt.py.
Input: a movie laid out by hand here (its bytes written field by field, the
expected values known from that layout), and the game's SCRAUDIOOPT when the
game data is present."""
import os
import struct
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ui'))
import apt  # noqa: E402

UI_AST = os.path.join(ROOT, 'game', 'DATA', 'ui.AST')


def be(*words):
    return b''.join(struct.pack('>I', w) for w in words)


def hand_movie():
    """Data: magic (16), root movie at 0x10 (64 bytes: 1 frame at 0x54, 1
    character listed at 0x50, 640x480, 33 ms, no imports or exports), the
    frame's item list at 0x5C -> one action item at 0x64 whose code at 0x6C is
        PushData [const 0, const 1]  (0x96, 3 pad, count 2, list at 0x7C)
        SetVariable, PushByte 7, End (at 0x78, 0x79, 0x7B).
    Constants: "x" (string), 5 (int)."""
    data = bytearray(b'Apt Data:1:7:4\x1a\x00')            # 0x00..0x0F
    data += be(9, apt.CHARACTER_MARK, 0, 0,                 # 0x10 root
               1, 0x54, 0,                                  # frames: 1 at 0x54
               1, 0x50,                                     # characters: 1, list at 0x50
               640, 480, 33,                                # size, ms
               0, 0, 0, 0)                                  # imports, exports
    assert len(data) == 0x50
    data += be(0x10)                                        # 0x50 character list -> root
    data += be(1, 0x5C)                                     # 0x54 frame: 1 item, list 0x5C
    data += be(0x64, 0)                                     # 0x5C item list (+pad)
    data += be(1, 0x6C)                                     # 0x64 action item, code 0x6C
    assert len(data) == 0x6C
    data += bytes([0x96, 0, 0, 0]) + be(2, 0x7C)            # 0x6C PushData, count 2, list 0x7C
    data += bytes([0x1D, 0xB5, 7, 0x00])                    # 0x78 SetVariable, PushByte 7, End
    data += be(0, 1)                                        # 0x7C the list: constants 0 and 1
    const = bytearray(b'Apt constant file\x1a\x00\x00')      # 20 bytes
    const += be(0x10, 2, 32)                                # root, count, table
    const += be(1, 48, 7, 5)                                # 0x20: "x" at 48, int 5
    const += b'x\x00'
    return bytes(data), bytes(const)


def test_header_of_hand_movie():
    m = apt.Movie(*hand_movie())
    h = m.header()
    assert h['size'] == (640, 480)
    assert h['ms_per_frame'] == 33
    assert h['frames'] == (1, 0x54)
    assert m.characters() == [0x10]
    assert [c.text for c in m.constants] == ['x', '']
    assert m.constant_text(1) == '5'


def test_frame_and_bytecode_of_hand_movie():
    m = apt.Movie(*hand_movie())
    items = m.frames(1, 0x54)[0]
    assert items == [{'at': 0x64, 'type': 1, 'code': 0x6C}]
    code = m.disassemble(0x6C)
    assert [(i.at, i.name) for i in code] == [
        (0x6C, 'PushData'), (0x78, 'SetVariable'), (0x79, 'PushByte'), (0x7B, 'End')]
    assert code[0].args == [0, 1]
    assert code[0].text == "'x', 5"
    assert code[2].args == [7]


def test_rejects_non_movie():
    data, const = hand_movie()
    with pytest.raises(ValueError):
        apt.Movie(b'junk' + data[4:], const)


@pytest.mark.skipif(not os.path.exists(UI_AST), reason='game data not present')
def test_audio_options_screen():
    # Known from the game's audio options: two Slider rows labelled as the
    # screen shows them, the Slider imported from components/Slider.
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'audio'))
    import ea_ast
    archive = ea_ast.Bgfa(UI_AST)
    m = apt.Movie(*apt.read_bgfa_movie(archive.read(archive.by_name()['SCRAUDIOOPT'])))
    h = m.header()
    assert h['size'] == (1280, 720)
    assert h['imports'] == [('components/Slider', 'Slider', 1)]
    screen = m.characters()[2]
    rows = [it for it in m.frames(m.u32(screen + 16), m.u32(screen + 20))[0] if it['type'] == 3]
    assert [r['name'] for r in rows] == ['Music', 'SoundFX']
    labels = [i.args[0] for r in rows for i in m.disassemble(r['events'][0][1]) if i.name == 'SetStringVar'
              and i.args[0] not in ('_global.gFM',)]
    assert labels == ['Music Volume', 'Sound Effects']
