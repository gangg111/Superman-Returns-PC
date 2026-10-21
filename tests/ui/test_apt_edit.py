"""Tests for tools/ui/apt_edit.py.
Inputs: the hand-built movie of test_apt.py (its layout known byte by byte),
the hash ui.AST stores for SCRAUDIOOPT, and the game's archives when present
(rebuilding one with nothing changed must give its own bytes back)."""
import os
import struct
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'ui'))
sys.path.insert(0, os.path.dirname(__file__))
import apt  # noqa: E402
import apt_edit  # noqa: E402
from test_apt import hand_movie  # noqa: E402

UI_AST = os.path.join(ROOT, 'game', 'DATA', 'ui.AST')
have_game = pytest.mark.skipif(not os.path.exists(UI_AST), reason='game data not present')


def test_name_hash_matches_the_archive():
    # ui.AST's TOC holds bc5f6625e0c45454 for SCRAUDIOOPT (read with a hex dump).
    assert apt_edit.fnv1a64('SCRAUDIOOPT').to_bytes(8, 'little').hex() == 'bc5f6625e0c45454'
    assert apt_edit.fnv1a64('scraudioopt') == apt_edit.fnv1a64('SCRAUDIOOPT')


def test_constants_round_trip_and_additions():
    _, raw = hand_movie()
    c = apt_edit.Constants(raw)
    assert c.string('x') == 0          # already there
    assert c.int(5) == 1
    assert c.string('new') == 2        # appended
    assert c.bool(True) == 3
    out = c.write()
    root, count, table = struct.unpack_from('>III', out, 20)
    assert (root, count, table) == (0x10, 4, 32)
    entries = [struct.unpack_from('>II', out, 32 + i * 8) for i in range(4)]
    assert entries[1] == (7, 5) and entries[3] == (5, 1)
    # strings laid out after the table (32 + 4 * 8 = 64), each aligned to 4
    assert entries[0] == (1, 64) and out[64:66] == b'x\0'
    assert entries[2] == (1, 68) and out[68:72] == b'new\0'


def test_assembler_bytes():
    data, raw = hand_movie()
    m = apt_edit.MovieEdit(data, raw)
    start = len(m.b)                      # the hand movie's 0x84 bytes, aligned to 16
    assert start == 0x90
    code = m.code()
    code.push('x', 5)
    code.op('SetVariable')
    code.end()
    code.finish()
    b = bytes(m.b.b)
    # 0x90: 0x96, 3 pad, count 2 (0x94), list offset (0x98); 0x9C: 0x1D, 0x00;
    # the list at the next multiple of 4, 0xA0: constants 0 and 1
    assert b[0x90:0x9C] == bytes([0x96, 0, 0, 0]) + struct.pack('>II', 2, 0xA0)
    assert b[0x9C:0x9E] == bytes([0x1D, 0x00])
    assert b[0xA0:0xA8] == struct.pack('>II', 0, 1)


def test_method_and_branch_disassemble():
    data, raw = hand_movie()
    m = apt_edit.MovieEdit(data, raw)
    code = m.code()
    at = code.start()
    body = code.begin_method(['k'])
    code.push(('r', 3))
    skip = code.branch_if_true()
    code.push('x')
    code.op('Pop')
    code.land(skip)
    code.end_method(body)
    code.end()
    code.finish()
    d, c = m.write()
    ins = apt.Movie(d, c).disassemble(at)
    assert [i.name for i in ins] == ['DefineFunction2', 'PushData', 'BranchIfTrue', 'PushData', 'Pop', 'End']
    fname, params, size = ins[0].args
    assert params == ['r3=k']
    assert ins[1].text == 'r3'
    assert ins[2].args[0] == ins[5].at        # lands on the End after the body
    assert size == ins[5].at - ins[1].at      # the body: from the first push to the End


@have_game
def test_archive_rebuilt_unchanged_holds_the_same_entries():
    # The game's archives pad with leftover bytes, so the rebuilt one is
    # compared entry by entry: the same TOC fields and the same data.
    raw = open(UI_AST, 'rb').read()
    a = apt_edit.Archive(raw)
    assert len(a.entries) == 828
    rebuilt = apt_edit.Archive(raw).build()
    assert rebuilt[:0x0C] == raw[:0x0C]
    b = apt_edit.Archive(rebuilt)
    assert b.entries == a.entries
    inner = a.data(a.find('SCRAUDIOOPT'))
    ia, ib = apt_edit.Archive(inner), apt_edit.Archive(apt_edit.Archive(inner).build())
    assert [(e['hash'], e['flags'], ib.data(e)) for e in ib.entries] == \
        [(e['hash'], e['flags'], ia.data(e)) for e in ia.entries]
    assert ib.data(ib.entries[1]).startswith(b'Apt Data:1:7:4')


@have_game
def test_archive_add_and_replace():
    raw = open(UI_AST, 'rb').read()
    a = apt_edit.Archive(raw)
    out = a.build(replace={'SCRAUDIOOPT': b'replaced'}, add=[('SCRNEW', b'new data', 272)])
    b = apt_edit.Archive(out)
    assert len(b.entries) == 829
    assert b.data(b.find('SCRAUDIOOPT')) == b'replaced'
    assert b.data(b.find('SCRNEW')) == b'new data'
    assert b.find('SCRNEW')['hash'] == apt_edit.fnv1a64('SCRNEW').to_bytes(8, 'little')
    assert b.data(b.find('SCRCONTROLROOM')) == a.data(a.find('SCRCONTROLROOM'))
