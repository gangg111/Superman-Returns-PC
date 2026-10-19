import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
AUDIO_TOOLS = ROOT / "tools" / "audio"
GAME_DATA = ROOT.parent / "SupermanReturnsPC" / "game" / "DATA"
sys.path.insert(0, str(AUDIO_TOOLS))


def _prop(tag, ptype, raw):
    # {tag reversed, u32 type, u32 length, u32 count} + data, little endian
    return tag[::-1].encode() + struct.pack("<III", ptype, len(raw), 1) + raw


def _sabr(props, snr, payload=b""):
    """Single-sound record: "rbas", 4 bytes, property list, zero pad to 16, SNR, payload."""
    body = b"rbas" + b"\0" * 4 + b"".join(_prop(*p) for p in props)
    body += b"\0" * (-len(body) % 16)
    return body + snr + payload


def _block(samples, streams, last=False):
    """EAAC block: u8 flag + u24 BE size, u32 BE samples, per stream u32 BE (len*4) + data,
    where len counts the 4-byte length field itself."""
    body = b"".join(struct.pack(">I", (4 + len(s)) * 4) + s for s in streams)
    size = 8 + len(body)
    return struct.pack(">II", (0x80 if last else 0) << 24 | size, samples) + body


def _bgfa(entries):
    """Uncompressed BGFA: header 0x30, data, TOC (flags 1 B, offset 4, stored size 4, name 16)."""
    data, toc = bytearray(), b""
    for name, body in entries:
        toc += b"\0" + struct.pack("<II", 0x30 + len(data), len(body)) + name.encode().ljust(16, b"\0")
        data += body
    head = b"BGFA" + b"0003" + b"\0" * 4 + struct.pack("<IIII", len(entries), 0x30 + len(data), 0, len(toc))
    head += b"\0" * 4 + bytes([0, 0, 0, 4, 4, 0, 0, 0]) + struct.pack("<II", 0, 16)
    return head + bytes(data) + toc


@pytest.fixture
def make_bgfa():
    return _bgfa


@pytest.fixture
def make_prop():
    return _prop


@pytest.fixture
def make_sabr():
    return _sabr


@pytest.fixture
def make_block():
    return _block
