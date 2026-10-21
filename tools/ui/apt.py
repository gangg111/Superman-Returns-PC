"""Reader and disassembler for the game's menus: EA Apt movies (Xbox 360).

The front end (DATA/ui.AST, a BGFA archive) holds one entry per screen or
component; each entry is itself a BGFA holding the movie's "Apt Data:1:7:4"
blob, its "Apt constant file" and, for icons, textures. Both blobs are
big-endian and every offset in them is relative to the blob's own start.

The layout is EA's Apt as OpenSAGE reads it for C&C 3 (little-endian there)
and as Xerenge's aptdump reads it for Burnout Revenge (big-endian, same
generation):
  constants   "Apt constant file" (20 bytes), root character offset in the
              data, entry count, offset of the entries; an entry is
              (type, value): 1 string (offset), 2 property, 3 undefined,
              4 register, 5 bool, 6 float, 7 int, 8 constant pool lookup
  character   type, 0x09876543, two zero words (8 bytes more than Burnout's
              header), then by type (movie 9, sprite 5, button 4, text 2,
              shape 1, image 7, font 3)
  frame       item count, offset of a list of item pointers
  item        1 action, 2 label, 3 place, 4 remove, 5 background, 8 init action
  actions     SWF ActionScript 2 bytecode plus EA's opcodes; some operands are
              aligned to 4 and point elsewhere in the blob

Read-only: nothing here writes game data.
"""
import argparse
import struct
import sys
from dataclasses import dataclass, field

MAGIC_DATA = b"Apt Data:"
MAGIC_CONST = b"Apt constant file"
CHARACTER_MARK = 0x09876543

OPS = {
    0x00: "End", 0x04: "NextFrame", 0x05: "PrevFrame", 0x06: "Play", 0x07: "Stop",
    0x0A: "Add", 0x0B: "Subtract", 0x0C: "Multiply", 0x0D: "Divide", 0x0E: "Equals",
    0x0F: "LessThan", 0x10: "And", 0x11: "Or", 0x12: "Not", 0x13: "StringEquals",
    0x14: "StringLength", 0x15: "SubString", 0x17: "Pop", 0x18: "ToInteger",
    0x1C: "GetVariable", 0x1D: "SetVariable", 0x20: "SetTarget2", 0x21: "StringConcat",
    0x22: "GetProperty", 0x23: "SetProperty", 0x24: "CloneSprite", 0x25: "RemoveSprite",
    0x26: "Trace", 0x29: "StringCompare", 0x2A: "Throw", 0x2B: "CastOp",
    0x2C: "ImplementsOp", 0x30: "Random", 0x34: "GetTimer", 0x3A: "Delete",
    0x3B: "Delete2", 0x3C: "DefineLocal", 0x3D: "CallFunction", 0x3E: "Return",
    0x3F: "Modulo", 0x40: "NewObject", 0x41: "Var", 0x42: "InitArray",
    0x43: "InitObject", 0x44: "TypeOf", 0x45: "TargetPath", 0x46: "Enumerate",
    0x47: "Add2", 0x48: "LessThan2", 0x49: "Equals2", 0x4A: "ToNumber",
    0x4B: "ToString", 0x4C: "PushDuplicate", 0x4D: "StackSwap", 0x4E: "GetMember",
    0x4F: "SetMember", 0x50: "Increment", 0x51: "Decrement", 0x52: "CallMethod",
    0x53: "NewMethod", 0x54: "InstanceOf", 0x55: "Enumerate2", 0x56: "PushThis",
    0x58: "PushGlobal", 0x59: "PushZero", 0x5A: "PushOne", 0x5B: "CallFuncPop",
    0x5C: "CallFunc", 0x5D: "CallMethodPop", 0x5E: "CallMethod(EA)", 0x60: "BitwiseAnd",
    0x61: "BitwiseOr", 0x62: "BitwiseXOr", 0x63: "ShiftLeft", 0x64: "ShiftRight",
    0x65: "ShiftRight2", 0x66: "StrictEqual", 0x67: "Greater", 0x68: "StringGreater",
    0x69: "Extends", 0x70: "PushThisVar", 0x71: "PushGlobalVar", 0x72: "ZeroVar",
    0x73: "PushTrue", 0x74: "PushFalse", 0x75: "PushNull", 0x76: "PushUndefined",
    0x77: "TraceStart", 0x81: "GotoFrame", 0x83: "GetURL", 0x87: "SetRegister",
    0x88: "ConstantPool", 0x8A: "WaitForFrame", 0x8B: "SetTarget", 0x8C: "GotoLabel",
    0x8D: "WaitForFrameExpr", 0x8E: "DefineFunction2", 0x8F: "Try", 0x94: "With",
    0x96: "PushData", 0x99: "BranchAlways", 0x9A: "GetURL2", 0x9B: "DefineFunction",
    0x9D: "BranchIfTrue", 0x9E: "CallFrame", 0x9F: "GotoFrame2", 0xA1: "PushString",
    0xA2: "PushConstantByte", 0xA3: "PushConstantWord", 0xA4: "GetStringVar",
    0xA5: "GetStringMember", 0xA6: "SetStringVar", 0xA7: "SetStringMember",
    0xAE: "PushValueOfVar", 0xAF: "GetNamedMember", 0xB0: "CallNamedFuncPop",
    0xB1: "CallNamedFunc", 0xB2: "CallNamedMethodPop", 0xB3: "CallNamedMethod",
    0xB4: "PushFloat", 0xB5: "PushByte", 0xB6: "PushShort", 0xB7: "PushLong",
    0xB8: "BranchIfFalse", 0xB9: "PushRegister", 0xFF: "Padding",
}
# Opcodes whose operands start at the next multiple of 4.
ALIGNED = {0x9B, 0x8E, 0x88, 0x9D, 0x99, 0x96, 0x83, 0x8C, 0x87, 0x8B, 0x81, 0x9F,
           0x94, 0xA1, 0xB8, 0xA4, 0xA6, 0xA5, 0xA7}
POOL_BYTE = {0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3}


@dataclass
class Constant:
    type: int
    value: int
    text: str = ""


@dataclass
class Instruction:
    at: int
    op: int
    name: str
    args: list = field(default_factory=list)  # decoded operands
    text: str = ""  # operands as shown


class Movie:
    def __init__(self, data: bytes, constants: bytes):
        if not data.startswith(MAGIC_DATA) or not constants.startswith(MAGIC_CONST):
            raise ValueError("not an Apt movie")
        self.data = data
        self.raw_constants = constants
        self.root = self._u32(constants, 20)
        count = self._u32(constants, 24)
        table = self._u32(constants, 28)
        self.constants = []
        for i in range(count):
            t, v = struct.unpack_from(">II", constants, table + i * 8)
            self.constants.append(Constant(t, v, self._str(constants, v) if t == 1 else ""))
        if self.u32(self.root) != 9 or self.u32(self.root + 4) != CHARACTER_MARK:
            raise ValueError(f"no movie character at {self.root:X}")

    @staticmethod
    def _u32(b, at):
        return struct.unpack_from(">I", b, at)[0] if at + 4 <= len(b) else 0

    @staticmethod
    def _str(b, at):
        end = b.find(b"\0", at)
        return b[at:end if end >= 0 else len(b)].decode("latin-1")

    def u8(self, at):
        return self.data[at] if at < len(self.data) else 0

    def u16(self, at):
        return struct.unpack_from(">H", self.data, at)[0]

    def u32(self, at):
        return self._u32(self.data, at)

    def i32(self, at):
        return struct.unpack_from(">i", self.data, at)[0]

    def f32(self, at):
        return struct.unpack_from(">f", self.data, at)[0]

    def str(self, at):
        return self._str(self.data, at)

    # --- structure -----------------------------------------------------------

    def header(self):
        r = self.root
        return {
            "frames": (self.u32(r + 16), self.u32(r + 20)),
            "characters": (self.u32(r + 28), self.u32(r + 32)),
            "size": (self.u32(r + 36), self.u32(r + 40)),
            "ms_per_frame": self.u32(r + 44),
            "imports": [(self.str(self.u32(im)), self.str(self.u32(im + 4)), self.u32(im + 8))
                        for im in (self.u32(r + 52) + i * 16 for i in range(self.u32(r + 48)))],
            "exports": [(self.str(self.u32(ex)), self.u32(ex + 4))
                        for ex in (self.u32(r + 60) + i * 8 for i in range(self.u32(r + 56)))],
        }

    def characters(self):
        count, lst = self.header()["characters"]
        return [self.u32(lst + i * 4) for i in range(count)]

    def frames(self, count, lst):
        """Each frame's items as dicts."""
        out = []
        for f in range(count):
            frame = lst + f * 8
            items = []
            n, item_list = self.u32(frame), self.u32(frame + 4)
            for i in range(n):
                items.append(self.item(self.u32(item_list + i * 4)))
            out.append(items)
        return out

    def item(self, at):
        t = self.u32(at)
        it = {"at": at, "type": t}
        if t == 1:
            it["code"] = self.u32(at + 4)
        elif t == 2:
            it["label"] = self.str(self.u32(at + 4))
        elif t == 3:
            flags = self.u32(at + 4) & 0xFF
            it.update(flags=flags, depth=self.i32(at + 8),
                      character=self.i32(at + 12) if flags & 2 else -1,
                      matrix=[self.f32(at + 16 + k * 4) for k in range(4)],
                      pos=(self.f32(at + 32), self.f32(at + 36)),
                      name=self.str(self.u32(at + 52)) if flags & 0x20 else "",
                      events=[])
            if flags & 0x80 and self.u32(at + 60):
                ev = self.u32(at + 60)
                for e in range(self.u32(ev)):
                    rec = self.u32(ev + 4) + e * 12
                    it["events"].append((self.u32(rec), self.u32(rec + 8)))
        elif t == 4:
            it["depth"] = self.i32(at + 4)
        elif t == 8:
            it.update(sprite=self.u32(at + 4), code=self.u32(at + 8))
        return it

    # --- bytecode ------------------------------------------------------------

    def constant_text(self, index, pool=None):
        if index >= len(self.constants):
            return f"const#{index}?"
        c = self.constants[index]
        if c.type == 1:
            return repr(c.text)
        if c.type == 2:
            return f"property {c.value}"
        if c.type == 3:
            return "undefined"
        if c.type == 4:
            return f"r{c.value}"
        if c.type == 5:
            return "true" if c.value else "false"
        if c.type == 6:
            return repr(struct.unpack(">f", struct.pack(">I", c.value))[0])
        if c.type == 7:
            return str(struct.unpack(">i", struct.pack(">I", c.value))[0])
        if c.type == 8:
            if pool is not None and c.value < len(pool) and pool[c.value] != index:
                return self.constant_text(pool[c.value])
            return f"lookup {c.value}"
        return f"type{c.type}:{c.value}"

    def disassemble(self, start):
        """One block of bytecode from start to its End (past the furthest branch).
        Nested function bodies are part of the block."""
        out = []
        pool = []
        pc = furthest = start
        while pc < len(self.data):
            at = pc
            op = self.u8(pc)
            pc += 1
            name = OPS.get(op)
            if name is None:
                out.append(Instruction(at, op, f"op{op:02X}?", text="(unknown opcode, stopped)"))
                break
            if op in ALIGNED:
                pc = (pc + 3) & ~3
            args, text = [], ""
            if op == 0x88:
                count, lst = self.u32(pc), self.u32(pc + 4)
                pc += 8
                pool = [self.u32(lst + i * 4) for i in range(count)]
                args = list(pool)
                text = f"{count} constant(s)"
            elif op == 0x96:
                count, lst = self.u32(pc), self.u32(pc + 4)
                pc += 8
                args = [self.u32(lst + i * 4) for i in range(count)]
                text = ", ".join(self.constant_text(a, pool) for a in args)
            elif op in POOL_BYTE or op == 0xA3:
                if op == 0xA3:
                    index = self.u16(pc)
                    pc += 2
                else:
                    index = self.u8(pc)
                    pc += 1
                args = [index]
                text = self.constant_text(pool[index]) if index < len(pool) else f"pool#{index}?"
            elif op in (0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0x8C):
                args = [self.str(self.u32(pc))]
                text = repr(args[0])
                pc += 4
            elif op == 0x83:
                args = [self.str(self.u32(pc)), self.str(self.u32(pc + 4))]
                text = f"{args[0]!r}, {args[1]!r}"
                pc += 8
            elif op in (0x81, 0x87, 0x9F, 0x8B, 0x94):
                args = [self.i32(pc)]
                text = str(args[0])
                pc += 4
            elif op in (0x99, 0x9D, 0xB8):
                offset = self.i32(pc)
                pc += 4
                target = pc + offset
                furthest = max(furthest, target)
                args = [target]
                text = f"-> {target:06X}"
            elif op == 0xB4:
                args = [self.f32(pc)]
                text = repr(args[0])
                pc += 4
            elif op in (0xB5, 0xB9):
                args = [self.u8(pc)]
                text = str(args[0])
                pc += 1
            elif op == 0xB6:
                args = [self.u16(pc)]
                text = str(args[0])
                pc += 2
            elif op == 0xB7:
                args = [self.i32(pc)]
                text = str(args[0])
                pc += 4
            elif op in (0x9B, 0x8E):
                fname = self.str(self.u32(pc))
                if op == 0x9B:
                    count, lst = self.u32(pc + 4), self.u32(pc + 8)
                    params = [self.str(self.u32(lst + i * 4)) for i in range(count)]
                    body = self.u32(pc + 12)
                    pc += 16 + 8
                else:
                    count, lst = self.u32(pc + 4), self.u32(pc + 12)
                    params = [f"r{self.u32(lst + i * 8)}={self.str(self.u32(lst + i * 8 + 4))}"
                              for i in range(count)]
                    body = self.u32(pc + 16)
                    pc += 20 + 8
                args = [fname, params, body]
                text = f"{fname!r} ({', '.join(params)}) body {body} byte(s)"
            out.append(Instruction(at, op, name, args, text))
            if op == 0x00 and pc > furthest:
                break
        return out


def read_bgfa_movie(entry_bytes):
    """The (data, constants) pair of an entry of ui.AST."""
    sys.path.insert(0, __file__.rsplit("tools", 1)[0] + "tools/audio")
    import ea_ast  # noqa: E402
    inner = ea_ast.Bgfa(None, data=entry_bytes)
    data = constants = None
    for e in inner.entries:
        b = inner.read(e)
        if b.startswith(MAGIC_DATA):
            data = b
        elif b.startswith(MAGIC_CONST):
            constants = b
    if data is None or constants is None:
        raise ValueError("entry holds no Apt movie")
    return data, constants


def dump(movie, out=sys.stdout):
    h = movie.header()
    w = out.write
    w(f"root at {movie.root:X}: {h['size'][0]}x{h['size'][1]}, {h['ms_per_frame']} ms a frame, "
      f"{h['frames'][0]} frame(s), {h['characters'][0]} character(s)\n")
    for src, name, ch in h["imports"]:
        w(f"  import {name!r} from {src!r} as character {ch}\n")
    for name, ch in h["exports"]:
        w(f"  export {name!r} = character {ch}\n")
    seen = set()

    def code(start, indent):
        if start in seen:
            w(f"{indent}(code at {start:06X}, shown above)\n")
            return
        seen.add(start)
        for ins in movie.disassemble(start):
            w(f"{indent}{ins.at:06X}  {ins.name:<20} {ins.text}\n")

    def frames(count, lst, indent):
        for f, items in enumerate(movie.frames(count, lst)):
            w(f"{indent}frame {f} ({len(items)} item(s))\n")
            for it in items:
                t = it["type"]
                if t == 1:
                    w(f"{indent}  action:\n")
                    code(it["code"], indent + "    ")
                elif t == 2:
                    w(f"{indent}  label {it['label']!r}\n")
                elif t == 3:
                    m = it["matrix"]
                    w(f"{indent}  place depth {it['depth']} character {it['character']}"
                      f"{' name ' + repr(it['name']) if it['name'] else ''} flags {it['flags']:02X}"
                      f" matrix [{m[0]:g} {m[1]:g} {m[2]:g} {m[3]:g}] at ({it['pos'][0]:g}, {it['pos'][1]:g})\n")
                    for flags, c in it["events"]:
                        w(f"{indent}    clip event {flags:08X}:\n")
                        code(c, indent + "      ")
                elif t == 4:
                    w(f"{indent}  remove depth {it['depth']}\n")
                elif t == 8:
                    w(f"{indent}  init action for sprite {it['sprite']}:\n")
                    code(it["code"], indent + "    ")
                else:
                    w(f"{indent}  item type {t} at {it['at']:06X}\n")

    for i, ch in enumerate(movie.characters()):
        if not ch or i == 0:
            continue
        t = movie.u32(ch)
        if t == 5:
            w(f"  character {i}: sprite, {movie.u32(ch + 16)} frame(s)\n")
            frames(movie.u32(ch + 16), movie.u32(ch + 20), "    ")
        elif t == 2:
            w(f"  character {i}: text\n")
        else:
            w(f"  character {i}: type {t}\n")
    w("  the movie's own frames:\n")
    frames(*h["frames"], "    ")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="DATA/ui.AST")
    ap.add_argument("names", nargs="+", help="entries, e.g. SCRAUDIOOPT")
    args = ap.parse_args()
    sys.path.insert(0, __file__.rsplit("tools", 1)[0] + "tools/audio")
    import ea_ast  # noqa: E402
    archive = ea_ast.Bgfa(args.archive)
    by = archive.by_name()
    for name in args.names:
        print(f"== {name}")
        dump(Movie(*read_bgfa_movie(archive.read(by[name]))))


if __name__ == "__main__":
    main()
