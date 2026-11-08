#!/bin/sh
# log pid + case list, then exec the real deqp-vk (same pid)
d=/home/arch/wrap-log; mkdir -p $d
for a in "$@"; do case $a in --deqp-caselist-file=*) cp "${a#*=}" $d/$$.txt;; esac; done
cd /home/arch/src/cts/build/external/vulkancts/modules/vulkan && exec /home/arch/src/cts/build/external/vulkancts/modules/vulkan/deqp-vk "$@"
