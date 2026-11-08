#!/bin/sh
# win.sh <list> <from> <to> : run lines from..to of list, print fault count
sed -n "$2,$3p" $1 > /home/arch/wrap-log/win.txt; chmod 644 /home/arch/wrap-log/win.txt
dmesg -C; cd /home/arch/src/cts/build/external/vulkancts/modules/vulkan
sudo -u arch env XDG_RUNTIME_DIR=/run/user/1002 VK_ICD_FILENAMES=${ICD:-/usr/share/vulkan/icd.d/panfrost_icd.json} PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 MESA_VK_IGNORE_CONFORMANCE_WARNING=1 $WRAP ./deqp-vk --deqp-caselist-file=/home/arch/wrap-log/win.txt --deqp-log-filename=/home/arch/win.qpa --deqp-log-images=disable --deqp-log-shader-sources=disable --deqp-surface-type=pbuffer > /home/arch/win.out 2>&1
echo "$2-$3 faults=$(dmesg | grep -c "js fault")"
