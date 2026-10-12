#!/bin/sh
# Compiles untouched upstream minimal sources, avoiding its unnecessary GTK
# configure prerequisite. Uses the same GStreamer 0.10/FAAD/glib as the patch.
# Usage: build-upstream-arm-control.sh UPSTREAM_DIR GST_PREFIX OUTPUT_DIR
set -eu
src=$(realpath "$1")
gst=$(realpath "$2")
out=$(realpath -m "$3")
mkdir -p "$out"
export PKG_CONFIG_LIBDIR="$gst/lib/pkgconfig:/usr/lib/arm-linux-gnueabihf/pkgconfig"
for file in mp4read audio unicode_support; do
    arm-linux-gnueabihf-gcc -Os -DNDEBUG -I"$src" -I"$src/mpeg4" \
        -c "$src/mpeg4/$file.c" -o "$out/$file.o"
done
arm-linux-gnueabihf-g++ -Os -DNDEBUG -std=c++11 \
    -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard \
    '-DKINAMP_VERSION="3.0.0"' '-DKINAMP_AUDIO_SINK="mixersink"' \
    -I"$src" -I"$src/mpeg4" "$src/cli_player.cpp" "$src/music_backend.cpp" "$src/icy_client.cpp" "$src/tags.cpp" \
    "$out/mp4read.o" "$out/audio.o" "$out/unicode_support.o" \
    $(pkg-config --cflags --libs gstreamer-0.10 glib-2.0 gobject-2.0) \
    -lfaad -ldl -lrt -pthread -o "$out/KinAMP-minimal"
