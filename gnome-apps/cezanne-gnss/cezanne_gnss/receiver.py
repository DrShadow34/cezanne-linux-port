# The receiver: boots both DSPs, sends the per-second host commands (epoch
# tick, time / frequency, per-satellite code phase / Doppler prediction,
# channel plan), reads the DSP's channel records and solves positions from
# the fully locked ones (see NOTES "GNSS").
#
# Clock: the receiver's oscillator offset (Hz at L1; -200..+70 on this
# phone, with temperature) starts at the saved value with a wide Doppler
# window; the locked channels' carrier Doppler then measures it together
# with the receiver's velocity (class Doppler), which also smooths the
# positions and gives speed / course.
# Epoch timing: FE02 predicts each satellite's code phase for the DSP's
# last epoch (tick in FE28); its GPS time comes from the locked channels
# (transmit time + flight time), and once it is known the code window
# narrows to a few chips - that is what makes the DSP lock the weak
# satellites within seconds.
import json
import os
import struct
import threading
import time

import numpy as np

from . import aid, atmo, dsp, nav
from .orbits import C, OMEGA_E, best, ecef2geo, enu, gps_now, predicted, rinex_nav, sat_pos, usable

CARRIER = {'L1': 1575.42e6, 'L5': 1176.45e6}
CODES = {0x01: 'G', 0x20: 'C'}		# positions from GPS + BeiDou (Galileo: aided only so far)
MIN_CN0 = 15
# setup burst after the boot, as the stock software sends it (GLP off)
SETUP = [
    ('L5', 0xfe05, '50000000000000000000000000'),
    ('L1', 0xfe05, '3200000100'), ('L5', 0xfe05, '3200000100'),
    ('L1', 0xfe05, '0a0000000000000000'), ('L1', 0xfe05, '460200010beeff986e'),
    ('L1', 0xfe05, '460100010ae1ffa470'), ('L1', 0xfe05, '3600000000'),
    ('L1', 0xfe05, '2a00000000'), ('L1', 0xfe05, '0f0000e803'), ('L1', 0xfe05, '1500000200'),
    ('L5', 0xfe05, '460000010b98ffcdcc'), ('L5', 0xfe05, '3600000000'),
    ('L5', 0xfe05, '2a00000000'), ('L5', 0xfe05, '0f0000e803'), ('L5', 0xfe05, '1500000200'),
    ('L1', 0xfe04, 'ffffffff00000f00ffffffff00007f0000000000f93fffffffffffffff7fff3f'),
    ('L5', 0xfe04, 'ffffffff00000f00ffffffff00007f0000000000ffffffffffffff7f'),
    ('L1', 0xfe08, '10001000'), ('L5', 0xfe08, '10001000'), ('L1', 0xfe08, '1800'),
]
# oscillator model points (FE10) and FE11, constant copies from the stock
# software (capture 4); the measured clock absorbs what they do not cover
FE10 = {'L1': '01964f0000ffff85fab6520000ffffc5fbd6550000ffff0bfd',
        'L5': '01964f0000ffffedfbb6520000ffffdafcd6550000ffffcdfd'}
FE11 = '010000fc3a000000000000'


def mix32(p, i):
    """32-bit field stored as halves, high half first"""
    return (p[i + 2] | p[i + 3] << 8) | (p[i] << 16) | (p[i + 1] << 24)


def s32(v):
    return v - (1 << 32) if v >= 1 << 31 else v


def transmit_time(p, week0):
    """GPS time of transmission of a fully locked FE29 record: p[21..24]
    time unit (GPS: 1.5 s, BeiDou: BDT s), p[25..30] code chips in it"""
    cp = mix32(p, 25) + (p[29] | p[30] << 8) / 65536
    if p[2] == 1:
        return week0 + 1.5 * mix32(p, 21) + cp / 1.023e6
    return week0 + mix32(p, 21) + cp / 2.046e6 + 14


class Doppler:
    """carrier Doppler of the fully locked channels (-d/dt of the
    accumulated carrier phase, mix32 p[35..38] in cycles; dt from the
    records' transmit times, arrival times jitter too much) against the
    orbit Doppler of a receiver at rest: (measured - orbit) / carrier =
    the oscillator's frequency offset (per constellation: BeiDou reads ~2
    ppb off GPS) + u.v / c for a receiver moving at v. One epoch's samples
    give velocity and offset (least squares, cycle slips dropped); with
    too few satellites the receiver is taken as at rest."""

    def __init__(self):
        self.prev, self.samples, self.known = {}, [], False

    def record(self, key, e, p, ttx, pos):
        adr = s32(mix32(p, 35))
        last = self.prev.get(key)
        self.prev[key] = (ttx, adr)
        if last and 0.5 < ttx - last[0] < 1.5:
            f = aid.SYSTEMS[key[0]][1]
            rho, rate, clk, sat = predicted(e, pos, ttx)
            meas = -(adr - last[1]) / (ttx - last[0])
            self.samples.append((key[0], (meas + rate * f / C) / f, (sat - pos) / rho))

    def update(self):
        """(oscillator offset in Hz at L1, ECEF velocity m/s or None) for
        the last epoch, or None"""
        s, self.samples = self.samples, []
        if len(s) < (1 if self.known else 2):
            return None
        self.known = True
        systems = sorted({q for q, _, _ in s})
        keep = list(range(len(s)))
        while len(keep) >= 5 + len(systems):
            h = np.array([list(s[i][2] / C) + [1.0 if s[i][0] == q else 0.0 for q in systems] for i in keep])
            y = np.array([s[i][1] for i in keep])
            x = np.linalg.lstsq(h, y, rcond=None)[0]
            r = np.abs(y - h @ x)
            if r.max() < 5e-9:
                ref = 'G' if 'G' in systems else systems[0]
                return x[3 + systems.index(ref)] * 1575.42e6, x[:3]
            del keep[int(np.argmax(r))]
        ref = [v for q, v, _ in s if q == 'G'] or [v for q, v, _ in s]
        return float(np.median(ref)) * 1575.42e6, None


def solve(eph, obs, x, iono=None, atmos=True):
    """position from full transmit times: unknowns x, y, z and one clock
    per constellation; ionosphere (broadcast Klobuchar params, see
    atmo.iono) and troposphere delays modelled once near the ground;
    drops the worst satellite while a residual exceeds 100 m.
    obs: {(sys, prn): (t_tx, cn)}. Returns a fix dict or None."""
    med = np.median([t for t, _ in obs.values()])
    obs = {k: v for k, v in obs.items() if abs(v[0] - med) < 0.1}
    while len(obs) >= 3 + len({k[0] for k in obs}):
        systems = sorted({k[0] for k in obs})
        trx = max(t for t, _ in obs.values()) + 0.07
        clk = {q: 0.0 for q in systems}
        y = x.copy()
        for it in range(10):
            rows, res = [], []
            near = atmos and abs(np.linalg.norm(y) - 6.37e6) < 2e5
            if near:
                lat, lon, hgt = ecef2geo(y)
                lat, lon = np.radians(lat), np.radians(lon)
                r_enu = enu(y)
            for k, (ttx, cn) in obs.items():
                s, sclk = sat_pos(best(eph[k], ttx), ttx)
                th = OMEGA_E * (trx - ttx)
                s = np.array([s[0] * np.cos(th) + s[1] * np.sin(th), -s[0] * np.sin(th) + s[1] * np.cos(th), s[2]])
                rho = np.linalg.norm(s - y)
                u = (s - y) / rho
                delay = 0.0
                if near:
                    v = r_enu @ u
                    el, az = np.arcsin(v[2]), np.arctan2(v[0], v[1])
                    delay = atmo.iono(iono, ttx, lat, lon, az, el, aid.SYSTEMS[k[0]][1]) + atmo.tropo(lat, hgt, el)
                res.append(C * (trx - (ttx - sclk)) - (rho + clk[k[0]] + delay))
                rows.append([-u[0], -u[1], -u[2]] + [1.0 if k[0] == q else 0.0 for q in systems])
            h = np.array(rows)
            d = np.linalg.lstsq(h, np.array(res), rcond=None)[0]
            y = y + d[:3]
            for i, q in enumerate(systems):
                clk[q] += d[3 + i]
            if np.linalg.norm(d[:3]) > 1e7:
                return None
        res = np.abs(np.array(res))
        spare = len(obs) - 3 - len(systems)
        if res.max() < 100 or spare == 0:
            q = np.linalg.inv(h.T @ h)
            r = enu(y)
            qe = r @ q[:3, :3] @ r.T
            lat, lon, alt = ecef2geo(y)
            return dict(t=trx - clk[systems[0]] / C, x=y, lat=lat, lon=lon, alt=alt, used=sorted(obs),
                        cn={k: v[1] for k, v in obs.items()}, spare=spare,
                        rms=float(np.sqrt(np.mean(res ** 2))),
                        hdop=float(np.sqrt(qe[0, 0] + qe[1, 1])), vdop=float(np.sqrt(qe[2, 2])))
        del obs[list(obs)[int(np.argmax(res))]]
    return None


def checked(fix, pos, pos_unc, rms=75):
    """a fix good enough to report (and with rms=30 to aid from): spare
    satellites, residuals below rms (m; indoors multipath gives 10..50),
    sane height, within reach of the aiding position (or, away from it,
    two spare satellites)"""
    if not fix or fix['rms'] >= rms or not -500 < fix['alt'] < 9000:
        return False
    near = np.linalg.norm(fix['x'] - pos) < max(50e3, 2 * pos_unc)
    return fix['spare'] >= (1 if near else 2)


# power modes: continuous tracking, or at rest a duty cycle: after STILL s
# below 0.5 m/s the DSPs idle (booted, no satellites: costs nothing
# measurable, tracking ~20..30 mA) for SLEEP s, then track again until
# BURST checked fixes (at most BURST_MAX s); moved -> continuous again.
# Idling keeps the DSP's epoch counter, so the aiding after it is narrow.
MODES = {'continuous': None,
         'balanced': dict(still=60, sleep=30, burst=5, burst_max=90),
         'low': dict(still=30, sleep=120, burst=3, burst_max=60)}

# cold start without any orbit data: the stock software's blind search list
# (FE03 list 1: GPS, QZSS, GLONASS, BeiDou, Galileo) with FE01 status 0x0400
# ("time not known") and no FE02 at all; with status 0x0421 and no FE02 the
# DSP does not search
BLIND = {'L1': bytes.fromhex('016201010e011d011701020115010b0110011c010501160112010601130107010901190108010d011e010a01030104011b011a0118010f011f0111010c012001c108c208c308c408c508c608c7080110021003100410051006100710081009100a100b100c100d100e1006200720082009200a200b200c200d200e20012002200320042005200f2010201120122013201420152016201720182019201a201b201c201d201e201f20202021202220232024202520034006400740024009400440054008400140'),
         'L5': bytes.fromhex('014801010e011d011701020115010b0110011c010501160112010601130107010901190108010d011e010a01030104011b011a0118010f011f0111010c012001c108c208c308c70806200720082009200a200b200c200d200e20012002200320042005200f2010201120122013201420152016201720182019201a201b201c201d201e201f20202021202220232024202520')}
DECODED_DAYS = 7	# decoded ephemerides kept (aiding only beyond 4 h)


class Receiver:
    """run(stop) until the event is set; on_fix(fix) for every checked fix
    (position smoothed with the Doppler velocity: lat, lon, alt, speed m/s,
    course deg, acc = horizontal accuracy estimate m), on_state(text) for
    progress. Orbits: the downloaded file nav (may be None or missing:
    offline) merged with the ephemerides the receiver decodes from the
    satellites' own messages, kept in cache/decoded.json together with the
    ionosphere coefficients."""

    def __init__(self, nav_file, pos, pos_unc=100e3, clock_hz=0.0, firmware='/usr/lib/firmware/mediatek/cezanne',
                 on_fix=None, on_state=None, cache=None, mode='balanced'):
        """pos: rough ECEF position, pos_unc: its uncertainty (m); mode: a
        MODES key"""
        self.mode = MODES.get(mode, MODES['balanced'])
        self.nav, self.pos, self.clock_hz = nav_file, np.array(pos, dtype=float), clock_hz
        self.pos_unc = pos_unc
        self.firmware = firmware
        self.on_fix = on_fix or (lambda fix: None)
        self.on_state = on_state or (lambda text: None)
        self.eph, self.nav_mtime = {}, None
        self.track, self.course, self.vel, self.vel_age = None, None, None, 0
        self.cache = cache and os.path.join(cache, 'decoded.json')
        self.decoded, self.iono = {}, None
        self.gps, self.bds = nav.GpsNav(), nav.BdsNav()
        try:
            c = json.load(open(self.cache))
            self.decoded = {(k[0], int(k[1:])): v for k, v in c.get('eph', {}).items()}
            self.iono = tuple(c['iono']) if c.get('iono') else None
        except (TypeError, OSError, ValueError, KeyError):
            pass

    def _orbits(self):
        """(re)load the downloaded file when it changed and merge in the
        decoded ephemerides"""
        try:
            m = os.stat(self.nav).st_mtime if self.nav else None
        except OSError:
            m = None
        if m == self.nav_mtime and self.eph:
            return
        eph = rinex_nav(self.nav) if m else {}
        self.nav_mtime = m
        for key, recs in self.decoded.items():
            have = {r['toe_abs'] for r in eph.get(key, [])}
            eph.setdefault(key, []).extend(r for r in recs if r['toe_abs'] not in have)
        self.eph = eph

    def _decoded(self, key, e, confirmed=True):
        """a new ephemeris from the satellite: use it, keep it in the cache
        (a confirmed one replaces one of the same toe from single words)"""
        recs = self.decoded.setdefault(key, [])
        same = [r for r in recs if r['toe_abs'] == e['toe_abs']]
        if same and (not confirmed or same[0].get('confirmed', True)):
            return
        for r in same:
            recs.remove(r)
            if r in self.eph.get(key, []):
                self.eph[key].remove(r)
        e['confirmed'] = confirmed
        recs.append(e)
        now = gps_now()
        for k in list(self.decoded):
            self.decoded[k] = [r for r in self.decoded[k] if now - r['toe_abs'] < DECODED_DAYS * 86400]
        have = self.eph.setdefault(key, [])
        if not any(r['toe_abs'] == e['toe_abs'] for r in have):
            have.append(e)
        self._save()

    def _save(self):
        if not self.cache:
            return
        try:
            with open(self.cache + '.new', 'w') as f:
                json.dump(dict(eph={f'{k[0]}{k[1]}': v for k, v in self.decoded.items()},
                               iono=list(self.iono) if self.iono else None), f)
            os.replace(self.cache + '.new', self.cache)
        except OSError:
            pass

    def _plausible(self, key, e):
        """an ephemeris from words received once, checked against an older
        record of the satellite (days-old broadcast orbits are within ~1.5
        km and well under a microsecond; one wrong bit in a significant
        field moves the satellite much further)"""
        old = self.eph.get(key)
        now = gps_now()
        if not old or abs(e['toe_abs'] - now) > 3 * 3600:
            return False
        ref = best(old, now)
        if abs(now - ref['toe_abs']) > aid.STALE_CODE:	# beyond 3 days the range drifts too far
            return False
        for t in (now, now + 1800):
            (p1, c1), (p0, c0) = sat_pos(e, t), sat_pos(ref, t)
            if np.linalg.norm(p1 - p0) > 3000 or abs(c1 - c0) * C > 1000:
                return False
        return True

    def _navmsg(self, mid, p):
        """navigation message frames: ephemerides and ionosphere"""
        week = int(gps_now() // 604800)
        src = self.gps if mid == 0xfe2b else self.bds
        prn = src.add(p)
        if prn:
            key = ('G' if mid == 0xfe2b else 'C', prn)
            e, confirmed = src.ephemeris(prn, week), True
            if not e:
                confirmed = False
                cands = [src.ephemeris(prn, week, confirmed=False)]
                if src is self.bds:
                    cands.append(src.ephemeris(prn, week, confirmed=False, latest=True))
                e = next((c for c in cands if c and self._plausible(key, c)), None)
            if e:
                self._decoded(key, e, confirmed)
        iono = ('G',) + tuple(self.gps.iono) if self.gps.iono else \
            ('C',) + tuple(self.bds.iono) if self.bds.iono and not (self.iono and self.iono[0] == 'G') else None
        if iono and iono != self.iono:
            self.iono = iono
            self._save()

    def fe01(self, k, link, cold=False):
        """time / frequency: s16 receiver clock offset (Hz at the link's
        carrier), u16 0, u16 its uncertainty, u32 the previous FE00 tick
        (high half first), u16 status (0x0421; cold start 0x0400 with the
        stock software's uncertainty 7092), u8 0, u16 GPS time of week in
        1.5 s units (low bits), u8 0, u16 0"""
        tick = 1014 + 1000 * (k - 1) if k else 0
        unit = int(((gps_now() % 604800) // 1.5)) & 0xffff
        return struct.pack('<hHHHHHBHBH', int(round(self.clock_hz * CARRIER[link] / CARRIER['L1'])), 0,
                           7092 if cold else 4305, tick >> 16, tick & 0xffff, 0x0400 if cold else 0x0421,
                           0, unit, 0, 0)

    def _smooth(self, fix, vel):
        """velocity-aided smoothing (gain K per epoch; reset on a jump or a
        gap) and speed / course from the velocity averaged over ~2 s: at
        rest indoors single epochs read 0.35 m/s (95 %: 1.2 m/s), so below
        0.5 m/s the speed is 0 and the course follows only above 1.5 m/s"""
        K = 0.25
        x, t = fix['x'], fix['t']
        if self.track is not None and t - self.track[1] < 5:
            pred = self.track[0] + (vel if vel is not None else 0) * (t - self.track[1])
            if np.linalg.norm(x - pred) < 150:
                x = pred + K * (x - pred)
        self.track = (x, t)
        fix['lat'], fix['lon'], fix['alt'] = ecef2geo(x)
        fix['xs'] = x
        fix['acc'] = fix['hdop'] * max(fix['rms'], 3.0) * (0.7 if vel is not None else 1.0)
        fix['speed'] = None
        if vel is not None:
            self.vel = vel if self.vel is None else 0.5 * (self.vel + vel)
            self.vel_age = 0
        else:
            self.vel_age += 1		# an epoch without enough Doppler: keep the last few s
            if self.vel_age > 5:
                self.vel = None
        if self.vel is not None:
            v = enu(x) @ self.vel
            speed = float(np.hypot(v[0], v[1]))
            fix['speed'] = speed if speed >= 0.5 else 0.0
            if speed > 1.5:
                self.course = float(np.degrees(np.arctan2(v[0], v[1])) % 360)
        fix['course'] = self.course

    def run(self, stop, duration=None):
        self._orbits()
        links = {}
        try:
            for name in ('L5', 'L1'):		# L5 must boot first
                links[name] = dsp.Link(name)
                image = open(f'{self.firmware}/gnss-{name.lower()}.bin', 'rb').read()
                if not dsp.boot(links[name], image, verbose=False):
                    self.on_state(f'{name} DSP boot failed')
                    return False
            for name, mid, data in SETUP:
                links[name].send(mid, bytes.fromhex(data))
            self.on_state('DSPs running')
            self._loop(links, stop, duration)
            return True
        finally:
            for ln in links.values():
                ln.close()

    def _loop(self, links, stop, duration):
        doppler = Doppler()
        epochs = {}		# DSP tick -> host GPS time of its FE28
        timed = None	# (tick, GPS time) of the last epoch timed by locked channels
        epoch = None	# the epoch being received: [tick, host monotonic time of its FE28, obs, receive times]
        fix, nlocked = None, 0
        start = time.monotonic()
        k = 0
        # duty cycle: idle until (monotonic), recent (time, speed) at rest,
        # burst: [fixes so far, started at, position before the idle,
        # its positions, fast fixes in a row]
        idle_until, still_since, burst, rest = 0.0, [], None, [None]

        def finish():
            """an epoch's records are in (they follow its FE28 within ms):
            time it, velocity / clock, position"""
            nonlocal timed, fix, nlocked
            tick, _, obs, rx = epoch
            fresh = [t for t, f in rx if f]
            if rx:		# fresh orbits time the epoch to ~a chip, days-old ones to ~1.5 km
                timed = (tick, float(np.median(fresh or [t for t, _ in rx])), 0.0 if fresh else 1500.0)
            d = doppler.update()
            vel = None
            if d:
                self.clock_hz, vel = d
            nlocked = len(obs)
            fix = solve(eph, obs, self.pos, self.iono) if obs else None
            if checked(fix, self.pos, self.pos_unc):
                fix['clock_hz'] = self.clock_hz
                self._smooth(fix, vel)
                if checked(fix, self.pos, self.pos_unc, 30):
                    self.pos, self.pos_unc = fix['xs'], 30.0
                self.on_fix(fix)
                duty(fix)

        def duty(fix):
            """at rest long enough: idle; after an idle, a short burst"""
            nonlocal idle_until, still_since, burst
            m = self.mode
            if not m:
                return
            if burst is not None:
                # moved: two fixes in a row faster than 1.5 m/s, or at the
                # burst's end its median position away from the one before
                # the pause, horizontally (indoors single fixes scatter
                # 20..40 m, heights ~100 m)
                burst[4] = burst[4] + 1 if (fix['speed'] or 0) > 1.5 else 0
                if fix['hdop'] < 3 and fix['spare'] >= 2:	# right after reacquiring: few sats, weak geometry
                    burst[0] += 1
                    burst[3].append(fix['xs'])
                why = burst[4] >= 2 and f'{fix["speed"]:.1f} m/s'
                if not why and burst[0] >= m['burst']:
                    d = np.linalg.norm((enu(burst[2]) @ (np.median(burst[3], axis=0) - burst[2]))[:2])	# horizontal
                    why = d > max(75.0, 3 * fix['acc']) and f'{d:.0f} m away'
                    if not why:
                        rest[0] = 0.5 * (rest[0] + np.median(burst[3], axis=0))
                        burst, idle_until = None, time.monotonic() + m['sleep']
                if why:
                    burst, still_since[:] = None, []	# moving: continuous
                    self.on_state(f'moving ({why}): continuous tracking')
                return
            # at rest: the last STILL s of speeds, under 20 % of them 0.5 m/s
            # or more (indoors single readings reach ~0.9 m/s), none 2 m/s
            still_since.append((fix['t'], fix['speed'], fix['xs']))
            while still_since and fix['t'] - still_since[0][0] > m['still']:
                still_since.pop(0)
            sp = [v for _, v, _ in still_since]
            if fix['t'] - still_since[0][0] >= m['still'] * 0.9 and None not in sp and \
               sum(v >= 0.5 for v in sp) < 0.2 * len(sp) and max(sp) < 2.0:
                rest[0] = np.median([x for _, _, x in still_since], axis=0)	# where it rests
                idle_until, still_since[:] = time.monotonic() + m['sleep'], []
                self.on_state(f'at rest: tracking {m["sleep"]} s off, then {m["burst"]} fixes')

        while not stop.is_set() and (duration is None or time.monotonic() - start < duration):
            if k % 60 == 0:
                self._orbits()
            eph = self.eph
            week0 = (int(gps_now()) // 604800) * 604800	# GPS weeks start at the epoch
            # GPS time of the DSP's last epoch (the previous FE00 tick, also
            # in FE01), which the aiding refers to: from the last timed
            # epoch, else from the system time its FE28 arrived at
            last = 1014 + 1000 * (k - 1)
            if timed and 0 <= last - timed[0] < 900000:
                # the DSP's ms counter runs on the receiver oscillator (its
                # offset ~1e-8: ~1 chip per 100 s since the timed epoch)
                dt = (last - timed[0]) / 1000
                t_ep, known = timed[1] + dt, True
                t_unc = timed[2] + dt * (1e-8 + abs(self.clock_hz) / 1575.42e6) * C
            else:
                t_ep, known, t_unc = epochs.get(last, gps_now() - 0.1), False, 0.0
            idle = time.monotonic() < idle_until
            if not idle and idle_until and burst is None:
                burst, idle_until = [0, time.monotonic(), rest[0], [], 0], 0.0
            if burst is not None and time.monotonic() - burst[1] > self.mode['burst_max']:
                burst, idle_until = None, time.monotonic() + self.mode['sleep']	# no fix: try later
                idle = True
            plan = [] if idle else aid.fe02(eph, self.pos, t_ep, self.clock_hz, known, self.pos_unc + t_unc)
            cold = not plan and not idle	# no orbit data at all (offline, nothing decoded yet)
            for name in ('L1', 'L5'):
                ln = links[name]
                ln.send(0xfe05, bytes.fromhex('3400000100'))
                tick_k = 1014 + 1000 * k		# DSP epoch tick, ms (32 bit, high half first)
                ln.send(0xfe00, struct.pack('<HH', tick_k >> 16, tick_k & 0xffff))
                ln.send(0xfe10, bytes.fromhex(FE10[name]))
                ln.send(0xfe11, bytes.fromhex(FE11))
                ln.send(0xfe01, self.fe01(k, name, cold))
                if name == 'L1':
                    for _, _, payload in plan:
                        ln.send(0xfe02, payload)
                    ln.send(0xfe03, bytes([0, len(plan)]) +
                            b''.join(bytes([prn, aid.SYSTEMS[s][0]]) for s, prn, _ in plan))
                if cold:
                    if name == 'L5':
                        ln.send(0xfe03, bytes([0, 0]))
                    ln.send(0xfe03, BLIND[name])
                elif name == 'L1':		# (idle: both lists empty)
                    ln.send(0xfe03, bytes([1, 0]))
                ln.send(0xfe1c)
                ln.send(0xfe05, bytes.fromhex('2900'))
            k += 1
            # the DSP's reports until the next second: per epoch an FE28
            # (its tick) then its FE29 channel records; fully locked channels
            # (state 20, byte 6 = 64) time the epoch and give the position
            end = start + k
            while time.monotonic() < end and not stop.is_set():
                time.sleep(0.05)
                links['L5'].take()
                for _, mid, seq, p in links['L1'].take():
                    if mid in (0xfe2b, 0xfe4b):
                        self._navmsg(mid, p)
                        continue
                    if mid == 0xfe28:
                        if epoch:
                            finish()
                        tick = (p[3] | p[4] << 8) | (p[1] | p[2] << 8) << 16
                        epochs[tick] = gps_now()
                        epochs.pop(tick - 5000, None)
                        epoch = [tick, time.monotonic(), {}, []]
                        continue
                    if mid != 0xfe29 or len(p) < 39 or not epoch:
                        continue
                    sys_ = CODES.get(p[2])
                    if not sys_ or p[5] != 20 or p[6] != 64 or p[3] < MIN_CN0:
                        continue
                    ttx = transmit_time(p, week0)
                    if ttx - t_ep > 302400:		# week rollover between now and the record
                        ttx -= 604800
                    elif t_ep - ttx > 302400:
                        ttx += 604800
                    key = (sys_, p[1])
                    e = usable(eph, key, ttx)
                    if e:
                        epoch[2][key] = (ttx, p[3])
                    elif eph.get(key):		# days-old orbits: Doppler / timing only
                        e = best(eph[key], ttx)
                        if abs(ttx - e['toe_abs']) > aid.STALE or int(e['svh']):
                            continue
                    else:
                        continue
                    if p[3] >= 18:
                        doppler.record(key, e, p, ttx, self.pos)
                        if key in epoch[2] or abs(ttx - e['toe_abs']) < aid.STALE_CODE:
                            rho, rate, clk, s_ = predicted(e, self.pos, ttx + 0.075)
                            epoch[3].append((ttx + rho / C - clk, key in epoch[2]))
                if epoch and time.monotonic() - epoch[1] > 0.3:
                    finish()
                    epoch = None
            if k % 10 == 0:
                sol = f'rms {fix["rms"]:.0f} m, {fix["spare"]} spare' if fix else 'no solution'
                self.on_state(f'{k} s: {"idle" if idle else "cold search" if cold else f"{len(plan)} planned"}, '
                              f'{nlocked} locked ({sol}), '
                              f'clock {self.clock_hz:+.0f} Hz, epoch {"timed" if known else "from system time"}, '
                              f'{len(self.decoded)} decoded')
