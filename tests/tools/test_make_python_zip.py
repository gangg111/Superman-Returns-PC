"""make_python_zip.py: wejście to katalog Lib/ biblioteki standardowej Pythona
(drzewo plików .py), wyjście to .zip z samymi źródłami .py, bez pakietów z SKIP.
Tu Lib/ to małe drzewo zbudowane w tmp_path."""
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import make_python_zip  # noqa: E402


def build_lib(lib: Path):
    files = {
        "os.py": "x = 1\n",
        "test.py": "kept = True\n",          # plik o nazwie 'test.py' nie jest katalogiem test/
        "json/__init__.py": "",
        "json/decoder.py": "d = 2\n",
        "json/notes.txt": "nie .py\n",
        "test/test_os.py": "skip\n",
        "json/tests/test_json.py": "skip\n",
        "tkinter/__init__.py": "skip\n",
        "encodings/__pycache__/cached.py": "skip\n",
        "site-packages/pkg/mod.py": "skip\n",
    }
    for name, text in files.items():
        path = lib / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode())  # bajty, bez zamiany \n na \r\n przez Windows


def test_only_needed_py_sources_are_zipped(tmp_path):
    lib, out = tmp_path / "Lib", tmp_path / "python.zip"
    build_lib(lib)
    assert make_python_zip.main(lib, out) == 0
    with zipfile.ZipFile(out) as archive:
        names = sorted(archive.namelist())
        infos = archive.infolist()
        decoder = archive.read("json/decoder.py")
    # źródło: policzone ręcznie z drzewa build_lib (odrzucone: .txt, test/, tests/, tkinter/,
    # __pycache__/, site-packages/; zostaje test.py w korzeniu, bo liczą się tylko katalogi)
    assert names == ["json/__init__.py", "json/decoder.py", "os.py", "test.py"]
    # źródło: treść zapisana wyżej w build_lib
    assert decoder == b"d = 2\n"
    # źródło: docstring narzędzia (archiwum deflate), sprawdzone przez zipfile.ZipInfo
    assert all(info.compress_type == zipfile.ZIP_DEFLATED for info in infos)


def test_empty_lib_reports_failure(tmp_path):
    lib = tmp_path / "Lib"
    lib.mkdir()
    (lib / "README.txt").write_text("brak .py")
    # źródło: kod wyjścia z założenia narzędzia (0 modułów = błąd 1), policzone ręcznie
    assert make_python_zip.main(lib, tmp_path / "empty.zip") == 1
