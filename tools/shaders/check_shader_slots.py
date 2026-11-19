"""Checks the runtime's texture slot masks (graphics/guest/shader_slots.cpp, printed by
tests/native/shader_slots_dump) against XenosRecomp's HLSL of the same shaders.

The HLSL defines the descriptor index of every texture it can read as SR_SHARED_UINT(4 * slot)
(the 2D index, shared[0..64)): those slots must all be in the mask, or a draw would lose a texture.
Extra slots in the mask only cost time. Usage:
  check_shader_slots.py <masks.txt> <hlsl folder>
"""
import re
import sys
from pathlib import Path

DEFINE = re.compile(r'#define \w+_Texture2DDescriptorIndex SR_SHARED_UINT\((\d+)\)')


def hlsl_slots(text: str) -> int:
    mask = 0
    for m in DEFINE.finditer(text):
        offset = int(m.group(1))
        if offset < 128:
            mask |= 1 << (offset // 4)
    return mask


def main() -> int:
    masks = {}
    for line in Path(sys.argv[1]).read_text().splitlines():
        name, value = line.split()
        masks[name.removesuffix('.bin')] = int(value, 16)
    hlsl = Path(sys.argv[2])
    missing = extra_total = unknown = checked = 0
    for name, mask in sorted(masks.items()):
        path = hlsl / f'{name}.hlsl'
        if not path.exists():
            continue
        checked += 1
        want = hlsl_slots(path.read_text(errors='replace'))
        if mask == 0xFFFFFFFF:
            unknown += 1
            continue
        if want & ~mask:
            missing += 1
            print(f'MISSING {name}: mask {mask:08X} lacks {want & ~mask:08X} (HLSL {want:08X})')
        extra_total += bin(mask & ~want).count('1')
    print(f'checked {checked}, unknown (all slots) {unknown}, missing {missing}, extra slots {extra_total}')
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
