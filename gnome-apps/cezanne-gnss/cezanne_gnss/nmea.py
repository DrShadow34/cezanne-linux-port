# NMEA 0183 sentences from a fix (GeoClue reads GGA and RMC).
import time

from . import orbits


def sentence(body):
    cs = 0
    for ch in body.encode():
        cs ^= ch
    return f'${body}*{cs:02X}\r\n'


def _utc(t_gps):
    t = round(t_gps + 315964800 - orbits.LEAP, 2)
    return time.gmtime(int(t)), t - int(t)


def _latlon(lat, lon):
    la, lo = abs(lat), abs(lon)
    return (f'{int(la):02d}{(la - int(la)) * 60:09.6f},{"N" if lat >= 0 else "S"},'
            f'{int(lo):03d}{(lo - int(lo)) * 60:09.6f},{"E" if lon >= 0 else "W"}')


def geoclue_hdop(acc):
    """the HDOP field carries our accuracy estimate (m): GeoClue turns HDOP
    into an accuracy by buckets (<= 1: 0 m, <= 2: 1 m, <= 5: 3 m, <= 10:
    50 m, <= 20: 100 m, else 300 m), so pick the bucket that fits"""
    for limit, hdop in ((2, 1.5), (6, 4.0), (75, 8.0), (200, 15.0)):
        if acc < limit:
            return hdop
    return 25.0


def gga(fix):
    """position, satellites used, accuracy as HDOP (see geoclue_hdop),
    height (ellipsoidal: no geoid model yet, so the separation field is 0)"""
    tm, frac = _utc(fix['t'])
    hms = time.strftime('%H%M%S', tm) + f'{frac:.2f}'[1:]
    return sentence(f'GPGGA,{hms},{_latlon(fix["lat"], fix["lon"])},1,{len(fix["used"]):02d},'
                    f'{geoclue_hdop(fix["acc"]):.1f},{fix["alt"]:.1f},M,0.0,M,,')


def rmc(fix):
    """time, date, position, speed (knots) and course (deg) from the Doppler
    velocity; empty fields make GeoClue derive them from position changes,
    which indoors jitter by tens of m, so the course is always given (the
    last one moving faster than 1 m/s, else 0)"""
    tm, frac = _utc(fix['t'])
    hms = time.strftime('%H%M%S', tm) + f'{frac:.2f}'[1:]
    speed = f'{fix["speed"] * 1.943844:.2f}' if fix.get('speed') is not None else ''
    course = f'{fix["course"] or 0.0:.1f}' if speed else ''
    return sentence(f'GPRMC,{hms},A,{_latlon(fix["lat"], fix["lon"])},{speed},{course},'
                    f'{time.strftime("%d%m%y", tm)},,,A')
