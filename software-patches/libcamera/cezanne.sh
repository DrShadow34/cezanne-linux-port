# 0001-cezanne.patch = v0.7.2 -> the tree the camera stack was built and tested
# with (apps/libcamera-cezanne-all.patch + the buffer pool half of
# apps/libcamera-gst-no-busy-loop.patch): sensor helpers and tunings
# (hi1337, s5k5e9, ov02b1b), soft-ISP AF / capture / rotation, debayer fixes.
cz_patch libcamera 0001-cezanne.patch

# no documentation (texlive / sphinx / doxygen on the phone) and no test run
pkgname=($(printf '%s\n' "${pkgname[@]}" | grep -vx libcamera-docs))
makedepends=($(printf '%s\n' "${makedepends[@]}" |
	grep -vxE 'doxygen|texlive-core|python-sphinx.*|graphviz'))
# werror off: GCC 16 flags a false -Warray-bounds in base/log.cpp (shared_ptr)
arch-meson() { command arch-meson "$@" -D documentation=disabled -D werror=false; }
check() { :; }
# upstream's package_libcamera moves usr/share/doc into the docs package
cz_before package_libcamera 'mkdir -p "$pkgdir/usr/share/doc"'
