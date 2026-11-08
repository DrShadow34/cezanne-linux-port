#!/bin/sh
# drain.sh TARGET_CAP MIN_S MAX_S: CPU + GPU stress (eemstress.sh) until the
# battery is at TARGET_CAP %, at least MIN_S, at most MAX_S; log every 20 s
TGT=${1:-70}; MIN=${2:-900}; MAX=${3:-5400}
B=/sys/class/power_supply/battery
z() { for t in /sys/class/thermal/thermal_zone*; do [ "$(cat $t/type)" = "$1-thermal" ] && cat $t/temp; done; }
/root/eemstress.sh $MAX &
start=$(date +%s)
echo "time s cap vbat_mV ibat_mA Tbat_C big_C gpu_C bigMHz gpuMHz" > /root/drain.log
while :; do
	now=$(date +%s); el=$((now - start)); c=$(cat $B/capacity)
	echo "$(date +%T) $el $c $(($(cat $B/voltage_now)/1000)) $(($(cat $B/current_now)/1000)) $(($(cat $B/temp)/10)) $(($(z cpu-big0)/1000)) $(($(z gpu0)/1000)) $(($(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq)/1000)) $(($(cat /sys/class/devfreq/13000000.gpu/cur_freq)/1000000))" >> /root/drain.log
	[ $el -ge $MAX ] && break
	[ $el -ge $MIN ] && [ $c -le $TGT ] && break
	sleep 20
done
echo "done at $(date +%T) after ${el}s, cap $c" >> /root/drain.log
pkill -x glmark2-es2-wayland; pkill -x openssl; pkill -f "eemstress.sh"
