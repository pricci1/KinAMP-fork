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
export PKG_CONFIG_LIBDIR="$ffmpeg/lib/pkgconfig:$ffmpeg/opus/lib/pkgconfig"
"${cross}g++" ${CXXFLAGS:-} -Os -std=c++11 -I"$source" "$source/tests/opus_pcm.cpp" "$source/opus_mp4.cpp" \
    $(pkg-config --cflags --libs --static libavformat libavcodec libswresample libavutil) ${LDFLAGS:-} -o "$out/opus-pcm"
"${cross}g++" ${CXXFLAGS:-} -Os -std=c++11 -Wall -Wextra "$source/tests/opus_timing.cpp" \
    $(pkg-config --cflags --libs --static libavformat libavcodec libavutil opus) ${LDFLAGS:-} -o "$out/opus-timing"
for file in mp4read audio unicode_support; do
    "${cross}gcc" ${CFLAGS:-} -Os -I"$upstream" -I"$upstream/mpeg4" -c "$upstream/mpeg4/$file.c" -o "$out/$file.o"
done
"${cross}g++" ${CXXFLAGS:-} -Os -I"$upstream" "$source/tests/aac_probe.cpp" \
    "$out/mp4read.o" "$out/audio.o" "$out/unicode_support.o" ${LDFLAGS:-} -lfaad -o "$out/aac-probe"
