"""make_zip.py <folder> <zip>: wejście to dowolny katalog; każdy plik trafia do zipa
pod ścieżką względną z ukośnikami '/', skompresowany deflate. Katalog budujemy w tmp_path."""
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "release" / "make_zip.py"


def test_every_file_with_relative_forward_slash_path(tmp_path):
    src = tmp_path / "pkg"
    files = {
        "run.cmd": b"@echo off\r\n",
        "shader_tools/dxc.exe": b"MZ" + bytes(range(256)),
        "shader_tools/vulkan/a.py": b"print(1)\n",
        "licenses/LICENSE.txt": b"",
    }
    for name, data in files.items():
        path = src / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    out = tmp_path / "release.zip"
    result = subprocess.run([sys.executable, str(TOOL), str(src), str(out)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    with zipfile.ZipFile(out) as archive:
        names = sorted(archive.namelist())
        contents = {name: archive.read(name) for name in names}
        kinds = {info.compress_type for info in archive.infolist()}
    # źródło: lista plików zapisanych wyżej, przepisana ręcznie z '/' jako separatorem
    assert names == ["licenses/LICENSE.txt", "run.cmd", "shader_tools/dxc.exe", "shader_tools/vulkan/a.py"]
    # źródło: bajty zapisane wyżej w files
    assert contents == files
    # źródło: docstring narzędzia ("deflate every file"), odczyt przez zipfile.ZipInfo
    assert kinds == {zipfile.ZIP_DEFLATED}
