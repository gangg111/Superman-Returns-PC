"""tools/analysis/rdc_dump_target.py with a fake `renderdoc` module.

The tool is a qrenderdoc --python script: it runs at import time and ends with os._exit, so
it is executed in a subprocess with a stub renderdoc package on PYTHONPATH. Expected input:
env SR_RDC (capture path), SR_RDC_EVENTS (comma-separated event IDs), SR_RDC_OUT_DIR.
Output: event_<eid>.raw per event and index.txt with "eid,resource,w,h,format,bytes,path".
"""
import os
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analysis" / "rdc_dump_target.py"

FAKE = '''
import os
class ResultCode:
    Succeeded = 0
    Failed = 1
class ReplayOptions: pass
class Subresource: pass
class RID:
    def __init__(self, n): self.n = n
    def __str__(self): return f"ResourceId::{self.n}"
class Fmt:
    def __init__(self, name): self._n = name
    def Name(self): return self._n
class Tex:
    def __init__(self, n, w, h, fmt): self.resourceId, self.width, self.height, self.format = RID(n), w, h, Fmt(fmt)
class Target:
    def __init__(self, n): self.resource = RID(n)
class Pipe:
    def GetOutputTargets(self): return [Target(10), Target(11)]
class Controller:
    eid = 0
    def GetTextures(self): return [Tex(10, 4, 2, "R8G8B8A8_UNORM"), Tex(11, 8, 8, "R16G16B16A16_FLOAT")]
    def SetFrameEvent(self, eid, force): self.eid = eid
    def GetPipelineState(self): return Pipe()
    def GetTextureData(self, rid, sub): return bytes([rid.n]) * (self.eid * 4)
    def Shutdown(self): pass
class Cap:
    def OpenFile(self, path, fmt, prog):
        return ResultCode.Failed if path.endswith("bad.rdc") else ResultCode.Succeeded
    def OpenCapture(self, opts, prog): return ResultCode.Succeeded, Controller()
    def Shutdown(self): pass
def OpenCaptureFile(): return Cap()
'''


def run(tmp_path, capture):
    stub = tmp_path / "stub"
    stub.mkdir()
    (stub / "renderdoc.py").write_text(FAKE, encoding="utf-8")
    out = tmp_path / "out"
    env = dict(os.environ, PYTHONPATH=str(stub), SR_RDC=capture, SR_RDC_EVENTS="3,5",
               SR_RDC_OUT_DIR=str(out))
    proc = subprocess.run([sys.executable, str(TOOL)], env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return out


def test_dumps_first_color_target(tmp_path):
    out = run(tmp_path, "frame.rdc")
    index = (out / "index.txt").read_text(encoding="utf-8").splitlines()
    # źródło: ręcznie, cel 0 = ResourceId::10 4x2 R8G8B8A8_UNORM, fałszywe dane = eid*4 bajtów
    assert index == [
        f"3,ResourceId::10,4,2,R8G8B8A8_UNORM,12,{out / 'event_3.raw'}",
        f"5,ResourceId::10,4,2,R8G8B8A8_UNORM,20,{out / 'event_5.raw'}",
    ]
    # źródło: ręcznie, bajt = numer zasobu (10), długość eid*4
    assert (out / "event_3.raw").read_bytes() == b"\x0a" * 12
    assert (out / "event_5.raw").read_bytes() == b"\x0a" * 20


def test_open_failure_is_reported_in_index(tmp_path):
    out = run(tmp_path, "bad.rdc")
    text = (out / "index.txt").read_text(encoding="utf-8")
    # źródło: fałszywy OpenFile zwraca Failed (1), narzędzie zapisuje traceback zamiast danych
    assert "RuntimeError: open failed: 1" in text
    assert not list(out.glob("event_*.raw"))
