# Atmospheric delays of a pseudorange: ionosphere from the broadcast
# Klobuchar coefficients (GPS model, IS-GPS-200 20.3.3.5.2.5; BeiDou's
# variant, BDS-SIS-ICD 5.2.4.7, when only BeiDou's are known) and
# troposphere (Saastamoinen, standard atmosphere, 70 % humidity).
import math

C = 299792458.0


def iono_gps(alpha, beta, t, lat, lon, az, el):
    """L1 group delay (m); t = GPS seconds, lat / lon / az / el in rad"""
    e = el / math.pi
    psi = 0.0137 / (e + 0.11) - 0.022
    phi = lat / math.pi + psi * math.cos(az)
    phi = max(-0.416, min(0.416, phi))
    lam = lon / math.pi + psi * math.sin(az) / math.cos(phi * math.pi)
    phim = phi + 0.064 * math.cos((lam - 1.617) * math.pi)
    tt = (43200 * lam + t) % 86400
    f = 1 + 16 * (0.53 - e) ** 3
    amp = max(0.0, sum(a * phim ** i for i, a in enumerate(alpha)))
    per = max(72000.0, sum(b * phim ** i for i, b in enumerate(beta)))
    x = 2 * math.pi * (tt - 50400) / per
    d = 5e-9 + (amp * (1 - x * x / 2 + x ** 4 / 24) if abs(x) < 1.57 else 0)
    return C * f * d


def iono_bds(alpha, beta, t, lat, lon, az, el):
    """B1I group delay (m), BeiDou's Klobuchar variant (geographic latitude
    of the pierce point at 375 km, BDT seconds t)"""
    r = 6378.0 / (6378.0 + 375.0)
    psi = math.pi / 2 - el - math.asin(r * math.cos(el))
    phim = math.asin(math.sin(lat) * math.cos(psi) + math.cos(lat) * math.sin(psi) * math.cos(az))
    lamm = lon + math.asin(math.sin(psi) * math.sin(az) / math.cos(phim))
    tt = (t + lamm * 43200 / math.pi) % 86400
    p = abs(phim / math.pi)
    a2 = max(0.0, sum(a * p ** i for i, a in enumerate(alpha)))
    a4 = min(172800.0, max(72000.0, sum(b * p ** i for i, b in enumerate(beta))))
    iz = 5e-9 + (a2 * math.cos(2 * math.pi * (tt - 50400) / a4) if abs(tt - 50400) < a4 / 4 else 0)
    return C * iz / math.sqrt(1 - (r * math.cos(el)) ** 2)


def iono(params, t, lat, lon, az, el, freq):
    """delay (m) at carrier freq; params: ('G' or 'C', alpha, beta) or None"""
    if not params or el <= 0:
        return 0.0
    kind, alpha, beta = params
    if kind == 'G':
        return iono_gps(alpha, beta, t, lat, lon, az, el) * (1575.42e6 / freq) ** 2
    return iono_bds(alpha, beta, t - 14, lat, lon, az, el) * (1561.098e6 / freq) ** 2


def tropo(lat, h, el):
    """slant delay (m), Saastamoinen with a standard atmosphere"""
    if el <= 0 or not -100 < h < 1e4:
        return 0.0
    h = max(h, 0.0)
    pres = 1013.25 * (1 - 2.2557e-5 * h) ** 5.2568
    temp = 15.0 - 6.5e-3 * h + 273.16
    e = 6.108 * 0.7 * math.exp((17.15 * temp - 4684.0) / (temp - 38.45))
    z = math.pi / 2 - el
    trph = 0.0022768 * pres / (1 - 0.00266 * math.cos(2 * lat) - 0.00028 * h / 1e3) / math.cos(z)
    trpw = 0.002277 * (1255.0 / temp + 0.05) * e / math.cos(z)
    return trph + trpw
