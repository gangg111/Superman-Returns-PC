"""Checks the runtime's float constant use (graphics/guest/shader_slots.cpp ShaderFloatConstants, printed
by `tests/native/shader_slots_dump --constants`) against XenosRecomp's HLSL of the same shaders.

The recorder copies only the registers in that set into a draw's constant block, so every register the
HLSL can read from the bank must be in it; a shader whose HLSL reads the bank through CONST_REL or an
array with a computed index must be `all`. The HLSL reads a register as
  - a named constant: `#define NAME SR_LOAD_FLOAT4(Stage, OFFSET)` used in the shader body,
  - an element of a named array: `#define NAME(INDEX) select(..., SR_LOAD_FLOAT4(Stage, (BASE + ...`,
  - `cN` (a register without a name), unless the body defines it as a literal (`float4 cN = ...`).
Extra registers in the set only cost copying. Usage:
  check_shader_constants.py <constants.txt> <hlsl folder>
"""
import re
import sys
from pathlib import Path

SINGLE = re.compile(r'#define (\w+) SR_LOAD_FLOAT4\(\w+, (\d+)\)')
ARRAY = re.compile(r'#define (\w+)\(INDEX\) select\(\(INDEX\) < \d+, SR_LOAD_FLOAT4\(\w+, \((\d+) \+')
START = re.compile(r'^(\[shader\(|void srVsMain\()', re.M)
LITERAL = re.compile(r'\bfloat4 c(\d+) = ')
REGISTER = re.compile(r'(?<![\w.])c(\d+)\b')


def hlsl_registers(text: str):
    """(registers, relative) the shader body reads from its float constant bank."""
    singles = {name: int(offset) // 16 for name, offset in SINGLE.findall(text)}
    arrays = {name: int(base) for name, base in ARRAY.findall(text)}
    start = START.search(text)
    body = text[start.start():] if start else text
    body = '\n'.join(line.split('//')[0] for line in body.splitlines() if not line.lstrip().startswith('#'))
    literals = {int(r) for r in LITERAL.findall(body)}
    registers, relative = set(), False
    # CONST_REL(N) with a literal is a fixed register (the port's own code in the shaders reads some, e.g.
    # the glow of the Daily Planet signs at c210/c211); with a0/aL it can reach any register.
    for index in re.findall(r'CONST_REL\(([^()]*)\)', body):
        if index.strip().isdigit():
            registers.add(int(index))
        else:
            relative = True
    # A direct bank load in the body (not through a #define).
    for stage, offset in re.findall(r'SR_LOAD_FLOAT4\((Vertex|Pixel),\s*([^()]*)\)', body):
        if offset.strip().isdigit():
            registers.add(int(offset) // 16)
        else:
            relative = True
    for name, reg in singles.items():
        if re.search(rf'(?<![\w.]){name}\b(?!\s*\()', body):
            registers.add(reg)
    for name, base in arrays.items():
        for index in re.findall(rf'(?<![\w.]){name}\(([^()]*)\)', body):
            if index.strip().isdigit():
                registers.add(base + int(index))
            else:
                relative = True
    registers |= {int(r) for r in REGISTER.findall(body) if int(r) not in literals and int(r) < 256}
    return registers, relative


def parse_use(value: str):
    if value == 'all':
        return None
    bits = int(value, 16)
    return {r for r in range(256) if bits >> r & 1}


def main() -> int:
    hlsl = Path(sys.argv[2])
    missing = relative_not_all = checked = all_count = 0
    copied = full = 0
    for line in Path(sys.argv[1]).read_text().splitlines():
        name, value = line.split()
        path = hlsl / f"{name.removesuffix('.bin')}.hlsl"
        if not path.exists():
            continue
        checked += 1
        want, relative = hlsl_registers(path.read_text(errors='replace'))
        use = parse_use(value)
        full += 256
        if use is None:
            all_count += 1
            copied += 256
            continue
        copied += len(use)
        if relative:
            relative_not_all += 1
            print(f'RELATIVE {name}: the HLSL addresses the bank, the set is not all')
        if want - use:
            missing += 1
            print(f'MISSING {name}: {sorted(want - use)}')
    print(f'checked {checked}, all {all_count}, missing {missing}, relative but not all {relative_not_all}, '
          f'registers kept {copied} of {full}')
    return 1 if missing or relative_not_all else 0


if __name__ == '__main__':
    sys.exit(main())
