"""Generates port/src/native_renderer/light_regs.inc from the shader catalog: for every
shader declaring the game's lighting or sky constants, the float-constant registers the
day/night cycle rewrites (port/src/daynight.h), each with its role:
  0 sun position (gLightPos: a far point; rotated with the sun)
  1 key light colour (gKey1-3, gLightColor; scaled by the sun's colour)
  2 ambient / fill colour (gMid4, gFil5-7, gShadCol, gOverBright; scaled by the sky's light)
  3 fog / sky colour (gHorizFogCol_KFog, TopFogColor, BottomFogColor, fogColor,
    gContinentLight, the sky's L0_tint; scaled by the sky's colour)
  4 output tint (c200) of the sky and its backdrop: the sky's colour
  7 output tint (c200) of the water: the sky's light (brighter than the sky's colour at night)
  8 output tint (c200) of soft particles (colorMap + depthMap + tileScaleOffset: the cloud
    sprites, smoke, fire): the sky's light on alpha-blended draws, none on additive ones (fire)
  5 output tint (c200) of the sun's flare: the sun's visibility
  6 exposure (tone mapping and bloom: scale = exposure / scene luminance): .x times the scene's
    darkening, so the automatic exposure does not lift the night back to day
The water's copies carry a T prefix (TgKey1, ...).
usage: python tools/shaders/gen_light_regs.py"""
import json
import os

ROLES = {
    'gLightPos': 0,
    'gKey1': 1, 'gKey2': 1, 'gKey3': 1, 'gLightColor': 1,
    'gMid4': 2, 'gFil5': 2, 'gFil6': 2, 'gFil7': 2, 'gShadCol': 2, 'gOverBright': 2,
    'gHorizFogCol_KFog': 3, 'TopFogColor': 3, 'BottomFogColor': 3, 'fogColor': 3, 'gContinentLight': 3,
    # Characters: a key light far point (same azimuth as the sun, ~41 degrees up) and coloured lights.
    'gKeyLightPosition': 0,
    'gKey1_col': 1, 'gKey2_col': 1, 'gKey3_col': 1,
    'gMid4_col': 2, 'gFil5_col': 2, 'gFil6_col': 2, 'gFil7_col': 2, 'gShadowColor': 2,
    # Superman's own rig (gSupesLightPos is a parameter, not a position: left alone).
    'gSupesKey1': 1, 'gSupesKey2': 1, 'gSupesKey3': 1, 'gSupes_rim_col': 1,
    'gSupesMid4': 2, 'gSupesFil5': 2, 'gSupesFil6': 2, 'gSupesFil7': 2,
    'exposure': 6,
}
SKY_TINT_SHADERS = {'19F2721057EAECAD'}  # the distant sky backdrop: its L0_tint is the sky's colour
MAX_ENTRIES = 16
# Pixel shaders whose colour comes from textures rather than the game's lights get an output
# tint in c200 (patch 0033, shader_recompiler.cpp srDayNightTintShader: the same rule):
#   4 sky dome (TopFogColor / gContinentLight), sky backdrop (gHorizFogCol_KFog without
#     gLightPos or gWaterSpec), water (gWaterSpec): the sky's colour
#   5 the sun's flare (sampler flareSampler): the sun's visibility
TINT_REG = 200
# Building shaders (constant Kenv, sampler mat_map; patch 0034 srWindowShader) add night
# window light from c201: role 9, the windows' colour times the night.
WINDOW_REG = 201


def is_window_shader(e):
    if e['type'] != 'ps':
        return False
    refl = e['reflection']
    consts = {k['name'] for k in refl.get('constants', [])}
    samplers = {k['name'] if isinstance(k, dict) else str(k) for k in refl.get('samplers', [])}
    # buildings only (gBuildingTint): props and vehicles share the glass layer
    return 'Kenv' in consts and 'gBuildingTint' in consts and 'mat_map' in samplers


def tint_role(e):
    if e['type'] != 'ps':
        return None
    refl = e['reflection']
    names = {k['name'] for k in refl.get('constants', [])}
    names |= {k['name'] if isinstance(k, dict) else str(k) for k in refl.get('samplers', [])}
    if 'flareSampler' in names:
        return 5
    sky = 'TopFogColor' in names or 'gContinentLight' in names
    water = 'gWaterSpec' in names
    backdrop = 'gHorizFogCol_KFog' in names and 'gLightPos' not in names and not water
    if water:
        return 7
    if {'colorMap', 'depthMap', 'tileScaleOffset'} <= names:
        return 8
    return 4 if sky or backdrop else None


def collect(shaders):
    rows = []
    for e in shaders:
        regs = []
        for k in e['reflection'].get('constants', []):
            name = k['name']
            role = ROLES.get(name[1:] if name.startswith('T') and name[1:] in ROLES else name)
            if role is None and name == 'L0_tint' and e['container_hash'] in SKY_TINT_SHADERS:
                role = 3
            if role is None or k.get('reg') is None:
                continue
            regs.append((k['reg'], role))
        tint = tint_role(e)
        if tint is not None:
            regs.append((TINT_REG, tint))
        if is_window_shader(e):
            regs.append((WINDOW_REG, 9))
        if regs:
            regs = sorted(set(regs))[:MAX_ENTRIES]
            rows.append((int(e['container_hash'], 16), 1 if e['type'] == 'ps' else 0, regs))
    return sorted(rows)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    catalog = json.load(open(os.path.join(root, 'artifacts', 'shaders', 'catalog.json')))
    rows = collect(catalog['shaders'])
    out = ['// Generated by tools/shaders/gen_light_regs.py; do not edit.',
           '// {shader hash, pixel shader, entry count, {{register, role}, ...}}']
    for h, ps, regs in rows:
        body = ', '.join(f'{{{r}, {role}}}' for r, role in regs)
        out.append(f'{{0x{h:016X}ull, {ps}, {len(regs)}, {{{body}}}}},')
    path = os.path.join(root, 'port', 'src', 'native_renderer', 'light_regs.inc')
    open(path, 'w', newline='\n').write('\n'.join(out) + '\n')
    print(path, len(rows), 'shaders')


if __name__ == '__main__':
    main()
