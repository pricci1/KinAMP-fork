#!/bin/sh
# Target-linked Paperwhite 6 prototype. Device libraries are PRIVATE inputs.
# Usage: build-kindle-candidate.sh PRIVATE_TARGET_ROOT OUTPUT_ROOT
# Requires Debian gcc/g++-arm-linux-gnueabihf 12, cmake, pkg-config, curl,
# make, xz, bzip2, gettext, qemu-user, glib-mkenums. No vendor libs distributed.
set -eu
target=$(realpath "$1")
out=$(realpath -m "$2")
source=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cache=${KINAMP_BUILD_CACHE:-$(dirname "$out")/sources}
mkdir -p "$out" "$cache"
cache=$(realpath "$cache")
fetch() {
    name=$1; hash=$2; url=$3
    if [ ! -f "$cache/$name" ]; then curl -fL "$url" -o "$cache/$name"; fi
    echo "$hash  $cache/$name" | sha256sum -c -
}
fetch glib-2.29.18.tar.xz fef675190f256bc2f0246750a08f605fa9a4a5c79bf6e022e28117613a4a81c1 \
    https://download.gnome.org/sources/glib/2.29/glib-2.29.18.tar.xz
fetch gstreamer-0.10.25.tar.bz2 39b2ba7b3bfa8df6d998a9461e7091c27757e36a53e93969d7d9982a56526578 \
    https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-0.10.25.tar.bz2
fetch faad2_2.10.1.orig.tar.gz 4c16c71295ca0cbf7c3dfe98eb11d8fa8d0ac3042e41604cfd6cc11a408cf264 \
    https://deb.debian.org/debian/pool/main/f/faad2/faad2_2.10.1.orig.tar.gz
fetch zlib-1.2.11.tar.gz c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1 \
    https://zlib.net/fossils/zlib-1.2.11.tar.gz
if [ "$(arm-linux-gnueabihf-g++ -dumpversion)" != 12 ]; then
    echo 'This measured recipe requires GCC 12; re-run ABI validation for another compiler.' >&2
    exit 1
fi
mkdir -p "$out/headers" "$out/glib-config" "$out/gst-config"
sysroot="$out/sysroot-private"
mkdir -p "$sysroot/usr/include" "$sysroot/usr/lib" "$sysroot/lib"
# GCC 12's C/C++ development ABI plus actual target runtimes. We validate every
# required symbol: the development headers are newer than target libc (2.35).
cp -a /usr/arm-linux-gnueabihf/include/. "$sysroot/usr/include/"
cp /usr/arm-linux-gnueabihf/lib/libc_nonshared.a "$sysroot/usr/lib/"
printf 'GROUP ( /lib/libc.so.6 /usr/lib/libc_nonshared.a AS_NEEDED ( /lib/ld-linux-armhf.so.3 ) )\n' \
    > "$sysroot/usr/lib/libc.so"
# No vendor binaries enter the public source/build output bundle.
cp -a "$target/lib/." "$sysroot/lib/"
cp -a "$target/usr/lib/." "$sysroot/usr/lib/"
cp "$target/mnt/us/KinAMP/libs_hf/libfaad.so.2" "$sysroot/usr/lib/"
# FFmpeg's configure prefers -latomic even when inlined atomics suffice. Keep
# it static rather than introduce a libatomic.so.1 absent from the inventory.
cp "$(arm-linux-gnueabihf-gcc -print-file-name=libatomic.a)" "$sysroot/usr/lib/"
for pair in 'faad:faad.so.2' 'lipc:lipc.so.1' 'glib-2.0:glib-2.0.so.0' \
    'gobject-2.0:gobject-2.0.so.0' 'gthread-2.0:gthread-2.0.so.0' \
    'gmodule-2.0:gmodule-2.0.so.0' 'gstreamer-0.10:gstreamer-0.10.so.0' \
    'stdc++:stdc++.so.6' 'z:z.so.1'; do
    ln -sf "lib${pair#*:}" "$sysroot/usr/lib/lib${pair%%:*}.so"
done
for name in dl rt pthread m gcc_s; do
    soname=lib$name.so
    case $name in gcc_s|rt) version=1;; pthread) version=0;; m) version=6;; dl) version=2;; esac
    ln -sf "../../lib/$soname.$version" "$sysroot/usr/lib/$soname"
done
tar -xf "$cache/glib-2.29.18.tar.xz" -C "$out/headers"
tar -xf "$cache/gstreamer-0.10.25.tar.bz2" -C "$out/headers"
tar -xf "$cache/faad2_2.10.1.orig.tar.gz" -C "$out/headers"
tar -xf "$cache/zlib-1.2.11.tar.gz" -C "$out/headers"
cp "$out/headers/zlib-1.2.11/zlib.h" "$out/headers/zlib-1.2.11/zconf.h" "$sysroot/usr/include/"
export CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++
export CFLAGS="--sysroot=$sysroot -isystem $sysroot/usr/include -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
export LDFLAGS="--sysroot=$sysroot -L$sysroot/usr/lib -Wl,-rpath-link,$sysroot/usr/lib -Wl,-rpath-link,$sysroot/lib"
export PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=
mkdir -p "$PKG_CONFIG_LIBDIR"
# Measure the non-guessable ARM pthread layout/initializer using actual libc.
$CC $CFLAGS $LDFLAGS "$source/scripts/kindle-glib-config.c" -o "$out/glib-config/probe"
qemu-arm -L "$target" -E LD_LIBRARY_PATH="$target/lib:$target/usr/lib" \
    "$out/glib-config/probe" > "$out/glib-config/measured.cache"
# Configure only: derive ARM public configuration headers, never replace GLib.
(cd "$out/glib-config" && \
    glib_cv_stack_grows=no glib_cv_uscore=no glib_cv_have_qsort_r=yes \
    glib_cv_long_long_format=ll glib_cv_monotonic_clock=yes \
    LIBFFI_CFLAGS="-I$sysroot/usr/include" LIBFFI_LIBS=-l:libffi.so.5 \
    "$out/headers/glib-2.29.18/configure" --host=arm-linux-gnueabihf \
    --cache-file="$out/glib-config/measured.cache" \
    --prefix="$sysroot/usr" --disable-nls --disable-gtk-doc --disable-selinux \
    --disable-fam --with-pcre=internal > configure.log 2>&1)
mkdir -p "$sysroot/usr/include/glib-2.0/glib" "$sysroot/usr/include/glib-2.0/gobject" \
    "$sysroot/usr/lib/glib-2.0/include"
cp "$out/headers/glib-2.29.18/glib/"*.h "$sysroot/usr/include/glib-2.0/glib/"
cp "$out/headers/glib-2.29.18/glib/glib.h" "$out/headers/glib-2.29.18/glib/glib-object.h" \
    "$out/headers/glib-2.29.18/gmodule/gmodule.h" \
    "$sysroot/usr/include/glib-2.0/"
cp "$out/headers/glib-2.29.18/gobject/"*.h "$sysroot/usr/include/glib-2.0/gobject/"
cp "$out/glib-config/glib/glibconfig.h" "$sysroot/usr/lib/glib-2.0/include/"
cp "$out/glib-config/"*.pc "$PKG_CONFIG_LIBDIR/"
# pkg-config otherwise prefers source-tree -uninstalled files over installed PC.
rm -f "$PKG_CONFIG_LIBDIR/"*-uninstalled.pc
# GStreamer's configure generates gstconfig.h/gstversion.h. No GST is rebuilt.
(cd "$out/gst-config" && \
    "$out/headers/gstreamer-0.10.25/configure" --host=arm-linux-gnueabihf \
    --prefix="$sysroot/usr" --disable-loadsave --disable-gtk-doc --disable-examples \
    --disable-tests --disable-nls > configure.log 2>&1)
make -C "$out/gst-config/gst" gstenumtypes.h gstmarshal.h > "$out/gst-config/headers.log" 2>&1
mkdir -p "$sysroot/usr/include/gstreamer-0.10/gst"
cp "$out/headers/gstreamer-0.10.25/gst/"*.h "$sysroot/usr/include/gstreamer-0.10/gst/"
cp "$out/gst-config/gst/gstconfig.h" "$out/gst-config/gst/gstversion.h" "$out/gst-config/gst/gstenumtypes.h" \
    "$out/gst-config/gst/gstmarshal.h" \
    "$sysroot/usr/include/gstreamer-0.10/gst/"
cp "$out/gst-config/pkgconfig/gstreamer.pc" "$PKG_CONFIG_LIBDIR/gstreamer-0.10.pc"
find "$out/headers" -name neaacdec.h -exec cp {} "$sysroot/usr/include/" \;
KINAMP_BUILD_CACHE="$cache" "$source/scripts/build-opus-ffmpeg.sh" \
    "$out/ffmpeg" arm-linux-gnueabihf- "$sysroot"
export PKG_CONFIG_LIBDIR="$out/ffmpeg/lib/pkgconfig:$sysroot/usr/lib/pkgconfig"
# FFmpeg's prefix is outside the sysroot: pkg-config must not prepend it again.
export PKG_CONFIG_SYSROOT_DIR=
cmake --fresh -S "$source" -B "$out/player" \
    -DCMAKE_TOOLCHAIN_FILE="$source/scripts/kindle-candidate.cmake" \
    -DKINAMP_TARGET_SYSROOT="$sysroot" -DKINAMP_FFMPEG_PREFIX="$out/ffmpeg" \
    -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS -Wl,-Map,$out/player.link.map" \
    -DGST10=OFF -DKINAMP_MINIMAL_ONLY=ON -DKINAMP_OPUS_MP4=ON \
    -DKINAMP_CHECK_ABI=OFF -DCMAKE_BUILD_TYPE=MinSizeRel > "$out/cmake.log" 2>&1
cmake --build "$out/player" --target KinAMP-minimal -j"${JOBS:-4}" > "$out/player-build.log" 2>&1
cp "$out/player/KinAMP-minimal" "$out/KinAMP-minimal.pw6-gst010"
arm-linux-gnueabihf-strip "$out/KinAMP-minimal.pw6-gst010"
export CXXFLAGS="$CFLAGS"
"$source/scripts/build-prototype-tests.sh" "$out/tests" "$out/ffmpeg" "$source" arm-linux-gnueabihf-
arm-linux-gnueabihf-strip "$out/tests/opus-pcm" "$out/tests/aac-probe"
python3 "$source/scripts/check-target-abi.py" "$target" "$out/KinAMP-minimal.pw6-gst010" \
    "$out/tests/opus-pcm" "$out/tests/aac-probe" > "$out/abi.json"
echo "Built $out/KinAMP-minimal.pw6-gst010; target symbol preflight passed, native acceptance pending."
