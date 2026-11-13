"""Compile the no-ray-tracing variants (<hash>.ps.nort.dxil) of an existing shader
corpus without rebuilding it: every pixel shader whose DXIL targets shader model 6.5
gets a ps_6_0 build of its standalone HLSL (artifacts/shaders/work/std), the same
step build_catalog.py runs for new corpora (compile_no_rt). pack_shaders.py stores
them as stage 2; the renderer uses them on GPUs without DXR 1.1 / shader model 6.5.

usage: python tools/shaders/build_no_rt_variants.py [--corpus artifacts/shaders] [--dxc PATH]
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_catalog import NO_RT_SUFFIX, compile_no_rt, first_error  # noqa: E402
from srpaths import ARTIFACTS, DXC  # noqa: E402


def program_version(blob: bytes):
    """Program version dword of a DXIL container's 'DXIL' part (pixel 6.5 = 0x65,
    vertex 6.0 = 0x10060), or None."""
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return None
    count = struct.unpack_from("<I", blob, 28)[0]
    for i in range(count):
        off = struct.unpack_from("<I", blob, 32 + 4 * i)[0]
        if blob[off:off + 4] == b"DXIL":
            return struct.unpack_from("<I", blob, off + 8)[0]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=str(ARTIFACTS))
    ap.add_argument("--dxc", default=str(DXC))
    a = ap.parse_args()
    corpus = Path(a.corpus)
    made = failed = 0
    for dx in sorted((corpus / "dxil").glob("*.ps.dxil")):
        if program_version(dx.read_bytes()) != 0x65:
            continue
        stem = dx.name[: -len(".dxil")]
        src = corpus / "work" / "std" / f"{stem}.hlsl"
        if not src.exists():
            print(f"[no-rt] {stem}: no {src}")
            failed += 1
            continue
        rc, out, err = compile_no_rt(a.dxc, src, corpus / "dxil" / f"{stem}{NO_RT_SUFFIX}")
        if rc != 0:
            print(f"[no-rt] {stem}: {first_error(err + out)}")
            failed += 1
        else:
            made += 1
    print(f"[no-rt] {made} variants, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
