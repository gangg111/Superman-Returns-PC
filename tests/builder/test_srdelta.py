"""srdelta: the builder's patches (a target from a source, our bytes added, the rest copied).

Input: two byte strings, the source (the player's game file) and the target (our file)."""
import hashlib
import os
import random
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "builder"))
import srdelta  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def sample():
    rnd = random.Random(7)
    source = bytes(rnd.getrandbits(8) for _ in range(20000))
    # the target: a copy of 5000..9000, our 100 bytes, the source's start and a changed byte
    ours = b"OUR-BYTES-" * 10
    target = source[5000:9000] + ours + source[:3000] + b"\x99" + source[3001:6000]
    return source, target, ours


def test_round_trip_and_only_our_bytes_added():
    source, target, ours = sample()
    patch = srdelta.make(source, target)
    assert srdelta.apply(source, patch) == target
    # source: the target built by hand above: 100 bytes of ours + 1 changed byte; copies need >= 48
    assert srdelta.added_bytes(patch) == len(ours) + 1


def test_header_names_both_files():
    source, target, _ = sample()
    patch = srdelta.make(source, target)
    assert patch[:8] == b"SRDLT001"
    # source: the format in srdelta's docstring
    assert struct.unpack_from("<Q", patch, 8)[0] == len(source)
    assert patch[16:48] == hashlib.sha256(source).digest()
    assert struct.unpack_from("<Q", patch, 48)[0] == len(target)
    assert patch[56:88] == hashlib.sha256(target).digest()


def test_wrong_source_is_refused():
    source, target, _ = sample()
    patch = srdelta.make(source, target)
    with pytest.raises(ValueError):
        srdelta.apply(source[:-1] + b"\0", patch)


@pytest.mark.skipif(not os.path.exists(os.path.join(ROOT, "game", "DATA", "text_sr.AST")),
                    reason="needs the game's text.AST and the built text_sr.AST")
def test_game_text_patch_carries_little():
    src = open(os.path.join(ROOT, "game", "DATA", "text.AST"), "rb").read()
    dst = open(os.path.join(ROOT, "game", "DATA", "text_sr.AST"), "rb").read()
    patch = srdelta.make(src, dst)
    assert srdelta.apply(src, patch) == dst
    # source: text_sr.AST adds our texts (a few KB of XML) to INTERFACETEXT; most is copied
    assert srdelta.added_bytes(patch) < len(dst) // 10
