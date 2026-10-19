"""prepare_vulkan_emitter.prepare(root): wejście to drzewo <root>/.tools/xenosrecomp
(src/ = repo git na rewizji PIN z XenosRecomp/shader_recompiler.cpp, deps/fmt, deps/xxHash,
opcjonalny stempel src/.sr_pin) i łatki <root>/tools/shaders/xenosrecomp/patches/*.patch.
Wyjście: kopia w <root>/build/vulkan-m2/emitter-tree z nałożonymi łatkami 0011 i 0014
oraz abi-patch.sha256 = sha256(0011 + 0014). Całe drzewo budujemy w tmp_path; PIN podmieniamy
na HEAD lokalnego repo testowego (prawdziwej rewizji nie da się zrobić ręcznie)."""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "shaders"))
import prepare_vulkan_emitter as pve  # noqa: E402

needs_git = pytest.mark.skipif(shutil.which("git") is None, reason="wymaga git w PATH")

P0011 = (b"diff --git a/XenosRecomp/shader_recompiler.cpp b/XenosRecomp/shader_recompiler.cpp\n"
         b"--- a/XenosRecomp/shader_recompiler.cpp\n+++ b/XenosRecomp/shader_recompiler.cpp\n"
         b"@@ -1 +1,2 @@\n line1\n+binding\n")
P0014 = (b"diff --git a/XenosRecomp/tfetch3d.h b/XenosRecomp/tfetch3d.h\nnew file mode 100644\n"
         b"--- /dev/null\n+++ b/XenosRecomp/tfetch3d.h\n@@ -0,0 +1 @@\n+stacked\n")
P0001 = b"nie nakladana przez prepare\n"


def make_tree(root, monkeypatch, source_text=b"line1\n"):
    # globalne core.autocrlf=true na tej maszynie zamienia LF na CRLF przy git apply;
    # wyłączamy je dla procesów git z testu, żeby porównywać dokładne bajty
    monkeypatch.setenv("GIT_CONFIG_COUNT", "1")
    monkeypatch.setenv("GIT_CONFIG_KEY_0", "core.autocrlf")
    monkeypatch.setenv("GIT_CONFIG_VALUE_0", "false")
    src = root / ".tools/xenosrecomp/src"
    (src / "XenosRecomp").mkdir(parents=True)
    (src / "XenosRecomp/shader_recompiler.cpp").write_bytes(source_text)
    for dep, name in (("fmt", "format.h"), ("xxHash", "xxhash.h")):
        (root / ".tools/xenosrecomp/deps" / dep / "test").mkdir(parents=True)
        (root / ".tools/xenosrecomp/deps" / dep / name).write_bytes(b"//" + dep.encode())
        (root / ".tools/xenosrecomp/deps" / dep / "test/t.c").write_bytes(b"pomijane")
    git = ["git", "-C", str(src), "-c", "user.name=t", "-c", "user.email=t@t", "-c", "core.autocrlf=false"]
    subprocess.run([*git, "init", "-q"], check=True)
    subprocess.run([*git, "add", "-A"], check=True)
    subprocess.run([*git, "commit", "-qm", "pin"], check=True)
    head = subprocess.run([*git, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    monkeypatch.setattr(pve, "PIN", head)
    patches = root / "tools/shaders/xenosrecomp/patches"
    patches.mkdir(parents=True)
    for name, data in (("0001-other.patch", P0001), ("0011-vulkan-buffer-bindings.patch", P0011),
                       ("0014-stacked-tfetch3d.patch", P0014)):
        (patches / name).write_bytes(data)
    return root / "build/vulkan-m2/emitter-tree"


def expected_fingerprint():
    h = hashlib.sha256()  # źródło: sha256 liczone przyrostowo na bajtach łatek 0011 i 0014
    h.update(P0011)
    h.update(P0014)
    return h.hexdigest()


@needs_git
def test_copies_tree_applies_abi_patches_and_stamps(tmp_path, monkeypatch):
    target = make_tree(tmp_path, monkeypatch)
    assert pve.prepare(tmp_path) == target
    src = target / "src/XenosRecomp"
    assert (src / "shader_recompiler.cpp").read_bytes() == b"line1\nbinding\n"  # źródło: łatka 0011 ręcznie
    assert (src / "tfetch3d.h").read_bytes() == b"stacked\n"                    # źródło: łatka 0014 ręcznie
    assert (target / "deps/fmt/format.h").read_bytes() == b"//fmt"              # źródło: make_tree
    assert not (target / "deps/fmt/test").exists()                             # źródło: wzorzec ignore 'test'
    assert (target / "abi-patch.sha256").read_text() == expected_fingerprint()
    # drugi przebieg: 0014 już jest w kopii, więc działa ścieżka --reverse --check bez błędu
    pve.prepare(tmp_path)
    assert (src / "shader_recompiler.cpp").read_bytes() == b"line1\nbinding\n"  # źródło: jw.
    # źródło: oryginalne źródło nietknięte (zasada narzędzia "source preserved")
    assert (tmp_path / ".tools/xenosrecomp/src/XenosRecomp/shader_recompiler.cpp").read_bytes() == b"line1\n"


@needs_git
def test_stamped_source_is_not_patched_again(tmp_path, monkeypatch):
    target = make_tree(tmp_path, monkeypatch)
    series = [(b"0001-other.patch", P0001), (b"0011-vulkan-buffer-bindings.patch", P0011),
              (b"0014-stacked-tfetch3d.patch", P0014)]
    # źródło: definicja odcisku serii z fetch_xenosrecomp (nazwa\0bajty\0 po kolei), liczona tu od nowa
    fingerprint = hashlib.sha256(b"".join(n + b"\0" + d + b"\0" for n, d in series)).hexdigest()
    stamp = "PIN\n" + "\n".join(n.decode() for n, _ in series) + "\n" + fingerprint
    (tmp_path / ".tools/xenosrecomp/src/.sr_pin").write_text(stamp)
    pve.prepare(tmp_path)
    # źródło: stempel mówi, że źródło już ma łatki, więc kopia = źródło bez ponownego nakładania
    assert (target / "src/XenosRecomp/shader_recompiler.cpp").read_bytes() == b"line1\n"
    assert not (target / "src/XenosRecomp/tfetch3d.h").exists()


@needs_git
def test_conflicting_patch_raises_and_names_it(tmp_path, monkeypatch):
    make_tree(tmp_path, monkeypatch, source_text=b"zupelnie inna tresc\n")
    with pytest.raises(RuntimeError, match="0011-vulkan-buffer-bindings.patch"):  # źródło: kontekst 'line1' nie pasuje
        pve.prepare(tmp_path)


def test_wrong_revision_stops_before_copying(tmp_path, monkeypatch):
    # bez gita: rev-parse zwraca inną rewizję niż PIN
    class Done:
        returncode, stdout, stderr = 0, "0" * 40 + "\n", ""
    monkeypatch.setattr(pve.subprocess, "run", lambda *a, **k: Done())
    with pytest.raises(RuntimeError, match="Unexpected emitter source revision"):  # źródło: rewizja != PIN
        pve.prepare(tmp_path)
    assert not (tmp_path / "build").exists()  # źródło: nic nie zostało skopiowane
