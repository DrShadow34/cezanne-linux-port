#!/usr/bin/env bash
# Build a nolibc /init: build.sh <prog.c> <out>   (archboot.c -> initramfs/mainline-arch/init)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
K="$ROOT/src/linux-mainline"
clang --target=aarch64-linux-gnu -nostdlib -static -ffreestanding -fno-stack-protector -Os \
    -fuse-ld=lld -include "$K/tools/include/nolibc/nolibc.h" -I "$K/tools/include/nolibc" \
    "${1:?source}" -o "${2:?output}"
