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
# libopus returns packet-aligned PCM in every mode. Native FFmpeg Opus can
# delay SILK samples across frames, whose PTS still comes from the new packet.
# Keep the reference decoder static, without a new target runtime dependency.
opus_archive="$cache/opus-1.5.2.tar.gz"
if [ ! -f "$opus_archive" ]; then
    curl -fL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz -o "$opus_archive"
fi
echo "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1  $opus_archive" | sha256sum -c -
if [ ! -d "$cache/opus-1.5.2" ]; then tar -xf "$opus_archive" -C "$cache"; fi
mkdir -p "$prefix/opus/build"
cd "$prefix/opus/build"
set -- --prefix="$prefix/opus" --disable-shared --enable-static --with-pic \
    --disable-doc --disable-extra-programs --disable-asm --disable-intrinsics \
    --disable-rtcd --disable-dred --disable-deep-plc --disable-osce
if [ -n "$cross" ]; then
    set -- "$@" --host="${cross%-}"
    export CC="${cross}gcc" AR="${cross}ar" RANLIB="${cross}ranlib"
    export CFLAGS="${CFLAGS:-} -Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
    if [ -n "$sysroot" ]; then
        export CFLAGS="$CFLAGS --sysroot=$sysroot"
        export LDFLAGS="${LDFLAGS:-} --sysroot=$sysroot"
    fi
fi
"$cache/opus-1.5.2/configure" "$@" > configure-output.txt 2>&1
make -j"${JOBS:-4}" > build-output.txt 2>&1
make install > install-output.txt 2>&1
cp config.h "$prefix/opus/"
cp "$cache/opus-1.5.2/COPYING" "$prefix/opus/"
export PKG_CONFIG_LIBDIR="$prefix/opus/lib/pkgconfig${PKG_CONFIG_LIBDIR:+:$PKG_CONFIG_LIBDIR}"
build="$prefix/build"
mkdir -p "$build"
cd "$build"
set -- --prefix="$prefix" --disable-everything --disable-autodetect \
    --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-shared --enable-static --enable-small --enable-pic \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
    --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
    --enable-demuxer=mov --enable-libopus --enable-decoder=libopus --enable-protocol=file \
    --pkg-config-flags=--static
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
