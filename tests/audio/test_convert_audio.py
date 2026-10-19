"""tools/audio/convert_audio.py: WAV writing, FNV-1a keys, EAAC block lists and the
archive walk on small hand-built inputs (no game files, no FFmpeg).

Inputs the tool expects: int16 PCM (frames x channels) for wav_bytes; "rbas" sound
records with an SNR header and EAAC blocks; a DATA folder with BGFA *.AST archives."""
import struct
import wave

import numpy as np
import pytest
from pathlib import Path

import convert_audio
import ea_ast

ROOT = Path(__file__).resolve().parents[2]
GAME_DATA = ROOT.parent / "SupermanReturnsPC" / "game" / "DATA"

SNR_RAM = struct.pack(">II", 0x0304AC44, 0x00000080)       # codec 3, 2 ch, 44100 Hz, RAM, 128 samples
SNR_STREAM = struct.pack(">II", 0x0304AC44, 0x40000080)    # same, kind 1 = streamed (_SNS entry)
PROPS = [("numc", 0, struct.pack("<I", 1))]                # 20 bytes from 8 -> SNR at 32


def fnv1a_shifts(data):
    """FNV-1a 32 written as in the FNV reference C code (multiply by 16777619 as shifts)."""
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h + (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24)) & 0xFFFFFFFF
    return h


def test_fnv1a_known_vectors():
    # źródło: wektory testowe FNV-1a 32 z draft-eastlake-fnv / isthe.com
    assert convert_audio.fnv1a(b"") == 0x811C9DC5
    assert convert_audio.fnv1a(b"a") == 0xE40C292C
    assert convert_audio.fnv1a(b"foobar") == 0xBF9CF968
    assert fnv1a_shifts(b"foobar") == 0xBF9CF968  # kontrola wzoru referencyjnego


def test_wav_bytes_read_back_with_stdlib_wave(tmp_path):
    pcm = np.array([[1, -1], [2, -2], [300, -300]], dtype=np.int16)
    raw = convert_audio.wav_bytes(pcm, 44100, loop_start=1, loop_end=2)
    path = tmp_path / "x.wav"
    path.write_bytes(raw)
    with wave.open(str(path)) as w:  # źródło: niezależny parser RIFF z biblioteki standardowej
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) == (2, 2, 44100, 3)
        assert w.readframes(3) == struct.pack("<6h", 1, -1, 2, -2, 300, -300)
    # źródło: policzone ręcznie: RIFF 12 + fmt 24 + data 8+12 -> smpl przy 56, pętla przy 100
    assert struct.unpack_from("<I", raw, 4)[0] == len(raw) - 8
    assert raw[56:64] == b"smpl" + struct.pack("<I", 60)
    assert struct.unpack_from("<I", raw, 72)[0] == 22676    # źródło: 1e9 / 44100 = 22675,7 ns
    assert struct.unpack_from("<I", raw, 92)[0] == 1        # źródło: jedna pętla
    assert struct.unpack_from("<II", raw, 108) == (1, 2)    # źródło: początek i koniec pętli z wywołania


def test_keys_snr16_and_fnv256(make_sabr):
    payload = bytes(range(256)) * 2
    body = make_sabr(PROPS, SNR_RAM, payload)
    k = convert_audio.keys(body)
    tail = body[32:32 + 256]  # źródło: SNR na 32 (8 + 20 = 28, w górę do 16)
    assert k["snr16"] == (SNR_RAM + payload[:8]).hex()      # źródło: bajty wpisane wyżej
    assert k["tail"] == 8 + 512                              # źródło: długość od SNR do końca
    assert k["fnv256"] == f"{fnv1a_shifts(tail):08X}"        # źródło: FNV-1a wzorem z przesunięciami
    short = convert_audio.keys(make_sabr(PROPS, SNR_RAM, b"\0" * 10))
    assert short["fnv256"] is None and short["tail"] == 18   # źródło: 8 + 10 < 256


def test_blocks_of_ram_and_streamed(make_sabr, make_block):
    blocks = make_block(100, [b"\x11" * 4]) + make_block(28, [b"\x22" * 4], last=True)
    assert convert_audio.blocks_of(make_sabr(PROPS, SNR_RAM, blocks), None) == [100, 28]  # źródło: wpisane
    streamed = make_sabr(PROPS, SNR_STREAM)
    assert convert_audio.blocks_of(streamed, blocks) == [100, 28]  # źródło: bloki z wpisu _SNS
    assert convert_audio.blocks_of(streamed, None) == []


def test_jobs_for_walks_nested_archives(tmp_path, make_sabr, make_bgfa, make_prop, make_block):
    snd1 = make_sabr(PROPS, SNR_STREAM)
    sns1 = make_block(128, [b"\x01" * 4], last=True)
    snd2 = make_sabr(PROPS, SNR_RAM, make_block(128, [b"\x02" * 4], last=True))
    lists = make_prop("numc", 0, struct.pack("<I", 1))
    bank = b"rbas" + b"\0" * 4 + struct.pack("<III", 1, 0, 0) + struct.pack("<III", 0, 0, len(lists)) + lists + b"V\0"
    inner = make_bgfa([("SND2", snd2)])
    outer = make_bgfa([("SND1", snd1), ("SND1_SNS", sns1), ("BANK", bank), ("TXT", b"text"), ("INNER", inner)])
    (tmp_path / "a.AST").write_bytes(outer)
    (tmp_path / "notes.txt").write_bytes(b"not an archive")
    jobs, banks, skipped = convert_audio.jobs_for(str(tmp_path))
    # źródło: zawartość archiwów zbudowana ręcznie wyżej (2 dźwięki, 1 bank, nic nieczytelnego)
    assert [(j[0], j[1]) for j in jobs] == [("a.AST", "SND1"), ("a.AST/INNER", "SND2")]
    assert jobs[0][2] == snd1 and jobs[0][3] == sns1 and jobs[1][3] is None
    assert banks == 1 and skipped == []
    only, _, _ = convert_audio.jobs_for(str(tmp_path), only="SND2")
    assert [j[1] for j in only] == ["SND2"]


def test_convert_writes_wav_and_manifest_item(tmp_path, monkeypatch, make_sabr, make_block):
    body = make_sabr(PROPS, SNR_RAM, make_block(128, [b"\x02" * 4], last=True))
    pcm = np.zeros((128, 2), dtype=np.int16)
    # FFmpeg is replaced: the decoder returns 128 frames x 2 channels, as the SNR header says
    monkeypatch.setattr(ea_ast, "decode_sound", lambda b, s: (ea_ast.parse_sabr(b), pcm, 128))
    item = convert_audio.convert(("a.AST/INNER", "SND2", body, None), str(tmp_path))
    assert item["file"] == "a.AST/INNER/SND2.wav"  # źródło: <archiwum>/<NAZWA>.wav z opisu narzędzia
    assert (item["channels"], item["rate"], item["samples"], item["exact"]) == (2, 44100, 128, True)  # źródło: SNR_RAM
    assert item["blocks"] == [128]
    with wave.open(str(tmp_path / "a.AST" / "INNER" / "SND2.wav")) as w:
        assert w.getnframes() == 128


@pytest.mark.skipif(not (GAME_DATA / "sound.AST").exists(), reason="brak danych gry (sound.AST)")
def test_real_data_jobs_have_keys():
    arc = ea_ast.Bgfa(str(GAME_DATA / "sound.AST"))
    found = 0
    for e in arc.entries[:100]:
        try:
            body = arc.read(e)
        except NotImplementedError:
            continue
        if body[:4] == b"rbas" and not ea_ast.is_bank(body):
            k = convert_audio.keys(body)
            # źródło: odczyt z gry 2026-10-19: rekord strumieniowy kończy się na 8-bajtowym SNR,
            # więc snr16 ma tyle bajtów, ile zostało do końca rekordu (najwyżej 16)
            assert len(k["snr16"]) == 2 * min(16, k["tail"])
            assert k["snr16"][:2] == "03"  # źródło: wersja 0, kodek 3 (EA-XMA) w pierwszym bajcie
            found += 1
    assert found > 0
