#!/bin/sh
# bisect.sh <list> : minimal window [s,e] of list that produces a js fault
L=$1; n=$(wc -l < $L)
f() { sh /home/arch/win.sh $L $1 $2 | sed "s/.*faults=//"; }
[ "$(f 1 $n)" = 0 ] && { echo "$L: no fault"; exit; }
lo=1; hi=$n; while [ $lo -lt $hi ]; do m=$(((lo+hi)/2)); [ "$(f 1 $m)" != 0 ] && hi=$m || lo=$((m+1)); done; e=$lo
lo=1; hi=$e; while [ $lo -lt $hi ]; do m=$(((lo+hi+1)/2)); [ "$(f $m $e)" != 0 ] && lo=$m || hi=$((m-1)); done; s=$lo
echo "$L: $s-$e"; sed -n "${s},${e}p" $L
