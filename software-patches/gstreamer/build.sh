#!/bin/sh
# On the phone, as root: build the patched v4l2codecs and va plugins and
# install them over the package's (the originals kept as *.so.pkg).
#   build.sh /path/to/gst-plugins-bad-1.28.8.tar.xz
# Needs meson, ninja, gcc, the gstreamer / gst-plugins-base headers (the
# Arch packages ship them) and libva / libdrm. Only the codec libraries and
# the two plugins are built (a few minutes on the phone).
set -eu
T=${1:?tarball}; B=/home/arch/build/gst-bad; V=$(basename "$T" .tar.xz)
D0="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$B"; mkdir -p "$B"; cd "$B"; tar xf "$T"; cd "$V"
for p in "$D0"/0*.patch; do patch -p1 < "$p"; done
meson setup build -Dauto_features=disabled -Dv4l2codecs=enabled -Dva=enabled -Dgpl=disabled \
	-Dintrospection=disabled -Dtests=disabled -Dexamples=disabled -Ddoc=disabled \
	-Dnls=disabled -Dorc=disabled --buildtype=release > "$B/meson.log" 2>&1
ninja -C build sys/v4l2codecs/libgstv4l2codecs.so sys/va/libgstva.so > "$B/ninja.log" 2>&1
for p in v4l2codecs/libgstv4l2codecs va/libgstva; do
	D=/usr/lib/gstreamer-1.0/${p#*/}.so
	[ -f "$D.pkg" ] || cp "$D" "$D.pkg"
	install -m644 build/sys/$p.so "$D"
	echo "installed: $(md5sum "$D")"
done
rm -rf /home/arch/.cache/gstreamer-1.0 /root/.cache/gstreamer-1.0
