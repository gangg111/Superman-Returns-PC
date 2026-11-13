"""Pack the native renderer's DXIL shaders into one blob (embedded in
superman_returns.exe as an RCDATA resource, see port/CMakeLists.txt SR_NATIVE;
reader: port/src/native_renderer/shader_pack.h).

Ported from crazyriddler/rexglue-native-kit @136bc6c4, tools/shaders/pack_shaders.py;
stage 2 (<hash>.ps.nort.dxil, build_catalog.compile_no_rt) added for GPUs without
inline ray tracing.
usage: pack_shaders.py <dxil_dir> <out.pak>

Format (little-endian):
  'CNSH' u32 version(1) u32 count
  count x { u64 hash, u32 stage (0 = vs, 1 = ps, 2 = ps without RayQuery), u32 offset, u32 size }
  sorted by (hash, stage)
  data"""
import os
import re
import struct
import sys

src, out = sys.argv[1], sys.argv[2]
entries = []
for name in os.listdir(src):
    m = re.fullmatch(r'([0-9A-F]{16})\.(vs|ps\.nort|ps)\.dxil', name)
    if m:
        entries.append((int(m.group(1), 16), {'vs': 0, 'ps': 1, 'ps.nort': 2}[m.group(2)], name))
entries.sort()
header = struct.pack('<4sII', b'CNSH', 1, len(entries))
table_size = len(entries) * 20
offset = len(header) + table_size
table, blobs = [], []
for h, stage, name in entries:
    data = open(os.path.join(src, name), 'rb').read()
    table.append(struct.pack('<QIII', h, stage, offset, len(data)))
    blobs.append(data)
    offset += len(data)
tmp = out + '.tmp'
with open(tmp, 'wb') as f:
    f.write(header)
    f.writelines(table)
    f.writelines(blobs)
os.replace(tmp, out)
print(f'packed {len(entries)} shaders, {offset} bytes -> {out}')
