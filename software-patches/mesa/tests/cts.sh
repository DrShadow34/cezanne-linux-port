#!/bin/sh
# cts.sh <name> <case pattern> : run deqp-vk on the phone, print summary
name=$1; shift
cd /home/arch/src/cts/build/external/vulkancts/modules/vulkan
args="--deqp-case=$1"
sudo -u arch env XDG_RUNTIME_DIR=/run/user/1002 VK_ICD_FILENAMES=${ICD:-/usr/share/vulkan/icd.d/panfrost_icd.json} \
  PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_VK_ABORT_ON_DEVICE_LOSS=0 \
  ./deqp-vk $args --deqp-log-filename=/home/arch/cts-$name.qpa --deqp-log-images=disable \
  --deqp-log-shader-sources=disable --deqp-surface-type=pbuffer > /home/arch/cts-$name.out 2>&1
echo "rc=$?"
tail -12 /home/arch/cts-$name.out | grep -E "Passed|Failed|Not supported|Warnings|Crash|Timeout|Waived|Test run totals|DONE"
grep -B1 -E "^  (Fail|Crash|ResourceError|InternalError|Timeout)" /home/arch/cts-$name.out | grep "Test case" | head -40
