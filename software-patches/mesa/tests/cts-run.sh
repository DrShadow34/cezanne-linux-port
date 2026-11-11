#!/bin/bash
# ctsrun.sh <name> <fraction> <overlap 0|1> [caselist]: deqp-vk through
# deqp-runner as arch with the installed PanVK; results in /var/tmp/cts-out/<name>
W=/var/tmp/cts; V=$W/VK-GL-CTS/build/external/vulkancts/modules/vulkan
out=/var/tmp/cts-out/$1; frac=$2; ovl=$3; list=${4:-$W/vk-default-all.txt}
if [ ! -s $W/vk-default-all.txt ]; then
	M=$W/VK-GL-CTS/external/vulkancts/mustpass/main
	(cd $M || exit 1; tr -d '\r' < vk-default.txt | while read -r f; do cat "$f"; done) > $W/vk-default-all.txt
fi
rm -rf "$out"; mkdir -p "$out"; chown arch: "$out" /var/tmp/cts-out
start=$(date +%s)
cd $V || exit 1
runuser -u arch -- env XDG_RUNTIME_DIR=/run/user/1002 \
	VK_DRIVER_FILES=/usr/share/vulkan/icd.d/panfrost_icd.json PANVK_JM_OVERLAP=$ovl \
	MESA_VK_IGNORE_CONFORMANCE_WARNING=1 \
	$W/cargo/bin/deqp-runner run --deqp ./deqp-vk --caselist "$list" --output "$out" \
	--jobs 5 --fraction $frac --timeout 120 -- --deqp-log-images=disable \
	--deqp-log-shader-sources=disable --deqp-surface-type=pbuffer > "$out/runner.log" 2>&1
echo "rc=$? $(( $(date +%s) - start )) s" >> "$out/runner.log"
journalctl -k --since=@$start --no-pager -g 'panfrost|mali|gpu' > "$out/kmsg.txt"
echo RUN-DONE >> "$out/runner.log"
