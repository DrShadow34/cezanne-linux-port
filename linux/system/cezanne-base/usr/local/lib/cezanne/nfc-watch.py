#!/usr/bin/env python3
# nfc-watch.py [seconds]: start neard's poll loop on nfc0 (reader mode) and
# print every tag / device neard reports, with its NDEF records, until the
# time is up (default 600 s). neard restarts polling after a tag leaves
# (ConstantPoll in /etc/neard/main.conf).
import signal
import sys

from gi.repository import Gio, GLib, GLibUnix

ADAPTER = '/org/neard/nfc0'
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)


def call(path, iface, method, args=None, sig=None):
    return bus.call_sync('org.neard', path, iface, method, args,
                         GLib.VariantType(sig) if sig else None, 0, 5000, None)


def show(path, ifaces):
    for iface, props in ifaces.items():
        if not iface.startswith('org.neard.') or iface == 'org.freedesktop.DBus.Properties':
            continue
        kind = iface.rsplit('.', 1)[1]
        print('%s %s' % (kind.lower(), path.rsplit('/', 1)[1]), flush=True)
        for k, v in sorted(props.items()):
            print('  %s: %s' % (k, v), flush=True)


def added(conn, sender, path, iface, signal, params):
    obj, ifaces = params.unpack()
    show(obj, ifaces)


def removed(conn, sender, path, iface, signal, params):
    obj, ifaces = params.unpack()
    if any(i in ifaces for i in ('org.neard.Tag', 'org.neard.Device')):
        print('gone %s' % obj.rsplit('/', 1)[1], flush=True)


bus.signal_subscribe('org.neard', 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded',
                     '/', None, 0, added)
bus.signal_subscribe('org.neard', 'org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved',
                     '/', None, 0, removed)
try:
    call(ADAPTER, 'org.freedesktop.DBus.Properties', 'Set',
         GLib.Variant('(ssv)', ('org.neard.Adapter', 'Powered', GLib.Variant('b', True))))
except GLib.Error as e:
    if 'already enabled' not in e.message:
        raise
try:
    call(ADAPTER, 'org.neard.Adapter', 'StartPollLoop', GLib.Variant('(s)', ('Initiator',)))
except GLib.Error as e:
    if 'busy' not in e.message.lower():	# already polling
        raise
print('polling; hold a tag or card to the back of the phone', flush=True)
loop = GLib.MainLoop()
GLib.timeout_add_seconds(int(sys.argv[1]) if len(sys.argv) > 1 else 600, loop.quit)
GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, loop.quit)
GLibUnix.signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, loop.quit)
try:
    loop.run()
finally:
    try:
        call(ADAPTER, 'org.neard.Adapter', 'StopPollLoop')
    except GLib.Error:
        pass
