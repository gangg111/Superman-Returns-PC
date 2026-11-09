"""Makes the one-file builder: build/builder/Superman Returns Builder.exe.

The program (builder/*.cpp, clang from .tools with the static C runtime) with its payload
appended (builder/pack.h): the port's files from a white list (no logs, backups, caches or
measuring scripts), a clean superman_returns.toml, a README, the menus' patches against the
game's ui.AST and text.AST (srdelta.py: our bytes only) and builder.txt, the game files'
SHA-256 the build checks. No game data goes in; the build refuses to finish otherwise.

    py tools/builder/make_builder.py [--port <folder with the built port>] [--game <game folder>]

--port: the folder the port runs from (default: ../SupermanReturnsPC, its files are the
built ones); --game: the game (default: game, default.xex and DATA as on the disc).
"""
import argparse
import ctypes
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "ui"))
sys.path.insert(0, os.path.dirname(__file__))
import srdelta  # noqa: E402

OUT_DIR = os.path.join(ROOT, "build", "builder")
EXE_NAME = "Superman Returns Builder.exe"
CLANG = os.path.join(ROOT, ".tools", "clang+llvm-23.1.2-x86_64-pc-windows-msvc", "bin")
SOURCES = ["main.cpp", "build.cpp", "audio.cpp", "textures.cpp"]
# The remastered textures (builder/textures.h): where the made ones' game textures are on the
# disc (found once by their bytes in the renderer's dumps) and how each is made.
TEXTURES_JSON = os.path.join(os.path.dirname(__file__), "textures.json")
# builder/ffmpeg_min.sh's install folder (MSYS2 home): the DLLs the audio conversion loads
FFMPEG = os.environ.get("SR_FFMPEG_MIN", f"C:/msys64/home/{os.path.basename(os.path.expanduser('~'))}/srff/out")

# The port's files the game needs (the folder it runs from); folders whole.
PORT_FILES = [
    "superman_returns.exe", "rexruntime.dll", "rexgpu-xenos.dll", "nvngx_dlss.dll",
    "msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll",
    "superman_returns_pipelines.bin", "superman_returns_shaders.srsl", "superman_returns_vulkan.srvk",
    "THIRD_PARTY_NOTICES.txt",
]
PORT_DIRS = ["shader_tools", "licenses"]
PORT_SKIP = {"licenses/DOTNET-LICENSE.txt", "licenses/DOTNET-ThirdPartyNotices.txt"}  # the launcher's
# The menus: (game file, patch name in the payload, the file the patch makes)
PATCHES = [("DATA/ui.AST", "patches/ui.srdelta", "DATA/ui_sr.AST"),
           ("DATA/text.AST", "patches/text.srdelta", "DATA/text_sr.AST")]
MADE_BY_US = {"DATA/ui_sr.AST", "DATA/text_sr.AST"}

DEFAULT_TOML = """\
# Superman Returns PC: settings. The game's menus (OPTIONS) write here; delete a line to get
# its default back. Keys: OPTIONS > KEYS MAPPING (Y or X restores the defaults).
mnk_mode = true
mnk_mouse = true
sr_mouse_sensitivity = 15
"""

README = """\
Superman Returns PC (native port)

Start: superman_returns.exe. Every setting is in the game's menus: pause menu (Esc) > OPTIONS
(Graphics, Display, Keys Mapping, Mouse Options and the game's own). Keyboard and mouse or a pad;
the game switches with the one you use.

This folder holds data from your own disc: for your private use only, do not share it.

---

Superman Returns PC (natywny port)

Start: superman_returns.exe. Wszystkie ustawienia są w menu gry: menu pauzy (Esc) > OPTIONS
(grafika, ekran, klawisze, mysz i opcje samej gry). Klawiatura z myszą albo pad; gra przełącza
się na to, czego używasz.

Ten folder zawiera dane z Twojej płyty: tylko do Twojego prywatnego użytku, nie udostępniaj go.
"""


def lzms(data: bytes) -> bytes:
    """Windows Compression API, LZMS (the builder unpacks with the same, pack.h)."""
    cab = ctypes.WinDLL("cabinet.dll")
    handle = ctypes.c_void_p()
    if not cab.CreateCompressor(5, None, ctypes.byref(handle)):  # COMPRESS_ALGORITHM_LZMS
        raise OSError("CreateCompressor")
    out = ctypes.create_string_buffer(len(data) + len(data) // 8 + 65536)
    size = ctypes.c_size_t()
    ok = cab.Compress(handle, data, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(len(out)), ctypes.byref(size))
    cab.CloseCompressor(handle)
    if not ok:
        raise OSError("Compress")
    return out.raw[:size.value]


def pack(files, base=0):
    """files: [(payload path, bytes)] -> the archive appended to the program (pack.h); the
    index's offsets count from the .exe's start, the archive starting at `base`."""
    body = bytearray()
    index = bytearray(struct.pack("<I", len(files)))
    for path, data in files:
        packed = lzms(data) if len(data) > 4096 else data
        method = 1 if packed is not data and len(packed) < len(data) else 0
        if not method:
            packed = data
        name = path.encode("utf-8")
        index += struct.pack("<H", len(name)) + name + struct.pack("<BQQQ", method, base + len(body), len(packed), len(data))
        index += hashlib.sha256(data).digest()
        body += packed
    return bytes(body), bytes(index)


def tiled_address(x, y, pitch_macro, bpb_log2):
    """Xenos 2D tiling in bytes (builder/textures.cpp TiledAddress2D)."""
    outer = ((y >> 5) * pitch_macro + (x >> 5)) << 6
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oi = (outer | inner) << bpb_log2
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return ((y & 1) << 4) | (pipe << 6) | (((y >> 4) & 1) << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) | \
        (((oi >> 5) & 7) << 8) | (oi >> 8 << 12)


def decode_game_texture(tiled, width, height, fmt):
    """RGBA8 of a game texture as the tools saw it (Pillow on the renderer's dump): untiled,
    8-in-16 swapped, BC1 (format 18) or BC3 (20) decoded by Pillow."""
    from PIL import Image  # noqa: PLC0415
    bpb, log2 = (16, 4) if fmt == 20 else (8, 3)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    pitch = (bw + 31) >> 5
    blocks = bytearray()
    for y in range(bh):
        for x in range(bw):
            at = tiled_address(x, y, pitch, log2)
            b = tiled[at:at + bpb]
            blocks += bytes(b[i ^ 1] for i in range(bpb))
    header = struct.pack("<4s7I44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, height, width,
                         len(blocks), 0, 1)
    header += struct.pack("<2I4s5I", 32, 0x4, b"DXT5" if fmt == 20 else b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return Image.open(io.BytesIO(header + bytes(blocks))).convert("RGBA").tobytes()


def textures_payload(port, game, files):
    """The switch's textures: ours whole (own), the made ones as a recipe on the player's game
    (src + window, or src + diff: our difference to the game's sky, not the sky)."""
    tex = os.path.join(port, "textures")
    spec = json.load(open(TEXTURES_JSON, encoding="utf-8"))
    made = {e["name"] + ("_win.dds" if "window_kenv" in e else ".dds") for e in spec}
    lines = []
    for dirpath, _, names in os.walk(tex):
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(dirpath, n), tex).replace("\\", "/")
            if n in made:
                continue
            files.append(("textures/" + rel, open(os.path.join(tex, rel), "rb").read()))
            lines.append("own textures/" + rel)
    for e in spec:
        with open(os.path.join(game, e["file"]), "rb") as f:
            f.seek(e["offset"])
            tiled = f.read(e["size"])
        if hashlib.sha256(tiled).hexdigest() != e["sha256"]:
            raise SystemExit(f"{e['name']}: not at {e['file']}+{e['offset']}")
        lines.append(f"src {e['name']} {e['file']} {e['offset']} {e['size']} {e['width']} {e['height']} "
                     f"{e['format']} {e['sha256']}")
        if "window_kenv" in e:
            lines.append("window " + e["name"] + "".join(f" {float(k)!r}" for k in e["window_kenv"]))
        else:
            ref = open(os.path.join(tex, e["name"] + ".dds"), "rb").read()
            game_rgba = decode_game_texture(tiled, e["width"], e["height"], e["format"])
            if len(ref) != 128 + len(game_rgba):
                raise SystemExit(f"{e['name']}.dds: not RGBA8 of the game's size")
            import numpy as np  # noqa: PLC0415
            diff = (np.frombuffer(ref, np.uint8, offset=128) - np.frombuffer(game_rgba, np.uint8)).astype(np.uint8)
            name = f"textures_made/{e['name']}.diff"
            files.append((name, ref[:128] + diff.tobytes()))
            lines.append(f"diff {e['name']} {name}")
    # the night's street lamps (the user's lamp_cache.bin, 2026-11-09: positions only, 3 floats a
    # lamp): the renderer knows only the lamps of sectors already loaded, so a new folder lit them
    # near Superman alone until the city had been flown over
    lamps = os.path.join(port, "lamp_cache.bin")
    data = open(lamps, "rb").read()
    if not data or len(data) % 12:
        raise SystemExit(f"{lamps}: not whole lamps")
    files.append(("night/lamp_cache.bin", data))
    lines.append("root night/lamp_cache.bin")
    files.append(("textures.txt", ("\n".join(lines) + "\n").encode("utf-8")))
    print(f"textures: {sum(l.startswith('own ') for l in lines)} ours, {len(spec)} made from the game")


def payload(port, game):
    files = []
    for name in PORT_FILES:
        files.append(("files/" + name, open(os.path.join(port, name), "rb").read()))
    for d in PORT_DIRS:
        for dirpath, _, names in os.walk(os.path.join(port, d)):
            for n in sorted(names):
                rel = os.path.relpath(os.path.join(dirpath, n), port).replace("\\", "/")
                if rel not in PORT_SKIP:
                    files.append(("files/" + rel, open(os.path.join(port, rel), "rb").read()))
    files.append(("files/superman_returns.toml", DEFAULT_TOML.encode("utf-8")))
    ff_bin = os.path.join(FFMPEG, "bin")
    for n in sorted(os.listdir(ff_bin)):
        if n.endswith(".dll"):
            files.append(("ffmpeg/" + n, open(os.path.join(ff_bin, n), "rb").read()))
    files.append(("files/README.txt", README.replace("\n", "\r\n").encode("utf-8")))

    plan = []
    disc = ["default.xex"] + sorted("DATA/" + n for n in os.listdir(os.path.join(game, "DATA")))
    for rel in disc:
        if rel in MADE_BY_US:
            continue
        h = hashlib.sha256()
        with open(os.path.join(game, rel), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 24), b""):
                h.update(chunk)
        plan.append(f"game {rel} {h.hexdigest()}")
    # the menus, built now from the game's files (tools/ui/build_ui_mod.py), as patches
    import build_ui_mod  # noqa: PLC0415
    with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "build")) as tmp:
        build_ui_mod.build(os.path.join(game, "DATA"), tmp)
        for src, patch_name, target in PATCHES:
            source = open(os.path.join(game, src), "rb").read()
            made = open(os.path.join(tmp, os.path.basename(target)), "rb").read()
            patch = srdelta.make(source, made)
            print(f"{patch_name}: {len(patch)} bytes, {srdelta.added_bytes(patch)} ours")
            files.append((patch_name, patch))
            plan.append(f"patch {src} {patch_name} {target}")
    textures_payload(port, game, files)
    files.append(("builder.txt", ("\n".join(plan) + "\n").encode("utf-8")))
    # no game file goes in whole
    disc_hashes = {line.split()[2] for line in plan if line.startswith("game ")}
    for path, data in files:
        if hashlib.sha256(data).hexdigest() in disc_hashes:
            raise SystemExit(f"{path} is a game file: not in the builder")
    return files


def compile_builder(exe):
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                        capture_output=True, text=True, check=True).stdout.strip()
    src = [os.path.join(ROOT, "builder", s) for s in SOURCES]
    rc = os.path.join(ROOT, "builder", "builder.rc")
    res = os.path.join(os.path.dirname(exe), "builder.res")
    cmd = (f'call "{vs}\\Common7\\Tools\\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && '
           f'"{CLANG}\\llvm-rc.exe" /fo "{res}" "{rc}" && '
           f'"{CLANG}\\clang++.exe" -std=c++20 -O2 -fms-runtime-lib=static -DUNICODE -D_UNICODE -DNOMINMAX '
           f'-I"{FFMPEG}/include" '
           + " ".join(f'"{s}"' for s in src)
           + f' "{res}" -o "{exe}" -Xlinker /MAP -Xlinker /SUBSYSTEM:WINDOWS -Xlinker /ENTRY:wWinMainCRTStartup '
           "-luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -lbcrypt -lcabinet")
    script = os.path.join(os.path.dirname(exe), "compile_builder.cmd")
    with open(script, "w", encoding="utf-8", newline="\r\n") as f:  # cmd.exe needs CRLF
        f.write("@echo off\nchcp 65001 >nul\n" + cmd + "\n")
    subprocess.run([script], check=True)


def make_icon():
    """builder/app.ico from port/resources/cursor_s.png (the S shield)."""
    from PIL import Image  # noqa: PLC0415
    ico = os.path.join(ROOT, "builder", "app.ico")
    Image.open(os.path.join(ROOT, "port", "resources", "cursor_s.png")).convert("RGBA").save(
        ico, sizes=[(16, 16), (32, 32), (48, 48)])
    return ico


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=os.path.join(ROOT, "..", "SupermanReturnsPC"))
    ap.add_argument("--game", default=os.path.join(ROOT, "game"))
    a = ap.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)
    make_icon()
    program = os.path.join(OUT_DIR, "builder_program.exe")
    compile_builder(program)
    files = payload(os.path.abspath(a.port), os.path.abspath(a.game))
    prog = open(program, "rb").read()
    body, index = pack(files, len(prog))
    exe = os.path.join(OUT_DIR, EXE_NAME)
    with open(exe, "wb") as f:
        f.write(prog)
        index_at = len(prog) + len(body)
        f.write(body)
        f.write(index)
        f.write(struct.pack("<Q", index_at) + b"SRBPAK01")
    print(f"{exe}: {os.path.getsize(exe) / 2**20:.1f} MB, {len(files)} files")
    # FFmpeg rides in the builder (LGPL 2.1): its licences and where its source is, beside it
    lic = os.path.join(OUT_DIR, "licenses", "ffmpeg")
    shutil.rmtree(lic, ignore_errors=True)
    shutil.copytree(os.path.join(FFMPEG, "bin", "licenses"), lic)
    with open(os.path.join(lic, "SOURCE.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("The builder carries FFmpeg 8.0.1 (avcodec-62, avformat-62, avutil-60) and winpthreads as\n"
                "DLLs, LGPL 2.1, unchanged. Source: https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz\n"
                "Built by builder/ffmpeg_min.sh (its configure line is the whole configuration).\n")


if __name__ == "__main__":
    main()
