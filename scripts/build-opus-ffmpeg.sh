#!/bin/sh
# Restricted static FFmpeg for the isolated KinAMP prototype (2026-10-12).
# Usage: build-opus-ffmpeg.sh PREFIX [CROSS_PREFIX [SYSROOT]]
set -eu
prefix=$(realpath -m "$1")
cross=${2:-}
sysroot=${3:-}
cache=${KINAMP_BUILD_CACHE:-$(dirname "$prefix")/sources}
mkdir -p "$cache" "$prefix"
cache=$(realpath "$cache")
archive="$cache/ffmpeg-7.1.1.tar.xz"
if [ ! -f "$archive" ]; then
    curl -fL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz -o "$archive"
fi
echo "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1  $archive" | sha256sum -c -
if [ ! -d "$cache/ffmpeg-7.1.1" ]; then tar -xf "$archive" -C "$cache"; fi
build="$prefix/build"
mkdir -p "$build"
cd "$build"
set -- --prefix="$prefix" --disable-everything --disable-autodetect \
    --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-shared --enable-static --enable-small --enable-pic \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
    --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
    --enable-demuxer=mov --enable-decoder=opus --enable-protocol=file
if [ -n "$cross" ]; then
    # ARMv7 VFPv3-D16 hard-float, deliberately no NEON requirement.
    set -- "$@" --enable-cross-compile --cross-prefix="$cross" --arch=arm \
        --target-os=linux --cpu=armv7-a --disable-neon \
        '--extra-cflags=-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard' \
        '--extra-ldflags=-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard'
    if [ -n "$sysroot" ]; then set -- "$@" --sysroot="$sysroot"; fi
fi
"$cache/ffmpeg-7.1.1/configure" "$@" > configure-output.txt
make -j"${JOBS:-4}" > build-output.txt 2>&1
make install > install-output.txt
cp ffbuild/config.mak config.h "$prefix/"
mkdir -p "$prefix/notices"
cp "$cache/ffmpeg-7.1.1/"COPYING.* "$cache/ffmpeg-7.1.1/LICENSE.md" "$prefix/notices/"
echo "Restricted FFmpeg installed in $prefix"
