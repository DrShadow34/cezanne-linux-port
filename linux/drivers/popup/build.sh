#!/bin/sh
# Build the pop-up camera modules against the mainline tree.
set -e
TOP=$HOME/arch-port/src
make -C $TOP/linux-mainline O=$TOP/out-mainline ARCH=arm64 LLVM=1 M=$(cd "$(dirname "$0")" && pwd) modules "$@"
