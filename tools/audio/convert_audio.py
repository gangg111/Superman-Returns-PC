"""Convert all of Superman Returns' sounds to PCM for the native audio engine.

Walks every BGFA archive in the game's DATA folder (and BGFA archives nested
inside them), decodes every single-sound record (EA "sabr" + SNR header,
EA-XMA) with FFmpeg, and writes:
  <out>/<archive path>/<NAME>.wav   16-bit PCM, 44.1 kHz, the record's channel
                                    count (4 = front pair + rear pair), with a
                                    RIFF "smpl" loop when the record loops
  <out>/manifest.json               one entry per sound: source, properties,
                                    channels, rate, samples, loop, result

Every sound is checked: the decoded length must equal the SNR header's sample
count (criterion 2 of docs/natywny-dzwiek-plan.md). Bank records (variant
lists) carry no audio and are listed in the manifest only by count.

usage: convert_audio.py <game DATA dir> <out dir> [--jobs N] [--only NAME]
"""
import argparse
import json
import os
import struct
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ea_ast  # noqa: E402


def wav_bytes(pcm, rate, loop_start=None, loop_end=None):
    frames, channels = pcm.shape
    data = pcm.astype("<i2").tobytes()
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(data)) + data
    if loop_start is not None:
        smpl = struct.pack("<9I", 0, 0, round(1e9 / rate), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack("<6I", 0, 0, loop_start, loop_end, 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


def walk(path, data=None, prefix=""):
    """Yield (archive label, Bgfa) for an archive and the BGFA archives nested in it."""
    arc = ea_ast.Bgfa(path, data=data)
    yield prefix, arc
    for e in arc.entries:
        try:
            body = arc.read(e)
        except NotImplementedError:
            continue
        if body[:4] == b"BGFA":
            yield from walk(path, data=body, prefix=f"{prefix}/{e.name}")


def jobs_for(data_dir, only=None):
    jobs, banks, skipped = [], 0, []
    for file in sorted(os.listdir(data_dir)):
        if not file.upper().endswith(".AST"):
            continue
        for label, arc in walk(os.path.join(data_dir, file), prefix=file):
            names = arc.by_name()
            for e in arc.entries:
                try:
                    body = arc.read(e)
                except NotImplementedError as ex:
                    skipped.append((label, e.name, str(ex)))
                    continue
                if body[:4] != b"rbas":
                    continue
                if ea_ast.is_bank(body):
                    banks += 1
                    continue
                if only and e.name != only:
                    continue
                sns_entry = names.get(e.name + "_SNS")
                sns = arc.read(sns_entry) if sns_entry else None
                jobs.append((label, e.name, body, sns))
    return jobs, banks, skipped


def convert(job, out_dir):
    label, name, body, sns = job
    item = {"archive": label, "name": name}
    try:
        rec, pcm, block_samples = ea_ast.decode_sound(body, sns)
        snr = rec.snr
        item.update(props=rec.props, channels=snr.channels, rate=snr.rate,
                    streamed=bool(snr.kind), samples=snr.samples, loop=snr.loop,
                    loop_start=snr.loop_start, decoded=int(pcm.shape[0]))
        item["exact"] = pcm.shape[0] == snr.samples and pcm.shape[1] == snr.channels
        folder = os.path.join(out_dir, *label.split("/"))
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, name + ".wav")
        loop = (snr.loop_start, snr.samples - 1) if snr.loop else (None, None)
        with open(path, "wb") as f:
            f.write(wav_bytes(pcm, snr.rate, *loop))
        item["file"] = os.path.relpath(path, out_dir).replace("\\", "/")
    except Exception as ex:  # recorded in the manifest, the run goes on
        item["error"] = f"{type(ex).__name__}: {ex}"[:300]
    return item


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--only")
    args = ap.parse_args()
    jobs, banks, skipped = jobs_for(args.data_dir, args.only)
    print(f"sounds: {len(jobs)}, banks (no audio): {banks}, unreadable entries: {len(skipped)}",
          flush=True)
    os.makedirs(args.out_dir, exist_ok=True)
    results = []
    with ProcessPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(convert, j, args.out_dir) for j in jobs]
        for i, fut in enumerate(as_completed(futures), 1):
            results.append(fut.result())
            if i % 250 == 0:
                print(f"{i}/{len(jobs)}", flush=True)
    results.sort(key=lambda r: (r["archive"], r["name"]))
    exact = sum(1 for r in results if r.get("exact"))
    errors = [r for r in results if "error" in r]
    inexact = [r for r in results if "exact" in r and not r["exact"]]
    manifest = {"sounds": results, "banks": banks,
                "skipped": [{"archive": a, "name": n, "reason": w} for a, n, w in skipped],
                "summary": {"sounds": len(results), "exact": exact, "inexact": len(inexact),
                            "errors": len(errors)}}
    with open(os.path.join(args.out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(json.dumps(manifest["summary"]))
    for r in (errors + inexact)[:10]:
        print("  ", r["archive"], r["name"], r.get("error") or (r.get("samples"), r.get("decoded")))
    return 0 if not errors and not inexact else 1


if __name__ == "__main__":
    sys.exit(main())
