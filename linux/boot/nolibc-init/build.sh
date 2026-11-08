#!/usr/bin/env bash
# Build a nolibc /init: build.sh <prog.c> <out>   (archboot.c -> initramfs/mainline-arch/init)
# Against the kernel's arm64 UAPI headers, never the build host's (an x86_64
# host's asm/unistd.h gives x86 system call numbers: a boot loop). Needs
# rsync (headers_install).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K="${KSRC:-$ROOT/src/linux-mainline}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
make -s -C "$K" O="$T/o" ARCH=arm64 LLVM=1 headers_install INSTALL_HDR_PATH="$T/uapi"
cc=(clang --target=aarch64-linux-gnu -nostdlib -nostdlibinc -static -ffreestanding -fno-stack-protector -Os
    -fuse-ld=lld -I "$T/uapi/include" -include "$K/tools/include/nolibc/nolibc.h" -I "$K/tools/include/nolibc")
[[ $(printf '#include <asm/unistd.h>\n__NR_mount\n' | "${cc[@]}" -E -P - | tail -1) == 40 ]] ||
    { echo "build.sh: asm/unistd.h is not arm64's" >&2; exit 1; }
"${cc[@]}" "${1:?source}" -o "${2:?output}"
