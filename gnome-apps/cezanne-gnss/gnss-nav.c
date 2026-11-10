// SPDX-License-Identifier: MIT
/*
 * Navigation messages decoded from the DSP's reports, so the receiver needs
 * no downloaded orbits: ephemerides (struct eph), ionosphere parameters
 * (Klobuchar) and GPS - UTC. Formats and rules as in the Python version's
 * nav.py:
 *
 * BeiDou D1 (MEO / IGSO, BDS-SIS-ICD): FE4A = batch header every 6 s (u8
 * number of FE4B, u32 tick); FE4B = one subframe: u8, u16, u8 PRN, u8, u8,
 * u8, then 10 slots of 4 bytes, each a 32-bit value stored as halves (high
 * half first, little-endian) holding 30 received bits at bits 2..31. The
 * slots follow the DSP's bit sync, not necessarily the word boundaries
 * (seen: words starting at bit 0 or 2 of a slot's 30 bits). Slot 1 is
 * stored as received; slots 2..10 de-interleaved as words 2..10 would be
 * ([odd bits 1..11][even bits 1..11][odd 12..15][even 12..15]), so the
 * decoder re-interleaves them to the received stream, finds the preamble
 * (0x712) and cuts the words itself: word 1 = preamble, Rev, then a
 * BCH(15,11) codeword (FraID, SOW high 8 bits); words 2..10 = two
 * interleaved BCH(15,11) codewords of 11 info bits each (single errors
 * corrected). An inverted codeword is a codeword too (all ones is one), so
 * a carrier phase flip inside a subframe passes the check: words count only
 * once two receptions agree (a stream with an inverted preamble is inverted
 * back).
 *
 * GPS LNAV (IS-GPS-200): FE2A = header of a batch every 6 s (u8 number of
 * FE2B that follow, u32 epoch tick high half first, ...); FE2B = one
 * subframe of one satellite: u8 status (0xff: incomplete), u8, u8 PRN, u8 0,
 * u8 subframe ID 1..5, u8, then 10 words of 4 bytes: data bits 1..8, the six
 * parity bits, data bits 17..24, data bits 9..16 (the data is the source
 * data, already freed of the D30* inversion; words not received read 00 1a
 * 00 00 and fail the parity check).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gnss.h"

#define PI_ICD	3.1415926535898		/* the ICD's value */

/* ---- GPS LNAV ---- */

/*
 * IS-GPS-200 table 20-XIV: data bits (1..24) in each parity bit, and which
 * of D29* / D30* of the previous word goes in
 */
static const struct {
	int prev;
	int bits[16];
} parity_tab[6] = {
	{ 29, { 1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23, 0 } },
	{ 30, { 2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24, 0 } },
	{ 29, { 1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22, 0 } },
	{ 30, { 2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23, 0 } },
	{ 30, { 1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24, 0 } },
	{ 29, { 3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24, 0 } },
};

/* the six parity bits (D25..D30 as an int, D25 first) of 24 data bits */
static int parity(uint32_t d, int d29, int d30)
{
	int out = 0, k, j;

	for (k = 0; k < 6; k++) {
		int b = parity_tab[k].prev == 29 ? d29 : d30;

		for (j = 0; parity_tab[k].bits[j]; j++)
			b ^= (d >> (24 - parity_tab[k].bits[j])) & 1;
		out = out << 1 | b;
	}
	return out;
}

/* the 10 data words (24 bits) of an FE2B payload; ok[i] = 0: failed parity */
static void gps_words(const unsigned char *p, uint32_t w[10], int ok[10])
{
	int d29 = 0, d30 = 0, i;	/* word 10 of the previous subframe ends 0, 0 */

	for (i = 0; i < 10; i++) {
		const unsigned char *b = p + 6 + 4 * i;
		uint32_t d = (uint32_t)b[0] << 16 | b[3] << 8 | b[2];
		int par = b[1] & 0x3f;

		ok[i] = parity(d, d29, d30) == par;
		w[i] = d;
		d29 = par >> 1 & 1;
		d30 = par & 1;
	}
}

/* n bits of data word w starting at data bit first (1..24) */
static long long bits(uint32_t w, int first, int n, int sgn)
{
	long long v = (w >> (24 - first - n + 1)) & ((1LL << n) - 1);

	return sgn && v >> (n - 1) ? v - (1LL << n) : v;
}

static long long join(long long hi, int nhi, long long lo, int nlo, int sgn)
{
	long long v = hi << nlo | lo;
	int n = nhi + nlo;

	return sgn && v >> (n - 1) ? v - (1LL << n) : v;
}

static struct gps_sat *gps_sat(struct gps_nav *g, int prn, int create)
{
	size_t i;

	for (i = 0; i < g->n; i++)
		if (g->s[i].prn == prn)
			return &g->s[i];
	if (!create)
		return NULL;
	g->s = realloc(g->s, (g->n + 1) * sizeof(*g->s));
	memset(&g->s[g->n], 0, sizeof(*g->s));
	g->s[g->n].prn = prn;
	return &g->s[g->n++];
}

/* pending issue: -1 if none */
static int *gps_pending(struct gps_nav *g, int prn, int sid, int create)
{
	size_t i;

	for (i = 0; i < g->npending; i++)
		if (g->pending[i].prn == prn && g->pending[i].sid == sid)
			return &g->pending[i].v;
	if (!create)
		return NULL;
	g->pending = realloc(g->pending, (g->npending + 1) * sizeof(*g->pending));
	g->pending[g->npending].prn = prn;
	g->pending[g->npending].sid = sid;
	g->pending[g->npending].v = -1;
	return &g->pending[g->npending++].v;
}

static double p2(int e)
{
	return ldexp(1.0, e);
}

/*
 * an FE2B payload; returns the PRN whose ephemeris may be complete, or 0.
 * Subframes 1..3 repeat every 30 s with the same data for ~2 h, so words
 * add up over repeats (indoors few subframes arrive whole). A word counts
 * once two receptions agree, or when its whole subframe passed parity at
 * once (noise passes the 6 parity bits 1 time in 64); a subframe's words
 * start over when its issue word (IODC low bits in 1 / word 8, IODE in 2 /
 * word 3 and 3 / word 10) changes to a confirmed new value.
 */
int gps_nav_add(struct gps_nav *g, const unsigned char *p, size_t len)
{
	static const int issue[4] = { 0, 7, 2, 9 };	/* word holding the issue of data */
	uint32_t w[10];
	int ok[10], sid, prn, i;

	if (len < 46)
		return 0;
	gps_words(p, w, ok);
	/* the subframe ID: from the HOW if it came through, else the header */
	sid = ok[1] ? (int)bits(w[1], 20, 3, 0) : p[4];
	prn = p[2];
	if (sid >= 1 && sid <= 3) {
		struct gps_sat *s = gps_sat(g, prn, 1);
		struct gps_word *have = s->sub[sid - 1];
		int whole = 1, k = issue[sid];

		if (!s->have[sid - 1]) {
			s->have[sid - 1] = 1;
			memset(have, 0, sizeof(s->sub[0]));
		}
		for (i = 2; i < 10; i++)
			whole &= ok[i];
		if (ok[k] && have[k].set && have[k].confirmed &&
		    bits(w[k], 1, 8, 0) != bits(have[k].w, 1, 8, 0)) {
			int nw = bits(w[k], 1, 8, 0), *pend = gps_pending(g, prn, sid, 1);

			if (whole || *pend == nw)
				memset(have, 0, sizeof(s->sub[0]));	/* a new issue */
			*pend = nw;
		}
		for (i = 2; i < 10; i++) {
			struct gps_word *old = &have[i];

			if (!ok[i])
				continue;
			if (old->set && old->confirmed && old->w != w[i] && !whole)
				continue;
			have[i].confirmed = whole || (old->set && old->w == w[i]);
			have[i].w = w[i];
			have[i].set = 1;
		}
		return prn;
	}
	if (sid == 4 && ok[2] && ok[3] && ok[4] && ok[5] && ok[6] && ok[7] && ok[8] &&
	    bits(w[2], 3, 6, 0) == 56) {	/* page 18: ionosphere, UTC */
		g->has_iono = 1;
		g->iono_alpha[0] = bits(w[2], 9, 8, 1) * p2(-30);
		g->iono_alpha[1] = bits(w[2], 17, 8, 1) * p2(-27);
		g->iono_alpha[2] = bits(w[3], 1, 8, 1) * p2(-24);
		g->iono_alpha[3] = bits(w[3], 9, 8, 1) * p2(-24);
		g->iono_beta[0] = bits(w[3], 17, 8, 1) * p2(11);
		g->iono_beta[1] = bits(w[4], 1, 8, 1) * p2(14);
		g->iono_beta[2] = bits(w[4], 9, 8, 1) * p2(16);
		g->iono_beta[3] = bits(w[4], 17, 8, 1) * p2(16);
		g->has_utc = 1;
		g->utc_a1 = bits(w[5], 1, 24, 1) * p2(-50);
		g->utc_a0 = join(bits(w[6], 1, 24, 0), 24, bits(w[7], 1, 8, 0), 8, 1) * p2(-30);
		g->utc_tot = bits(w[7], 9, 8, 0) * p2(12);
		g->utc_wnt = bits(w[7], 17, 8, 0);
		g->utc_dtls = bits(w[8], 1, 8, 1);
	}
	return 0;
}

/*
 * the record from subframes 1..3 with matching issue (IODC low 8 bits =
 * IODE in 2 and 3): 1, or 0; week_now: current GPS week (the message
 * carries it modulo 1024); confirmed = 0: from the latest words even if
 * received once (to be checked by the caller)
 */
int gps_nav_ephemeris(struct gps_nav *g, int prn, int week_now, int confirmed, struct eph *e)
{
	struct gps_sat *s = gps_sat(g, prn, 0);
	uint32_t s1[10], s2[10], s3[10];
	long long iodc, iode2, iode3, wn, week;
	int k, i;

	if (!s)
		return 0;
	for (k = 0; k < 3; k++) {
		if (!s->have[k])
			return 0;
		for (i = 2; i < 10; i++)
			if (!s->sub[k][i].set || !(s->sub[k][i].confirmed || !confirmed))
				return 0;
	}
	for (i = 2; i < 10; i++) {
		s1[i] = s->sub[0][i].w;
		s2[i] = s->sub[1][i].w;
		s3[i] = s->sub[2][i].w;
	}
	iodc = join(bits(s1[2], 23, 2, 0), 2, bits(s1[7], 1, 8, 0), 8, 0);
	iode2 = bits(s2[2], 1, 8, 0);
	iode3 = bits(s3[9], 1, 8, 0);
	if (!(iode2 == iode3 && iode3 == (iodc & 0xff)))
		return 0;
	wn = bits(s1[2], 1, 10, 0);
	week = week_now - ((((week_now - wn) % 1024) + 1024) % 1024);
	if (week_now - week > 512)
		week += 1024;
	memset(e, 0, sizeof(*e));
	e->week = (double)week;
	e->codes = (double)bits(s1[2], 11, 2, 0);
	e->sva = (double)bits(s1[2], 13, 4, 0);
	e->svh = (double)bits(s1[2], 17, 6, 0);
	e->iode = (double)iode2;
	e->iodc = (double)iodc;
	e->has_iodc = 1;
	e->l2p = 0.0;
	e->tgd = bits(s1[6], 17, 8, 1) * p2(-31);
	e->toc = bits(s1[7], 9, 16, 0) * 16.0;
	e->af2 = bits(s1[8], 1, 8, 1) * p2(-55);
	e->af1 = bits(s1[8], 9, 16, 1) * p2(-43);
	e->af0 = bits(s1[9], 1, 22, 1) * p2(-31);
	e->crs = bits(s2[2], 9, 16, 1) * p2(-5);
	e->dn = bits(s2[3], 1, 16, 1) * p2(-43) * PI_ICD;
	e->m0 = join(bits(s2[3], 17, 8, 0), 8, bits(s2[4], 1, 24, 0), 24, 1) * p2(-31) * PI_ICD;
	e->cuc = bits(s2[5], 1, 16, 1) * p2(-29);
	e->e = join(bits(s2[5], 17, 8, 0), 8, bits(s2[6], 1, 24, 0), 24, 0) * p2(-33);
	e->cus = bits(s2[7], 1, 16, 1) * p2(-29);
	e->sqa = join(bits(s2[7], 17, 8, 0), 8, bits(s2[8], 1, 24, 0), 24, 0) * p2(-19);
	e->toe = bits(s2[9], 1, 16, 0) * 16.0;
	e->cic = bits(s3[2], 1, 16, 1) * p2(-29);
	e->omg0 = join(bits(s3[2], 17, 8, 0), 8, bits(s3[3], 1, 24, 0), 24, 1) * p2(-31) * PI_ICD;
	e->cis = bits(s3[4], 1, 16, 1) * p2(-29);
	e->i0 = join(bits(s3[4], 17, 8, 0), 8, bits(s3[5], 1, 24, 0), 24, 1) * p2(-31) * PI_ICD;
	e->crc = bits(s3[6], 1, 16, 1) * p2(-5);
	e->w = join(bits(s3[6], 17, 8, 0), 8, bits(s3[7], 1, 24, 0), 24, 1) * p2(-31) * PI_ICD;
	e->omgd = bits(s3[8], 1, 24, 1) * p2(-43) * PI_ICD;
	e->idot = bits(s3[9], 9, 14, 1) * p2(-43) * PI_ICD;
	e->toe_abs = week * 604800 + e->toe;
	e->toc = week * 604800 + e->toc;	/* kept absolute */
	/*
	 * toe / toc in the next week's start: the 10-bit week is that of
	 * transmission, the times may lie on either side of a week boundary
	 */
	if (e->toe_abs - ((double)week_now * 604800 + 302400) > 302400) {
		e->toe_abs -= 604800;
		e->toc -= 604800;
	}
	e->confirmed = 1;
	return 1;
}

/* ---- BeiDou D1 ---- */

/* BCH(15,11) parity (g = x^4 + x + 1) of 11 data bits */
static int bch(int m)
{
	int r = m << 4, i;

	for (i = 14; i > 3; i--)
		if (r >> i & 1)
			r ^= 0x13 << (i - 4);
	return r & 0xf;
}

/* syndrome -> the single bit error (bit 14 = first bit of the codeword) */
static int syndrome[16];

static void syndromes(void)
{
	static int done;
	int i;

	if (done)
		return;
	for (i = 0; i < 16; i++)
		syndrome[i] = -1;
	for (i = 0; i < 15; i++) {
		int c = 1 << i;

		syndrome[bch(c >> 4) ^ (c & 0xf)] = i;
	}
	done = 1;
}

/* 11 data bits of a 15-bit codeword, one error corrected; *clean: no error */
static int bch_decode(int c, int *clean)
{
	int syn = bch(c >> 4) ^ (c & 0xf);

	if (syn)
		c ^= 1 << syndrome[syn];
	*clean = !syn;
	return c >> 4;
}

static uint32_t mixed32(const unsigned char *b)
{
	return ((uint32_t)b[1] << 8 | b[0]) << 16 | ((uint32_t)b[3] << 8 | b[2]);
}

/* n bits of a 32-bit value v starting at bit first (0 = MSB) */
static uint32_t field32(uint32_t v, int first, int n)
{
	return (v >> (32 - first - n)) & ((1u << n) - 1);
}

/* the 300 received bits, MSB first: bit i = s[i] */
static void bds_stream(const unsigned char *p, unsigned char s[300])
{
	int i, k, n = 0;

	for (i = 0; i < 10; i++) {
		uint32_t v = mixed32(p + 7 + 4 * i), raw = 0;

		if (i == 0) {
			raw = field32(v, 2, 30);
		} else {
			uint32_t odd = field32(v, 2, 11) << 4 | field32(v, 24, 4);
			uint32_t even = field32(v, 13, 11) << 4 | field32(v, 28, 4);

			for (k = 14; k >= 0; k--)
				raw = raw << 2 | (odd >> k & 1) << 1 | (even >> k & 1);
		}
		for (k = 29; k >= 0; k--)
			s[n++] = raw >> k & 1;
	}
}

/* n bits of the stream from bit first (as one value) */
static uint32_t sfield(const unsigned char s[300], int first, int n)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < n; i++)
		v = v << 1 | s[first + i];
	return v;
}

struct bds_wordv {
	uint32_t info;
	int clean;
};

/*
 * the FraID and 10 words of an FE4B payload (-1: no preamble): word 1 info =
 * 26 bits (preamble ... SOW high), words 2..10 = 22 info bits; a word cut
 * short at the end of the stream (when the slots lag the words) is clean
 * only if one completion of its missing bits checks
 */
static int bds_words(const unsigned char *p, struct bds_wordv words[10])
{
	unsigned char s[300];
	int phase = -1, o, j, k;

	syndromes();
	bds_stream(p, s);
	for (o = 0; o < 20 && phase < 0; o++) {
		uint32_t pre = sfield(s, o, 11);

		if (pre == 0x712 || pre == 0x0ed)
			phase = o;
	}
	if (phase < 0)
		return -1;
	if (sfield(s, phase, 11) == 0x0ed)	/* received with inverted polarity */
		for (k = 0; k < 300; k++)
			s[k] ^= 1;
	for (j = 0; j < 10; j++) {
		int start = phase + 30 * j, have = 300 - start < 30 ? 300 - start : 30, a = 0, b = 0, ca, cb;
		uint32_t t = sfield(s, start, have) << (30 - have);

		if (j == 0) {	/* 15 raw bits, then one codeword */
			int clean = 0, data;

			if (have == 30)
				data = bch_decode(t & 0x7fff, &clean);
			else
				data = (t & 0x7fff) >> 4;
			words[0].info = (t >> 15) << 11 | data;
			words[0].clean = clean;
			continue;
		}
		for (k = 0; k < 15; k++) {
			a = a << 1 | (t >> (29 - 2 * k) & 1);
			b = b << 1 | (t >> (28 - 2 * k) & 1);
		}
		if (have < 30) {
			/* the missing last bits: A's (have < 29) and B's last parity bit */
			int ma, mb, found = 0;

			for (ma = 0; ma <= (have < 29 ? 1 : 0) && !found; ma++)
				for (mb = 0; mb <= 1 && !found; mb++) {
					int x = a ^ ma, y = b ^ mb, c1, c2;

					bch_decode(x, &c1);
					bch_decode(y, &c2);
					if (c1 && c2) {
						a = x;
						b = y;
						found = 1;
					}
				}
		}
		{
			int da = bch_decode(a, &ca), db = bch_decode(b, &cb);

			words[j].info = (uint32_t)da << 11 | db;
			words[j].clean = ca && cb;
		}
	}
	return (words[0].info >> (26 - 15 - 3)) & 7;
}

static struct bds_sat *bds_sat(struct bds_nav *b, int prn, int create)
{
	size_t i;

	for (i = 0; i < b->n; i++)
		if (b->s[i].prn == prn)
			return &b->s[i];
	if (!create)
		return NULL;
	b->s = realloc(b->s, (b->n + 1) * sizeof(*b->s));
	memset(&b->s[b->n], 0, sizeof(*b->s));
	b->s[b->n].prn = prn;
	return &b->s[b->n++];
}

static int bds_pending(struct bds_nav *b, int prn, int fra, uint32_t *v)
{
	size_t i;

	for (i = 0; i < b->npending; i++)
		if (b->pending[i].prn == prn && b->pending[i].fra == fra) {
			*v = b->pending[i].v;
			return 1;
		}
	return 0;
}

static void bds_set_pending(struct bds_nav *b, int prn, int fra, uint32_t v)
{
	size_t i;

	for (i = 0; i < b->npending; i++)
		if (b->pending[i].prn == prn && b->pending[i].fra == fra) {
			b->pending[i].v = v;
			return;
		}
	b->pending = realloc(b->pending, (b->npending + 1) * sizeof(*b->pending));
	b->pending[b->npending].prn = prn;
	b->pending[b->npending].fra = fra;
	b->pending[b->npending++].v = v;
}

static void votes_clear(struct bds_votes *v)
{
	free(v->v);
	memset(v, 0, sizeof(*v));
}

/*
 * BeiDou D1 subframes 1..3 per satellite, word by word (they repeat every
 * 30 s, a new issue about every hour): a word counts once two receptions
 * agree (word 2 without its SOW bits); a changed toe (subframe 3 word 2) or
 * AODC (subframe 1 word 2) starts the issue over
 */
int bds_nav_add(struct bds_nav *b, const unsigned char *p, size_t len)
{
	struct bds_wordv words[10];
	struct bds_sat *s;
	struct bds_word *have;
	struct bds_votes *votes;
	int fra, prn, i, any;
	uint32_t mask;

	if (len < 47)
		return 0;
	fra = bds_words(p, words);
	prn = p[3];
	if (fra < 1 || fra > 3 || !words[0].clean)
		return 0;
	s = bds_sat(b, prn, 1);
	have = s->sub[fra - 1];
	if (!s->have[fra - 1]) {
		s->have[fra - 1] = 1;
		memset(have, 0, sizeof(s->sub[0]));
	}
	/*
	 * a new issue: AODC (subframe 1 word 2) or toe (subframe 3 word 2)
	 * changed, the same new value received twice
	 */
	mask = fra == 1 ? 0x1f << 4 : fra == 3 ? 0x3ff : 0;
	if (mask && have[1].set && have[1].confirmed && ((words[1].info ^ have[1].v) & mask)) {
		uint32_t nw = words[1].info & mask, pend;

		if (bds_pending(b, prn, fra, &pend) && pend == nw) {
			memset(have, 0, sizeof(s->sub[0]));
			if (fra == 3)
				s->have[1] = 0;		/* subframe 2 popped */
		}
		bds_set_pending(b, prn, fra, nw);
	}
	votes = s->votes[fra - 1];
	if (!s->votes_set[fra - 1])
		s->votes_set[fra - 1] = 1;
	any = 0;
	for (i = 1; i < 10; i++)
		any |= have[i].set;
	if (!any)		/* a new issue */
		for (i = 0; i < 10; i++)
			votes_clear(&votes[i]);
	for (i = 1; i < 10; i++) {
		uint32_t info = words[i].info & (i == 1 ? 0x3ff : 0x3fffff);	/* word 2: SOW bits out */
		struct bds_word old = have[i];
		struct bds_votes *v = &votes[i];
		size_t k, top;

		if (old.set && old.confirmed && old.v != info)
			continue;	/* confirmed: keep (a new issue resets above) */
		/*
		 * unconfirmed: the most frequent value so far, clean receptions
		 * (BCH syndrome zero) counting double
		 */
		v->set = 1;
		for (k = 0; k < v->n && v->v[k].v != info; k++)
			;
		if (k == v->n) {
			v->v = realloc(v->v, (v->n + 1) * sizeof(*v->v));
			v->v[v->n].v = info;
			v->v[v->n++].score = 0;
		}
		v->v[k].score += words[i].clean ? 2 : 1;
		v->latest = info;
		v->has_latest = 1;
		for (top = 0, k = 1; k < v->n; k++)
			if (v->v[k].score > v->v[top].score)
				top = k;
		have[i].v = v->v[top].v;
		have[i].confirmed = (old.set && old.v == info) || v->v[top].score >= 4;
		have[i].set = 1;
	}
	return prn;
}

/* n bits of the 198-bit concatenation of words 2..10 (bit 0 = first info bit) */
static long long cat_field(const uint32_t w[10], int first, int n, int sgn)
{
	long long v = 0;
	int i;

	for (i = 0; i < n; i++) {
		int bit = first + i, word = 1 + bit / 22, pos = bit % 22;

		v = v << 1 | (w[word] >> (21 - pos) & 1);
	}
	return sgn && v >> (n - 1) ? v - (1LL << n) : v;
}

/* as gps_nav_ephemeris; latest: unconfirmed words as last received instead of the most voted */
int bds_nav_ephemeris(struct bds_nav *b, int prn, int week_now, int confirmed, int latest,
		      struct eph *e)
{
	struct bds_sat *s = bds_sat(b, prn, 0);
	uint32_t w[3][10];
	double toc, toe;
	long long wn, bdt_week, week;
	int k, i;

#define F(k, first, n) cat_field(w[(k) - 1], first, n, 0)
#define FS(k, first, n) cat_field(w[(k) - 1], first, n, 1)
	if (!s)
		return 0;
	for (k = 0; k < 3; k++) {
		if (!s->have[k])
			return 0;
		for (i = 1; i < 10; i++)
			if (!s->sub[k][i].set || !(s->sub[k][i].confirmed || !confirmed))
				return 0;
	}
	for (k = 0; k < 3; k++)
		for (i = 1; i < 10; i++) {
			const struct bds_word *x = &s->sub[k][i];
			const struct bds_votes *v = &s->votes[k][i];

			w[k][i] = x->v;
			if (latest && !x->confirmed && s->votes_set[k] && v->set && v->has_latest)
				w[k][i] = v->latest;
		}
	toc = F(1, 35, 17) * 8.0;
	memset(e, 0, sizeof(*e));
	e->svh = (double)F(1, 12, 1);
	e->iodc = (double)F(1, 13, 5);
	e->has_iodc = 1;
	e->sva = (double)F(1, 18, 4);
	e->tgd = FS(1, 52, 10) * 1e-10;
	e->codes = 0.0;
	e->l2p = 0.0;
	e->af2 = FS(1, 136, 11) * p2(-66);
	e->af0 = FS(1, 147, 24) * p2(-33);
	e->af1 = FS(1, 171, 22) * p2(-50);
	e->iode = (double)F(1, 193, 5);
	e->dn = FS(2, 12, 16) * p2(-43) * PI_ICD;
	e->cuc = FS(2, 28, 18) * p2(-31);
	e->m0 = FS(2, 46, 32) * p2(-31) * PI_ICD;
	e->e = F(2, 78, 32) * p2(-33);
	e->cus = FS(2, 110, 18) * p2(-31);
	e->crc = FS(2, 128, 18) * p2(-6);
	e->crs = FS(2, 146, 18) * p2(-6);
	e->sqa = F(2, 164, 32) * p2(-19);
	e->i0 = FS(3, 27, 32) * p2(-31) * PI_ICD;
	e->cic = FS(3, 59, 18) * p2(-31);
	e->omgd = FS(3, 77, 24) * p2(-43) * PI_ICD;
	e->cis = FS(3, 101, 18) * p2(-31);
	e->idot = FS(3, 119, 14) * p2(-43) * PI_ICD;
	e->omg0 = FS(3, 133, 32) * p2(-31) * PI_ICD;
	e->w = FS(3, 165, 32) * p2(-31) * PI_ICD;
	toe = (double)(F(2, 196, 2) << 15 | F(3, 12, 15)) * 8.0;
	b->has_iono = 1;
	b->iono_alpha[0] = FS(1, 72, 8) * p2(-30);
	b->iono_alpha[1] = FS(1, 80, 8) * p2(-27);
	b->iono_alpha[2] = FS(1, 88, 8) * p2(-24);
	b->iono_alpha[3] = FS(1, 96, 8) * p2(-24);
	b->iono_beta[0] = FS(1, 104, 8) * p2(11);
	b->iono_beta[1] = FS(1, 112, 8) * p2(14);
	b->iono_beta[2] = FS(1, 120, 8) * p2(16);
	b->iono_beta[3] = FS(1, 128, 8) * p2(16);
	wn = F(1, 22, 13);
#undef F
#undef FS
	if (toe != toc)		/* subframes of different issues */
		return 0;
	bdt_week = week_now - 1356;
	week = bdt_week - ((((bdt_week - wn) % 8192) + 8192) % 8192);
	e->week = (double)week;
	e->toe = toe;
	e->toe_abs = (week + 1356) * 604800 + toe + 14;
	e->toc = (week + 1356) * 604800 + toc + 14;
	e->confirmed = 1;
	return 1;
}
