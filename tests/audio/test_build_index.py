"""tools/audio/build_index.py: index.bin from a tiny manifest.json (no game files).

Input the tool expects: <dir>/manifest.json with "sounds", each having file, exact,
fnv256 (hex or null), snr16 (hex), channels, streamed, loop, rate, samples,
loop_start, archive, name and optionally blocks. Only sounds with a file and
exact == true go into the index."""
import json
import subprocess
import sys

import pytest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUDIO_TOOLS = ROOT / "tools" / "audio"


TOOL = AUDIO_TOOLS / "build_index.py"
NATIVE_AUDIO = ROOT.parent / "SupermanReturnsPC" / "native_audio"

MANIFEST = {"sounds": [
    {"archive": "a.AST", "name": "S1", "file": "a.AST/S1.wav", "exact": True, "fnv256": "DEADBEEF",
     "snr16": "0304ac44", "channels": 2, "streamed": True, "loop": False, "rate": 44100,
     "samples": 1000, "loop_start": 0, "blocks": [500, 500], "sns_offset": 4096},
    {"archive": "a.AST", "name": "BROKEN", "error": "RuntimeError: x"},
    {"archive": "a.AST", "name": "S3", "file": "a.AST/S3.wav", "exact": False, "fnv256": None,
     "snr16": "", "channels": 1, "streamed": False, "loop": False, "rate": 1, "samples": 1, "loop_start": 0},
    {"archive": "b.AST", "name": "S4", "file": "S4.wav", "exact": True, "fnv256": None,
     "channels": 1, "streamed": False, "loop": True, "rate": 22050, "samples": 300, "loop_start": 100},
]}

# źródło: bajty wypisane ręcznie z opisu układu w docstringu narzędzia (LE, wersja 3)
EXPECTED = bytes.fromhex(
    "53524149" "03000000" "02000000"                   # "SRAI", version 3, 2 sounds (BROKEN, S3 out)
    # S1: fnv 0xDEADBEEF, has 1, 2 ch, streamed, no loop, 44100 = 0xAC44, 1000 = 0x3E8, loop 0
    "efbeadde" "01" "02" "01" "00" "44ac0000" "e8030000" "00000000"
    "0304ac44" + "00" * 12 +                           # snr16 zero padded to 16
    "0c00" + "a.AST/S1.wav".encode().hex() + "0800" + "a.AST/S1".encode().hex() +
    "02000000" "f4010000" "f4010000"                   # 2 blocks of 500 = 0x1F4
    "00100000"                                         # sns_offset 4096 = 0x1000
    # S4: no fnv, 1 ch, RAM, loop, 22050 = 0x5622, 300 = 0x12C, loop start 100 = 0x64
    "00000000" "00" "01" "00" "01" "22560000" "2c010000" "64000000" + "00" * 16 +
    "0600" + "S4.wav".encode().hex() + "0800" + "b.AST/S4".encode().hex() + "00000000"
    "ffffffff")                                        # no sns_offset


def test_index_bytes_from_tiny_manifest(tmp_path):
    (tmp_path / "manifest.json").write_text(json.dumps(MANIFEST), encoding="utf-8")
    proc = subprocess.run([sys.executable, str(TOOL), str(tmp_path)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    data = (tmp_path / "index.bin").read_bytes()
    assert data == EXPECTED
    assert f"2 sounds, {len(EXPECTED)} bytes" in proc.stdout  # źródło: długość bajtów policzonych ręcznie


@pytest.mark.skipif(not (NATIVE_AUDIO / "index.bin").exists(), reason="brak native_audio/index.bin z gry")
def test_real_index_header():
    data = (NATIVE_AUDIO / "index.bin").read_bytes()
    manifest = json.loads((NATIVE_AUDIO / "manifest.json").read_text(encoding="utf-8"))
    count = sum(1 for s in manifest["sounds"] if s.get("file") and s.get("exact"))
    assert data[:8] == b"SRAI\x03\x00\x00\x00"  # źródło: format wersja 3 z docstringu
    assert int.from_bytes(data[8:12], "little") == count  # źródło: liczba z manifestu
