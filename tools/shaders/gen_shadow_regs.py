"""Generates port/src/native_renderer/shadow_regs.inc from the shader catalog:
for every vertex shader that declares the sun shadow light matrix rows
shadowMapU / shadowMapV / shadowMapZ (oTexCoord7 = their dot products with the
world position), the float-constant registers of the three rows. The native
renderer reads the frame's cascade-0 light matrix from them and inverts it for
the ray-traced shadows (docs/cienie-rt-projekt.md).
usage: python tools/shaders/gen_shadow_regs.py"""
import json
import math
import os
import re
import struct
import sys

NAMES = ('shadowMapU', 'shadowMapV', 'shadowMapZ')


def collect(shaders):
    """[(hash, reg_u, reg_v, reg_z)] sorted by hash, for vertex shaders with all three rows."""
    rows = []
    for e in shaders:
        if e['type'] != 'vs':
            continue
        regs = {k['name']: k['reg'] for k in e['reflection'].get('constants', [])
                if k['name'] in NAMES}
        if len(regs) == 3:
            rows.append((int(e['container_hash'], 16),) + tuple(regs[n] for n in NAMES))
    return sorted(rows)


def collect_world(shaders):
    """[(hash, world matrix reg, skinned)] sorted by hash, for vertex shaders with a 4-row
    gWorldXform (or Mobj in the shadow caster pass); skinned = the shader also declares
    RwSkinMatrices (posed in the shader)."""
    rows = []
    for e in shaders:
        if e['type'] != 'vs':
            continue
        consts = {k['name']: k for k in e['reflection'].get('constants', [])}
        # gWorldXform (scene shaders) or Mobj (the shadow caster pass, C7C0E381...).
        world = consts.get('gWorldXform') or consts.get('Mobj')
        if world and world.get('rows') == 4:
            rows.append((int(e['container_hash'], 16), world['reg'], int('RwSkinMatrices' in consts)))
    return sorted(rows)


def collect_instanced(shaders, hlsl):
    """[(hash, instance_data reg, instance_transforms reg, regs per instance)] sorted by hash,
    for vertex shaders drawing several copies of a mesh per non-indexed draw (vertex id ->
    instance, mesh indices from the POSITION1 stream). hlsl(hash) gives the translated source:
    4 registers per instance when it reads instance_transforms + 3 (a full 4x4 matrix,
    world = x*c0 + y*c1 + z*c2 + c3), else 2 (translation c0.xyz and a yaw c1)."""
    rows = []
    for e in shaders:
        if e['type'] != 'vs':
            continue
        consts = {k['name']: k for k in e['reflection'].get('constants', [])}
        data, xforms = consts.get('instance_data'), consts.get('instance_transforms')
        if not data or not xforms:
            continue
        src = hlsl(e['container_hash'])
        per = 4 if f"CONST_REL({xforms['reg'] + 3} + a0)" in src else 2
        rows.append((int(e['container_hash'], 16), data['reg'], xforms['reg'], per))
    return sorted(rows)


ALBEDO_PREFERENCE = ('col_map', 'L1_col_map', 'L0_col_map', 'L2_col_map')

# Symbolic values of one register component while walking a pixel shader:
#   ('i', k, c)          interpolant iTexCoordk component c
#   ('a', k, c, f, o)    iTexCoordk.c * f + o   (f, o: scalars below)
#   ('c', reg, comp)     pixel shader float constant register component
#   ('l', value)         literal
#   ('u',)               anything else (per pixel: view vectors, texture results, ps)
UNKNOWN = ('u',)
ONE, ZERO = ('l', 1.0), ('l', 0.0)
COMPS = 'xyzw'


def _scalar(v):
    return v[0] in ('c', 'l')


def _mul(a, b):
    if a[0] == 'i' and _scalar(b):
        return ('a', a[1], a[2], b, ZERO)
    if b[0] == 'i' and _scalar(a):
        return ('a', b[1], b[2], a, ZERO)
    if a == ONE:
        return b
    if b == ONE:
        return a
    return UNKNOWN


def _add(a, b):
    """Sums keep the UV part; a per-pixel term (parallax offset) is dropped."""
    if a == UNKNOWN:
        return b
    if b == UNKNOWN:
        return a
    if a[0] in ('i', 'a') and _scalar(b):
        a, b = a, b
    elif b[0] in ('i', 'a') and _scalar(a):
        a, b = b, a
    else:
        return UNKNOWN
    if a[0] == 'i':
        return ('a', a[1], a[2], ONE, b)
    if a[4] == ZERO:
        return a[:4] + (b,)
    return UNKNOWN


def _neg(v):
    if v[0] == 'l':
        return ('l', -v[1])
    return UNKNOWN if v != ZERO else v


def _operand(tok, regs, consts, literals, snapshot):
    """Per-component values of one operand ('-r6.xy', 'L1_UV_freq.xy', 'c252.zw', 'ps', ...)."""
    tok = tok.strip()
    neg = tok.startswith('-')
    tok = tok.lstrip('-')
    m = re.fullmatch(r'(\w+)\.([xyzw]+)', tok)
    if not m:
        return None
    name, sw = m.groups()
    if name == 'aluVectorSource':
        src = snapshot
    elif name in regs:
        src = regs[name]
    elif name in consts:
        src = [('c', consts[name], c) for c in range(4)]
    elif name in literals:
        src = [('l', v) for v in literals[name]]
    else:
        src = [UNKNOWN] * 4
    vals = [src[COMPS.index(ch)] for ch in sw]
    return [_neg(v) for v in vals] if neg else vals


def _expression(expr, regs, consts, literals, snapshot, width):
    """Component values of 'A', 'A * B' or 'A * B + C' (other forms: unknown)."""
    def comp(vals, i):
        return vals[i] if i < len(vals) else vals[-1]
    m = re.fullmatch(r'(\S+) \* (\S+) \+ (\S+)', expr) or re.fullmatch(r'(\S+) \* (\S+)', expr) or \
        re.fullmatch(r'(\S+) \+ (\S+)', expr) or re.fullmatch(r'(\S+)', expr)
    if not m:
        return [UNKNOWN] * width
    ops = [_operand(t, regs, consts, literals, snapshot) for t in m.groups()]
    if any(o is None for o in ops):
        return [UNKNOWN] * width
    out = []
    for i in range(width):
        if '*' in expr:
            v = _mul(comp(ops[0], i), comp(ops[1], i))
            if len(ops) == 3:
                v = _add(v, comp(ops[2], i))
        elif len(ops) == 2:
            v = _add(comp(ops[0], i), comp(ops[1], i))
        else:
            v = comp(ops[0], i)
        out.append(v)
    return out


def analyze_albedo_uv(src, sampler):
    """(interpolant, comp u, comp v, (fu, fv, ou, ov)) of the UV a translated pixel shader passes to
    tfetch2D(<sampler>): uv = iTexCoord<interpolant>.<comp> * f + o per component, f and o constants
    (('c', register, component) or ('l', value)); None when it is not of that form."""
    consts = {m.group(1): int(m.group(2)) // 16 for m in
              re.finditer(r'#define (\w+) SR_LOAD_FLOAT4\(Pixel, (\d+)\)', src)}
    literals = {}
    for m in re.finditer(r'float4 (c\d+) = asfloat\(uint4\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), '
                         r'(0x[0-9A-Fa-f]+)\)\);', src):
        literals[m.group(1)] = [struct.unpack('<f', struct.pack('<I', int(x, 16)))[0] for x in m.groups()[1:]]
    regs = {}
    snapshot = [UNKNOWN] * 4
    for line in src.splitlines():
        line = line.strip()
        m = re.fullmatch(r'float4 (r\d+) = (iTexCoord(\d+)|0\.0);', line)
        if m:
            regs[m.group(1)] = ([('i', int(m.group(3)), c) for c in range(4)] if m.group(3)
                                else [ZERO] * 4)
            continue
        m = re.fullmatch(r'float4 aluVectorSource = (r\d+);', line)
        if m:
            snapshot = list(regs.get(m.group(1), [UNKNOWN] * 4))
            continue
        m = re.fullmatch(rf'r\d+\.\w+ = tfetch2D\({sampler}_Texture2DDescriptorIndex, \w+, (r\d+)\.([xyzw]{{2}}), .*', line)
        if m:
            uv = [regs.get(m.group(1), [UNKNOWN] * 4)[COMPS.index(c)] for c in m.group(2)]
            uv = [('a', v[1], v[2], ONE, ZERO) if v[0] == 'i' else v for v in uv]
            if any(v[0] != 'a' for v in uv) or uv[0][1] != uv[1][1]:
                return None
            return uv[0][1], uv[0][2], uv[1][2], (uv[0][3], uv[1][3], uv[0][4], uv[1][4])
        m = re.fullmatch(r'(r\d+)\.([xyzw]+) = (.*);', line)
        if m:
            reg, mask, expr = m.groups()
            vals = _expression(expr, regs, consts, literals, snapshot, len(mask))
            cur = regs.setdefault(reg, [UNKNOWN] * 4)
            for ch, v in zip(mask, vals):
                cur[COMPS.index(ch)] = v
    return None


def analyze_tint_blend(src, layer):
    """How a building pixel shader mixes its colour from the <layer>mat_map mask (layer 'L1_',
    'L0_' or ''): {tint name: (mask channel 0..3 of the texture, normalised)} for R_tint, G_tint
    and B_tint, where normalised = divided by saturate(sum of the three channels) first; None when
    the three tints are not each weighted by one mask channel. Tracked values per register
    component: ('m', channel, normalised), ('s',) the channel sum, ('r',) its reciprocal."""
    weights = {}
    regs = {}
    ps = UNKNOWN
    tints = {f'{layer}{c}_tint': c for c in 'RGB'}
    # Blocks under if (p0) / if (!p0): one of two paths at run time, so a predicated write that
    # yields nothing known does not erase what the other path keeps.
    blocks, pending = [], False

    def comps(reg, sw):
        cur = regs.get(reg, [UNKNOWN] * 4)
        return [cur[COMPS.index(ch)] for ch in sw]

    for line in src.splitlines():
        line = line.strip()
        if re.fullmatch(r'if \(!?p0\)', line):
            pending = True
            continue
        if line == '{':
            blocks.append(pending)
            pending = False
            continue
        if line == '}':
            if blocks:
                blocks.pop()
            continue
        predicated = any(blocks)
        m = re.fullmatch(rf'(r\d+)\.([xyzw]+) = tfetch2D\({layer}mat_map_Texture2DDescriptorIndex, .*\)\.([xyzw]+);', line)
        if m:
            reg, mask, sw = m.groups()
            cur = regs.setdefault(reg, [UNKNOWN] * 4)
            for ch, s in zip(mask, sw):
                cur[COMPS.index(ch)] = ('m', COMPS.index(s), False)
            continue
        m = re.fullmatch(r'ps = .*rcp\((r\d+)\.([xyzw])\).*;', line)
        if m:
            ps = ('r',) if comps(m.group(1), m.group(2))[0] == ('s',) else UNKNOWN
            continue
        # A tint weighted on the scalar lane: ps = L1_B_tint.x * r0.w (or the operands swapped).
        m = re.fullmatch(r'ps = (?P<name>\w+)\.[xyzw] \* (?P<reg>r\d+)\.(?P<ch>[xyzw]);', line) or \
            re.fullmatch(r'ps = (?P<reg>r\d+)\.(?P<ch>[xyzw]) \* (?P<name>\w+)\.[xyzw];', line)
        if m and m.group('name') in tints:
            v = comps(m.group('reg'), m.group('ch'))[0]
            if v[0] == 'm':
                weights[m.group('name')[len(layer):-5]] = (v[1], v[2])
            ps = UNKNOWN
            continue
        if line.startswith('ps = '):
            ps = UNKNOWN
            continue
        m = re.fullmatch(r'(r\d+)\.([xyzw]+) = (.*);', line)
        if not m:
            continue
        reg, mask, expr = m.groups()
        vals = [UNKNOWN] * len(mask)
        if expr == 'ps':
            vals = [ps] * len(mask)
        elif (d := re.fullmatch(r'saturate\(dot\((r\d+)\.([xyzw]{3}), c\d+\.[xyzw]{3}\)\)', expr)):
            src_vals = comps(d.group(1), d.group(2))
            if all(v[0] == 'm' and not v[2] for v in src_vals) and len({v[1] for v in src_vals}) == 3:
                vals = [('s',)] * len(mask)
        elif (t := re.fullmatch(r'(r\d+)\.([xyzw]+) \* (\w+)\.[xyzw]+( \+ .*)?', expr)) and t.group(3) in tints:
            v = comps(t.group(1), t.group(2))[0]
            if v[0] == 'm':
                weights[t.group(3)[len(layer):-5]] = (v[1], v[2])
        elif (p := re.fullmatch(r'(r\d+)\.([xyzw]+) \* (r\d+)\.([xyzw]+)', expr)):
            a, b = comps(p.group(1), p.group(2)), comps(p.group(3), p.group(4))
            for i in range(len(mask)):
                x, y = a[min(i, len(a) - 1)], b[min(i, len(b) - 1)]
                if x == ('r',) and y[0] == 'm':
                    vals[i] = ('m', y[1], True)
                elif y == ('r',) and x[0] == 'm':
                    vals[i] = ('m', x[1], True)
        elif (c := re.fullmatch(r'(r\d+)\.([xyzw]+)', expr)):
            vals = comps(c.group(1), c.group(2))
            vals += [vals[-1]] * (len(mask) - len(vals))
        cur = regs.setdefault(reg, [UNKNOWN] * 4)
        for ch, v in zip(mask, vals):
            if v != UNKNOWN or not predicated:
                cur[COMPS.index(ch)] = v
    if set(weights) != {'R', 'G', 'B'}:
        return None
    return {f'{c}_tint': weights[c] for c in 'RGB'}


def _literals(src):
    """Literal float4 registers of a translated shader: name -> [4 floats] (nan where a component is
    not a plain literal, e.g. the shadow range scale)."""
    out = {}
    for m in re.finditer(r'float4 (c\d+) = (.*);', src):
        name, expr = m.groups()
        u = re.fullmatch(r'asfloat\(uint4\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)\)', expr)
        if u:
            out[name] = [struct.unpack('<f', struct.pack('<I', int(x, 16)))[0] for x in u.groups()]
            continue
        f = re.fullmatch(r'float4\((.*)\)', expr)
        if f:
            parts, depth, cur = [], 0, ''
            for ch in f.group(1):  # split on the top-level commas
                if ch == ',' and depth == 0:
                    parts.append(cur)
                    cur = ''
                    continue
                depth += (ch == '(') - (ch == ')')
                cur += ch
            parts.append(cur)
            vals = []
            for p in parts:
                p = p.strip()
                v = re.fullmatch(r'asfloat\((0x[0-9A-Fa-f]+)u\)', p)
                vals.append(struct.unpack('<f', struct.pack('<I', int(v.group(1), 16)))[0] if v else float('nan'))
            if len(vals) == 4:
                out[name] = vals
    return out


def _eval_numeric(lines, regs, literals):
    """Runs straight-line translated HLSL on numbers: assignments rN.mask = expr with operands
    rN.sw / cN.sw / -x, the operators + and *, select(x == 0.0, a, b) and floor(x); anything else
    makes the written components nan. regs: name -> list of 4 floats, updated in place."""
    nan = float('nan')

    def operand(tok, width):
        tok = tok.strip()
        neg = tok.startswith('-')
        tok = tok.lstrip('-')
        m = re.fullmatch(r'(\w+)\.([xyzw]+)', tok)
        if not m:
            return None
        src = regs.get(m.group(1)) or literals.get(m.group(1))
        if src is None:
            return [nan] * width
        vals = [src[COMPS.index(c)] for c in m.group(2)]
        vals += [vals[-1]] * (width - len(vals))
        return [-v for v in vals] if neg else vals

    def expression(expr, width):
        s = re.fullmatch(r'select\((\S+) == 0\.0, (\S+), (\S+)\)', expr)
        if s:
            c, a, b = (operand(t, width) for t in s.groups())
            if None in (c, a, b):
                return [nan] * width
            return [a[i] if c[i] == 0.0 else b[i] if c[i] == c[i] else nan for i in range(width)]
        f = re.fullmatch(r'floor\((\S+)\)', expr)
        if f:
            a = operand(f.group(1), width)
            return [math.floor(v) if v == v else nan for v in a] if a else [nan] * width
        m = re.fullmatch(r'(\S+) \* (\S+) \+ (\S+)', expr)
        if m:
            a, b, c = (operand(t, width) for t in m.groups())
            return [a[i] * b[i] + c[i] for i in range(width)] if None not in (a, b, c) else [nan] * width
        m = re.fullmatch(r'(\S+) ([+*]) (\S+)', expr)
        if m:
            a, b = operand(m.group(1), width), operand(m.group(3), width)
            if a is None or b is None:
                return [nan] * width
            return [a[i] + b[i] if m.group(2) == '+' else a[i] * b[i] for i in range(width)]
        a = operand(expr, width)
        return a if a else [nan] * width

    for line in lines:
        m = re.fullmatch(r'(r\d+)\.([xyzw]+) = (.*);', line.strip())
        if not m:
            continue
        reg, mask, expr = m.groups()
        vals = expression(expr, len(mask))
        cur = regs.setdefault(reg, [nan] * 4)
        for ch, v in zip(mask, vals):
            cur[COMPS.index(ch)] = v


def analyze_palette_blend(src):
    """The city shaders' colour (8A6DD1D52752FAD6 and its family): mask channels weight R_tint,
    G_tint, B_tint and, with the rest 1 - saturate(sum), A_tint; that colour is blended towards a
    per-building palette colour by saturate(dot(mask channels and rest, gBuildingTint)); the palette
    entry is floor(iTexCoord<k>.<c> * scale), its colours come from a select chain on literals.
    Returns {'channels': (R, G, B mask channels), 'building': (gBuildingTint component for the
    R, G, B channels and the rest), 'index': (k, c, scale), 'palette': [rgb of entries 0..n-1]}
    or None when the shader is not of that form."""
    blend = analyze_tint_blend(src, '')
    if blend is None or any(n for (_, n) in blend.values()) or '#define A_tint ' not in src:
        return None
    lines = [l.strip() for l in src.splitlines()]
    fetch = next((re.fullmatch(r'(r\d+)\.([xyzw]+) = tfetch2D\(mat_map_Texture2DDescriptorIndex, .*\)\.([xyzw]+);', l)
                  for l in lines if 'tfetch2D(mat_map_Texture2DDescriptorIndex' in l), None)
    dot = next((re.fullmatch(r'r\d+\.[xyzw] = saturate\(dot\((r\d+)\.([xyzw]{4}), gBuildingTint\.([xyzw]{4})\)\);', l)
                for l in lines if 'gBuildingTint.' in l and 'dot(' in l), None)
    if not fetch or not dot or dot.group(1) != fetch.group(1):
        return None
    mask, sw = fetch.group(2), fetch.group(3)
    building = [None] * 4  # R, G, B, rest
    for c, g in zip(dot.group(2), dot.group(3)):
        idx = COMPS.index(sw[mask.index(c)]) if c in mask else 3
        if idx < 3:
            idx = [blend[f'{n}_tint'][0] for n in 'RGB'].index(idx)
        building[idx] = COMPS.index(g)
    if None in building:
        return None
    fl = next((i for i, l in enumerate(lines) if re.fullmatch(r'r\d+\.[xyzw] = floor\(r\d+\.[xyzw]\);', l)), None)
    if fl is None:
        return None
    freg, fcomp, areg, acomp = re.fullmatch(r'(r\d+)\.([xyzw]) = floor\((r\d+)\.([xyzw])\);', lines[fl]).groups()
    # floor(r0.x): r0.x = ps, ps = cN.c * r0.x, r0.x = max(rI.c, rI.c), rI = iTexCoordK
    scale = source = None
    literals = _literals(src)
    for l in reversed(lines[:fl]):
        m = re.fullmatch(rf'ps = (c\d+)\.([xyzw]) \* {areg}\.{acomp};', l)
        if m and scale is None and m.group(1) in literals:
            scale = literals[m.group(1)][COMPS.index(m.group(2))]
            continue
        m = re.fullmatch(rf'{areg}\.{acomp} = max\((r\d+)\.([xyzw]), \1\.\2\);', l)
        if m and scale is not None:
            init = next((re.fullmatch(rf'float4 {m.group(1)} = iTexCoord(\d+);', x) for x in lines
                         if x.startswith(f'float4 {m.group(1)} = ')), None)
            if init:
                source = (int(init.group(1)), COMPS.index(m.group(2)))
            break
    if scale is None or source is None or not (1 <= scale <= 16):
        return None
    end = next((i for i in range(fl, len(lines))
                if re.fullmatch(r'(r\d+)\.xyz = -r\d+\.\w+ \+ \1\.xyz;', lines[i])), None)
    if end is None:
        return None
    preg = re.fullmatch(r'(r\d+)\.xyz', lines[end].split(' = ')[0]).group(1)
    palette = []
    for n in range(int(scale)):
        regs = {freg: [float('nan')] * 4}
        regs[freg][COMPS.index(fcomp)] = float(n)
        _eval_numeric(lines[fl + 1:end], regs, literals)
        rgb = regs.get(preg, [float('nan')] * 4)[:3]
        if any(v != v for v in rgb):
            return None
        palette.append(tuple(round(v, 6) for v in rgb))
    return {'channels': tuple(blend[f'{n}_tint'][0] for n in 'RGB'), 'building': tuple(building),
            'index': (source[0], source[1], scale), 'palette': palette}


def collect_palette(shaders, hlsl):
    """[(hash, A_tint register, gBuildingTint register, analyze_palette_blend result)] sorted by
    hash, for pixel shaders whose tint blend (collect_tint) also has the rest weight and the
    per-building palette (8A6DD1D52752FAD6 and its family)."""
    rows = []
    for e in shaders:
        if e['type'] != 'ps':
            continue
        if not any(s['name'] == 'mat_map' for s in e['reflection'].get('samplers', [])):
            continue
        src = hlsl(e['container_hash'])
        consts = {m.group(1): int(m.group(2)) // 16 for m in
                  re.finditer(r'#define (\w+) SR_LOAD_FLOAT4\(Pixel, (\d+)\)', src)}
        if 'A_tint' not in consts or 'gBuildingTint' not in consts:
            continue
        r = analyze_palette_blend(src)
        if r:
            r['tone'] = tuple(consts.get(n, -1) for n in ('gShadCol', 'ramp_col_exp', 'Kramp_lo_hi_Kao'))
            rows.append((int(e['container_hash'], 16), consts['A_tint'], consts['gBuildingTint'], r))
    return sorted(rows, key=lambda r: r[0])


def render_palette(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {pixel shader hash, A_tint register, gBuildingTint register, gBuildingTint component for the',
             '//  R, G, B mask channels and the rest, palette index interpolant, component, scale, entries,',
             '//  palette rgb (8 entries, unused ones 0), gShadCol / ramp_col_exp / Kramp_lo_hi_Kao registers}']
    for h, a, b, r in rows:
        pal = list(r['palette'])[:8] + [(0.0, 0.0, 0.0)] * (8 - min(len(r['palette']), 8))
        k, c, scale = r['index']
        cols = ', '.join(f'{{{x!r}f, {y!r}f, {z!r}f}}' for x, y, z in pal)
        bc = r['building']
        lines.append(f'{{0x{h:016X}ull, {a}, {b}, {{{bc[0]}, {bc[1]}, {bc[2]}, {bc[3]}}}, {k}, {c}, {scale!r}f, '
                     f'{len(r["palette"])}, {{{cols}}}, {{{r["tone"][0]}, {r["tone"][1]}, {r["tone"][2]}}}}},')
    return '\n'.join(lines) + '\n'


def collect_tint(shaders, hlsl):
    """[(hash, mat slot, uv, channels, normalised, tint registers, col slot)] sorted by hash, for
    pixel shaders mixing their colour from a mask (analyze_tint_blend): colour =
    sum(mask[channel] (/ saturate(channel sum)) * <c>_tint) * tint * col_map. Layer: L1_, then
    none, then L0_. uv = analyze_albedo_uv of the mask fetch; channels = (R, G, B) mask channels;
    tint registers = (R_tint, G_tint, B_tint, tint) pixel constant registers (-1: no tint);
    col slot = fetch slot of the layer's col_map (-1: none)."""
    rows = []
    for e in shaders:
        if e['type'] != 'ps':
            continue
        samplers = {s['name']: s['reg'] for s in e['reflection'].get('samplers', [])
                    if s.get('type') == 'sampler2d'}
        src = None
        for layer in ('L1_', '', 'L0_'):
            if f'{layer}mat_map' not in samplers:
                continue
            src = src if src is not None else hlsl(e['container_hash'])
            consts = {m.group(1): int(m.group(2)) // 16 for m in
                      re.finditer(r'#define (\w+) SR_LOAD_FLOAT4\(Pixel, (\d+)\)', src)}
            if any(f'{layer}{c}_tint' not in consts for c in 'RGB'):
                continue
            blend = analyze_tint_blend(src, layer)
            uv = analyze_albedo_uv(src, f'{layer}mat_map')
            if blend is None or uv is None:
                continue
            channels = tuple(blend[f'{c}_tint'][0] for c in 'RGB')
            normalised = blend['R_tint'][1]
            regs = tuple(consts[f'{layer}{c}_tint'] for c in 'RGB') + (consts.get(f'{layer}tint', -1),)
            rows.append((int(e['container_hash'], 16), samplers[f'{layer}mat_map'], uv, channels, normalised,
                         regs, samplers.get(f'{layer}col_map', -1)))
            break
    return sorted(rows, key=lambda r: r[0])


def render_tint(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {pixel shader hash, mask (*mat_map) fetch slot, UV interpolant, u comp, v comp, scale u, scale v,',
             '//  offset u, offset v (as in rt_albedo_regs.inc), R/G/B mask channels, normalised,',
             '//  R_tint/G_tint/B_tint/tint pixel constant registers (-1 = none), col_map fetch slot (-1 = none)}']
    for h, mat, uv, ch, norm, regs, col in rows:
        k, cu, cv, scal = uv
        lines.append(f'{{0x{h:016X}ull, {mat}, {k}, {cu}, {cv}, ' + ', '.join(_render_scalar(v) for v in scal) +
                     f', {{{ch[0]}, {ch[1]}, {ch[2]}}}, {int(norm)}, {{{regs[0]}, {regs[1]}, {regs[2]}, {regs[3]}}}, {col}}},')
    return '\n'.join(lines) + '\n'


def collect_albedo(shaders, hlsl):
    """[(hash, fetch slot, uv)] sorted by hash, for pixel shaders with a 2D colour texture (a
    sampler named *col_map): the texture the ray-traced reflections take a hit's colour from.
    Several layers (L0/L1/...): col_map first, then L1, L0, L2, then the first other *col_map by
    name. uv = analyze_albedo_uv of hlsl(hash) (None: the colour is the texture's mean)."""
    rows = []
    for e in shaders:
        if e['type'] != 'ps':
            continue
        maps = {s['name']: s['reg'] for s in e['reflection'].get('samplers', [])
                if s.get('type') == 'sampler2d' and s['name'].endswith('col_map')}
        if not maps:
            continue
        name = next((n for n in ALBEDO_PREFERENCE if n in maps), sorted(maps)[0])
        rows.append((int(e['container_hash'], 16), maps[name], analyze_albedo_uv(hlsl(e['container_hash']), name)))
    return sorted(rows, key=lambda r: r[0])


def _render_scalar(v):
    return f'{{{v[1]}, {v[2]}, 0.0f}}' if v[0] == 'c' else f'{{-1, 0, {v[1]!r}f}}'


def render_albedo(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {pixel shader hash, fetch slot of its colour texture (*col_map), UV interpolant (-1 = none),',
             '//  u component, v component, scale u, scale v, offset u, offset v}; uv = interpolant * scale +',
             '//  offset, each {pixel constant register, component, value used when the register is -1}']
    for h, s, uv in rows:
        if uv is None:
            lines.append(f'{{0x{h:016X}ull, {s}, -1, 0, 0, {{-1, 0, 1.0f}}, {{-1, 0, 1.0f}}, {{-1, 0, 0.0f}}, '
                         f'{{-1, 0, 0.0f}}}},')
        else:
            k, cu, cv, scal = uv
            lines.append(f'{{0x{h:016X}ull, {s}, {k}, {cu}, {cv}, ' + ', '.join(_render_scalar(v) for v in scal) + '},')
    return '\n'.join(lines) + '\n'


def render_instanced(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {vertex shader hash, instance_data register, instance_transforms register, '
             'registers per instance}']
    lines += [f'{{0x{h:016X}ull, {d}, {t}, {p}}},' for h, d, t, p in rows]
    return '\n'.join(lines) + '\n'


def render_world(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {vertex shader hash, gWorldXform register, skinned (RwSkinMatrices)}']
    lines += [f'{{0x{h:016X}ull, {r}, {s}}},' for h, r, s in rows]
    return '\n'.join(lines) + '\n'


def render(rows):
    lines = ['// Generated by tools/shaders/gen_shadow_regs.py; do not edit.',
             '// {vertex shader hash, shadowMapU, shadowMapV, shadowMapZ register}']
    lines += [f'{{0x{h:016X}ull, {u}, {v}, {z}}},' for h, u, v, z in rows]
    return '\n'.join(lines) + '\n'


def main():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from srpaths import ARTIFACTS, NATIVE_SRC  # noqa: E402
    shaders = json.load(open(ARTIFACTS / 'catalog.json'))['shaders']
    rows = collect(shaders)
    out = NATIVE_SRC / 'shadow_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render(rows))
    print(f'{len(rows)} vertex shaders -> {out}')
    world = collect_world(shaders)
    out = NATIVE_SRC / 'rt_world_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render_world(world))
    print(f'{len(world)} vertex shaders -> {out}')
    inst = collect_instanced(shaders, lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.vs.hlsl').read())
    out = NATIVE_SRC / 'rt_instanced_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render_instanced(inst))
    print(f'{len(inst)} vertex shaders -> {out}')
    albedo = collect_albedo(shaders, lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.ps.hlsl').read())
    out = NATIVE_SRC / 'rt_albedo_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render_albedo(albedo))
    print(f'{len(albedo)} pixel shaders ({sum(r[2] is not None for r in albedo)} with UV) -> {out}')
    tint = collect_tint(shaders, lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.ps.hlsl').read())
    out = NATIVE_SRC / 'rt_tint_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render_tint(tint))
    print(f'{len(tint)} pixel shaders with a tint blend -> {out}')
    palette = collect_palette(shaders, lambda h: open(ARTIFACTS / 'hlsl' / f'{h}.ps.hlsl').read())
    out = NATIVE_SRC / 'rt_palette_regs.inc'
    with open(out, 'w', newline='\n') as f:
        f.write(render_palette(palette))
    print(f'{len(palette)} pixel shaders with a building palette -> {out}')


if __name__ == '__main__':
    main()
