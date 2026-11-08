# Arch's PKGBUILD for aarch64 (the AppImage bundles a Mesa that cannot drive
# the G77). 0001 makes magpie (the data helper) take GPU render / memory /
# decoder / encoder usage from libcezanne-top (apps/cezanne-top) instead of
# nvtop (no platform GPUs, no V4L2 codecs, and its panfrost code aborted
# magpie every few seconds here), find memory modules on the
# mt6885-dram-info device when there is no SMBIOS, and prefer the kernel's
# CPU "model name".
depends=("${depends[@]/#nvtop/cezanne-top}")
makedepends+=(cezanne-top)
cz_patch mission-center 0001-magpie-cezanne-gpu-cpu-memory.patch
# Arch's build() without fat LTO, a single codegen unit and debug info: on
# the phone rustc crashed twice in it (LLVM SIGSEGV in an LTO pass compiling
# upower_dbus, then "corrupted double-linked list" linking magpie), and the
# disk is small. Stops when Arch's build() no longer has those settings.
eval "$(declare -f build | sed -e 's/CARGO_PROFILE_RELEASE_LTO=true/CARGO_PROFILE_RELEASE_LTO=false/' \
	-e 's/CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1 //' \
	-e 's/CARGO_PROFILE_RELEASE_DEBUG=2/CARGO_PROFILE_RELEASE_DEBUG=0/')"
if ! declare -f build | grep -q 'CARGO_PROFILE_RELEASE_LTO=false' ||
   ! declare -f build | grep -q 'CARGO_PROFILE_RELEASE_DEBUG=0'; then
	echo "cezanne: mission-center's build() changed upstream: check cezanne.sh" >&2
	exit 1
fi
