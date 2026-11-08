# Per-satellite acquisition aiding (FE02) from broadcast orbits.
# FE02 payload (20 B, sent once a second per satellite in view; layout
# checked on stock-software capture 4, all satellites agree to < 1 us): sv,
# constellation, u16 (GPS 4, BeiDou / Galileo 6, GLONASS 1), then the
# predicted transmit time (satellite clock) of the signal that reaches the
# receiver at the DSP's last epoch (the tick in FE28 / FE01): u16 time unit
# (low bits; GPS: time of week / 1.5 s, BeiDou: BDT s, Galileo: s, GLONASS:
# s of day) + u32 code chips within that unit (high half first; 1.023 /
# 2.046 / 1.023 / 0.511 Mcps) - i.e. the code phase prediction; s16 Doppler
# incl. receiver clock offset (Hz), u16 Doppler window (Hz), u16 code
# window (chips: whole code while the epoch time is not known, 4..5 once
# it is), u16 flags (0x0001; 0x0301 / 0x0302 with the code known), u16 0x41.
import struct

import numpy as np

from .orbits import C, best, predicted, usable

# older ephemerides still aid: Doppler within 1.4 Hz and range within 1.4 km
# of fresh ones after 3 days (narrow windows, epoch timing), Doppler within
# 23 Hz but range up to 63 km after 7 days (wide code window)
STALE = 7 * 86400
STALE_CODE = 3 * 86400

# constellation: (FE02 / FE03 / FE29 code, carrier Hz, chip rate, FE02 time
# unit s, offset of its time scale to GPS time s)
SYSTEMS = {'G': (0x01, 1575.42e6, 1.023e6, 1.5, 0), 'E': (0x04, 1575.42e6, 1.023e6, 1.0, 0),
           'C': (0x20, 1561.098e6, 2.046e6, 1.0, -14)}
# per constellation: u16 at 2, Doppler window (Hz), code window (chips),
# flags, flags; before the epoch time is known (whole code searched) and
# once it is (as the stock software does from its first fix)
WIDE = {'G': (4, 260, 512, 1, 0x41), 'C': (6, 260, 1023, 1, 0x41), 'E': (6, 260, 2046, 1, 0x01)}
NARROW = {'G': (4, 68, 4, 0x0301, 0x41), 'C': (6, 68, 5, 0x0302, 0x41), 'E': (6, 68, 5, 0x0301, 0x41)}


def fe02(eph, pos, t_rx, clock_hz=0.0, known=False, pos_unc=0.0, systems='GEC'):
    """FE02 payloads for the satellites above ~3 deg: [(sys, prn, payload)];
    t_rx: GPS time of the DSP epoch the prediction is for, clock_hz:
    receiver clock offset at L1, known: t_rx is known (narrow code /
    Doppler windows); pos_unc: the uncertainty of pos (m), widens the code
    window once t_rx is known and the Doppler window (~1 Hz / km) before"""
    out = []
    up = pos / np.linalg.norm(pos)
    for (sys_, prn) in sorted(eph):
        if sys_ not in systems:
            continue
        e = usable(eph, (sys_, prn), t_rx)
        fresh = e is not None
        if not fresh:		# a stale (decoded) record: Doppler good, code not
            e = best(eph[(sys_, prn)], t_rx)
            if abs(t_rx - e['toe_abs']) > STALE or int(e['svh']):
                continue
        rho, rate, clk, s = predicted(e, pos, t_rx)
        if np.dot((s - pos) / rho, up) < 0.05:
            continue
        code, freq, rate_c, unit_s, offset = SYSTEMS[sys_]
        dopp = -rate * freq / C + clock_hz * freq / 1575.42e6
        tt = (t_rx - rho / C + clk + offset) % 604800	# transmit time, satellite clock
        unit = int(tt // unit_s)
        chips = int(round((tt - unit * unit_s) * rate_c)) % int(unit_s * rate_c)
        near = fresh or abs(t_rx - e['toe_abs']) < STALE_CODE
        a, dunc, cunc, f1, f2 = (NARROW if known and near else WIDE)[sys_]
        if known and near:		# days-old orbits: range within ~1.5 km (measured), add 2 km
            cunc = min(WIDE[sys_][2], cunc + int(np.ceil((pos_unc + (0 if fresh else 2000)) * rate_c / C)))
        else:
            dunc = min(1000, dunc + int(pos_unc / 1000))
        out.append((sys_, prn, struct.pack('<BBHHHHhHHHH', prn, code, a, unit & 0xffff, chips >> 16,
                                           chips & 0xffff, int(round(dopp)), dunc, cunc, f1, f2)))
    return out

