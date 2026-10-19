"""Readers for Superman Returns' audio data (Xbox 360, EA Tiburon 2006).

BGFA archives (*.AST): little-endian header, table of contents at the end of
the file, variable-width fields (layout as in Aluigi's nbajamfire.bms).
Sound records ("sabr", stored as the reversed tag "rbas"): a property list
(tag, type, length, count, value) followed by an EAAudioCore SNR header and,
for RAM sounds, the EA-XMA blocks. Streamed sounds keep their blocks in a
separate entry named <NAME>_SNS.

Everything here is read-only; nothing is written to the game data.
"""
import struct
import zlib
from dataclasses import dataclass, field


def refpack_decompress(src: bytes) -> bytes:
    """EA RefPack (QFS): header 0x10FB (+0x0100 bit: 4-byte sizes, +0x0001 bit:
    compressed size present), big-endian uncompressed size, then commands that
    copy literals and back-references."""
    flags = src[0]
    if src[1] != 0xFB:
        raise ValueError("not RefPack")
    size_len = 4 if flags & 0x80 else 3
    pos = 2
    if flags & 0x01:
        pos += size_len  # compressed size
    out_size = int.from_bytes(src[pos:pos + size_len], "big")
    pos += size_len
    out = bytearray()
    n = len(src)
    while pos < n:
        b0 = src[pos]
        if b0 < 0x80:  # 2 bytes: 0-3 literals, copy 3-10 from offset up to 1024
            b1 = src[pos + 1]
            pos += 2
            lit = b0 & 3
            out += src[pos:pos + lit]
            pos += lit
            count = ((b0 >> 2) & 7) + 3
            offset = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:  # 3 bytes: copy 4-67 from offset up to 16384
            b1, b2 = src[pos + 1], src[pos + 2]
            pos += 3
            lit = b1 >> 6
            out += src[pos:pos + lit]
            pos += lit
            count = (b0 & 0x3F) + 4
            offset = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:  # 4 bytes: copy 5-1028 from offset up to 131072
            b1, b2, b3 = src[pos + 1], src[pos + 2], src[pos + 3]
            pos += 4
            lit = b0 & 3
            out += src[pos:pos + lit]
            pos += lit
            count = ((b0 & 0x0C) << 6) + b3 + 5
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:  # literals only, 4-112
            lit = ((b0 & 0x1F) << 2) + 4
            pos += 1
            out += src[pos:pos + lit]
            pos += lit
            continue
        else:  # end: 0-3 trailing literals
            lit = b0 & 3
            pos += 1
            out += src[pos:pos + lit]
            break
        start = len(out) - offset
        for i in range(count):  # overlapping copies are byte by byte
            out.append(out[start + i])
    if len(out) != out_size:
        raise ValueError(f"RefPack size {len(out)} != {out_size}")
    return bytes(out)


@dataclass
class Entry:
    index: int
    flags: int
    hash: bytes
    offset: int
    stored_size: int
    size: int
    name: str


class Bgfa:
    def __init__(self, path, base=0, data=None):
        self.path, self.base = path, base
        self._data = data
        head = self._read(0, 0x30)
        if head[:4] != b"BGFA":
            raise ValueError(f"{path}: not a BGFA archive")
        self.version = head[4:8].decode("latin1")
        files, toc_off, _, toc_size = struct.unpack_from("<IIII", head, 0x0C)
        _, flag_w, hash_w, off_w, zsize_w, size_w, shift, _ = head[0x20:0x28]
        num, name_w = struct.unpack_from("<II", head, 0x28)
        toc = self._read(toc_off, toc_size)
        p = 4 * num
        self.entries = []
        for i in range(files):
            if flag_w:
                flags = struct.unpack_from("<H", toc, p)[0]
                p += 2
            else:
                flags = toc[p]
                p += 1
            h = toc[p:p + hash_w]
            p += hash_w
            off = int.from_bytes(toc[p:p + off_w], "little")
            p += off_w
            zsize = int.from_bytes(toc[p:p + zsize_w], "little") if zsize_w else 0
            p += zsize_w
            size = int.from_bytes(toc[p:p + size_w], "little") if size_w else 0
            p += size_w
            name = toc[p:p + name_w].split(b"\0")[0].decode("latin1")
            p += name_w
            self.entries.append(Entry(i, flags, h, off << shift, zsize, size, name))
        self.toc_used, self.toc_size = p, toc_size

    def _read(self, off, n):
        if self._data is not None:
            return self._data[off:off + n]
        with open(self.path, "rb") as f:
            f.seek(self.base + off)
            return f.read(n)

    def read(self, e: Entry) -> bytes:
        raw = self._read(e.offset, e.stored_size)
        if e.size:  # compressed: uncompressed size present
            if raw[1:2] == b"\xfb" and raw[0] in (0x10, 0x11, 0x90, 0x91):
                # The TOC's size field is not the unpacked size for RefPack entries
                # (lunits.AST: 211071 against a header size of 295688); the
                # decompressor checks the header's own size instead.
                return refpack_decompress(raw)
            for wbits in (15, -15):
                try:
                    return zlib.decompress(raw, wbits)
                except zlib.error:
                    pass
            raise NotImplementedError(f"{e.name}: compressed, not zlib/deflate "
                                      f"(flags {e.flags:#x}, head {raw[:4].hex()})")
        return raw

    def by_name(self):
        return {e.name: e for e in self.entries}


@dataclass
class Snr:
    codec: int
    channels: int
    rate: int
    kind: int          # 0 = RAM, 1 = streamed (SNS entry)
    loop: bool
    samples: int
    loop_start: int
    header_size: int


def parse_snr(d: bytes, pos: int) -> Snr:
    h1, h2 = struct.unpack_from(">II", d, pos)
    version = h1 >> 28
    if version != 0:
        raise ValueError(f"SNR version {version}")
    codec = (h1 >> 24) & 15
    channels = ((h1 >> 18) & 0x3F) + 1
    rate = h1 & 0x3FFFF
    kind, loop, samples = h2 >> 30, bool((h2 >> 29) & 1), h2 & 0x1FFFFFFF
    loop_start = struct.unpack_from(">I", d, pos + 8)[0] if loop else 0
    return Snr(codec, channels, rate, kind, loop, samples, loop_start, 12 if loop else 8)


@dataclass
class Sabr:
    props: dict = field(default_factory=dict)
    snr: Snr = None
    snr_pos: int = 0


def _prop_value(ptype, raw, count):
    if ptype == 0:
        return struct.unpack_from("<i", raw)[0] if len(raw) >= 4 else raw.hex()
    if ptype == 1:  # 24.8 fixed point (inferred: volm 0x6400 = 100.0)
        return struct.unpack_from("<i", raw)[0] / 256.0 if len(raw) >= 4 else raw.hex()
    if ptype == 2:
        return raw.split(b"\0")[0].decode("latin1")
    return raw.hex()


def parse_props(d: bytes, p: int):
    """Property list as the game walks it (sub_8232DD10): entries of
    {tag, type, length, count} + length bytes of data; the value of the first
    "numc" entry is the number of entries, itself included. Returns the
    properties and the position after the last entry."""
    props, total, n = {}, None, 0
    while total is None or n < total:
        tag = d[p:p + 4][::-1].decode("latin1")
        ptype, length, count = struct.unpack_from("<III", d, p + 4)
        raw = d[p + 16:p + 16 + length]
        if tag == "numc":
            total = struct.unpack_from("<I", raw)[0]
        props[tag] = _prop_value(ptype, raw, count)
        p += 16 + length
        n += 1
        if n > 64:
            raise ValueError("property list does not end")
    return props, p


def parse_sabr(d: bytes) -> Sabr:
    """A single-sound record (sound.AST): the property list from offset 8, then
    the SNR header at the next 16-byte boundary."""
    if d[:4] != b"rbas":
        raise ValueError("not a sabr record")
    rec = Sabr()
    rec.props, end = parse_props(d, 8)
    rec.snr_pos = (end + 15) & ~15
    rec.snr = parse_snr(d, rec.snr_pos)
    return rec


def xma2_riff(data: bytes, channels: int, rate: int, samples: int) -> bytes:
    """Standard XMA2 RIFF around raw XMA packets (one stream, 1 or 2 channels)."""
    fmt = struct.pack("<HHIIHHH", 0x166, channels, rate, rate * channels * 2, 0x800, 16, 34)
    fmt += struct.pack("<HIIIIIIIBBH", 1, 3 if channels == 2 else 4, samples, 0x10000, 0,
                       samples, 0, 0, 0, 4, max(1, len(data) // 0x10000 + 1))
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" +
            struct.pack("<I", len(data)) + data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def decode_xma(data: bytes, channels: int, rate: int, samples: int):
    """Decode one XMA stream with FFmpeg to int16 samples (frames x channels)."""
    import subprocess

    import numpy as np
    proc = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "wav", "-i", "pipe:0", "-f", "s16le", "pipe:1"],
        input=xma2_riff(data, channels, rate, samples), capture_output=True)
    if proc.returncode:
        raise RuntimeError(proc.stderr.decode(errors="replace")[:200])
    return np.frombuffer(proc.stdout, dtype="<i2").reshape(-1, channels)


def decode_sound(record: bytes, sns: bytes = None):
    """PCM (int16, frames x channels) of a single-sound record; streamed records
    take their blocks from the _SNS entry. Channels come as stereo pairs, one
    XMA stream each (4 channels = streams 0 and 1)."""
    import numpy as np
    rec = parse_sabr(record)
    snr = rec.snr
    if snr.codec != 3:
        raise NotImplementedError(f"codec {snr.codec}")
    if snr.kind == 1:
        if sns is None:
            raise ValueError("streamed sound needs its _SNS entry")
        buf, pos = sns, 0
    else:
        buf, pos = record, rec.snr_pos + snr.header_size
    parts, block_samples = [], None
    for stream in range((snr.channels + 1) // 2):
        ch = min(2, snr.channels - 2 * stream)
        data, block_samples = deblock(buf, pos, stream)
        parts.append(decode_xma(data, ch, snr.rate, block_samples))
    n = min(len(p) for p in parts)
    return rec, np.concatenate([p[:n] for p in parts], axis=1), block_samples


def is_bank(d: bytes) -> bool:
    """A bank record starts its body with a variant count, not with "numc"."""
    return d[:4] == b"rbas" and d[8:12] != b"cmun"


def parse_bank(d: bytes):
    """Bank of variants (e.g. speech lines): u32 count, two u32, count name
    offsets, count property-list offsets, u32 size of the lists, the lists
    (numc, flsz, ...), then NUL-terminated names of the variant records.
    Layout read from the records and the game's use in sub_8233AF50."""
    n = struct.unpack_from("<I", d, 8)[0]
    names_off = struct.unpack_from(f"<{n}I", d, 0x14)
    props_off = struct.unpack_from(f"<{n}I", d, 0x14 + 4 * n)
    props_size = struct.unpack_from("<I", d, 0x14 + 8 * n)[0]
    props_base = 0x18 + 8 * n
    names_base = props_base + props_size
    variants = []
    for i in range(n):
        props, _ = parse_props(d, props_base + props_off[i])
        start = names_base + names_off[i]
        name = d[start:d.index(b"\0", start)].decode("latin1")
        variants.append((name, props))
    return variants


def block_list(buf: bytes, pos: int):
    """Sample count of every EAAC block (same walk as deblock): the native
    SndPlayer consumes the converted PCM in these blocks, like the game."""
    blocks = []
    while pos + 8 <= len(buf):
        flag = buf[pos]
        size = struct.unpack_from(">I", buf, pos)[0] & 0xFFFFFF
        if size == 0:
            break
        blocks.append(struct.unpack_from(">I", buf, pos + 4)[0])
        pos += size
        if flag == 0x80:
            break
    return blocks


def deblock(buf: bytes, pos: int, stream_no: int):
    """XMA packet data of one stream from EAAC blocks (layout per vgmstream
    ea_eaac_streamfile.h): u8 flag (0x80 = last) + u24 BE block size, u32 BE
    sample count, then per stream u32 BE (size * 4) + data. Returns the data
    padded to 2048-byte packets and the summed block sample counts."""
    out, total = bytearray(), 0
    while pos + 8 <= len(buf):
        flag = buf[pos]
        size = struct.unpack_from(">I", buf, pos)[0] & 0xFFFFFF
        if size == 0:
            break
        total += struct.unpack_from(">I", buf, pos + 4)[0]
        skip = 8
        for _ in range(stream_no):
            skip += struct.unpack_from(">I", buf, pos + skip)[0] // 4
        dsz = struct.unpack_from(">I", buf, pos + skip)[0] // 4
        data = buf[pos + skip + 4:pos + skip + dsz]
        if len(data) % 0x800:
            data += b"\xff" * (0x800 - len(data) % 0x800)
        out += data
        pos += size
        if flag == 0x80:
            break
    return bytes(out), total
