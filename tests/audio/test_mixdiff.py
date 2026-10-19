"""tools/audio/mixdiff.py on raw captures written in the test (no game files).

Input the tool expects: --audio_dump_raw captures, frames of 256 samples x 6
channels, big-endian float32, channel-sequential inside a frame (all 256 samples
of channel 0, then channel 1, ...), 48 kHz."""
from pathlib import Path
import subprocess
import sys

import numpy as np

import mixdiff

ROOT = Path(__file__).resolve().parents[2]
AUDIO_TOOLS = ROOT / "tools" / "audio"


def write_raw(path, pcm):
    """pcm: samples x 6 (samples a multiple of 256) -> frame / channel / sample, >f4."""
    frames = pcm.shape[0] // 256
    pcm[:frames * 256].reshape(frames, 256, 6).transpose(0, 2, 1).astype(">f4").tofile(path)


def test_load_deinterleaves_channel_sequential_frames(tmp_path):
    raw = np.arange(2 * 6 * 256, dtype=">f4")  # value = position in the file
    path = tmp_path / "a.raw"
    raw.tofile(path)
    pcm = mixdiff.load(str(path))
    assert pcm.shape == (512, 6)  # źródło: 2 ramki x 256 próbek
    # źródło: policzone ręcznie: pozycja = ramka * 1536 + kanał * 256 + próbka w ramce
    assert pcm[0, 0] == 0 and pcm[1, 0] == 1 and pcm[0, 1] == 256
    assert pcm[300, 2] == 1536 + 2 * 256 + 44
    assert pcm[511, 5] == 1536 + 5 * 256 + 255


def test_band_energy_of_bin_exact_sine():
    t = np.arange(4800) / 48000
    pcm = np.zeros((4800, 6))
    pcm[:, 0] = np.sin(2 * np.pi * 1000 * t)  # 1 kHz, amplitude 1, one 100 ms window
    e = mixdiff.band_energy(pcm)
    assert e.shape == (1, 3)
    # źródło: Parseval + suma okna Hanna do kwadratu = 3N/8: energia = 3 N^2 / 32 = 2 160 000
    assert abs(e[0, 1] - 10 * np.log10(3 * 4800 ** 2 / 32)) < 0.05
    assert e[0, 0] < e[0, 1] - 60 and e[0, 2] < e[0, 1] - 60  # źródło: 1 kHz leży w paśmie 300-4000 Hz


def test_main_alignment_and_level_difference(tmp_path):
    on = np.random.default_rng(1).random(120) < 0.5          # 12 s of 100 ms windows, burst pattern
    t = np.arange(120 * 4800) / 48000
    a = np.zeros((120 * 4800, 6))
    a[:, 0] = np.sin(2 * np.pi * 1000 * t) * np.repeat(on, 4800)
    # same mix, 1 s later, twice the amplitude; + 128 zeros so that 13 s (624000 samples)
    # fill whole 256-sample frames (624128 = 2438 * 256) and the last window is not cut
    b = np.vstack([np.zeros((48000, 6)), 2 * a, np.zeros((128, 6))])
    write_raw(tmp_path / "a.raw", a)
    write_raw(tmp_path / "b.raw", b)
    proc = subprocess.run([sys.executable, str(AUDIO_TOOLS / "mixdiff.py"),
                           str(tmp_path / "a.raw"), str(tmp_path / "b.raw")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    # źródło: b opóźnione o 1 s względem a (wpisane wyżej); przesunięcie b wzgl. a jest ujemne
    assert "alignment shift: -1.0 s" in proc.stdout
    # źródło: 20 * log10(2) = 6,02 dB (podwójna amplituda) w paśmie 300-4000 Hz
    assert "band   300- 4000 Hz: median |dB diff|  6.02, p95  6.02, max  6.02" in proc.stdout
    assert f"compared windows: {int(on.sum())} " in proc.stdout  # źródło: tylko okna z sygnałem
