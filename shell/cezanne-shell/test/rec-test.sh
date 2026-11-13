#!/bin/bash
# rec-test.sh NAME [X Y W H] [PIPELINE]: record the sandbox niri's output
# for 6 s with cezanne-screencast (the sandbox's own D-Bus session) ->
# out/rec-NAME.mp4, the log and ffprobe's view in out/rec-NAME.log
D=$(dirname "$0")/..; O=$D/out; n=$1; shift
c=$(python3 -c 'from gi.repository import Gio
b=Gio.bus_get_sync(Gio.BusType.SESSION)
r=b.call_sync("org.gnome.Mutter.DisplayConfig","/org/gnome/Mutter/DisplayConfig","org.gnome.Mutter.DisplayConfig","GetCurrentState",None,None,0,-1,None)
print(r.unpack()[1][0][0][0])')
crop=(); [ $# -ge 4 ] && { crop=("$1" "$2" "$3" "$4"); shift 4; }
[ -n "$1" ] && export CEZANNE_SCREENCAST_PIPELINE=$1
rm -f "$O/rec-$n.mp4"
{ echo "connector $c crop ${crop[*]} pipeline ${1:-any}"
  (sleep 6; echo stop) | "$D/bin/cezanne-screencast" "$O/rec-$n.mp4" monitor "$c" "${crop[@]}" 2>&1
  ffprobe -v error -show_entries stream=codec_name,width,height,nb_frames -show_entries format=duration -of compact "$O/rec-$n.mp4" 2>&1
} > "$O/rec-$n.log"
