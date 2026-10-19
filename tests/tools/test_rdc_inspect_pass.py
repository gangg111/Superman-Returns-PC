"""tools/analysis/rdc_inspect_pass.py with a fake `renderdoc` module.

The tool is a qrenderdoc --python script (runs at import, ends with os._exit), so it runs
in a subprocess with a stub renderdoc on PYTHONPATH. Expected input: env SR_RDC, SR_RDC_OUT,
optional SR_RDC_EVENTS ("3,99") or SR_RDC_INDEX_COUNT (select draws with that index count).

Fake actions: eid 3 draw 36 idx, eid 7 draw 36 idx, eid 8 draw 12 idx, eid 9 dispatch 36 idx.
Every event sees color target ResourceId::10 (1280x720 R8G8B8A8_UNORM, described) and depth
target ResourceId::99 (not in the texture list).
"""
import os
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analysis" / "rdc_inspect_pass.py"

FAKE = '''
from types import SimpleNamespace as SN
class ResultCode:
    Succeeded = 0
class ReplayOptions: pass
class ActionFlags:
    Drawcall, Dispatch = 1, 2
class ShaderStage:
    Vertex, Pixel = "V", "P"
class RID:
    def __init__(self, n): self.n = n
    def __str__(self): return f"ResourceId::{self.n}"
class Fmt:
    def __init__(self, name): self._n = name
    def Name(self): return self._n
class A:
    def __init__(self, eid, flags, idx):
        self.eventId, self.flags, self.numIndices, self.numInstances = eid, flags, idx, 1
        self.children = []
    def GetName(self, sf): return f"DrawIndexed({self.numIndices})"
class Pipe:
    def GetOutputTargets(self): return [SN(resource=RID(10), format=Fmt("R8G8B8A8_UNORM"))]
    def GetDepthTarget(self): return SN(resource=RID(99), format=Fmt("D24S8"))
    def GetDepthTestState(self): return SN(depthEnable=True, depthFunction="Less")
    def GetColorBlends(self):
        return [SN(enabled=True, writeMask=15, colorBlend=SN(source="SrcAlpha"), alphaBlend=SN(source="One"))]
    def GetShader(self, stage): return "VS#1" if stage == "V" else "PS#2"
class Controller:
    def GetTextures(self): return [SN(resourceId=RID(10), width=1280, height=720, format=Fmt("R8G8B8A8_UNORM"))]
    def GetRootActions(self):
        root = A(1, 0, 0)
        root.children = [A(3, 1, 36), A(7, 1, 36), A(8, 1, 12), A(9, 2, 36)]
        return [root]
    def SetFrameEvent(self, eid, force): pass
    def GetPipelineState(self): return Pipe()
    def GetStructuredFile(self): return None
    def Shutdown(self): pass
class Cap:
    def OpenFile(self, path, fmt, prog): return ResultCode.Succeeded
    def OpenCapture(self, opts, prog): return ResultCode.Succeeded, Controller()
    def Shutdown(self): pass
def OpenCaptureFile(): return Cap()
'''


def run(tmp_path, **extra):
    stub = tmp_path / "stub"
    stub.mkdir()
    (stub / "renderdoc.py").write_text(FAKE, encoding="utf-8")
    report = tmp_path / "report.txt"
    env = dict(os.environ, PYTHONPATH=str(stub), SR_RDC="frame.rdc", SR_RDC_OUT=str(report))
    for key in ("SR_RDC_EVENTS", "SR_RDC_INDEX_COUNT", "SR_RDC_API_HELP", "SR_RDC_RESOURCE_USAGE"):
        env.pop(key, None)
    env.update(extra)
    proc = subprocess.run([sys.executable, str(TOOL)], env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return report.read_text(encoding="utf-8").splitlines()


def block(eid):
    # źródło: ręcznie z fałszywego potoku; słowniki to atrybuty w kolejności alfabetycznej dir()
    return [
        f"eid {eid}: DrawIndexed(36) indices=36 instances=1",
        "  color: ResourceId::10 1280x720 fmt=R8G8B8A8_UNORM",
        "  color view formats: R8G8B8A8_UNORM",
        "  depth: ResourceId::99 (no texture description)",
        "  depth test: {'depthEnable': 'True', 'depthFunction': 'Less'}",
        "  blend 0: {'alphaBlend': \"namespace(source='One')\", "
        "'colorBlend': \"namespace(source='SrcAlpha')\", 'enabled': 'True', 'writeMask': '15'}",
        "  colorBlend: {'source': 'SrcAlpha'}",
        "  alphaBlend: {'source': 'One'}",
        "  VS: VS#1",
        "  PS: PS#2",
    ]


def test_explicit_events(tmp_path):
    lines = run(tmp_path, SR_RDC_EVENTS="99, 3")
    # źródło: ręcznie, zdarzenia posortowane rosnąco, 99 nie istnieje w drzewie akcji
    assert lines == block(3) + ["eid 99: no action"]


def test_index_count_selects_matching_draws(tmp_path):
    lines = run(tmp_path, SR_RDC_INDEX_COUNT="36")
    # źródło: ręcznie, 36 indeksów mają draw 3 i 7; eid 9 to dispatch, eid 8 ma 12 indeksów
    assert lines == block(3) + block(7)
