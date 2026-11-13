#!/bin/bash
# sandbox.sh DIR STEPS: on the phone, as the desktop user, inside
# dbus-run-session: the cezanne shell from DIR (shell.qml, niri/config.kdl)
# in a niri nested in a headless cage (kiosk: niri's window fills its
# 1080x2400 output, set with wlr-randr), so it can be driven and
# screenshotted without the real screen. Steps, one per line:
#   sleep N | shot NAME | ipc TARGET FUNC [ARGS] | niri ACTION [ARGS] | run CMD
#   | mem LABEL (qs's PSS + swap PSS, the nested niri's PSS in kB to out/mem.log)
# Output in DIR/out: shots/NAME.png, niri.log, qs.log.
# Run: systemd-inhibit --what=sleep runuser -u arch -- env
#   XDG_RUNTIME_DIR=/run/user/1002 dbus-run-session -- sandbox.sh DIR STEPS
set -u
DIR=$1
STEPS=$2
OUT=$DIR/out
mkdir -p "$OUT/shots"
rm -f "$OUT"/shots/*.png "$OUT/mem.log"
WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 cage -- sleep 900 >"$OUT/cage.log" 2>&1 &
M=$!
# the socket cage listens on (it may reuse a stale wayland-N)
D=
for _ in $(seq 100); do
	D=$(ss -xlpn | grep "pid=$M," | grep -o 'wayland-[0-9]*' | head -1)
	[ -n "$D" ] && break
	sleep 0.1
done
[ -n "$D" ] || { echo "cage did not start"; kill -9 $M; exit 1; }
WAYLAND_DISPLAY=$D wlr-randr --output HEADLESS-1 --custom-mode 1080x2400 >>"$OUT/cage.log" 2>&1

# the session config with the nested output in place of the panel
sed -e 's/output "DSI-1"/output "winit"/' "$DIR/niri/config.kdl" >"$OUT/niri.kdl"
# its D-Bus services (ScreenCast, DisplayConfig) on the sandbox's own bus,
# as a session instance has them (cezanne-screencast tests)
printf 'debug {\n    dbus-interfaces-in-non-session-instances\n}\n' >>"$OUT/niri.kdl"
WAYLAND_DISPLAY=$D niri -c "$OUT/niri.kdl" >"$OUT/niri.log" 2>&1 &
N=$!
S=
for _ in $(seq 150); do
	S=$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/niri.log" | grep -o 'listening on Wayland socket: wayland-[0-9]*' | grep -o 'wayland-[0-9]*')
	[ -n "$S" ] && break
	sleep 0.1
done
[ -n "$S" ] || { echo "niri did not start"; kill $N $M; exit 1; }
sleep 0.5
export WAYLAND_DISPLAY=$S
export NIRI_SOCKET=$(sed 's/\x1b\[[0-9;]*m//g' "$OUT/niri.log" | grep -o 'IPC listening on: .*' | cut -d' ' -f4)
export QT_QPA_PLATFORM=wayland
export QS_ICON_THEME=Adwaita
export LANG=C.UTF-8
export CEZANNE_SHELL_QML=$DIR/shell.qml
# the AOD must not doze, arm the RTC or sleep the phone from here
export CEZANNE_AOD_CTL=$(dirname "$0")/aod-ctl-dummy
# nor read or write the session's lock state (it shares XDG_RUNTIME_DIR)
export CEZANNE_LOCK_FILE=$OUT/locked
rm -f "$CEZANNE_LOCK_FILE"
# nor run a fingerprint conversation on the real sensor, nor switch the
# real panel's FOD HBM: a State timeline and a file instead
export CEZANNE_FP_PAM=off
export CEZANNE_FOD_MONITOR=$(dirname "$0")/fod-dummy
export CEZANNE_HBM=$OUT/hbm
echo 0 >"$CEZANNE_HBM"
# the panel's DC dimming / colour scheme, the user's saved choices and the
# NFC adapter: not the real ones either
export CEZANNE_PANEL=$OUT/panel CEZANNE_CONFIG=$OUT/config CEZANNE_NFC=off
mkdir -p "$CEZANNE_PANEL" "$CEZANNE_CONFIG"
# a backlight of its own (CEZANNE_AUTO_BRIGHTNESS=1 from the caller turns
# automatic brightness on without the user's setting)
export CEZANNE_BACKLIGHT=$OUT/backlight CEZANNE_LIGHT_MONITOR=$(dirname "$0")/light-dummy
# no notification light or vibration (feedbackd) and the user's feedbackd
# theme left alone
export CEZANNE_FBCLI=$(dirname "$0")/fbcli-dummy
# screenshots / screencasts into out/, not the user's folders
export CEZANNE_PICTURES=$OUT/Pictures CEZANNE_VIDEOS=$OUT/Videos
# the USB port's mode: read, never set
export CEZANNE_USB=off
rm -f "$OUT/fb.log"
mkdir -p "$CEZANNE_BACKLIGHT/test"
echo 2047 >"$CEZANNE_BACKLIGHT/test/max_brightness"
echo 1000 >"$CEZANNE_BACKLIGHT/test/brightness"
unset DISPLAY

# QML=greeter.qml for the login screen
qs -p "$DIR/${QML:-shell.qml}" >"$OUT/qs.log" 2>&1 &
Q=$!

while read -r cmd args; do
	case $cmd in
	''|'#'*) ;;
	sleep) sleep "$args" ;;
	# cage's output (niri takes no screenshots while locked)
	shot) WAYLAND_DISPLAY=$D grim "$OUT/shots/$args.png" ;;
	ipc) qs ipc -p "$DIR/${QML:-shell.qml}" call $args >>"$OUT/ipc.log" 2>&1 ;;
	niri) niri msg action $args >>"$OUT/ipc.log" 2>&1 ;;
	run) setsid sh -c "$args" >>"$OUT/apps.log" 2>&1 & ;;
	mem) echo "$args $(awk '/^Pss:/{p=$2} /^SwapPss:/{w=$2} END{print p, w}' /proc/$Q/smaps_rollup) niri $(awk '/^Pss:/{print $2}' /proc/$N/smaps_rollup)" >>"$OUT/mem.log" ;;
	esac
done <"$STEPS"

kill $Q 2>/dev/null
sleep 0.3
kill $N 2>/dev/null
sleep 0.5
pkill -P $$ 2>/dev/null
# cage stays up after SIGTERM: its sleep and itself go hard
pkill -9 -P $M 2>/dev/null
kill -9 $M 2>/dev/null
wait 2>/dev/null
ls "$OUT/shots"
