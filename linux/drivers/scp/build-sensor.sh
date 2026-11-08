#!/bin/sh
# Build the sensor hub modules (needs scp-port built first for its symvers).
set -e
TOP=$HOME/arch-port/src
make -C $TOP/linux-mainline O=$TOP/out-mainline ARCH=arm64 LLVM=1 M=$TOP/scp-port/sensor \
	KBUILD_EXTRA_SYMBOLS=$TOP/scp-port/Module.symvers modules "$@"
