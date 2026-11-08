# Mesa for the phone: ALARM's mesa with the Mali-G77 work (patches 0001-0017 =
# src/mesa/mesa-g77, mainline/mesa-g77/README.md): G77 in Panfrost, PanVK on
# the v9 job manager with the DXVK features, Wayland WSI pre-rotation, perf
# counters. Upstream's prepare() applies every *.patch of source=() in order.
cz_source 00*.patch

# only the drivers this SoC uses (the full ALARM set takes hours on the phone)
pkgname=(mesa vulkan-panfrost vulkan-swrast vulkan-mesa-implicit-layers vulkan-mesa-layers)
makedepends=($(printf '%s\n' "${makedepends[@]}" |
	grep -vxE 'rust|rust-bindgen|cbindgen|directx-headers|python-sphinx.*'))
arch-meson() {
	local out=() a o i=1
	while (( i <= $# )); do
		a=${!i}
		if [[ $a == -D ]]; then
			i=$((i + 1))
			o=${!i}
			case $o in
			gallium-drivers=*) o=gallium-drivers=panfrost,llvmpipe,softpipe,zink ;;
			vulkan-drivers=*) o=vulkan-drivers=panfrost,swrast ;;
			vulkan-layers=*) o=vulkan-layers=device-select,overlay,screenshot,anti-lag,vram-report-limit ;;
			gallium-rusticl=*) o=gallium-rusticl=false ;;
			teflon=*) o=teflon=false ;;
			html-docs=*) o=html-docs=disabled ;;
			amdgpu-virtio=* | intel-virtio-experimental=*) o=${o%%=*}=false ;;
			gallium-rusticl-enable-drivers=* | freedreno-kmds=*) o= ;;
			esac
			[[ -n $o ]] && out+=(-D "$o")
		else
			out+=("$a")
		fi
		i=$((i + 1))
	done
	# arch-meson turns auto features on: VA needs radeonsi / r600 / nouveau /
	# virgl / d3d12 (the VA driver here is cezanne-va)
	command arch-meson "${out[@]}" -D gallium-va=disabled
}
# upstream moves every driver's files out of mesa: skip those not built
_pick() {
	local p="$1" f d
	shift
	for f; do
		[[ -e $f || -L $f ]] || continue
		d="$srcdir/$p/${f#$pkgdir/}"
		mkdir -p "$(dirname "$d")"
		mv -v "$f" "$d"
		rmdir -p --ignore-fail-on-non-empty "$(dirname "$f")"
	done
}
