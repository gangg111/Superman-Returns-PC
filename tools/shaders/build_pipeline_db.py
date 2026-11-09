"""Merges recorded pipeline files (superman_returns_pipelines.bin, written by the
native renderer as it creates pipelines) into the base set embedded in the
executable (RCDATA 3): the pipelines precompiled at the first start, before any
local recording exists.

The format is the renderer's (pipeline_cache.cpp): a 16-byte header (magic
"CNPS", version, record size, reserved) and fixed-size records starting with the
pipeline key, the vertex shader hash and the pixel shader hash (u64 each,
little endian). Records are kept once per key, in first-seen order. A record
whose shaders are not in the DXIL directory (<hash>.vs.dxil / <hash>.ps.dxil;
pixel hash 0 = no pixel shader) could never be compiled and is dropped.

usage: build_pipeline_db.py --dxil DIR --out FILE input.bin [input.bin ...]
"""
import argparse
import os
import struct
import sys

MAGIC = b"CNPS"
HEADER = struct.Struct("<4sIII")


def read_records(path):
    """Returns (version, record size, list of record bytes) of a pipeline file."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size:
        raise ValueError(f"{path}: shorter than the header")
    magic, version, record_size, _ = HEADER.unpack_from(data)
    if magic != MAGIC or record_size < 24:
        raise ValueError(f"{path}: not a pipeline file")
    body = data[HEADER.size:]
    count = len(body) // record_size  # a torn last record (killed writer) is ignored
    return version, record_size, [body[i * record_size:(i + 1) * record_size] for i in range(count)]


def available_shaders(dxil_dir):
    names = set()
    for name in os.listdir(dxil_dir):
        parts = name.split(".")
        if len(parts) == 3 and parts[2] == "dxil" and parts[1] in ("vs", "ps"):
            names.add((parts[0].upper(), parts[1]))
    return names


def merge(inputs, shaders):
    """Merged records and statistics; all inputs must share version and record size."""
    version = record_size = None
    seen = set()
    kept = []
    stats = {"read": 0, "duplicates": 0, "missing_shaders": 0}
    for path in inputs:
        v, size, records = read_records(path)
        if version is None:
            version, record_size = v, size
        elif (v, size) != (version, record_size):
            raise ValueError(f"{path}: version {v} / record size {size}, expected "
                             f"{version} / {record_size}")
        for record in records:
            stats["read"] += 1
            key, vs, ps = struct.unpack_from("<QQQ", record)
            if key in seen:
                stats["duplicates"] += 1
                continue
            seen.add(key)
            if (f"{vs:016X}", "vs") not in shaders or (ps and (f"{ps:016X}", "ps") not in shaders):
                stats["missing_shaders"] += 1
                continue
            kept.append(record)
    return version, record_size, kept, stats


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dxil", required=True, help="directory of <hash>.vs/ps.dxil")
    parser.add_argument("--out", required=True)
    parser.add_argument("inputs", nargs="+")
    args = parser.parse_args(argv)
    version, record_size, kept, stats = merge(args.inputs, available_shaders(args.dxil))
    with open(args.out, "wb") as f:
        f.write(HEADER.pack(MAGIC, version, record_size, 0))
        f.writelines(kept)
    print(f"{len(kept)} pipelines -> {args.out} (read {stats['read']}, duplicates "
          f"{stats['duplicates']}, without shaders {stats['missing_shaders']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
