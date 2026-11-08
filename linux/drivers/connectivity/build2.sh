#!/bin/sh
# Build a connectivity module that links against conninfra (and others).
# usage: build2.sh <module-dir> [make vars/args...]
set -e
TOP=$HOME/arch-port/src
C=$TOP/connectivity
M=$C/$1; shift
# symbols of the modules built before this one
EXTRA=""
for d in conninfra wlan-adaptor connfem btif; do
	[ "$C/$d" != "$M" ] && [ -f $C/$d/Module.symvers ] && EXTRA="$EXTRA $C/$d/Module.symvers"
done
make -C $TOP/linux-mainline O=$TOP/out-mainline ARCH=arm64 LLVM=1 M=$M \
	TOP=$TOP/vtop CONNINFRA_SRC=$C/conninfra GEN4M_SRC=$C/wlan-core-gen4m \
	CONFIG_MTK_COMBO_CHIP_CONSYS_6885=y CONFIG_MTK_CONNSYS_DEDICATED_LOG_PATH=y \
	KBUILD_EXTRA_SYMBOLS="$EXTRA" \
	modules "$@"
