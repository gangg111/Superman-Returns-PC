"""tools/analysis/rdc_mesh_probe.py with a fake `renderdoc` module.

The tool is a qrenderdoc --python script (runs at import, ends with os._exit), so it runs
in a subprocess with a stub renderdoc on PYTHONPATH. Expected input: env SR_RDC (capture)
and SR_RDC_OUT (JSON report). It picks draws with more than 300 indices, largest first
(max 12), and reads 16 post-VS positions (4 floats each) per draw.

Fake actions: eid 3 draw 600 idx, eid 4 draw 900 idx, eid 5 draw 200 idx, eid 6 dispatch
1000 idx. Post-VS buffer: stride 16, vertex k = (k, 2k, 0.5, 1.0).
"""
import json
import os
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analysis" / "rdc_mesh_probe.py"

FAKE = '''
import os
import struct
from types import SimpleNamespace as SN
class ResultCode:
    Succeeded = 0
    Failed = 1
class ReplayOptions: pass
class ActionFlags:
    Drawcall, Dispatch = 1, 2
class MeshDataStage:
    VSOut = 1
class ResourceId:
    def __init__(self, n=0): self.n = n
    def __eq__(self, other): return self.n == other.n
    def __ne__(self, other): return self.n != other.n
    def __str__(self): return f"ResourceId::{self.n}"
    @staticmethod
    def Null(): return ResourceId(0)
class Controller:
    def GetRootActions(self):
        leaves = [SN(eventId=e, flags=f, numIndices=n, children=[])
                  for e, f, n in ((3, 1, 600), (4, 1, 900), (5, 1, 200), (6, 2, 1000))]
        return [SN(eventId=1, flags=0, numIndices=0, children=leaves)]
    def SetFrameEvent(self, eid, force): pass
    def GetPipelineState(self):
        return SN(GetVertexInputs=lambda: [SN(name="POSITION")],
                  GetVBuffers=lambda: [SN(byteStride=16)],
                  GetIBuffer=lambda: SN(byteStride=2))
    def GetPostVSData(self, inst, view, stage):
        return SN(vertexResourceId=ResourceId(5), vertexByteOffset=0, vertexByteStride=16)
    def GetBufferData(self, rid, offset, length):
        return b"".join(struct.pack("<4f", k, 2 * k, 0.5, 1.0) for k in range(length // 16))
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
    report = tmp_path / "mesh.json"
    env = dict(os.environ, PYTHONPATH=str(stub), SR_RDC=capture, SR_RDC_OUT=str(report))
    proc = subprocess.run([sys.executable, str(TOOL)], env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return json.loads(report.read_text(encoding="utf-8"))


def test_mesh_probe_on_fake_capture(tmp_path):
    report = run(tmp_path, "frame.rdc")
    # źródło: ręcznie, >300 indeksów i flaga draw mają tylko eid 4 (900) i 3 (600), malejąco
    assert [item["event"] for item in report] == [4, 3]
    first = report[0]
    # źródło: atrybuty fałszywej akcji, str() każdej wartości, kolejność alfabetyczna dir()
    assert first["action"] == {"children": "[]", "eventId": "4", "flags": "1", "numIndices": "900"}
    assert first["attributes"] == [{"name": "POSITION"}]  # źródło: fałszywy GetVertexInputs
    assert first["buffers"] == [{"byteStride": "16"}]  # źródło: fałszywy GetVBuffers
    assert first["indices"] == {"byteStride": "2"}  # źródło: fałszywy GetIBuffer
    assert first["post_vs"] == {"vertexByteOffset": "0", "vertexByteStride": "16",
                                "vertexResourceId": "ResourceId::5"}  # źródło: jw.
    # źródło: wejście bufora, wierzchołek k = (k, 2k, 0.5, 1.0), 16 wierzchołków co 16 bajtów
    assert first["positions"] == [[k, 2 * k, 0.5, 1.0] for k in range(16)]


def test_open_failure_reported_as_json_error(tmp_path):
    report = run(tmp_path, "bad.rdc")
    # źródło: fałszywy OpenFile zwraca Failed (1), narzędzie zapisuje {"error": traceback}
    assert list(report) == ["error"]
    assert "RuntimeError: 1" in report["error"]
