"""The builder's patches: a file made from a game file without shipping the game's bytes.

A patch says how to write the target from the source the player has: COPY a run of the
source, or ADD bytes of our own. The builder (builder/delta.h) applies it and checks the
SHA-256 of the source before and of the target after, so a patch never yields a wrong file.

    "SRDLT001"
    u64 source size, 32 bytes source SHA-256, u64 target size, 32 bytes target SHA-256
    ops: u8 1 COPY  u64 source offset  u64 length
         u8 2 ADD   u64 length  + the bytes
         u8 0 end
(little endian)

The encoder takes the longest common prefix and suffix as copies, then walks the rest:
a run of BLOCK bytes found in the source (indexed every BLOCK bytes) becomes a copy grown
both ways as far as the bytes agree; what no copy covers is added.

    py srdelta.py make <source> <target> <patch>
    py srdelta.py apply <source> <patch> <target>
"""
import hashlib
import struct
import sys

MAGIC = b"SRDLT001"
BLOCK = 32
MIN_COPY = 48  # shorter matches cost more as ops than as bytes


def _prefix(a, b):
    n = min(len(a), len(b))
    lo, step = 0, 1 << 20
    while lo < n:
        hi = min(n, lo + step)
        if a[lo:hi] == b[lo:hi]:
            lo = hi
            continue
        if step == 1:
            break
        step //= 2
    return lo


def make(source: bytes, target: bytes) -> bytes:
    ops = []  # ("C", offset, length) or ("A", bytes)
    pre = _prefix(source, target)
    rest_s, rest_t = source[pre:], target[pre:]
    suf = _prefix(rest_s[::-1], rest_t[::-1]) if rest_s and rest_t else 0
    if pre:
        ops.append(("C", 0, pre))
    mid_end = len(target) - suf
    index = {}
    for off in range(0, len(source) - BLOCK + 1, BLOCK):
        index.setdefault(source[off:off + BLOCK], off)
    i, add_from = pre, pre
    while i + BLOCK <= mid_end:
        src = index.get(target[i:i + BLOCK])
        if src is None:
            i += 1
            continue
        # grow forward and backward
        end_t, end_s = i + BLOCK, src + BLOCK
        while end_t < mid_end and end_s < len(source):
            n = min(4096, mid_end - end_t, len(source) - end_s)
            if target[end_t:end_t + n] == source[end_s:end_s + n]:
                end_t += n
                end_s += n
                continue
            while end_t < mid_end and end_s < len(source) and target[end_t] == source[end_s]:
                end_t += 1
                end_s += 1
            break
        start_t, start_s = i, src
        while start_t > add_from and start_s > 0 and target[start_t - 1] == source[start_s - 1]:
            start_t -= 1
            start_s -= 1
        if end_t - start_t < MIN_COPY:
            i += 1
            continue
        if start_t > add_from:
            ops.append(("A", target[add_from:start_t]))
        ops.append(("C", start_s, end_t - start_t))
        i = add_from = end_t
    if mid_end > add_from:
        ops.append(("A", target[add_from:mid_end]))
    if suf:
        ops.append(("C", len(source) - suf, suf))
    out = bytearray(MAGIC)
    out += struct.pack("<Q", len(source)) + hashlib.sha256(source).digest()
    out += struct.pack("<Q", len(target)) + hashlib.sha256(target).digest()
    for op in ops:
        if op[0] == "C":
            out += struct.pack("<BQQ", 1, op[1], op[2])
        else:
            out += struct.pack("<BQ", 2, len(op[1])) + op[1]
    out += b"\0"
    return bytes(out)


def apply(source: bytes, patch: bytes) -> bytes:
    if patch[:8] != MAGIC:
        raise ValueError("not a patch")
    src_size, = struct.unpack_from("<Q", patch, 8)
    src_hash = patch[16:48]
    dst_size, = struct.unpack_from("<Q", patch, 48)
    dst_hash = patch[56:88]
    if len(source) != src_size or hashlib.sha256(source).digest() != src_hash:
        raise ValueError("the source is not the expected file")
    out = bytearray()
    p = 88
    while True:
        op = patch[p]
        p += 1
        if op == 0:
            break
        if op == 1:
            off, n = struct.unpack_from("<QQ", patch, p)
            p += 16
            out += source[off:off + n]
        elif op == 2:
            n, = struct.unpack_from("<Q", patch, p)
            p += 8
            out += patch[p:p + n]
            p += n
        else:
            raise ValueError("bad op %d" % op)
    if len(out) != dst_size or hashlib.sha256(out).digest() != dst_hash:
        raise ValueError("the patch made a wrong file")
    return bytes(out)


def added_bytes(patch: bytes) -> int:
    """How many bytes the patch carries itself (not copied from the source)."""
    p, total = 88, 0
    while patch[p]:
        if patch[p] == 1:
            p += 17
        else:
            n, = struct.unpack_from("<Q", patch, p + 1)
            total += n
            p += 9 + n
    return total


def main():
    cmd, a, b, c = sys.argv[1:5]
    if cmd == "make":
        patch = make(open(a, "rb").read(), open(b, "rb").read())
        open(c, "wb").write(patch)
        print(f"{c}: {len(patch)} bytes, {added_bytes(patch)} of them added")
    elif cmd == "apply":
        open(c, "wb").write(apply(open(a, "rb").read(), open(b, "rb").read()))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
