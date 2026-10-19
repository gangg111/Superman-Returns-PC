"""tools/analysis/rdc_gpu_profile.py with a fake `renderdoc` module.

The tool is a qrenderdoc --python script (runs at import, ends with os._exit), so it runs
in a subprocess with a stub renderdoc on PYTHONPATH. Expected input: env SR_RDC (capture)
and SR_RDC_OUT (report path); the replay supplies an action tree and EventGPUDuration
counter values in seconds.

Fake frame (durations in seconds):
  marker "Frame" (eid 1)
    marker "Shadow" (eid 2)
      Draw3      draw     0.002
      Draw4      draw     0.001
    Dispatch5    dispatch 0.0006
  Clear6 (root)  clear    0.0004
"""
import os
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).resolve().parents[2] / "tools" / "analysis" / "rdc_gpu_profile.py"

FAKE = '''
class ResultCode:
    Succeeded = 0
class ReplayOptions: pass
class GPUCounter:
    EventGPUDuration = 1
class ActionFlags:
    Drawcall, Dispatch, Copy, Resolve, Clear = 1, 2, 4, 8, 16
class A:
    def __init__(self, eid, name, flags=0, children=()):
        self.eventId, self.name, self.flags, self.children = eid, name, flags, list(children)
    def GetName(self, sf): return self.name
class V:
    def __init__(self, d): self.d = d
class R:
    def __init__(self, eid, d): self.eventId, self.value = eid, V(d)
class Controller:
    def __init__(self, counters): self.counters = counters
    def EnumerateCounters(self): return self.counters
    def FetchCounters(self, which):
        return [R(3, 0.002), R(4, 0.001), R(5, 0.0006), R(6, 0.0004)]
    def GetStructuredFile(self): return None
    def GetRootActions(self):
        shadow = A(2, "Shadow", 0, [A(3, "Draw3", 1), A(4, "Draw4", 1)])
        return [A(1, "Frame", 0, [shadow, A(5, "Dispatch5", 2)]), A(6, "Clear6", 16)]
    def Shutdown(self): pass
class Cap:
    def OpenFile(self, path, fmt, prog):
        self.path = path
        return ResultCode.Succeeded
    def OpenCapture(self, opts, prog):
        return ResultCode.Succeeded, Controller([] if "nocounter" in self.path else [1])
    def Shutdown(self): pass
def OpenCaptureFile(): return Cap()
'''


def run(tmp_path, capture):
    stub = tmp_path / "stub"
    stub.mkdir()
    (stub / "renderdoc.py").write_text(FAKE, encoding="utf-8")
    report = tmp_path / "report.txt"
    env = dict(os.environ, PYTHONPATH=str(stub), SR_RDC=capture, SR_RDC_OUT=str(report))
    proc = subprocess.run([sys.executable, str(TOOL)], env=env, capture_output=True, text=True)
    return proc.returncode, report.read_text(encoding="utf-8").splitlines()


def test_profile_report_on_fake_frame(tmp_path):
    code, lines = run(tmp_path, "frame.rdc")
    assert code == 0  # źródło: ścieżka sukcesu kończy finish(0)
    # źródło: wszystko policzone ręcznie z drzewa w docstringu, suma 4.00 ms
    assert lines == [
        "capture: frame.rdc",
        "actions: 4  total GPU time: 4.00 ms",
        "",
        "by kind:",
        "      3.00 ms  75.0%       2  draw",      # 2+1 ms z 4 ms
        "      0.60 ms  15.0%       1  dispatch",
        "      0.40 ms  10.0%       1  clear",
        "",
        "by marker (depth 1):",
        "      3.60 ms  90.0%  Frame",             # Draw3+Draw4+Dispatch5
        "      0.40 ms  10.0%  (none)",            # Clear6 bez znacznika
        "",
        "by marker (depth 2):",
        "      3.00 ms  75.0%  Frame > Shadow",
        "      0.60 ms  15.0%  Frame",
        "      0.40 ms  10.0%  (none)",
        "",
        "top actions:",
        "    2.000 ms  eid      3  draw         Frame > Shadow :: Draw3",
        "    1.000 ms  eid      4  draw         Frame > Shadow :: Draw4",
        "    0.600 ms  eid      5  dispatch     Frame :: Dispatch5",
        "    0.400 ms  eid      6  clear         :: Clear6",   # pusta ścieżka
    ]


def test_missing_counter_fails(tmp_path):
    code, lines = run(tmp_path, "nocounter.rdc")
    # źródło: fałszywy EnumerateCounters zwraca pustą listę, narzędzie kończy kodem 1
    assert code == 1
    assert lines == ["EventGPUDuration counter not available"]
