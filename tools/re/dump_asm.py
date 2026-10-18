"""Print the PPC assembly (from the generated code's comments) of the given
functions, with labels and calls (callee names).
usage: dump_asm.py <generated dir> sub_XXXXXXXX [...]"""
import glob
import os
import re
import sys

gen, want = sys.argv[1], set(sys.argv[2:])
for path in sorted(glob.glob(os.path.join(gen, "superman_returns_recomp.*.cpp"))):
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)", line)
        if m:
            cur = m.group(1) if m.group(1) in want else None
            if cur:
                print(f"\n=== {cur}")
            continue
        if not cur:
            continue
        s = line.rstrip()
        if s.startswith("\t// "):
            print("   " + s[4:])
        elif re.match(r"loc_[0-9A-F]+:", s):
            print(s)
        else:
            c = re.search(r"\b(__imp__\w+|sub_[0-9A-F]{8})\(ctx, base\)", s)
            if c:
                print(f"      -> {c.group(1)}")
