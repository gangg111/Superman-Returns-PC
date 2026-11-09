#!/usr/bin/env python3
# Ported from crazyriddler/rexglue-native-kit @136bc6c4, tools/shaders/build_catalog.py.
# Changes for Superman Returns: tool and output paths (srpaths.py: translator
# and DXC under .tools/, the Markdown catalog under artifacts/shaders/ so no
# generated output lands in the tracked tree).
"""
Translate every extracted container with XenosRecompCorpus, compile the HLSL
with DXC, parse reflection from the container, and write:

  artifacts/shaders/hlsl/<hash>.<vs|ps>.hlsl          XenosRecomp output
  artifacts/shaders/dxil/<hash>.<vs|ps>.dxil          vs_6_0/ps_6_0, spec constants = 0 (default variant)
  artifacts/shaders/dxil/<hash>.<vs|ps>.lib.dxil      lib_6_3 (only when specConstantsMask != 0; the
                                                     form UnleashedRecomp/re:Blue link at runtime)
  artifacts/shaders/spirv/<hash>.<vs|ps>.spv          versioned Vulkan buffer ABI (optional, --spirv)
  artifacts/shaders/logs/<hash>.<vs|ps>.<step>.log    translator / DXC diagnostics for failures
  artifacts/shaders/catalog.json                      machine-readable catalog
  artifacts/shaders/SHADER_CATALOG.md                 human summary + table

Status ladder per shader: extracted -> translated -> compiles
(-> visually_validated / optimized are set manually later via
artifacts/shaders/validation.json: {"<container_hash>": {"status": "...", "note": "..."}}).
"""
import argparse
import concurrent.futures as cf
import datetime
import json
import os
import re
import struct
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from srpaths import ARTIFACTS, DXC, ROOT, SHADER_COMMON, TRANSLATOR  # noqa: E402

from compile_vulkan import compile_shader, Toolchain

REGSET = ["bool", "int4", "float4", "sampler"]
PCLASS = ["scalar", "vector", "matrix_rows", "matrix_columns", "object", "struct"]
PTYPE = ["void", "bool", "int", "float", "string", "texture", "texture1d", "texture2d", "texture3d",
         "texturecube", "sampler", "sampler1d", "sampler2d", "sampler3d", "samplercube", "pixelshader",
         "vertexshader", "pixelfragment", "vertexfragment", "unsupported"]
USAGE = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
         "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def cstr(b, o):
    e = b.index(b"\0", o)
    return b[o:e].decode("ascii", "replace")


def reflect(blob: bytes):
    """Parse the XDK container: D3DX constant table, VS inputs, interpolators, PS outputs, literals."""
    flags, vsize, psize, _, ctab_off, def_off, sh_off = struct.unpack_from(">7I", blob, 0)
    is_ps = (flags & 1) == 0
    r = {"constants": [], "samplers": [], "bools": [], "ints": []}
    ct = ctab_off + 4  # ConstantTableContainer{u32 size; ConstantTable}
    if ctab_off == 0:  # minimal shader created without a constant table
        n_const, info_off = 0, 0
        r["ctab_creator"] = r["ctab_target"] = None
    else:
        n_const, info_off = u32(blob, ct + 12), u32(blob, ct + 16)
        r["ctab_creator"] = cstr(blob, ct + u32(blob, ct + 4)) if u32(blob, ct + 4) else None
        r["ctab_target"] = cstr(blob, ct + u32(blob, ct + 24)) if u32(blob, ct + 24) else None
    for i in range(n_const):
        o = ct + info_off + i * 20
        name = cstr(blob, ct + u32(blob, o))
        regset, reg, cnt = u16(blob, o + 4), u16(blob, o + 6), u16(blob, o + 8)
        ti = ct + u32(blob, o + 12)
        pclass, ptype, rows, cols, elems = (u16(blob, ti), u16(blob, ti + 2), u16(blob, ti + 4),
                                            u16(blob, ti + 6), u16(blob, ti + 8))
        e = {"name": name, "set": REGSET[regset] if regset < 4 else regset, "reg": reg, "count": cnt,
             "class": PCLASS[pclass] if pclass < len(PCLASS) else pclass,
             "type": PTYPE[ptype] if ptype < len(PTYPE) else ptype, "rows": rows, "cols": cols, "elements": elems}
        {"sampler": r["samplers"], "bool": r["bools"], "int4": r["ints"]}.get(e["set"], r["constants"]).append(e)

    sh = sh_off
    field_c = u32(blob, sh + 12)
    interp_info = u32(blob, sh + 20)
    n_interp = (interp_info >> 5) & 0x1F

    def interp(v):
        return {"usage": USAGE[(v >> 4) & 0xF] if ((v >> 4) & 0xF) < len(USAGE) else (v >> 4) & 0xF,
                "index": v & 0xF, "reg": (v >> 8) & 0xF}

    if is_ps:
        outputs = u32(blob, sh + 28)
        r["outputs"] = [n for bit, n in [(1, "COLOR0"), (2, "COLOR1"), (4, "COLOR2"), (8, "COLOR3"), (16, "DEPTH")]
                        if outputs & bit]
        r["interpolators"] = [interp(u32(blob, sh + 32 + 4 * i)) for i in range(n_interp)]
        r["vpos_reg"] = (field_c >> 8) & 0xFF
    else:
        f18, n_el = u32(blob, sh + 24), u32(blob, sh + 28)
        base = sh + 36
        r["vertex_inputs"] = []
        for i in range(n_el):
            v = u32(blob, base + 4 * (f18 + i))
            usage = (v >> 12) & 0xF
            r["vertex_inputs"].append({"usage": USAGE[usage] if usage < len(USAGE) else usage,
                                       "index": (v >> 16) & 0xF, "fetch_addr": v & 0xFFF})
        r["interpolators"] = [interp(u32(blob, base + 4 * (f18 + n_el + i))) for i in range(n_interp)]

    lit = 0
    if def_off:
        o = def_off + 20
        while o + 8 <= vsize and u32(blob, o) != 0:
            lit += (u16(blob, o + 2) + 3) // 4
            o += 8
    r["literal_float4"] = lit
    return r


def ucode_stats(blob: bytes, rec):
    u = blob[rec["ucode_offset"]: rec["ucode_offset"] + rec["ucode_size"]]
    return {"ucode_dwords": len(u) // 4}


def shader_profile(kind: str, src: str) -> str:
    """DXC target of a translated shader. Pixel shaders that trace inline
    RayQuery need shader model 6.5 (the device reports tier 1.2 / SM 6.5): the
    sun shadow receivers (docs/cienie-rt-projekt.md) and the water, whose sky
    reflection is ray-traced (constant gWaterSpec)."""
    if kind == "ps" and ("shadowMapOffsetScales" in src or "gWaterSpec" in src):
        return "ps_6_5"
    return "ps_6_0" if kind == "ps" else "vs_6_0"


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -999, "", "timeout"


def first_error(text: str) -> str:
    for line in text.splitlines():
        if "error" in line.lower() or "assert" in line.lower():
            line = re.sub(r"^.*?\.hlsl:\d+:\d+: ", "", line.strip())
            line = re.sub(r"^[A-Za-z]:[^:]*:\d+: ", "", line)
            return line[:200]
    t = text.strip().splitlines()
    return t[-1][:200] if t else ""


def process(s, a, dirs):
    stem = f"{s['container_hash']}.{s['type']}"
    raw = dirs["raw"] / f"{stem}.bin"
    res = {"status": "extracted"}
    if a.spirv:
        vk = compile_shader(raw, s["type"], dirs["raw"].parent / "vulkan-m2",
            Toolchain(Path(getattr(a,"vulkan_translator",None) or a.translator),
                      Path(getattr(a,"vulkan_include",None) or a.include),Path(a.dxc)))
        res["spirv_attempted"] = True
        res["spirv"] = vk.status == "ready"
        res["spirv_cache_key"] = vk.cache_key
        if res["spirv"]:
            shutil.copyfile(vk.binary_path,dirs["spirv"] / f"{stem}.spv")
        else:
            res["spirv_failure"] = vk.diagnostic
            (dirs["logs"] / f"{stem}.spirv.log").write_text(vk.diagnostic,encoding="utf-8")
    rc, out, err = run([a.translator, a.include, str(dirs["hlsl"]), str(raw)])
    m = re.search(r"^OK \S+ (\w+) specConstantsMask=([0-9A-F]+) structured=(\d)", out, re.M)
    hlsl = dirs["hlsl"] / f"{stem}.hlsl"
    if rc != 0 or not m or not hlsl.exists():
        reason = first_error(err) or f"exit {rc}"
        if rc not in (0, 2) and not err.strip():
            reason = f"translator crashed (exit {rc & 0xFFFFFFFF:#x})"
        (dirs["logs"] / f"{stem}.translate.log").write_text(f"rc={rc}\n{out}\n{err}")
        res["failure"] = "translate: " + reason
        return res
    res["status"] = "translated"
    spec = int(m.group(2), 16)
    res["spec_constants_mask"] = spec
    res["structured_cf"] = m.group(3) == "1"
    src = hlsl.read_text()
    res["hlsl_lines"] = src.count("\n")
    res["tfetch"] = len(re.findall(r"\btfetch(?:2D|3D|Cube)\(", src))
    # Per-instruction filter overrides (CONAN_RECOMP comments), unique per slot.
    res["tfetch_filters"] = sorted({tuple(int(x) for x in m2.groups()) for m2 in re.finditer(
        r"// conan_tfetch slot=(\d+) mag=(\d+) min=(\d+) mip=(\d+) aniso=(\d+)", src)})
    prof = shader_profile(s["type"], src)
    base = ["-HV", "2021", "-all-resources-bound", "-Wno-ignored-attributes"]
    # default (spec constants = 0) standalone variant: provide the g_SpecConstants body
    std_src = dirs["logs"].parent / "work" / "std" / f"{stem}.hlsl"
    std_src.parent.mkdir(parents=True, exist_ok=True)
    # Conan: spec constants come from the shared constant buffer at runtime
    # (alpha test, packed normals), so one DXIL serves every variant.
    std_src.write_text(src + "\n#ifndef __spirv__\n#ifdef CONAN_RECOMP\n"
                       "uint g_SpecConstants() { return g_SpecConstantsRuntime; }\n#else\n"
                       "uint g_SpecConstants() { return 0; }\n#endif\n#endif\n")
    dx = dirs["dxil"] / f"{stem}.dxil"
    rc, out, err = run([a.dxc, "-T", prof, *base, "-Qstrip_debug", str(std_src), "-Fo", str(dx)])
    if rc != 0:
        (dirs["logs"] / f"{stem}.dxil.log").write_text(err + out)
        res["failure"] = "dxc: " + first_error(err + out)
        return res
    res["dxil_size"] = dx.stat().st_size
    if spec:
        lib = dirs["dxil"] / f"{stem}.lib.dxil"
        lib_prof = "lib_6_5" if prof == "ps_6_5" else "lib_6_3"  # RayQuery receivers
        rc, out, err = run([a.dxc, "-T", lib_prof, *base, "-Qstrip_debug", "-Qstrip_reflect", str(hlsl), "-Fo", str(lib)])
        if rc != 0:
            (dirs["logs"] / f"{stem}.lib.log").write_text(err + out)
            res["failure"] = "dxc lib_6_3: " + first_error(err + out)
            return res
    res["status"] = "compiles"
    return res


def md_table_row(c):
    rf = c["reflection"]
    eff = ",".join(e.replace(".fxc", "") for e in c["effects"][:3]) + ("..." if len(c["effects"]) > 3 else "")
    samplers = " ".join(f"{x['name']}@s{x['reg']}" for x in rf["samplers"])
    if c["type"] == "vs":
        io = " ".join(f"{v['usage']}{v['index']}" for v in rf["vertex_inputs"])
    else:
        io = "->" + "+".join(rf["outputs"])
    rt = c.get("runtime", {})
    seen = rt.get("match", "") if rt else ""
    note = c.get("failure", "") or c.get("validation_note", "")
    return (f"| `{c['container_hash']}` | {c['type']} | `{c['ucode_hash']}` | {c['occurrences']} | {eff} | "
            f"{c['status']} | {len(rf['constants'])} | {samplers} | {io} | {seen} | {note} |")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=str(ARTIFACTS))
    ap.add_argument("--translator", default=str(TRANSLATOR))
    ap.add_argument("--include", default=str(SHADER_COMMON))
    ap.add_argument("--dxc", default=str(DXC))
    ap.add_argument("--vulkan-translator")
    ap.add_argument("--vulkan-include")
    ap.add_argument("--spirv", action="store_true", default=True)
    ap.add_argument("--no-spirv", dest="spirv", action="store_false")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument("--md", default=str(ARTIFACTS / "SHADER_CATALOG.md"))
    a = ap.parse_args()

    corpus = Path(a.corpus)
    dirs = {k: corpus / k for k in ("raw", "hlsl", "dxil", "spirv", "logs")}
    for k in ("hlsl", "dxil", "spirv", "logs"):
        d = dirs[k]
        d.mkdir(parents=True, exist_ok=True)
        for f in d.iterdir():
            f.unlink()
    manifest = json.loads((corpus / "manifest.json").read_text())
    shaders = manifest["shaders"]
    join = {}
    jp = corpus / "runtime_join.json"
    novf = {}
    if jp.exists():
        jj = json.loads(jp.read_text())
        novf = jj.get("novfetch_by_container", {})
        for r in jj["runtime_shaders"]:
            for ch in r["container_hashes"]:
                join.setdefault(ch, []).append({"ucode_data_hash": r["ucode_data_hash"], "match": r["match"]})
    validation = {}
    vp = corpus / "validation.json"
    if vp.exists():
        validation = json.loads(vp.read_text())

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(process, s, a, dirs): s for s in shaders}
        results = {}
        for f in cf.as_completed(futs):
            results[futs[f]["container_hash"]] = f.result()

    catalog = []
    for s in shaders:
        blob = (dirs["raw"] / f"{s['container_hash']}.{s['type']}.bin").read_bytes()
        c = {k: s[k] for k in ("container_hash", "ucode_hash", "type", "size", "ucode_size", "occurrences",
                               "effects", "first_source")}
        c["ucode_hash_novfetch"] = novf.get(s["container_hash"], s["ucode_hash"])
        c.update(results[s["container_hash"]])
        c["reflection"] = reflect(blob)
        c.update(ucode_stats(blob, s))
        rt = join.get(s["container_hash"])
        if rt:
            c["runtime"] = {"seen_in_xenos_cache": True, "match": rt[0]["match"],
                            "ucode_data_hashes": sorted({x["ucode_data_hash"] for x in rt})}
        v = validation.get(s["container_hash"])
        if v:
            c["status"] = v.get("status", c["status"])
            c["validation_note"] = v.get("note", "")
        catalog.append(c)

    counts = Counter(c["status"] for c in catalog)
    by_type = Counter((c["type"], c["status"]) for c in catalog)
    fails = Counter(c["failure"] for c in catalog if "failure" in c)
    spirv_ok = sum(1 for c in catalog if c.get("spirv"))
    runtime_seen = sum(1 for c in catalog if "runtime" in c)
    out = {
        "schema": 1,
        "generated": datetime.datetime.now().isoformat(timespec="seconds"),
        "toolchain": {"xenosrecomp": "zolaware/reblue-XenosRecomp@339af41 + tools/xenosrecomp/patches",
                      "dxc": run([a.dxc, "--version"])[1].strip()},
        "hash_definitions": manifest["hash_definitions"] | {
            "ucode_hash_novfetch": "XXH3_64 of the microcode with every vertex-fetch instruction reduced to "
                                   "word0&0x7FFFF, words1-2 zeroed (tools/shaders/join_runtime_cache.py); "
                                   "joins runtime VS hashes after XDK declaration binding"},
        "summary": {"unique": len(catalog), "status": dict(counts),
                    "by_type": {f"{t}_{st}": n for (t, st), n in sorted(by_type.items())},
                    "failures": dict(fails), "spirv_ok": spirv_ok,
                    "seen_in_runtime_xenos_cache": runtime_seen,
                    "occurrences": manifest["total_occurrences"]},
        "shaders": catalog,
    }
    (corpus / "catalog.json").write_text(json.dumps(out, indent=1))
    # Renderer sampler overrides: "<HASH> <slot> <mag> <min> <mip> <aniso>" for
    # tfetch instructions that override the fetch constant's filters.
    lines = []
    for c in catalog:
        for slot, mag, mn, mip, aniso in c.get("tfetch_filters", []):
            if mag != 3 or mn != 3 or mip != 3 or aniso != 7:
                lines.append(f"{c['container_hash']} {slot} {mag} {mn} {mip} {aniso}")
    (dirs["dxil"] / "tfetch_filters.txt").write_text("\n".join(lines) + "\n")
    print(json.dumps(out["summary"], indent=1))
    write_markdown(Path(a.md), out, manifest)


def write_markdown(path: Path, cat, manifest):
    s = cat["summary"]
    L = []
    L.append("# Shader Catalog\n")
    L.append("Generated by `tools/shaders/build_corpus.sh` (do not hand-edit the generated table; put manual "
             "validation status in `artifacts/shaders/validation.json` and rerun). Machine-readable: "
             "`artifacts/shaders/catalog.json`; runtime join: `artifacts/shaders/runtime_join.json`.\n")
    L.append(f"Generated: {cat['generated']}. Toolchain: {cat['toolchain']['xenosrecomp']}; DXC `{cat['toolchain']['dxc']}`.\n")
    L.append("## Summary\n")
    L.append(f"- Source: `$PORT_DIR/game` (see raw/manifest.json for the files) ({manifest['total_occurrences']} containers in "
             f"146 `.fxc` effect records; no containers in any other game file or in the decoded `default.xex` image).")
    L.append(f"- Unique containers: **{s['unique']}** ({manifest['unique_vs']} VS / {manifest['unique_ps']} PS); "
             f"unique microcode: {manifest['unique_ucode']}.")
    L.append(f"- Status: " + ", ".join(f"{k}: **{v}**" for k, v in sorted(s["status"].items())) +
             f"; SPIR-V also compiles for {s['spirv_ok']}.")
    L.append(f"- By stage: " + ", ".join(f"{k}: {v}" for k, v in s["by_type"].items()))
    L.append(f"- Seen in the Xenos-path runtime shader storage (`bench/**/<TITLE_ID>.xsh`): {s['seen_in_runtime_xenos_cache']} containers.")
    if s["failures"]:
        L.append("- Failures:")
        for k, v in sorted(s["failures"].items(), key=lambda kv: -kv[1]):
            L.append(f"  - {v} x `{k}`")
    L.append("")
    L.append("## Hash definitions\n")
    for k, v in cat["hash_definitions"].items():
        L.append(f"- `{k}`: {v}")
    L.append("")
    L.append("## Status ladder\n")
    L.append("unknown -> extracted (container in `artifacts/shaders/raw`) -> translated (XenosRecomp HLSL) -> "
             "compiles (DXIL vs/ps_6_0 default-spec variant + lib_6_3 when spec constants are used) -> "
             "visually_validated -> optimized. The last two come from `artifacts/shaders/validation.json`.\n")
    L.append("## Pipeline\n")
    L.append("```\nbash tools/shaders/build_corpus.sh      # everything: DXC fetch, XenosRecomp fetch+patch+build, "
             "extract, runtime join, translate, compile, catalog\n```\n")
    L.append("## XenosRecomp base and Conan patches\n")
    L.append("Base: `zolaware/reblue-XenosRecomp@339af41` built WITHOUT `REBLUE_RECOMP` (no Blue Dragon game hacks) "
             "but with its generic fixes (CF structurizer incl. if/else + loops, cube src swizzle). "
             "`tools/xenosrecomp/patches/0001-conan-recomp.patch` adds `CONAN_RECOMP` (evidence = the pre-patch run: "
             "234/591 compiled; 346 DXC errors, 11 translator asserts):\n")
    L.append("- 1-byte container scan in upstream `main.cpp` (1659/1800 containers at offset%4==1).")
    L.append("- Bools: PS bool constants are addressed by CF with the unified Xenos address (128+n). Upstream keyed "
             "them by local index -> undeclared `b128..b138` (158 shaders). Now `BOOL_BIT(n)` over a 256-bit file "
             "(`g_BooleansArr[2]`, c16-c17), plus re:Blue's `cexec bN` gating (upstream executed conditional "
             "exec blocks unconditionally) and the CondExecPredCleanEnd return fix.")
    L.append("- Loops: upstream emitted `for (aL=0; aL<iN.x; aL++)` with no `iN` declared (188 shaders; loop "
             "constants 16/17 = PS i0/i1, e.g. `g_nNumCheapLights`). Now xenos::LoopConstant semantics "
             "(count/start/step, aL save/restore, predicated break) from `g_LoopConstantsArr[8]` (c19-c26, "
             "32 loop constants) or from `defi` literals in the definition table (7 shaders use literal loop 31).")
    L.append("- Relative constant addressing (`c[a0+n]`, `c[aL+n]`) on singleton/unnamed registers asserted (11 shaders) "
             "and upstream always used const0Relative. Now per-operand slot rules from ucode.h "
             "`src_const_is_addressed()` and `CONST_REL(i)` over the whole 256-entry file; the DXIL cbuffer is "
             "one `float4 g_{Vertex,Pixel}ShaderConstantsArr[256]` with names as macros (PS file is 256, not UR's 224).")
    L.append("- PRED_SETINV result fix and unconditional `oPos = 0` init (from re:Blue).")
    L.append("- Driver: `tools/xenosrecomp/corpus_main.cpp` (one process per shader, asserts on, HLSL only); "
             "DXC runs out of process (`tools/dxc`, v1.9.2607). Shaders with spec constants (alpha test, "
             "R11G11B10 normals) get both a default-spec `vs/ps_6_0` DXIL and the `lib_6_3` library form.\n")
    L.append("Known gaps (see docs/OPEN_QUESTIONS.md): nothing is visually validated yet; VS input types "
             "(NORMAL/TANGENT/BLENDINDICES as uint4) and texture-format/swizzle semantics depend on host binding "
             "choices not yet made; 4 Bloom PS use the unstructured `switch(pc)` fallback.\n")
    L.append("## Table\n")
    L.append("Columns: container hash (XenosRecomp/UR key), stage, ucode hash (Xenos backend key), occurrences in "
             "shaders.stx, effects (.fxc records containing it), status, #float4 constants, samplers (name@slot), "
             "VS inputs or PS outputs, runtime join level, failure/notes.\n")
    L.append("| container_hash | st | ucode_hash | occ | effects | status | f4 | samplers | io | runtime | notes |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for c in sorted(cat["shaders"], key=lambda c: (c["type"] != "vs", ",".join(c["effects"]), c["container_hash"])):
        L.append(md_table_row(c))
    path.write_text("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
