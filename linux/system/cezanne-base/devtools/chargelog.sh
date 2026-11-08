#!/bin/sh
# chargelog.sh SECONDS [LOG]: drop the test input cap (/run/cezanne-aicr-max),
# re-apply the charger policy and log the charge every 10 s
T=${1:-1200}; L=${2:-/root/charge.log}
B=/sys/class/power_supply/battery
M=$(ls -d /sys/class/power_supply/mt6360-chg* | head -1)
X=/sys/class/power_supply/xmusb350
rm -f /run/cezanne-aicr-max
/usr/local/bin/mt6360-chg-setup
start=$(date +%s)
echo "time s cap vbat_mV ibat_mA W Tbat_C vbus_mV qc_mV aicr_mA ichg_mA" > $L
while :; do
	el=$(( $(date +%s) - start ))
	v=$(($(cat $B/voltage_now)/1000)); i=$(($(cat $B/current_now)/1000))
	echo "$(date +%T) $el $(cat $B/capacity) $v $i $((v * i / 1000000)) $(($(cat $B/temp)/10)) $(($(cat /sys/class/power_supply/bq25970/voltage_now)/1000)) $(cat $X/qc_voltage 2>/dev/null) $(($(cat $M/input_current_limit)/1000)) $(($(cat $M/constant_charge_current)/1000))" >> $L
	[ $el -ge $T ] && break
	sleep 10
done
