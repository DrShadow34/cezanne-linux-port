
# ---- cezanne overlay helpers (appended to the upstream PKGBUILD by cezanne-build) ----
# An overlay's cezanne.sh runs after the upstream PKGBUILD, so it can change any
# variable and wrap any function without copying the upstream file:
#   cz_source FILE...      add local files to source=() (SKIP checksums)
#   cz_after FUNC 'CODE'   run CODE after the upstream FUNC (prepare, build, ...)
#   cz_before FUNC 'CODE'  run CODE before it
#   cz_patch DIR PATCH...  in prepare: patch -Np1 in $srcdir/DIR (after upstream)
# cezanne-build then sets arch=(aarch64) unless the overlay set it, and pkgrel
# to the upstream pkgrel plus the overlay's suffix.

cz_source() {
	local f a
	for f; do
		source+=("$f")
		for a in md5sums sha1sums sha224sums sha256sums sha384sums sha512sums b2sums cksums; do
			if declare -p "$a" &>/dev/null; then
				eval "$a+=(SKIP)"
			fi
		done
	done
}

_cz_wrap() {
	local when=$1 fn=$2 code=$3 n=0
	while declare -f "_cz_${n}_$fn" >/dev/null; do n=$((n + 1)); done
	if declare -f "$fn" >/dev/null; then
		eval "$(declare -f "$fn" | sed "1s/^$fn /_cz_${n}_$fn /")"
	else
		eval "_cz_${n}_$fn() { :; }"
	fi
	if [[ $when == after ]]; then
		eval "$fn() { _cz_${n}_$fn; $code
}"
	else
		eval "$fn() { $code
_cz_${n}_$fn; }"
	fi
}
cz_after() { _cz_wrap after "$@"; }
cz_before() { _cz_wrap before "$@"; }

cz_patch() {
	local dir=$1 p code
	code="cd \"\$srcdir/$dir\""
	shift
	for p; do
		cz_source "$p"
		code+="; patch -Np1 -i \"\$srcdir/$p\""
	done
	cz_after prepare "$code"
}
# ---- end of cezanne overlay helpers ----
