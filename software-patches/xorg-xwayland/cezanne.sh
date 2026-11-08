# 0001: 20:9 modes in Xwayland's emulated XRandR mode list (games, and
# Proton, which offers the host's modes besides its own list)
# 0002: the emulated modes on the phone's rotated portrait panel: offered
# transposed (else only modes up to 1080 wide survived), matched and scaled
# in screen orientation
cz_patch 'xwayland-$pkgver' 0001-xwayland-20-9-modes.patch \
	0002-xwayland-emulated-modes-on-rotated-portrait-outputs.patch
