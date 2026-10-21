"""Editing the game's Apt movies and BGFA archives (see apt.py for the format).

Edits are append-only, as Xerenge does for Burnout Revenge's settings movie:
the original bytes stay where they are, so every offset the game's own code
and structures hold stays valid, and what is new goes after them - new code,
new strings, new placements, and the lists that point at them.

  Blob       a big-endian byte buffer
  Constants  a constant file, its entries kept and added to, written anew
  Code       an ActionScript assembler appending to a data blob
  Archive    a BGFA archive rewritten with entries replaced or added
"""
import struct

import apt

FUNCTION_MARK = (0x98765432, 0x12345678)
# DefineFunction2 flags as the game's own methods have them: this in r1,
# _global in r2 (PreloadGlobal | SuppressSuper | SuppressArgs | PreloadThis).
METHOD_FLAGS = 0x0129

# Constant types.
STRING, REGISTER, BOOL, UNDEFINED, INT = 1, 4, 5, 3, 7

OP = {name: op for op, name in apt.OPS.items()}


class Blob:
    def __init__(self, data=b""):
        self.b = bytearray(data)

    def __len__(self):
        return len(self.b)

    def here(self):
        return len(self.b)

    def u32(self, at):
        return struct.unpack_from(">I", self.b, at)[0]

    def set32(self, at, value):
        struct.pack_into(">I", self.b, at, value & 0xFFFFFFFF)

    def setf(self, at, value):
        struct.pack_into(">f", self.b, at, value)

    def put8(self, value):
        self.b.append(value & 0xFF)

    def put32(self, value):
        self.b += struct.pack(">I", value & 0xFFFFFFFF)

    def putf(self, value):
        self.b += struct.pack(">f", value)

    def align(self, to):
        while len(self.b) % to:
            self.b.append(0)

    def put_string(self, text):
        self.align(4)
        at = self.here()
        self.b += text.encode("latin-1") + b"\0"
        return at

    def str(self, at):
        end = self.b.index(0, at)
        return self.b[at:end].decode("latin-1")


class Constants:
    """A constant file: (type, value) entries, strings by text."""

    def __init__(self, raw):
        if not raw.startswith(apt.MAGIC_CONST):
            raise ValueError("not an Apt constant file")
        self.header = bytes(raw[:20])
        self.root, count, table = struct.unpack_from(">III", raw, 20)
        self.entries = []
        for i in range(count):
            t, v = struct.unpack_from(">II", raw, table + i * 8)
            text = raw[v:raw.index(b"\0", v)].decode("latin-1") if t == STRING else None
            self.entries.append([t, v, text])

    def _find(self, t, v=None, text=None):
        for i, (et, ev, etext) in enumerate(self.entries):
            if et == t and (etext == text if t == STRING else ev == v):
                return i
        self.entries.append([t, v or 0, text])
        return len(self.entries) - 1

    def string(self, text):
        return self._find(STRING, text=text)

    def int(self, value):
        return self._find(INT, value & 0xFFFFFFFF)

    def bool(self, value):
        return self._find(BOOL, 1 if value else 0)

    def undefined(self):
        return self._find(UNDEFINED, 0)

    def register(self, n):
        return self._find(REGISTER, n)

    def rename(self, old, new):
        n = 0
        for e in self.entries:
            if e[0] == STRING and e[2] == old:
                e[2] = new
                n += 1
        return n

    def write(self):
        b = Blob(self.header)
        b.put32(self.root)
        b.put32(len(self.entries))
        b.put32(32)
        table = b.here()
        b.b += bytes(8 * len(self.entries))
        for i, (t, v, text) in enumerate(self.entries):
            if t == STRING:
                v = b.put_string(text)
            b.set32(table + i * 8, t)
            b.set32(table + i * 8 + 4, v)
        b.align(16)
        return bytes(b.b)


class Code:
    """ActionScript into a data blob, using the constant file for every value.
    PushData lists and parameter lists go after the code (finish)."""

    def __init__(self, blob: Blob, constants: Constants):
        self.b, self.c = blob, constants
        self._lists = []    # (where the list offset goes, constant indices)
        self._params = []   # (where the list offset goes, [(register, name)])
        self._names = []    # (where a string's offset goes, the string)

    def start(self):
        return self.b.here()

    def op(self, name):
        self.b.put8(OP[name])

    def _aligned(self, name):
        self.b.put8(OP[name])
        self.b.align(4)

    def push(self, *values):
        """Values: str -> string constant, bool, int, None -> undefined,
        ('r', n) -> register n."""
        idx = []
        for v in values:
            if isinstance(v, tuple) and v[0] == "r":
                idx.append(self.c.register(v[1]))
            elif v is None:
                idx.append(self.c.undefined())
            elif isinstance(v, bool):
                idx.append(self.c.bool(v))
            elif isinstance(v, int):
                idx.append(self.c.int(v))
            else:
                idx.append(self.c.string(v))
        self._aligned("PushData")
        self.b.put32(len(idx))
        self._lists.append((self.b.here(), idx))
        self.b.put32(0)

    # --- expressions -------------------------------------------------------

    def path(self, *names):
        """Push the value of a dotted path: a register or a variable, then
        members."""
        first = names[0]
        if isinstance(first, tuple):
            self.push(first)
        else:
            self.push(first)
            self.op("GetVariable")
        for n in names[1:]:
            self.push(n)
            self.op("GetMember")

    def call_method(self, obj_path, method, args=(), keep=False):
        """obj.method(args...): each arg a callable that pushes it, or a value."""
        for a in reversed(args):
            self._arg(a)
        self.push(len(args))
        self.path(*obj_path)
        self.push(method)
        self.op("CallMethod")
        if not keep:
            self.op("Pop")

    def _arg(self, a):
        if callable(a):
            a()
        else:
            self.push(a)

    def set_member(self, obj_path, name, value):
        self.path(*obj_path)
        self.push(name)
        self._arg(value)
        self.op("SetMember")

    def set_variable(self, name, value):
        self.push(name)
        self._arg(value)
        self.op("SetVariable")

    def array(self, values):
        for v in reversed(values):
            self._arg(v)
        self.push(len(values))
        self.op("InitArray")

    def set_register(self, n):
        """The value on top of the stack into register n (the value popped)."""
        self._aligned("SetRegister")
        self.b.put32(n)
        self.op("Pop")

    # --- control flow ------------------------------------------------------

    def branch_if_true(self):
        self._aligned("BranchIfTrue")
        at = self.b.here()
        self.b.put32(0)
        return at

    def branch(self):
        self._aligned("BranchAlways")
        at = self.b.here()
        self.b.put32(0)
        return at

    def land(self, at):
        self.b.set32(at, self.b.here() - (at + 4))

    def begin_method(self, params=(), registers=8):
        """DefineFunction2 with this in r1, _global in r2 and the parameters
        in r3, r4, ...; returns the token for end_method."""
        self._aligned("DefineFunction2")
        self._names.append((self.b.here(), ""))
        self.b.put32(0)
        self.b.put32(len(params))
        self.b.put32((max(registers, 3 + len(params)) << 16) | METHOD_FLAGS)
        self._params.append((self.b.here(), [(3 + i, p) for i, p in enumerate(params)]))
        self.b.put32(0)
        size_at = self.b.here()
        self.b.put32(0)
        self.b.put32(FUNCTION_MARK[0])
        self.b.put32(FUNCTION_MARK[1])
        return size_at

    def end_method(self, size_at):
        self.b.set32(size_at, self.b.here() - (size_at + 12))

    def end(self):
        self.op("End")

    def finish(self):
        for at, idx in self._lists:
            self.b.align(4)
            lst = self.b.here()
            for i in idx:
                self.b.put32(i)
            self.b.set32(at, lst)
        for at, regs in self._params:
            self.b.align(4)
            lst = self.b.here()
            for reg, name in regs:
                self.b.put32(reg)
                self._names.append((self.b.here(), name))
                self.b.put32(0)
            self.b.set32(at, lst)
        for at, text in self._names:
            self.b.set32(at, self.b.put_string(text))
        self._lists, self._params, self._names = [], [], []


class MovieEdit:
    """A movie's data blob and constants, edited in place and appended to."""

    def __init__(self, data, constants):
        apt.Movie(data, constants)  # validates
        self.b = Blob(data)
        self.c = Constants(constants)
        self.root = self.c.root
        self.b.align(16)

    def code(self):
        return Code(self.b, self.c)

    def frame_items(self, character_at, frame=0):
        """(frame record offset, [item offsets]) of a sprite's or the movie's frame."""
        lst = self.b.u32(character_at + 20)
        rec = lst + frame * 8
        n, items = self.b.u32(rec), self.b.u32(rec + 4)
        return rec, [self.b.u32(items + i * 4) for i in range(n)]

    def set_frame_items(self, rec, items):
        self.b.align(4)
        at = self.b.here()
        for it in items:
            self.b.put32(it)
        self.b.set32(rec, len(items))
        self.b.set32(rec + 4, at)

    def character(self, index):
        return self.b.u32(self.b.u32(self.root + 32) + index * 4)

    def placement_name(self, item):
        return self.b.str(self.b.u32(item + 52)) if self.b.u32(item + 4) & 0x20 else ""

    def place_copy(self, template, name, depth, x, y, on_load):
        """A copy of a placement (flags, character, matrix, colour) with its own
        name, depth, position and load event code."""
        events_template = self.b.u32(template + 60)
        event_flags = self.b.u32(self.b.u32(events_template + 4))
        self.b.align(4)
        events = self.b.here()
        self.b.put32(1)
        self.b.put32(events + 8)
        self.b.put32(event_flags)
        self.b.put32(0)
        self.b.put32(on_load)
        self.b.align(4)
        at = self.b.here()
        self.b.b += self.b.b[template:template + 64]
        self.b.set32(at + 8, depth)
        self.b.setf(at + 32, x)
        self.b.setf(at + 36, y)
        self.b.set32(at + 52, self.b.put_string(name))
        self.b.set32(at + 60, events)
        return at

    def set_load_code(self, item, code_at):
        """A placement's load event replaced by `code_at` (its flags kept)."""
        old = self.b.u32(item + 60)
        event_flags = self.b.u32(self.b.u32(old + 4))
        self.b.align(4)
        events = self.b.here()
        self.b.put32(1)
        self.b.put32(events + 8)
        self.b.put32(event_flags)
        self.b.put32(0)
        self.b.put32(code_at)
        self.b.set32(item + 60, events)

    def init_action(self, sprite, code_at):
        """A frame item running code as the sprite's init action (#initclip)."""
        self.b.align(4)
        at = self.b.here()
        self.b.put32(8)
        self.b.put32(sprite)
        self.b.put32(code_at)
        return at

    def action(self, code_at):
        self.b.align(4)
        at = self.b.here()
        self.b.put32(1)
        self.b.put32(code_at)
        return at

    def rename_export(self, old, new):
        """Point every export named `old` at a new string `new`."""
        count, lst = self.b.u32(self.root + 56), self.b.u32(self.root + 60)
        n = 0
        for i in range(count):
            at = lst + i * 8
            if self.b.str(self.b.u32(at)) == old:
                self.b.set32(at, self.b.put_string(new))
                n += 1
        return n

    def write(self):
        self.b.align(16)
        return bytes(self.b.b), self.c.write()


# --- BGFA ------------------------------------------------------------------

def fnv1a64(text):
    h = 0xCBF29CE484222325
    for ch in text.upper().encode("latin-1"):
        h = ((h ^ ch) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Archive:
    """A BGFA archive, rewritten with the same field widths as the original."""

    def __init__(self, raw):
        if raw[:4] != b"BGFA":
            raise ValueError("not a BGFA archive")
        self.raw = raw
        self.head = bytearray(raw[:0x30])
        files, self.toc_off, _, self.toc_size = struct.unpack_from("<IIII", raw, 0x0C)
        (_, self.flag_w, self.hash_w, self.off_w, self.zsize_w, self.size_w,
         self.shift, _) = raw[0x20:0x28]
        self.num, self.name_w = struct.unpack_from("<II", raw, 0x28)
        toc = raw[self.toc_off:self.toc_off + self.toc_size]
        self.prefix = toc[:4 * self.num]
        p = 4 * self.num
        self.entries = []
        for _ in range(files):
            e = {}
            if self.flag_w:
                e["flags"] = struct.unpack_from("<H", toc, p)[0]
                p += 2
            else:
                e["flags"] = toc[p]
                p += 1
            e["hash"] = toc[p:p + self.hash_w]
            p += self.hash_w
            e["offset"] = int.from_bytes(toc[p:p + self.off_w], "little") << self.shift
            p += self.off_w
            e["zsize"] = int.from_bytes(toc[p:p + self.zsize_w], "little")
            p += self.zsize_w
            e["size"] = int.from_bytes(toc[p:p + self.size_w], "little")
            p += self.size_w
            e["name"] = toc[p:p + self.name_w].split(b"\0")[0].decode("latin-1")
            p += self.name_w
            self.entries.append(e)

    def find(self, name):
        for e in self.entries:
            if e["name"] == name:
                return e
        raise KeyError(name)

    def data(self, e):
        if e["size"]:
            raise NotImplementedError("compressed entry")
        return self.raw[e["offset"]:e["offset"] + e["zsize"]]

    def build(self, replace=None, add=None):
        """A new archive with the entries in `replace` ({name or hash bytes:
        data}) and the new ones in `add` ([(name, data, flags)], hashed by
        name) given new data.

        An archive with its TOC at the end (ui.AST) keeps its data where it is,
        the new data after it and the TOC after that. One with its TOC before
        the data (a screen's own archive) is laid out anew: header, TOC at its
        place, then every entry's data in the original order."""
        replace = dict(replace or {})
        unit = 1 << self.shift
        entries = [dict(e) for e in self.entries]
        toc_last = self.toc_off >= max(e["offset"] for e in entries)

        def new_data(e):
            for key in (e["name"], e["hash"]):
                if key and key in replace:
                    return replace.pop(key)
            return None

        def pad(buf):
            while len(buf) % unit:
                buf.append(0)

        if toc_last:
            out = bytearray(self.raw[:self.toc_off])
            for e in entries:
                d = new_data(e)
                if d is not None:
                    pad(out)
                    e["offset"], e["zsize"], e["size"] = len(out), len(d), 0
                    out += d
        else:
            if add:
                raise ValueError("adding to an archive with its TOC first")
            out = bytearray(self.raw[:self.toc_off + self.toc_size])
            for e in sorted(entries, key=lambda e: e["offset"]):
                d = new_data(e)
                d = self.data(e) if d is None else d
                pad(out)
                e["offset"], e["zsize"], e["size"] = len(out), len(d), 0
                out += d
        if replace:
            raise KeyError(f"not in the archive: {sorted(map(str, replace))}")
        for name, d, flags in add or ():
            pad(out)
            entries.append({"flags": flags, "hash": fnv1a64(name).to_bytes(8, "little"),
                            "offset": len(out), "zsize": len(d), "size": 0, "name": name})
            out += d
        pad(out)
        toc = bytearray(self.prefix)
        for e in entries:
            if self.flag_w:
                toc += struct.pack("<H", e["flags"])
            else:
                toc.append(e["flags"])
            toc += e["hash"]
            if e["offset"] % unit:
                raise ValueError("entry not aligned")
            for value, width in ((e["offset"] >> self.shift, self.off_w), (e["zsize"], self.zsize_w),
                                 (e["size"], self.size_w)):
                if value >= 1 << (8 * width) and width:
                    raise ValueError(f"{e['name']}: {value} does not fit {width} byte(s)")
                toc += value.to_bytes(width, "little")
            toc += e["name"].encode("latin-1")[:self.name_w].ljust(self.name_w, b"\0")
        if toc_last:
            toc_off = len(out)
            out += toc
        else:
            toc_off = self.toc_off
            if len(toc) != self.toc_size:
                raise ValueError("TOC changed size")
            out[toc_off:toc_off + len(toc)] = toc
        struct.pack_into("<I", out, 0x0C, len(entries))
        struct.pack_into("<I", out, 0x10, toc_off)
        struct.pack_into("<I", out, 0x18, len(toc))
        return bytes(out)
