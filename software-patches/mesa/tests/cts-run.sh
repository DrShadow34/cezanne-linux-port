#!/bin/sh
# cts-run.sh <outdir> <fraction> [caselist]
D=/home/arch/Documents/Cezanne-Development
out=$1; frac=$2; list=${3:-$D/cts-all.txt}
rm -rf $out; mkdir -p $out
cd $D/src/cts/build/external/vulkancts/modules/vulkan
env XDG_RUNTIME_DIR=/run/user/1002 VK_ICD_FILENAMES=${ICD:-/usr/share/vulkan/icd.d/panfrost_icd.json} \
  PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_VK_IGNORE_CONFORMANCE_WARNING=1 \
  /home/arch/.cargo/bin/deqp-runner run --deqp ./deqp-vk --caselist $list --output $out \
  --jobs 5 --fraction $frac --timeout 120 -- --deqp-log-images=disable --deqp-log-shader-sources=disable \
  --deqp-surface-type=pbuffer > $out/runner.log 2>&1
echo RUN-DONE >> $out/runner.log
