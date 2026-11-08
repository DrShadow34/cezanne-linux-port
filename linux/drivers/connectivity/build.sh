#!/bin/sh
# Build the connectivity modules against the mainline tree.
# usage: build.sh <module-dir> [make args...]
set -e
TOP=$HOME/arch-port/src
M=$TOP/connectivity/$1; shift
make -C $TOP/linux-mainline O=$TOP/out-mainline ARCH=arm64 LLVM=1 M=$M \
	CONNINFRA_SRC=$TOP/connectivity/conninfra \
	CONFIG_MTK_COMBO_CHIP_CONSYS_6885=y CONFIG_MTK_COMBO_CHIP_CONSYS_6893=n \
	modules "$@"
