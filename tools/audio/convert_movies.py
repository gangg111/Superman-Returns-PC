"""Decode the audio of the game's movies (ASF in fmv.AST / fmvlegal.AST:
WMV3 video; audio measured: 28 x WMA Pro 5.1, 2 x WMA Pro stereo, 4 x WMA v2
stereo, all 44.1 kHz) to PCM for the native audio engine.

Writes <out>/movies/<archive>/<ENTRY>.wav (16-bit, the stream's channels and
rate) and <out>/movies/manifest.json. Each movie is checked: the decoded length
must match the container's audio duration within one WMA Pro frame (2048
samples) or, when the audio ends before the video (EA_ID_OLD: audio 5.0 s in a
6.0 s container), end within one ASF packet (0.5 s) after the last audio
packet's start. The 6-channel order is the one FFmpeg reports (WAVE mask).

usage: convert_movies.py <game DATA dir> <out dir>
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ea_ast  # noqa: E402


def probe(data):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=codec_name,sample_rate,channels,channel_layout,duration", "-of", "json", "-"],
        input=data, capture_output=True, check=True).stdout
    return json.loads(out)["streams"][0]


def last_audio_pts(data):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "packet=pts_time",
         "-of", "csv=p=0", "-"], input=data, capture_output=True, check=True).stdout
    times = [float(t) for t in out.decode().split() if t.strip()]
    return max(times) if times else 0.0


def wav_frames(path, channels):
    """Frame count from the data chunk (6-channel WAVs use WAVE_FORMAT_EXTENSIBLE,
    whose header is longer than 44 bytes)."""
    import struct
    d = open(path, "rb").read()
    p = 12
    while p + 8 <= len(d):
        tag, size = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if tag == b"data":
            return size // (2 * channels)
        p += 8 + size + (size & 1)
    raise ValueError("no data chunk")


def main():
    data_dir, out_dir = sys.argv[1], sys.argv[2]
    results = []
    for file in ("fmv.AST", "fmvlegal.AST"):
        arc = ea_ast.Bgfa(os.path.join(data_dir, file))
        for e in arc.entries:
            body = arc.read(e)
            if not body.startswith(bytes.fromhex("3026B2758E66CF11")):
                continue
            item = {"archive": file, "name": e.name}
            try:
                info = probe(body)
                folder = os.path.join(out_dir, "movies", file)
                os.makedirs(folder, exist_ok=True)
                path = os.path.join(folder, e.name + ".wav")
                proc = subprocess.run(
                    ["ffmpeg", "-v", "error", "-y", "-i", "pipe:0", "-map", "0:a:0",
                     "-c:a", "pcm_s16le", path], input=body, capture_output=True)
                if proc.returncode:
                    raise RuntimeError(proc.stderr.decode(errors="replace")[:200])
                rate, ch = int(info["sample_rate"]), int(info["channels"])
                frames = wav_frames(path, ch)
                expected = round(float(info["duration"]) * rate)
                last = last_audio_pts(body)
                ends_early = last <= frames / rate <= last + 0.5
                item.update(codec=info["codec_name"], rate=rate, channels=ch,
                            layout=info.get("channel_layout"), samples=frames,
                            expected=expected, last_packet=last,
                            exact=abs(frames - expected) <= 2048 or ends_early,
                            file=os.path.relpath(path, out_dir).replace("\\", "/"))
            except Exception as ex:
                item["error"] = f"{type(ex).__name__}: {ex}"[:300]
            results.append(item)
            print(item.get("name"), item.get("samples"), item.get("expected"), item.get("error", ""),
                  flush=True)
    ok = sum(1 for r in results if r.get("exact"))
    with open(os.path.join(out_dir, "movies", "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"movies": results, "summary": {"movies": len(results), "exact": ok}}, f, indent=1)
    print(f"movies: {len(results)}, within one frame: {ok}")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
