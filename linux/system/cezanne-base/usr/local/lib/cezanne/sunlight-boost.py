#!/usr/bin/env python3
"""Sunlight boost for the cezanne panel (MIUI threashold_sunlight_brightness):
in bright ambient light switch the panel to its high brightness mode, above
what the backlight range reaches. Only while GNOME's automatic brightness is
on, like MIUI. Ambient light from iio-sensor-proxy (system bus)."""
import os, subprocess, time
from gi.repository import Gio, GLib

HBM = "/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm"
ON_LUX, OFF_LUX = 1000, 700         # MIUI threshold, hysteresis
HOLD = 3.0                          # seconds the level must hold
USER = "arch"

state = {"hbm": False, "since": None, "want": False}

def auto_brightness_on():
    """GNOME's 'Automatic Screen Brightness' of the logged-in user"""
    try:
        uid = int(subprocess.check_output(["id", "-u", USER]))
        out = subprocess.check_output(
            ["sudo", "-u", USER, f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus",
             "gsettings", "get", "org.gnome.settings-daemon.plugins.power", "ambient-enabled"],
            stderr=subprocess.DEVNULL, timeout=3)
        return out.strip() == b"true"
    except Exception:
        return False

def set_hbm(on):
    # never override the fingerprint spot (2)
    try:
        cur = open(HBM).read().strip()
        if cur == "2":
            return
        with open(HBM, "w") as f:
            f.write("1" if on else "0")
        state["hbm"] = on
        print(f"sunlight boost {'on' if on else 'off'}", flush=True)
    except OSError as e:
        print(f"hbm: {e}", flush=True)

def on_light(lux):
    want = state["hbm"]
    if lux >= ON_LUX:
        want = True
    elif lux <= OFF_LUX:
        want = False
    if want != state["want"]:
        state["want"], state["since"] = want, time.monotonic()
    if (want != state["hbm"] and time.monotonic() - state["since"] >= HOLD and
            (not want or auto_brightness_on())):
        set_hbm(want)

def main():
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
    proxy = Gio.DBusProxy.new_sync(bus, 0, None, "net.hadess.SensorProxy",
                                   "/net/hadess/SensorProxy", "net.hadess.SensorProxy")
    proxy.call_sync("ClaimLight", None, 0, -1, None)

    def poll():
        v = proxy.get_cached_property("LightLevel")
        if v is not None:
            on_light(v.unpack())
        return True

    GLib.timeout_add(500, poll)
    GLib.MainLoop().run()

if __name__ == "__main__":
    main()
