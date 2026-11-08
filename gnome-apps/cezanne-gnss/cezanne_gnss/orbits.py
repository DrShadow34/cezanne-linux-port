# Broadcast orbits: RINEX 3 navigation files (GPS, QZSS, Galileo, BeiDou
# Kepler records), satellite position / clock, coordinates.
import math
import time

import numpy as np

C = 299792458.0
MU = 3.986005e14
OMEGA_E = 7.2921151467e-5
LEAP = 18		# GPS - UTC, s (updated from the RINEX header)
FIELDS = ['af0', 'af1', 'af2', 'iode', 'crs', 'dn', 'm0', 'cuc', 'e', 'cus', 'sqa', 'toe', 'cic', 'omg0',
          'cis', 'i0', 'crc', 'w', 'omgd', 'idot', 'codes', 'week', 'l2p', 'sva', 'svh', 'tgd']


def gps_now():
    """GPS seconds since 1980-01-06 from the system clock"""
    return time.time() - 315964800 + LEAP


def gps_seconds(y, mo, d, h, mi, s):
    """seconds since the GPS epoch (1980-01-06) of a calendar time"""
    days = (np.datetime64(f'{y:04d}-{mo:02d}-{d:02d}') - np.datetime64('1980-01-06')).astype(int)
    return int(days) * 86400 + h * 3600 + mi * 60 + s


def rinex_nav(path):
    """Kepler records of GPS (G), QZSS (J), Galileo (E), BeiDou (C):
    {(sys, prn): [eph, ...]}; times in GPS seconds (BeiDou converted from
    BDT: weeks + 1356, + 14 s)"""
    global LEAP
    eph = {}
    lines = open(path).read().splitlines()
    end = next(k for k, l in enumerate(lines) if 'END OF HEADER' in l)
    for l in lines[:end]:
        if 'LEAP SECONDS' in l and l[:6].strip():
            LEAP = int(l[:6])
    i = end + 1
    while i < len(lines):
        # a record: its satellite line, then continuation lines (indented;
        # their number depends on the system and the RINEX version)
        l = lines[i]
        j = i + 1
        while j < len(lines) and lines[j][:1] == ' ':
            j += 1
        rec, i = lines[i:j], j
        sys_ = l[:1]
        if sys_ not in 'GJEC' or len(rec) < 8 or not l[1:3].strip().isdigit():
            continue
        rec = rec[:8]
        vals = []
        for k, r in enumerate(rec):
            body = r[23:] if k == 0 else r[4:]
            for j in range(0, 76 if k else 57, 19):	# blank (spare) fields keep their place
                f = body[j:j + 19].strip().replace('D', 'E')
                vals.append(float(f) if f else 0.0)
        prn = int(l[1:3])
        y, mo, d, h, mi, s = (int(x) for x in l[4:23].split())
        e = dict(zip(FIELDS, vals))
        e['toc'] = gps_seconds(y, mo, d, h, mi, s)
        if sys_ == 'C':
            if prn <= 5 or prn >= 59:		# GEO: other orbit model
                continue
            e['toc'] += 14
            e['toe_abs'] = (e['week'] + 1356) * 604800 + e['toe'] + 14
        else:
            e['toe_abs'] = e['week'] * 604800 + e['toe']
        if sys_ == 'E' and not int(e['codes']) & 1:	# Galileo: I/NAV (E1) records only
            continue
        lst = eph.setdefault((sys_, prn), [])
        if not lst or lst[-1]['toe_abs'] != e['toe_abs']:	# one record per toe
            lst.append(e)
    return eph


def best(ephs, t):
    return min(ephs, key=lambda e: abs(t - e['toe_abs']))


def usable(eph, key, t):
    """healthy record within 4 h of t, or None"""
    if key not in eph:
        return None
    e = best(eph[key], t)
    return e if abs(t - e['toe_abs']) < 4 * 3600 and not int(e['svh']) else None


def sat_pos(e, t):
    """ECEF position and clock offset (s) at GPS time t (s since epoch)"""
    a = e['sqa'] ** 2
    tk = t - e['toe_abs']
    n = math.sqrt(MU / a ** 3) + e['dn']
    m = e['m0'] + n * tk
    ea = m
    for _ in range(10):
        ea = m + e['e'] * math.sin(ea)
    nu = math.atan2(math.sqrt(1 - e['e'] ** 2) * math.sin(ea), math.cos(ea) - e['e'])
    phi = nu + e['w']
    du = e['cus'] * math.sin(2 * phi) + e['cuc'] * math.cos(2 * phi)
    dr = e['crs'] * math.sin(2 * phi) + e['crc'] * math.cos(2 * phi)
    di = e['cis'] * math.sin(2 * phi) + e['cic'] * math.cos(2 * phi)
    u, r = phi + du, a * (1 - e['e'] * math.cos(ea)) + dr
    i = e['i0'] + di + e['idot'] * tk
    x, y = r * math.cos(u), r * math.sin(u)
    om = e['omg0'] + (e['omgd'] - OMEGA_E) * tk - OMEGA_E * e['toe']	# toe in the system's own time
    pos = np.array([x * math.cos(om) - y * math.cos(i) * math.sin(om),
                    x * math.sin(om) + y * math.cos(i) * math.cos(om), y * math.sin(i)])
    dt = t - e['toc']
    clk = e['af0'] + e['af1'] * dt + e['af2'] * dt * dt - 4.442807633e-10 * e['e'] * e['sqa'] * math.sin(ea)
    return pos, clk - e['tgd']


def predicted(e, x, t):
    """geometric range (with Earth rotation), range rate, satellite clock
    and position for a signal received at x at GPS time t"""
    tau = 0.075
    for _ in range(3):
        s, clk = sat_pos(e, t - tau)
        th = OMEGA_E * tau
        s = np.array([s[0] * math.cos(th) + s[1] * math.sin(th), -s[0] * math.sin(th) + s[1] * math.cos(th), s[2]])
        rho = np.linalg.norm(s - x)
        tau = rho / C
    s2, _ = sat_pos(e, t - tau + 1)
    rate = np.linalg.norm(s2 - x) - np.linalg.norm(sat_pos(e, t - tau)[0] - x)
    return rho, rate, clk, s


def geo2ecef(lat, lon, h):
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    lat, lon = math.radians(lat), math.radians(lon)
    n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
    return np.array([(n + h) * math.cos(lat) * math.cos(lon), (n + h) * math.cos(lat) * math.sin(lon),
                     (n * (1 - e2) + h) * math.sin(lat)])


def ecef2geo(p):
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    lon = math.atan2(p[1], p[0])
    r = math.hypot(p[0], p[1])
    lat = math.atan2(p[2], r * (1 - e2))
    h = 0.0
    for _ in range(6):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        h = r / math.cos(lat) - n
        lat = math.atan2(p[2], r * (1 - e2 * n / (n + h)))
    return math.degrees(lat), math.degrees(lon), h


def enu(x):
    """rows: east, north, up unit vectors at x"""
    lat, lon, _ = ecef2geo(x)
    la, lo = math.radians(lat), math.radians(lon)
    return np.array([[-math.sin(lo), math.cos(lo), 0],
                     [-math.sin(la) * math.cos(lo), -math.sin(la) * math.sin(lo), math.cos(la)],
                     [math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)]])
