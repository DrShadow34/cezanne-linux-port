# Arch's x86_64 PKGBUILD for aarch64: no XNVCtrl (NVIDIA's library), no
# mangoapp (gamescope's overlay; it pulls glfw in), mangoplot's matplotlib /
# numpy optional. Games: MANGOHUD=1 %command% (Steam launch options); the
# Proton ARM64 container imports host Vulkan layers. x86 games under FEX use
# the x86 MangoHud of FEX's ArchLinux image (mainline/fex-guestos).
# 0001: CPU / GPU temperatures from MediaTek's LVTS zones (cpu_big0_thermal,
# gpu0_thermal), whose names MangoHud did not recognise.
_cz_drop() {
	local -n _cz_list=$1
	local _cz_keep=() _cz_x
	shift
	for _cz_x in "${_cz_list[@]}"; do
		[[ " $* " == *" $_cz_x "* ]] || _cz_keep+=("$_cz_x")
	done
	_cz_list=("${_cz_keep[@]}")
}
_cz_drop makedepends libxnvctrl
_cz_drop depends glfw python-matplotlib python-numpy
optdepends=('python-matplotlib: mangoplot'
            'python-numpy: mangoplot')
build() {
	arch-meson "$_pkgname-v$pkgver" build \
		-Dmangoapp=false \
		-Dmangohudctl=true \
		-Dwith_xnvctrl=disabled
	meson compile -C build
}
cz_patch 'MangoHud-v$pkgver' 0001-mediatek-thermal-zone-names.patch
