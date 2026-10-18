"""peek.py <image> addr[:nwords] ... : dump big-endian words at guest addresses,
and for each word that points into the image, show the string there if printable."""
import struct
import sys

data = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = struct.unpack_from("<H", data, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
    secs.append((0x82000000 + va, max(vsize, rsize), raw, rsize))


def off(a):
    for va, size, raw, rsize in secs:
        if va <= a < va + size:
            return raw + a - va if a - va < rsize else -1
    return None


def s_at(a):
    o = off(a)
    if o is None or o < 0:
        return None
    e = data.find(b"\0", o, o + 120)
    s = data[o:e if e > 0 else o + 120]
    if len(s) >= 3 and all(32 <= b < 127 for b in s):
        return s.decode()
    return None


for arg in sys.argv[2:]:
    a, _, n = arg.partition(":")
    a = int(a, 16)
    n = int(n or 8)
    print(f"== {a:08X}")
    for i in range(n):
        o = off(a + 4 * i)
        if o is None:
            print("  (not in image)")
            break
        if o < 0:
            print(f"  +{4*i:3d}: bss")
            continue
        w = struct.unpack_from(">I", data, o)[0]
        s = s_at(w) if 0x82000000 <= w < 0x84000000 else None
        extra = f'  "{s}"' if s else ""
        if 0x82000000 <= w < 0x84000000 and not s:
            o2 = off(w)
            if o2 is not None and o2 >= 0:
                w2 = struct.unpack_from(">I", data, o2)[0]
                s2 = s_at(w2) if 0x82000000 <= w2 < 0x84000000 else None
                if s2:
                    extra = f'  -> [{w2:08X}] "{s2}"'
        print(f"  +{4*i:3d}: {w:08X}{extra}")
