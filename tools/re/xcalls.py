"""xcalls.py <gen dir> <from_lo> <from_hi> <to_lo> <to_hi>: calls from functions in [from) to callees in [to)."""
import glob, os, re, sys
gen = sys.argv[1]
fl, fh, tl, th = (int(x, 16) for x in sys.argv[2:6])
out = {}
for path in glob.glob(os.path.join(gen, "superman_returns_recomp.*.cpp")):
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)", line)
        if m:
            a = int(m.group(1), 16)
            cur = a if fl <= a < fh else None
            continue
        if cur is None:
            continue
        for c in re.findall(r"\bsub_([0-9A-F]{8})\(ctx, base\)", line):
            ca = int(c, 16)
            if tl <= ca < th:
                out.setdefault(ca, set()).add(cur)
for ca in sorted(out):
    print(f"sub_{ca:08X} <- " + " ".join(f"{x:08X}" for x in sorted(out[ca])[:8]) + (f" (+{len(out[ca])-8})" if len(out[ca]) > 8 else ""))
