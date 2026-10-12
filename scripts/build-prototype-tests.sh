#!/bin/sh
# Build offline test entry points. The AAC probe compiles UNMODIFIED sources.
# Usage: build-prototype-tests.sh OUTPUT_DIR FFMPEG_PREFIX UPSTREAM_DIR [CROSS_PREFIX]
set -eu
out=$(realpath -m "$1")
ffmpeg=$(realpath "$2")
upstream=$(realpath "$3")
cross=${4:-}
source=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
export PKG_CONFIG_LIBDIR="$ffmpeg/lib/pkgconfig"
"${cross}g++" -Os -std=c++11 -I"$source" "$source/tests/opus_pcm.cpp" "$source/opus_mp4.cpp" \
    $(pkg-config --cflags --libs --static libavformat libavcodec libswresample libavutil) -o "$out/opus-pcm"
for file in mp4read audio unicode_support; do
    "${cross}gcc" -Os -I"$upstream" -I"$upstream/mpeg4" -c "$upstream/mpeg4/$file.c" -o "$out/$file.o"
done
"${cross}g++" -Os -I"$upstream" "$source/tests/aac_probe.cpp" \
    "$out/mp4read.o" "$out/audio.o" "$out/unicode_support.o" -lfaad -o "$out/aac-probe"
