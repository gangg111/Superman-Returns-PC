"""Binary index of the converted sounds for the native audio engine
(port/src/native_audio/sample_bank.cpp reads it; no JSON parser in the port).

usage: build_index.py <native_audio dir>   (reads manifest.json, writes index.bin)

Layout, little endian:
  "SRAI" u32 version=2 u32 count
  per sound: u32 fnv256 (0 = none) u8 has_fnv u8 channels u8 streamed u8 loop
             u32 rate u32 samples u32 loop_start
             u8[16] snr16 (first 16 bytes of the SNR header, zero padded)
             u16 len + utf-8 file path (relative to the dir)
             u16 len + utf-8 "archive/NAME"
             u32 block count + u32 frames per block (EAAC data blocks)
"""
import json
import os
import struct
import sys


def main():
    root = sys.argv[1]
    manifest = json.load(open(os.path.join(root, "manifest.json"), encoding="utf-8"))
    sounds = [s for s in manifest["sounds"] if s.get("file") and s.get("exact")]
    out = bytearray(b"SRAI" + struct.pack("<II", 2, len(sounds)))
    for s in sounds:
        fnv = s.get("fnv256")
        snr = bytes.fromhex(s.get("snr16", ""))[:16].ljust(16, b"\0")
        out += struct.pack("<IBBBBIII", int(fnv, 16) if fnv else 0, 1 if fnv else 0,
                           s["channels"], 1 if s["streamed"] else 0, 1 if s["loop"] else 0,
                           s["rate"], s["samples"], s["loop_start"])
        out += snr
        for text in (s["file"], s["archive"] + "/" + s["name"]):
            b = text.encode("utf-8")
            out += struct.pack("<H", len(b)) + b
        blocks = s.get("blocks", [])
        out += struct.pack("<I", len(blocks)) + struct.pack(f"<{len(blocks)}I", *blocks)
    with open(os.path.join(root, "index.bin"), "wb") as f:
        f.write(out)
    print(f"index.bin: {len(sounds)} sounds, {len(out)} bytes")


if __name__ == "__main__":
    main()
