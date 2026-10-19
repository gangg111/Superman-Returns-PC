"""tools/audio/convert_movies.py: frame count from WAV files written in the test
(no game files, no FFmpeg). Input wav_frames expects: a RIFF/WAVE file of 16-bit
PCM and its channel count; chunks before "data" may have any size (odd sizes are
padded to even, as RIFF requires)."""
import shutil
import struct
import wave

import pytest
from pathlib import Path

import convert_movies
import ea_ast

ROOT = Path(__file__).resolve().parents[2]
GAME_DATA = ROOT.parent / "SupermanReturnsPC" / "game" / "DATA"


def test_wav_frames_plain_header(tmp_path):
    path = tmp_path / "plain.wav"
    with wave.open(str(path), "wb") as w:  # źródło: zapis przez bibliotekę standardową, 10 ramek
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"\0\0" * 2 * 10)
    assert convert_movies.wav_frames(str(path), 2) == 10  # źródło: zapisane 10 ramek


def test_wav_frames_extensible_header_and_odd_chunk(tmp_path):
    fmt = struct.pack("<HHIIHH", 0xFFFE, 6, 44100, 44100 * 12, 12, 16) + b"\0" * 24  # 40-byte EXTENSIBLE
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"LIST" + struct.pack("<I", 3) + b"abc" + b"\0"          # odd chunk + pad byte
    body += b"data" + struct.pack("<I", 7 * 6 * 2) + b"\0" * 84       # 7 frames x 6 ch x 2 B
    path = tmp_path / "ext.wav"
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    assert convert_movies.wav_frames(str(path), 6) == 7  # źródło: 84 / (6 * 2) policzone ręcznie


def test_wav_frames_without_data_chunk(tmp_path):
    path = tmp_path / "nodata.wav"
    path.write_bytes(b"RIFF" + struct.pack("<I", 12) + b"WAVE" + b"JUNK" + struct.pack("<I", 0))
    with pytest.raises(ValueError):
        convert_movies.wav_frames(str(path), 2)


@pytest.mark.skipif(not (GAME_DATA / "fmvlegal.AST").exists() or not shutil.which("ffprobe"),
                    reason="brak fmvlegal.AST z gry albo ffprobe")
def test_real_movie_probe():
    arc = ea_ast.Bgfa(str(GAME_DATA / "fmvlegal.AST"))
    asf = [arc.read(e) for e in arc.entries]
    asf = [b for b in asf if b.startswith(bytes.fromhex("3026B2758E66CF11"))]
    assert asf
    info = convert_movies.probe(asf[0])
    # źródło: pomiar opisany w docstringu narzędzia (wszystkie filmy 44,1 kHz, WMA)
    assert int(info["sample_rate"]) == 44100 and info["codec_name"].startswith("wma")
