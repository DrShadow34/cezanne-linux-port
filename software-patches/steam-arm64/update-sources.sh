#!/bin/bash
# update-sources.sh: pin PKGBUILD's sources to Valve's current releases:
# the steam_client_linuxarm64 manifest (_zips / _zipsums: the "_all" and
# "_linuxarm64_linuxarm64" (native) components; the plain "_linuxarm64" ones
# are the x86 client Valve runs under FEX), the arm64 Steam Runtime and the
# launcher tarball (what upstream-version reports). pkgver = client version
# . runtime build . launcher patch level: it grows when any of them does.
# cezanne-build runs it in the build copy before each build (type follow);
# by hand it updates the PKGBUILD here.
set -eu
cd "$(dirname "$0")"
read -r rt launcher < <(./upstream-version)
m=$(curl -sSf https://client-update.fastly.steamstatic.com/steam_client_linuxarm64)
ver=$(printf '%s\n' "$m" | awk -F'"' '$2 == "version" {print $4; exit}')
# component name, file, sha2 (one line each)
comps=$(printf '%s\n' "$m" | awk -F'"' '
	NF == 3 && $2 ~ /_(all|linuxarm64_linuxarm64)$/ { name = $2; next }
	NF == 3 { name = "" }
	name != "" && $2 == "file" { file = $4 }
	name != "" && $2 == "sha2" { print name, file, $4; name = "" }')
[ -n "$comps" ] || { echo "no components: manifest format changed?" >&2; exit 1; }
rtsum=$(curl -sSf "https://repo.steampowered.com/steamrt3c/images/$rt/SHA256SUMS" |
	awk '$2 ~ /(^|[*\/])steam-runtime-steamrt-arm64\.tar\.xz$/ {print $1; exit}')
[ -n "$rtsum" ] || { echo "no checksum for the $rt runtime" >&2; exit 1; }
# Valve publishes no checksum for the launcher: hash the tarball, kept where
# makepkg looks for it (cezanne-build's SRCDEST) so it is not fetched twice
tarball="${SRCDEST:-.}/steam_$launcher.tar.gz"
[ -s "$tarball" ] || curl -sSf -o "$tarball" "https://repo.steampowered.com/steam/archive/stable/steam_$launcher.tar.gz"
launchersum=$(sha256sum "$tarball" | cut -d' ' -f1)
{
	echo "# BEGIN sources (update-sources.sh)"
	echo "pkgver=$ver.${rt##*.}.${launcher##*.}"
	echo "_launcher=$launcher"
	echo "_launchersum=$launchersum"
	echo "_runtime=$rt"
	echo "_runtimesum=$rtsum"
	echo "_zips=("
	printf '%s\n' "$comps" | awk '{print "  " $2}'
	echo ")"
	echo "_zipsums=("
	printf '%s\n' "$comps" | awk '{print "  " $3}'
	echo ")"
	echo "# END sources"
} > sources.tmp
awk 'FNR == NR { block = block $0 "\n"; next }
	/^# BEGIN sources/ { printf "%s", block; skip = 1; next }
	/^# END sources/ { skip = 0; next }
	!skip' sources.tmp PKGBUILD > PKGBUILD.new
mv PKGBUILD.new PKGBUILD
rm sources.tmp
echo "pkgver $ver.${rt##*.}.${launcher##*.}: runtime $rt, launcher $launcher, $(printf '%s\n' "$comps" | wc -l) client components"
