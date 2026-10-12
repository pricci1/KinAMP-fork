#!/bin/sh
# Headless ARM GStreamer 0.10 for QEMU only; not the Lab126 mixersink runtime.
# Usage: build-prototype-gst.sh PREFIX
set -eu
prefix=$(realpath -m "$1")
source=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cache=${KINAMP_BUILD_CACHE:-$(dirname "$prefix")/sources}
mkdir -p "$cache" "$prefix"
cache=$(realpath "$cache")
archive="$cache/gstreamer-0.10.36.tar.xz"
if [ ! -f "$archive" ]; then
    curl -fL https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-0.10.36.tar.xz -o "$archive"
fi
echo "9151aa108c177054387885763fa0e433e76780f7c5655c70a5390f2a6c6871da  $archive" | sha256sum -c -
src="$cache/gstreamer-0.10.36"
if [ ! -d "$src" ]; then
    tar -xf "$archive" -C "$cache"
fi
if ! grep -q '%lex-param { void \*scanner }' "$src/gst/parse/grammar.y"; then
    patch -d "$src" -p1 < "$source/scripts/gst-0.10-bison.patch"
fi
mkdir -p "$prefix/build"
cd "$prefix/build"
export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
"$src/configure" --host=arm-linux-gnueabihf --prefix="$prefix" \
    --disable-gtk-doc --disable-examples --disable-tests --disable-introspection --disable-nls > configure.log 2>&1
make -j"${JOBS:-4}" > build.log 2>&1
make install > install.log 2>&1
cp "$src/COPYING" "$src/AUTHORS" "$prefix/"
