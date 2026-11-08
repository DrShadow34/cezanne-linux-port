#!/bin/bash
# rot.sh <transform>: rotate the panel (temporary DisplayConfig), print the result
R="runuser -u arch -- env XDG_RUNTIME_DIR=/run/user/1002 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1002/bus"
DC="$R gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig --method org.gnome.Mutter.DisplayConfig"
serial=$($DC.GetCurrentState | sed -n 's/^(uint32 \([0-9]*\),.*/\1/p')
$DC.ApplyMonitorsConfig $serial 1 "[(0, 0, 2.0, $1, true, [('DSI-1', '1080x2400@121.718', {})])]" "{}"
sleep 1
$DC.GetCurrentState | grep -o "(0, 0, 2.0, uint32 [0-9]" | head -1
