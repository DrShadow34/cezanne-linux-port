# Navigation messages decoded from the DSP's reports, so the receiver needs
# no downloaded orbits: ephemerides (orbits.py record format), ionosphere
# parameters (Klobuchar) and GPS - UTC.
#
# BeiDou D1 (MEO / IGSO, BDS-SIS-ICD): FE4A = batch header every 6 s (u8
# number of FE4B, u32 tick); FE4B = one subframe: u8, u16, u8 PRN, u8, u8,
# u8, then 10 slots of 4 bytes, each a 32-bit value stored as halves (high
# half first, little-endian) holding 30 received bits at bits 2..31. The
# slots follow the DSP's bit sync, not necessarily the word boundaries
# (seen: words starting at bit 0 or 2 of a slot's 30 bits). Slot 1 is
# stored as received; slots 2..10 de-interleaved as words 2..10 would be
# ([odd bits 1..11][even bits 1..11][odd 12..15][even 12..15]), so the
# decoder re-interleaves them to the received stream, finds the preamble
# (0x712) and cuts the words itself: word 1 = preamble, Rev, then a
# BCH(15,11) codeword (FraID, SOW high 8 bits); words 2..10 = two
# interleaved BCH(15,11) codewords of 11 info bits each (single errors
# corrected). An inverted codeword is a codeword too (all ones is one), so
# a carrier phase flip inside a subframe passes the check: words count
# only once two receptions agree (a stream with an inverted preamble is
# inverted back).
#
# GPS LNAV (IS-GPS-200): FE2A = header of a batch every 6 s (u8 number of
# FE2B that follow, u32 epoch tick high half first, ...); FE2B = one
# subframe of one satellite: u8 status (0xff: incomplete), u8, u8 PRN, u8 0,
# u8 subframe ID 1..5, u8, then 10 words of 4 bytes: data bits 1..8, the six
# parity bits, data bits 17..24, data bits 9..16 (the data is the source
# data, already freed of the D30* inversion; words not received read
# 00 1a 00 00 and fail the parity check).
import math
import struct

PI = 3.1415926535898		# the ICD's value

# IS-GPS-200 table 20-XIV: data bits (1..24) in each parity bit, and
# which of D29* / D30* of the previous word goes in
PARITY = [(29, (1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23)),
          (30, (2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24)),
          (29, (1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22)),
          (30, (2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23)),
          (30, (1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24)),
          (29, (3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24))]


def parity(d, d29, d30):
    """the six parity bits (D25..D30 as an int, D25 first) of 24 data bits"""
    out = 0
    for prev, bits in PARITY:
        b = d29 if prev == 29 else d30
        for i in bits:
            b ^= (d >> (24 - i)) & 1
        out = out << 1 | b
    return out


def gps_words(p):
    """the 10 data words (24 bits) of an FE2B payload, None for a word that
    fails its parity check"""
    words, d29, d30 = [], 0, 0		# word 10 of the previous subframe ends 0, 0
    for i in range(10):
        b = p[6 + 4 * i:10 + 4 * i]
        d, par = b[0] << 16 | b[3] << 8 | b[2], b[1] & 0x3f
        ok = parity(d, d29, d30) == par
        words.append(d if ok else None)
        d29, d30 = par >> 1 & 1, par & 1
    return words


def bits(w, first, n, signed=False):
    """n bits of data word w starting at data bit first (1..24)"""
    v = (w >> (24 - first - n + 1)) & ((1 << n) - 1)
    return v - (1 << n) if signed and v >> (n - 1) else v


def join(hi, nhi, lo, nlo, signed=False):
    v = hi << nlo | lo
    n = nhi + nlo
    return v - (1 << n) if signed and v >> (n - 1) else v


class GpsNav:
    """collects GPS subframes per satellite, word by word: subframes 1..3
    repeat every 30 s with the same data for ~2 h, so words add up over
    repeats (indoors few subframes arrive whole). A word counts once two
    receptions agree, or when its whole subframe passed parity at once
    (noise passes the 6 parity bits 1 time in 64); a subframe's words start
    over when its issue word (IODC low bits in 1 / word 8, IODE in 2 /
    word 3 and 3 / word 10) changes to a confirmed new value.
    ephemeris() once all three are complete and of one issue."""

    def __init__(self):
        self.sub = {}		# prn -> {subframe id: 10 x (word, confirmed)}
        self.pending = {}	# (prn, subframe id) -> new issue seen once
        self.iono = None	# (alpha[4], beta[4])
        self.utc = None		# dict(a0, a1, tot, wnt, dtls)

    ISSUE = {1: 7, 2: 2, 3: 9}		# word holding the issue of data per subframe

    def add(self, p):
        """an FE2B payload; returns the PRN whose ephemeris may be complete"""
        if len(p) < 46:
            return None
        words = gps_words(p)
        # the subframe ID: from the HOW if it came through, else the header
        sid = bits(words[1], 20, 3) if words[1] is not None else p[4]
        prn = p[2]
        if sid in (1, 2, 3):
            have = self.sub.setdefault(prn, {}).setdefault(sid, [None] * 10)
            whole = None not in words[2:]
            k = self.ISSUE[sid]
            if words[k] is not None and have[k] and have[k][1] and \
               bits(words[k], 1, 8) != bits(have[k][0], 1, 8):
                new = bits(words[k], 1, 8)
                if whole or self.pending.get((prn, sid)) == new:
                    have[:] = [None] * 10		# a new issue
                self.pending[(prn, sid)] = new
            for i in range(2, 10):
                w = words[i]
                if w is None:
                    continue
                old = have[i]
                if old and old[1] and old[0] != w and not whole:
                    continue
                have[i] = (w, whole or bool(old and old[0] == w))
            return prn
        if sid == 4 and None not in words[2:9] and bits(words[2], 3, 6) == 56:	# page 18: ionosphere, UTC
            w = words
            self.iono = ([bits(w[2], 9, 8, True) * 2 ** -30, bits(w[2], 17, 8, True) * 2 ** -27,
                          bits(w[3], 1, 8, True) * 2 ** -24, bits(w[3], 9, 8, True) * 2 ** -24],
                         [bits(w[3], 17, 8, True) * 2 ** 11, bits(w[4], 1, 8, True) * 2 ** 14,
                          bits(w[4], 9, 8, True) * 2 ** 16, bits(w[4], 17, 8, True) * 2 ** 16])
            self.utc = dict(a1=bits(w[5], 1, 24, True) * 2 ** -50,
                            a0=join(bits(w[6], 1, 24), 24, bits(w[7], 1, 8), 8, True) * 2 ** -30,
                            tot=bits(w[7], 9, 8) * 2 ** 12, wnt=bits(w[7], 17, 8),
                            dtls=bits(w[8], 1, 8, True))
        return None

    def ephemeris(self, prn, week_now, confirmed=True):
        """orbits.py record from subframes 1..3 with matching issue (IODC
        low 8 bits = IODE in 2 and 3), or None; week_now: current GPS week
        (the message carries it modulo 1024); confirmed=False: from the
        latest words even if received once (to be checked by the caller)"""
        s = self.sub.get(prn, {})
        if not all(k in s and all(x and (x[1] or not confirmed) for x in s[k][2:]) for k in (1, 2, 3)):
            return None
        s1, s2, s3 = ([None, None] + [x[0] for x in s[k][2:]] for k in (1, 2, 3))
        iodc = join(bits(s1[2], 23, 2), 2, bits(s1[7], 1, 8), 8)
        iode2, iode3 = bits(s2[2], 1, 8), bits(s3[9], 1, 8)
        if not (iode2 == iode3 == iodc & 0xff):
            return None
        wn = bits(s1[2], 1, 10)
        week = week_now - ((week_now - wn) % 1024)
        if week_now - week > 512:
            week += 1024
        e = dict(
            week=float(week), codes=float(bits(s1[2], 11, 2)), sva=float(bits(s1[2], 13, 4)),
            svh=float(bits(s1[2], 17, 6)), iode=float(iode2), iodc=float(iodc), l2p=0.0,
            tgd=bits(s1[6], 17, 8, True) * 2 ** -31,
            toc=bits(s1[7], 9, 16) * 16.0,
            af2=bits(s1[8], 1, 8, True) * 2 ** -55, af1=bits(s1[8], 9, 16, True) * 2 ** -43,
            af0=bits(s1[9], 1, 22, True) * 2 ** -31,
            crs=bits(s2[2], 9, 16, True) * 2 ** -5,
            dn=bits(s2[3], 1, 16, True) * 2 ** -43 * PI,
            m0=join(bits(s2[3], 17, 8), 8, bits(s2[4], 1, 24), 24, True) * 2 ** -31 * PI,
            cuc=bits(s2[5], 1, 16, True) * 2 ** -29,
            e=join(bits(s2[5], 17, 8), 8, bits(s2[6], 1, 24), 24) * 2 ** -33,
            cus=bits(s2[7], 1, 16, True) * 2 ** -29,
            sqa=join(bits(s2[7], 17, 8), 8, bits(s2[8], 1, 24), 24) * 2 ** -19,
            toe=bits(s2[9], 1, 16) * 16.0,
            cic=bits(s3[2], 1, 16, True) * 2 ** -29,
            omg0=join(bits(s3[2], 17, 8), 8, bits(s3[3], 1, 24), 24, True) * 2 ** -31 * PI,
            cis=bits(s3[4], 1, 16, True) * 2 ** -29,
            i0=join(bits(s3[4], 17, 8), 8, bits(s3[5], 1, 24), 24, True) * 2 ** -31 * PI,
            crc=bits(s3[6], 1, 16, True) * 2 ** -5,
            w=join(bits(s3[6], 17, 8), 8, bits(s3[7], 1, 24), 24, True) * 2 ** -31 * PI,
            omgd=bits(s3[8], 1, 24, True) * 2 ** -43 * PI,
            idot=bits(s3[9], 9, 14, True) * 2 ** -43 * PI)
        e['toe_abs'] = week * 604800 + e['toe']
        e['toc'] = week * 604800 + e['toc']		# orbits.py keeps toc absolute
        # toe / toc in the next week's start: the 10-bit week is that of
        # transmission, the times may lie on either side of a week boundary
        if e['toe_abs'] - (week_now * 604800 + 302400) > 302400:
            e['toe_abs'] -= 604800
            e['toc'] -= 604800
        return e


def bch(m):
    """BCH(15,11) parity (g = x^4 + x + 1) of 11 data bits"""
    r = m << 4
    for i in range(14, 3, -1):
        if r >> i & 1:
            r ^= 0b10011 << (i - 4)
    return r & 0xf


# syndrome -> the single bit error (bit 14 = first bit of the codeword)
SYNDROME = {}
for _i in range(15):
    _c = 1 << _i
    SYNDROME[(bch(_c >> 4) ^ (_c & 0xf))] = _i


def bch_decode(c):
    """(11 data bits, clean) of a 15-bit codeword, one error corrected"""
    syn = bch(c >> 4) ^ (c & 0xf)
    if syn:
        c ^= 1 << SYNDROME[syn]
    return c >> 4, not syn


def mixed32(b):
    return (b[1] << 8 | b[0]) << 16 | (b[3] << 8 | b[2])


def field(v, first, n, total=32):
    """n bits of a total-bit value v starting at bit first (0 = MSB)"""
    return (v >> (total - first - n)) & ((1 << n) - 1)


def signed(v, n):
    return v - (1 << n) if v >> (n - 1) else v


def bds_stream(p):
    """the 300 received bits of an FE4B payload as an int (slot 1 first)"""
    out = 0
    for i in range(10):
        v = mixed32(p[7 + 4 * i:11 + 4 * i])
        if i == 0:
            raw = field(v, 2, 30)
        else:
            odd = field(v, 2, 11) << 4 | field(v, 24, 4)
            even = field(v, 13, 11) << 4 | field(v, 28, 4)
            raw = 0
            for k in range(14, -1, -1):
                raw = raw << 2 | (odd >> k & 1) << 1 | (even >> k & 1)
        out = out << 30 | raw
    return out


def bds_words(p):
    """(FraID, [10 x (info, clean) or None]) of an FE4B payload: word 1 info
    = 26 bits (preamble ... SOW high), words 2..10 = 22 info bits; a word
    cut short at the end of the stream (when the slots lag the words) is
    clean only if one completion of its missing bits checks"""
    s = bds_stream(p)
    phase = next((o for o in range(0, 20) if field(s, o, 11, 300) in (0x712, 0x0ed)), None)
    if phase is None:
        return None, None
    if field(s, phase, 11, 300) == 0x0ed:		# received with inverted polarity
        s ^= (1 << 300) - 1
    words = []
    for j in range(10):
        start = phase + 30 * j
        have = min(30, 300 - start)
        t = field(s, start, have, 300) << (30 - have)
        if j == 0:			# 15 raw bits, then one codeword
            data, clean = bch_decode(t & 0x7fff) if have == 30 else ((t & 0x7fff) >> 4, False)
            words.append(((t >> 15) << 11 | data, clean))
            continue
        a = b = 0
        for k in range(15):
            a = a << 1 | (t >> (29 - 2 * k) & 1)
            b = b << 1 | (t >> (28 - 2 * k) & 1)
        if have < 30:
            # the missing last bits: A's (have < 29) and B's last parity bit
            opts = [(a ^ ma, b ^ mb) for ma in ((0, 1) if have < 29 else (0,)) for mb in (0, 1)]
            ok = [(x, y) for x, y in opts if bch_decode(x)[1] and bch_decode(y)[1]]
            if ok:
                a, b = ok[0]
        da, ca = bch_decode(a)
        db, cb = bch_decode(b)
        words.append((da << 11 | db, ca and cb))
    return field(words[0][0], 15, 3, 26), words


class BdsNav:
    """BeiDou D1 subframes 1..3 per satellite, word by word (they repeat
    every 30 s, a new issue about every hour): a word counts once two
    receptions agree (word 2 without its SOW bits); a changed toe
    (subframe 3 word 2) or AODC (subframe 1 word 2) starts the issue over"""

    def __init__(self):
        self.sub = {}		# prn -> {fra: [10 x (value, confirmed)]}
        self.pending = {}	# (prn, fra) -> issue word value seen once
        self.votes = {}		# (prn, fra) -> per word {value: score}
        self.iono = None

    def add(self, p):
        if len(p) < 47:
            return None
        fra, words = bds_words(p)
        prn = p[3]
        if fra not in (1, 2, 3) or not words[0][1]:
            return None
        have = self.sub.setdefault(prn, {}).setdefault(fra, [None] * 10)
        # a new issue: AODC (subframe 1 word 2) or toe (subframe 3 word 2)
        # changed, the same new value received twice
        mask = {1: 0x1f << 4, 3: 0x3ff}.get(fra)
        if mask and have[1] and have[1][1] and (words[1][0] ^ have[1][0]) & mask:
            new = words[1][0] & mask
            if self.pending.get((prn, fra)) == new:
                have[:] = [None] * 10
                if fra == 3:
                    self.sub[prn].pop(2, None)
            self.pending[(prn, fra)] = new
        votes = self.votes.setdefault((prn, fra), [None] * 10)
        if not any(have[1:]):
            votes[:] = [None] * 10			# a new issue
        for i in range(1, 10):
            info = words[i][0] & (0x3ff if i == 1 else 0x3fffff)	# word 2: SOW bits out
            old = have[i]
            if old and old[1] and old[0] != info:
                continue				# confirmed: keep (a new issue resets above)
            # unconfirmed: the most frequent value so far, clean receptions
            # (BCH syndrome zero) counting double
            v = votes[i] = votes[i] or {}
            v[info] = v.get(info, 0) + (2 if words[i][1] else 1)
            v['latest'] = info
            top = max((x for x in v if x != 'latest'), key=v.get)
            have[i] = (top, bool(old and old[0] == info) or v[top] >= 4)
        return prn

    def ephemeris(self, prn, week_now, confirmed=True, latest=False):
        """as GpsNav.ephemeris; latest: unconfirmed words as last received
        instead of the most voted"""
        s = self.sub.get(prn, {})
        if not all(k in s and all(x and (x[1] or not confirmed) for x in s[k][1:]) for k in (1, 2, 3)):
            return None
        if latest:
            s = {k: [x if x is None or x[1] or not (self.votes.get((prn, k)) or [None] * 10)[i]
                     else (self.votes[(prn, k)][i]['latest'], False) for i, x in enumerate(s[k])]
                 for k in (1, 2, 3)}
        # concatenated info of words 2..10: 9 x 22 = 198 bits per subframe
        cat = {k: sum(s[k][i][0] << (22 * (9 - i)) for i in range(1, 10)) for k in (1, 2, 3)}

        def f(k, first, n, sgn=False):
            """field of subframe k: bit 0 = first info bit of word 2 (the
            ICD's split fields are contiguous in this stream)"""
            v = field(cat[k], first, n, 198)
            return signed(v, n) if sgn else v

        PI = 3.1415926535898
        toc = f(1, 35, 17) * 8.0
        e = dict(svh=float(f(1, 12, 1)), iodc=float(f(1, 13, 5)), sva=float(f(1, 18, 4)),
                 tgd=f(1, 52, 10, True) * 1e-10, codes=0.0, l2p=0.0,
                 af2=f(1, 136, 11, True) * 2 ** -66, af0=f(1, 147, 24, True) * 2 ** -33,
                 af1=f(1, 171, 22, True) * 2 ** -50, iode=float(f(1, 193, 5)),
                 dn=f(2, 12, 16, True) * 2 ** -43 * PI, cuc=f(2, 28, 18, True) * 2 ** -31,
                 m0=f(2, 46, 32, True) * 2 ** -31 * PI, e=f(2, 78, 32) * 2 ** -33,
                 cus=f(2, 110, 18, True) * 2 ** -31, crc=f(2, 128, 18, True) * 2 ** -6,
                 crs=f(2, 146, 18, True) * 2 ** -6, sqa=f(2, 164, 32) * 2 ** -19,
                 i0=f(3, 27, 32, True) * 2 ** -31 * PI, cic=f(3, 59, 18, True) * 2 ** -31,
                 omgd=f(3, 77, 24, True) * 2 ** -43 * PI, cis=f(3, 101, 18, True) * 2 ** -31,
                 idot=f(3, 119, 14, True) * 2 ** -43 * PI, omg0=f(3, 133, 32, True) * 2 ** -31 * PI,
                 w=f(3, 165, 32, True) * 2 ** -31 * PI)
        toe = (f(2, 196, 2) << 15 | f(3, 12, 15)) * 8.0
        self.iono = ([f(1, 72, 8, True) * 2 ** -30, f(1, 80, 8, True) * 2 ** -27,
                      f(1, 88, 8, True) * 2 ** -24, f(1, 96, 8, True) * 2 ** -24],
                     [f(1, 104, 8, True) * 2 ** 11, f(1, 112, 8, True) * 2 ** 14,
                      f(1, 120, 8, True) * 2 ** 16, f(1, 128, 8, True) * 2 ** 16])
        wn = f(1, 22, 13)
        if toe != toc:			# subframes of different issues
            return None
        bdt_week = week_now - 1356
        week = bdt_week - ((bdt_week - wn) % 8192)
        e.update(week=float(week), toe=toe)
        e['toe_abs'] = (week + 1356) * 604800 + toe + 14
        e['toc'] = (week + 1356) * 604800 + toc + 14
        return e
