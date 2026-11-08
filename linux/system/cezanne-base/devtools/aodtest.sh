#!/bin/bash
# aodtest.sh: AOD clock placement after locking in landscape (uses /root/rot.sh)
rm -f /tmp/aod-*.png
echo "before lock: $(/root/rot.sh 1 | tail -1)"
scr; cp /tmp/scr.png /tmp/aod-0-land.png
powerkey 50; sleep 1; scr; cp /tmp/scr.png /tmp/aod-1a.png; sleep 4
scr; cp /tmp/scr.png /tmp/aod-1.png
sleep $((62 - $(date +%S)))
scr; cp /tmp/scr.png /tmp/aod-2.png
journalctl --since "-2 min" --no-pager -o cat _COMM=gnome-shell | grep -iE "aod|error|warn" | tail -5
powerkey 50; sleep 2; loginctl unlock-sessions; sleep 3
echo "after: $(/root/rot.sh 0 | tail -1)"
pkill -f "gdm-session-worker \[pam/gdm-fingerprint\]"
