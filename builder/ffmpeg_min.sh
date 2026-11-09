#!/bin/bash
# The builder's FFmpeg (as t3native/builder/ffmpeg_min.sh): LGPL (no --enable-gpl), shared
# libraries only (the builder loads them with LoadLibrary), nothing but what the native audio
# conversion asks of it (tools/audio/convert_audio.py, convert_movies.py):
#   - the game's sounds: XMA2 in a RIFF wrapper (demuxer wav, decoders xma1 xma2)
#   - the movies' sound: ASF (demuxer asf, decoders wmapro wmav1 wmav2)
# Samples are turned to 16-bit by the builder itself (no libswresample).
# Run in MSYS2's MINGW64 shell, an out-of-tree build (a path without spaces):
#   bash ffmpeg_min.sh <FFmpeg source> <build dir> <prefix>
set -euo pipefail
SRC=$1
BUILD=$2
PREFIX=$3
export MSYSTEM=MINGW64
export PATH=/mingw64/bin:/usr/bin:$PATH
export TMPDIR=/tmp TMP=/tmp TEMP=/tmp
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" \
  --enable-shared --disable-static --enable-small \
  --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug \
  --disable-programs --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --enable-demuxer=wav,asf \
  --enable-decoder=xma1,xma2,wmapro,wmav1,wmav2
make -j"$(nproc)"
make install
cd "$PREFIX/bin"
for dll in $(ldd ./*.dll | awk '$3 ~ /^\/mingw64\// {print $3}' | sort -u); do cp -u "$dll" .; done
strip ./*.dll
mkdir -p licenses
cp "$SRC/COPYING.LGPLv2.1" "$SRC/LICENSE.md" licenses/
for f in /mingw64/share/licenses/winpthreads/*; do [ -e "$f" ] && cp "$f" "licenses/winpthreads-$(basename "$f")"; done
ls -la
