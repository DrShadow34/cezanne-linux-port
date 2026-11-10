// SPDX-License-Identifier: MIT
/*
 * The receiver: boots both DSPs, sends the per-second host commands (epoch
 * tick, time / frequency, per-satellite code phase / Doppler prediction,
 * channel plan), reads the DSP's channel records and solves positions from
 * the fully locked ones (see NOTES "GNSS"); receiver.py of the Python
 * version, the same rules and arithmetic.
 *
 * Clock: the receiver's oscillator offset (Hz at L1; -200..+70 on this
 * phone, with temperature) starts at the saved value with a wide Doppler
 * window; the locked channels' carrier Doppler then measures it together
 * with the receiver's velocity (doppler_*), which also smooths the positions
 * and gives speed / course.
 * Epoch timing: FE02 predicts each satellite's code phase for the DSP's
 * last epoch (tick in FE28); its GPS time comes from the locked channels
 * (transmit time + flight time), and once it is known the code window
 * narrows to a few chips - that is what makes the DSP lock the weak
 * satellites within seconds.
 */
#define _GNU_SOURCE
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <json-glib/json-glib.h>

#include "gnss.h"

#define MIN_CN0		15
#define DECODED_DAYS	7	/* decoded ephemerides kept (aiding only beyond 4 h) */

static double carrier(const char *link)
{
	return !strcmp(link, "L1") ? 1575.42e6 : 1176.45e6;
}

/* positions from GPS + BeiDou (Galileo: aided only so far) */
static char code_sys(int code)
{
	return code == 0x01 ? 'G' : code == 0x20 ? 'C' : 0;
}

/* setup burst after the boot, as the stock software sends it (GLP off) */
static const struct {
	const char *link;
	int mid;
	const char *hex;
} setup[] = {
	{ "L5", 0xfe05, "50000000000000000000000000" },
	{ "L1", 0xfe05, "3200000100" }, { "L5", 0xfe05, "3200000100" },
	{ "L1", 0xfe05, "0a0000000000000000" }, { "L1", 0xfe05, "460200010beeff986e" },
	{ "L1", 0xfe05, "460100010ae1ffa470" }, { "L1", 0xfe05, "3600000000" },
	{ "L1", 0xfe05, "2a00000000" }, { "L1", 0xfe05, "0f0000e803" }, { "L1", 0xfe05, "1500000200" },
	{ "L5", 0xfe05, "460000010b98ffcdcc" }, { "L5", 0xfe05, "3600000000" },
	{ "L5", 0xfe05, "2a00000000" }, { "L5", 0xfe05, "0f0000e803" }, { "L5", 0xfe05, "1500000200" },
	{ "L1", 0xfe04, "ffffffff00000f00ffffffff00007f0000000000f93fffffffffffffff7fff3f" },
	{ "L5", 0xfe04, "ffffffff00000f00ffffffff00007f0000000000ffffffffffffff7f" },
	{ "L1", 0xfe08, "10001000" }, { "L5", 0xfe08, "10001000" }, { "L1", 0xfe08, "1800" },
};

/*
 * oscillator model points (FE10) and FE11, constant copies from the stock
 * software (capture 4); the measured clock absorbs what they do not cover
 */
static const char fe10_l1[] = "01964f0000ffff85fab6520000ffffc5fbd6550000ffff0bfd";
static const char fe10_l5[] = "01964f0000ffffedfbb6520000ffffdafcd6550000ffffcdfd";
static const char fe11[] = "010000fc3a000000000000";

/*
 * cold start without any orbit data: the stock software's blind search list
 * (FE03 list 1: GPS, QZSS, GLONASS, BeiDou, Galileo) with FE01 status 0x0400
 * ("time not known") and no FE02 at all; with status 0x0421 and no FE02 the
 * DSP does not search
 */
static const char blind_l1[] = "016201010e011d011701020115010b0110011c010501160112010601130107010901190108010d011e010a01030104011b011a0118010f011f0111010c012001c108c208c308c408c508c608c7080110021003100410051006100710081009100a100b100c100d100e1006200720082009200a200b200c200d200e20012002200320042005200f2010201120122013201420152016201720182019201a201b201c201d201e201f20202021202220232024202520034006400740024009400440054008400140";
static const char blind_l5[] = "014801010e011d011701020115010b0110011c010501160112010601130107010901190108010d011e010a01030104011b011a0118010f011f0111010c012001c108c208c308c70806200720082009200a200b200c200d200e20012002200320042005200f2010201120122013201420152016201720182019201a201b201c201d201e201f20202021202220232024202520";

static size_t unhex(const char *h, unsigned char *out)
{
	size_t n = strlen(h) / 2, i;

	for (i = 0; i < n; i++) {
		unsigned v;

		sscanf(h + 2 * i, "%2x", &v);
		out[i] = v;
	}
	return n;
}

static int send_hex(struct link *l, int mid, const char *hex)
{
	unsigned char b[512];

	return link_send(l, mid, b, unhex(hex, b));
}

/* 32-bit field stored as halves, high half first */
static uint32_t mix32(const unsigned char *p, int i)
{
	return ((uint32_t)p[i + 2] | (uint32_t)p[i + 3] << 8) | ((uint32_t)p[i] << 16) |
	       ((uint32_t)p[i + 1] << 24);
}

static long long s32(uint32_t v)
{
	return v >= 1u << 31 ? (long long)v - (1LL << 32) : v;
}

/*
 * GPS time of transmission of a fully locked FE29 record: p[21..24] time
 * unit (GPS: 1.5 s, BeiDou: BDT s), p[25..30] code chips in it
 */
static double transmit_time(const unsigned char *p, double week0)
{
	double cp = mix32(p, 25) + (p[29] | p[30] << 8) / 65536.0;

	if (p[2] == 1)
		return week0 + 1.5 * mix32(p, 21) + cp / 1.023e6;
	return week0 + mix32(p, 21) + cp / 2.046e6 + 14;
}

/* ---- the Doppler ---- */

/*
 * carrier Doppler of the fully locked channels (-d/dt of the accumulated
 * carrier phase, mix32 p[35..38] in cycles; dt from the records' transmit
 * times, arrival times jitter too much) against the orbit Doppler of a
 * receiver at rest: (measured - orbit) / carrier = the oscillator's
 * frequency offset (per constellation: BeiDou reads ~2 ppb off GPS) + u.v /
 * c for a receiver moving at v. One epoch's samples give velocity and
 * offset (least squares, cycle slips dropped); with too few satellites the
 * receiver is taken as at rest.
 */
struct dsample {
	char sys;
	double v, u[3];
};

struct doppler {
	struct {
		char sys;
		int prn;
		double ttx;
		long long adr;
	} *prev;
	size_t nprev;
	struct dsample *s;
	size_t n, cap;
	int known;
};

static void doppler_record(struct doppler *d, char sys, int prn, const struct eph *e,
			   const unsigned char *p, double ttx, const double pos[3])
{
	long long adr = s32(mix32(p, 35));
	size_t i;
	int have = 0;
	double lt = 0;
	long long la = 0;

	for (i = 0; i < d->nprev; i++)
		if (d->prev[i].sys == sys && d->prev[i].prn == prn)
			break;
	if (i < d->nprev) {
		have = 1;
		lt = d->prev[i].ttx;
		la = d->prev[i].adr;
	} else {
		d->prev = realloc(d->prev, (d->nprev + 1) * sizeof(*d->prev));
		d->prev[i].sys = sys;
		d->prev[i].prn = prn;
		d->nprev++;
	}
	d->prev[i].ttx = ttx;
	d->prev[i].adr = adr;
	if (have && 0.5 < ttx - lt && ttx - lt < 1.5) {
		double f = sysinfo(sys)->freq, rho, rate, clk, sat[3], meas;

		predicted(e, pos, ttx, &rho, &rate, &clk, sat);
		meas = -(double)(adr - la) / (ttx - lt);
		if (d->n == d->cap) {
			d->cap = d->cap ? 2 * d->cap : 32;
			d->s = realloc(d->s, d->cap * sizeof(*d->s));
		}
		d->s[d->n].sys = sys;
		d->s[d->n].v = (meas + rate * f / C_LIGHT) / f;
		for (i = 0; i < 3; i++)
			d->s[d->n].u[i] = (sat[i] - pos[i]) / rho;
		d->n++;
	}
}

/*
 * (oscillator offset in Hz at L1, ECEF velocity m/s or none) for the last
 * epoch: 0 none, 1 offset only, 2 offset + velocity
 */
static int doppler_update(struct doppler *d, double *hz, double vel[3])
{
	struct dsample *s = d->s;
	size_t n = d->n, i;
	char systems[4];
	int nsys = 0, *keep, nkeep, k, ret = 0;

	d->s = NULL;
	d->n = d->cap = 0;
	if (n < (d->known ? 1u : 2u)) {
		free(s);
		return 0;
	}
	d->known = 1;
	for (i = 0; i < n; i++)
		if (!memchr(systems, s[i].sys, nsys))
			systems[nsys++] = s[i].sys;
	/* sorted */
	for (k = 0; k < nsys; k++)
		for (i = k + 1; i < (size_t)nsys; i++)
			if (systems[i] < systems[k]) {
				char t = systems[i];

				systems[i] = systems[k];
				systems[k] = t;
			}
	keep = malloc(n * sizeof(*keep));
	nkeep = n;
	for (i = 0; i < n; i++)
		keep[i] = i;
	while (nkeep >= 5 + nsys) {
		int cols = 3 + nsys, worst = 0;
		double *h = malloc(sizeof(double) * nkeep * cols), *y = malloc(sizeof(double) * nkeep);
		double x[8], rmax = -1;

		for (k = 0; k < nkeep; k++) {
			const struct dsample *q = &s[keep[k]];
			int j;

			for (j = 0; j < 3; j++)
				h[k * cols + j] = q->u[j] / C_LIGHT;
			for (j = 0; j < nsys; j++)
				h[k * cols + 3 + j] = q->sys == systems[j] ? 1.0 : 0.0;
			y[k] = q->v;
		}
		lstsq(h, y, nkeep, cols, x);
		for (k = 0; k < nkeep; k++) {
			double r = y[k];
			int j;

			for (j = 0; j < cols; j++)
				r -= h[k * cols + j] * x[j];
			r = fabs(r);
			if (r > rmax) {
				rmax = r;
				worst = k;
			}
		}
		free(h);
		free(y);
		if (rmax < 5e-9) {
			char ref = memchr(systems, 'G', nsys) ? 'G' : systems[0];
			int ri = (char *)memchr(systems, ref, nsys) - systems;

			*hz = x[3 + ri] * 1575.42e6;
			memcpy(vel, x, 3 * sizeof(double));
			ret = 2;
			goto out;
		}
		memmove(keep + worst, keep + worst + 1, (nkeep - worst - 1) * sizeof(*keep));
		nkeep--;
	}
	{
		double *ref = malloc(n * sizeof(double));
		int nr = 0;

		for (i = 0; i < n; i++)
			if (s[i].sys == 'G')
				ref[nr++] = s[i].v;
		if (!nr)
			for (i = 0; i < n; i++)
				ref[nr++] = s[i].v;
		*hz = median(ref, nr) * 1575.42e6;
		free(ref);
		ret = 1;
	}
out:
	free(keep);
	free(s);
	return ret;
}

/* ---- the position ---- */

struct ob {
	char sys;
	int prn;
	double ttx;
	int cn;
};

struct obs {
	struct ob *o;
	int n, cap;
};

static void obs_set(struct obs *ob, char sys, int prn, double ttx, int cn)
{
	int i;

	for (i = 0; i < ob->n; i++)
		if (ob->o[i].sys == sys && ob->o[i].prn == prn) {
			ob->o[i].ttx = ttx;	/* keeps its place, as a dict */
			ob->o[i].cn = cn;
			return;
		}
	if (ob->n == ob->cap) {
		ob->cap = ob->cap ? 2 * ob->cap : 32;
		ob->o = realloc(ob->o, ob->cap * sizeof(*ob->o));
	}
	ob->o[ob->n++] = (struct ob){ sys, prn, ttx, cn };
}

static int obs_has(const struct obs *ob, char sys, int prn)
{
	int i;

	for (i = 0; i < ob->n; i++)
		if (ob->o[i].sys == sys && ob->o[i].prn == prn)
			return 1;
	return 0;
}

static int cmp_sat(const void *a, const void *b)
{
	const struct sat_cn *x = a, *y = b;

	if (x->sys != y->sys)
		return x->sys < y->sys ? -1 : 1;
	return x->prn - y->prn;
}

/*
 * position from full transmit times: unknowns x, y, z and one clock per
 * constellation; ionosphere (broadcast Klobuchar params) and troposphere
 * delays modelled once near the ground; drops the worst satellite while a
 * residual exceeds 100 m. Returns 1 with the fix, or 0.
 */
static int solve(struct eph_map *eph, const struct obs *in, const double x0[3],
		 const struct iono *iono, struct fix *fix)
{
	struct obs ob = { 0 };
	double *t = malloc(sizeof(double) * (in->n ? in->n : 1)), med;
	int i, ret = 0;

	for (i = 0; i < in->n; i++)
		t[i] = in->o[i].ttx;
	med = median(t, in->n);
	free(t);
	for (i = 0; i < in->n; i++)
		if (fabs(in->o[i].ttx - med) < 0.1)
			obs_set(&ob, in->o[i].sys, in->o[i].prn, in->o[i].ttx, in->o[i].cn);
	for (;;) {
		char systems[4];
		int nsys = 0, cols, it, worst = 0, spare;
		double trx = -INFINITY, clk[4] = { 0 }, y[3], *h = NULL, *res = NULL, rmax = -1;

		for (i = 0; i < ob.n; i++)
			if (!memchr(systems, ob.o[i].sys, nsys))
				systems[nsys++] = ob.o[i].sys;
		if (ob.n < 3 + nsys)
			break;
		for (i = 0; i < nsys; i++) {
			int j;

			for (j = i + 1; j < nsys; j++)
				if (systems[j] < systems[i]) {
					char c = systems[i];

					systems[i] = systems[j];
					systems[j] = c;
				}
		}
		cols = 3 + nsys;
		for (i = 0; i < ob.n; i++)
			if (ob.o[i].ttx > trx)
				trx = ob.o[i].ttx;
		trx += 0.07;
		memcpy(y, x0, sizeof(y));
		h = malloc(sizeof(double) * ob.n * cols);
		res = malloc(sizeof(double) * ob.n);
		for (it = 0; it < 10; it++) {
			double lat = 0, lon = 0, hgt = 0, r_enu[3][3], d[8];
			int near = fabs(norm3(y) - 6.37e6) < 2e5;

			if (near) {
				ecef2geo(y, &lat, &lon, &hgt);
				lat = lat * M_PI / 180;
				lon = lon * M_PI / 180;
				enu(y, r_enu);
			}
			for (i = 0; i < ob.n; i++) {
				const struct ob *o = &ob.o[i];
				const struct eph *e = eph_best(eph_map_get(eph, o->sys, o->prn), o->ttx);
				double s[3], sclk, th, r0, rho, u[3], delay = 0.0;
				int j, si = (char *)memchr(systems, o->sys, nsys) - systems;

				sat_pos(e, o->ttx, s, &sclk);
				th = OMEGA_E * (trx - o->ttx);
				r0 = s[0];
				s[0] = r0 * cos(th) + s[1] * sin(th);
				s[1] = -r0 * sin(th) + s[1] * cos(th);
				{
					double dv[3] = { s[0] - y[0], s[1] - y[1], s[2] - y[2] };

					rho = norm3(dv);
					for (j = 0; j < 3; j++)
						u[j] = dv[j] / rho;
				}
				if (near) {
					double v[3], el, az;

					for (j = 0; j < 3; j++)
						v[j] = r_enu[j][0] * u[0] + r_enu[j][1] * u[1] + r_enu[j][2] * u[2];
					el = asin(v[2]);
					az = atan2(v[0], v[1]);
					delay = atmo_iono(iono, o->ttx, lat, lon, az, el, sysinfo(o->sys)->freq) +
						atmo_tropo(lat, hgt, el);
				}
				res[i] = C_LIGHT * (trx - (o->ttx - sclk)) - (rho + clk[si] + delay);
				h[i * cols + 0] = -u[0];
				h[i * cols + 1] = -u[1];
				h[i * cols + 2] = -u[2];
				for (j = 0; j < nsys; j++)
					h[i * cols + 3 + j] = o->sys == systems[j] ? 1.0 : 0.0;
			}
			lstsq(h, res, ob.n, cols, d);
			for (i = 0; i < 3; i++)
				y[i] += d[i];
			for (i = 0; i < nsys; i++)
				clk[i] += d[3 + i];
			if (norm3(d) > 1e7) {
				free(h);
				free(res);
				goto out;
			}
		}
		for (i = 0; i < ob.n; i++) {
			res[i] = fabs(res[i]);
			if (res[i] > rmax) {
				rmax = res[i];
				worst = i;
			}
		}
		spare = ob.n - 3 - nsys;
		if (rmax < 100 || spare == 0) {
			double q[64], r[3][3], qe[3][3], sum = 0;
			int a, b, c;

			for (a = 0; a < cols; a++)
				for (b = 0; b < cols; b++) {
					double s = 0;

					for (c = 0; c < ob.n; c++)
						s += h[c * cols + a] * h[c * cols + b];
					q[a * cols + b] = s;
				}
			inverse(q, cols);
			enu(y, r);
			for (a = 0; a < 3; a++)
				for (b = 0; b < 3; b++) {
					double s = 0;
					int m, k;

					for (m = 0; m < 3; m++)
						for (k = 0; k < 3; k++)
							s += r[a][m] * q[m * cols + k] * r[b][k];
					qe[a][b] = s;
				}
			memset(fix, 0, sizeof(*fix));
			fix->t = trx - clk[0] / C_LIGHT;
			memcpy(fix->x, y, sizeof(y));
			ecef2geo(y, &fix->lat, &fix->lon, &fix->alt);
			fix->nused = ob.n;
			fix->used = malloc(ob.n * sizeof(*fix->used));
			for (i = 0; i < ob.n; i++)
				fix->used[i] = (struct sat_cn){ ob.o[i].sys, ob.o[i].prn, ob.o[i].cn };
			qsort(fix->used, ob.n, sizeof(*fix->used), cmp_sat);
			fix->spare = spare;
			for (i = 0; i < ob.n; i++)
				sum += res[i] * res[i];
			fix->rms = sqrt(sum / ob.n);
			fix->hdop = sqrt(qe[0][0] + qe[1][1]);
			fix->vdop = sqrt(qe[2][2]);
			free(h);
			free(res);
			ret = 1;
			goto out;
		}
		free(h);
		free(res);
		memmove(ob.o + worst, ob.o + worst + 1, (ob.n - worst - 1) * sizeof(*ob.o));
		ob.n--;
	}
out:
	free(ob.o);
	return ret;
}

/*
 * a fix good enough to report (and with rms = 30 to aid from): spare
 * satellites, residuals below rms (m; indoors multipath gives 10..50), sane
 * height, within reach of the aiding position (or, away from it, two spare
 * satellites)
 */
static int checked(const struct fix *fix, int have, const double pos[3], double pos_unc, double rms)
{
	double d[3];
	int near;

	if (!have || fix->rms >= rms || !(-500 < fix->alt && fix->alt < 9000))
		return 0;
	d[0] = fix->x[0] - pos[0];
	d[1] = fix->x[1] - pos[1];
	d[2] = fix->x[2] - pos[2];
	near = norm3(d) < fmax(50e3, 2 * pos_unc);
	return fix->spare >= (near ? 1 : 2);
}

/* ---- the receiver ---- */

/*
 * power modes: continuous tracking, or at rest a duty cycle: after STILL s
 * below 0.5 m/s the DSPs idle (booted, no satellites: costs nothing
 * measurable, tracking ~20..30 mA) for SLEEP s, then track again until BURST
 * checked fixes (at most BURST_MAX s); moved -> continuous again. Idling
 * keeps the DSP's epoch counter, so the aiding after it is narrow.
 */
struct mode {
	int on;
	double still, sleep;
	int burst;
	double burst_max;
};

struct receiver {
	struct mode mode;
	char *nav;
	double pos[3], clock_hz, pos_unc;
	char *firmware;
	struct receiver_cb cb;
	struct eph_map eph;
	int have_mtime;
	double nav_mtime;
	int have_track;
	double track_x[3], track_t;
	int have_course;
	double course;
	int have_vel;
	double vel[3];
	int vel_age;
	char *cache;
	struct eph_map decoded;
	struct iono iono;
	struct gps_nav gps;
	struct bds_nav bds;
};

static void state(struct receiver *r, const char *fmt, ...)
{
	va_list ap;
	char *s;

	va_start(ap, fmt);
	if (vasprintf(&s, fmt, ap) < 0)
		s = NULL;
	va_end(ap);
	if (s && r->cb.on_state)
		r->cb.on_state(s, r->cb.data);
	free(s);
}

/* a JSON number as a double (Python writes floats, json-glib may write ints) */
static double jnum(JsonObject *o, const char *k, int *ok)
{
	JsonNode *n = json_object_get_member(o, k);

	if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) {
		*ok = 0;
		return 0;
	}
	if (json_node_get_value_type(n) == G_TYPE_INT64)
		return (double)json_node_get_int(n);
	if (json_node_get_value_type(n) == G_TYPE_BOOLEAN)
		return json_node_get_boolean(n);
	return json_node_get_double(n);
}

static int eph_from_json(JsonObject *o, struct eph *e)
{
	int ok = 1;

	memset(e, 0, sizeof(*e));
#define X(f) e->f = jnum(o, #f, &ok);
	EPH_FIELDS(X)
#undef X
	e->toc = jnum(o, "toc", &ok);
	e->toe_abs = jnum(o, "toe_abs", &ok);
	if (json_object_has_member(o, "iodc")) {
		e->iodc = jnum(o, "iodc", &ok);
		e->has_iodc = 1;
	}
	e->confirmed = json_object_has_member(o, "confirmed") ? json_object_get_boolean_member(o, "confirmed") : -1;
	return ok;
}

static void load_cache(struct receiver *r)
{
	JsonParser *p = json_parser_new();
	JsonNode *root;
	JsonObject *c, *eo;
	GList *keys, *k;

	if (!r->cache || !json_parser_load_from_file(p, r->cache, NULL) ||
	    !(root = json_parser_get_root(p)) || !JSON_NODE_HOLDS_OBJECT(root))
		goto out;
	c = json_node_get_object(root);
	if (json_object_has_member(c, "eph") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(c, "eph"))) {
		eo = json_object_get_object_member(c, "eph");
		keys = json_object_get_members(eo);
		for (k = keys; k; k = k->next) {
			const char *key = k->data;
			JsonNode *an = json_object_get_member(eo, key);
			JsonArray *a;
			guint i;
			struct eph_list *l;

			if (strlen(key) < 2 || !JSON_NODE_HOLDS_ARRAY(an))
				continue;
			a = json_node_get_array(an);
			l = eph_map_add(&r->decoded, key[0], atoi(key + 1));
			for (i = 0; i < json_array_get_length(a); i++) {
				JsonNode *en = json_array_get_element(a, i);
				struct eph e;

				if (JSON_NODE_HOLDS_OBJECT(en) && eph_from_json(json_node_get_object(en), &e))
					eph_list_append(l, &e);
			}
		}
		g_list_free(keys);
	}
	if (json_object_has_member(c, "iono") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(c, "iono"))) {
		JsonArray *a = json_object_get_array_member(c, "iono");

		if (json_array_get_length(a) == 3) {
			JsonArray *al = json_array_get_array_element(a, 1), *bl = json_array_get_array_element(a, 2);
			int i;

			r->iono.kind = json_array_get_string_element(a, 0)[0];
			for (i = 0; i < 4 && al && bl; i++) {
				r->iono.alpha[i] = json_node_get_double(json_array_get_element(al, i));
				r->iono.beta[i] = json_node_get_double(json_array_get_element(bl, i));
			}
		}
	}
out:
	g_object_unref(p);
}

static void save_cache(struct receiver *r)
{
	JsonBuilder *b;
	JsonGenerator *g;
	JsonNode *root;
	char *tmp;
	size_t i, k;
	int j;

	if (!r->cache)
		return;
	b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "eph");
	json_builder_begin_object(b);
	for (i = 0; i < r->decoded.n; i++) {
		struct eph_list *l = &r->decoded.l[i];
		char key[16];

		snprintf(key, sizeof(key), "%c%d", l->sys, l->prn);
		json_builder_set_member_name(b, key);
		json_builder_begin_array(b);
		for (k = 0; k < l->n; k++) {
			const struct eph *e = &l->e[k];

			json_builder_begin_object(b);
#define X(f) json_builder_set_member_name(b, #f); json_builder_add_double_value(b, e->f);
			EPH_FIELDS(X)
#undef X
			json_builder_set_member_name(b, "toc");
			json_builder_add_double_value(b, e->toc);
			json_builder_set_member_name(b, "toe_abs");
			json_builder_add_double_value(b, e->toe_abs);
			if (e->has_iodc) {
				json_builder_set_member_name(b, "iodc");
				json_builder_add_double_value(b, e->iodc);
			}
			if (e->confirmed >= 0) {
				json_builder_set_member_name(b, "confirmed");
				json_builder_add_boolean_value(b, e->confirmed);
			}
			json_builder_end_object(b);
		}
		json_builder_end_array(b);
	}
	json_builder_end_object(b);
	json_builder_set_member_name(b, "iono");
	if (r->iono.kind) {
		char kind[2] = { r->iono.kind, 0 };

		json_builder_begin_array(b);
		json_builder_add_string_value(b, kind);
		json_builder_begin_array(b);
		for (j = 0; j < 4; j++)
			json_builder_add_double_value(b, r->iono.alpha[j]);
		json_builder_end_array(b);
		json_builder_begin_array(b);
		for (j = 0; j < 4; j++)
			json_builder_add_double_value(b, r->iono.beta[j]);
		json_builder_end_array(b);
		json_builder_end_array(b);
	} else {
		json_builder_add_null_value(b);
	}
	json_builder_end_object(b);
	root = json_builder_get_root(b);
	g = json_generator_new();
	json_generator_set_root(g, root);
	tmp = g_strconcat(r->cache, ".new", NULL);
	if (json_generator_to_file(g, tmp, NULL))
		rename(tmp, r->cache);
	g_free(tmp);
	g_object_unref(g);
	json_node_unref(root);
	g_object_unref(b);
}

/* dict equality of two records (the same keys and values) */
static int eph_equal(const struct eph *a, const struct eph *b)
{
#define X(f) if (a->f != b->f) return 0;
	EPH_FIELDS(X)
#undef X
	return a->toc == b->toc && a->toe_abs == b->toe_abs && a->has_iodc == b->has_iodc &&
	       (!a->has_iodc || a->iodc == b->iodc) && a->confirmed == b->confirmed;
}

/* (re)load the downloaded file when it changed and merge in the decoded ephemerides */
static void load_orbits(struct receiver *r)
{
	struct stat st;
	int have = 0;
	double m = 0;
	struct eph_map eph = { 0 };
	size_t i, k;

	if (r->nav && !stat(r->nav, &st)) {
		have = 1;
		m = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
	}
	if (have == r->have_mtime && (!have || m == r->nav_mtime) && r->eph.n)
		return;
	if (have && m)
		rinex_nav(r->nav, &eph);
	r->have_mtime = have;
	r->nav_mtime = m;
	for (i = 0; i < r->decoded.n; i++) {
		struct eph_list *d = &r->decoded.l[i], *l = eph_map_get(&eph, d->sys, d->prn);
		size_t nhave = l ? l->n : 0;

		l = eph_map_add(&eph, d->sys, d->prn);
		for (k = 0; k < d->n; k++) {
			size_t j;
			int dup = 0;

			for (j = 0; j < nhave; j++)
				dup |= l->e[j].toe_abs == d->e[k].toe_abs;
			if (!dup)
				eph_list_append(l, &d->e[k]);
		}
	}
	eph_map_free(&r->eph);
	r->eph = eph;
	malloc_trim(0);		/* the parse's free heap back to the system */
}

/*
 * a new ephemeris from the satellite: use it, keep it in the cache (a
 * confirmed one replaces one of the same toe from single words)
 */
static void decoded(struct receiver *r, char sys, int prn, struct eph e, int confirmed)
{
	struct eph_list *recs = eph_map_add(&r->decoded, sys, prn), *have;
	struct eph *same = NULL;
	size_t i, j, nsame = 0;
	double now;
	int any;

	for (i = 0; i < recs->n; i++)
		if (recs->e[i].toe_abs == e.toe_abs) {
			if (!nsame)
				same = &recs->e[i];
			nsame++;
		}
	if (nsame && (!confirmed || same->confirmed != 0))
		return;
	for (i = 0; i < recs->n;) {
		if (recs->e[i].toe_abs == e.toe_abs) {
			struct eph r0 = recs->e[i];
			struct eph_list *el = eph_map_get(&r->eph, sys, prn);

			eph_list_remove(recs, i);
			if (el)
				for (j = 0; j < el->n; j++)
					if (eph_equal(&el->e[j], &r0)) {
						eph_list_remove(el, j);
						break;
					}
		} else {
			i++;
		}
	}
	e.confirmed = confirmed;
	eph_list_append(recs, &e);
	now = gps_now();
	for (i = 0; i < r->decoded.n; i++) {
		struct eph_list *l = &r->decoded.l[i];

		for (j = 0; j < l->n;)
			if (!(now - l->e[j].toe_abs < DECODED_DAYS * 86400))
				eph_list_remove(l, j);
			else
				j++;
	}
	have = eph_map_add(&r->eph, sys, prn);
	any = 0;
	for (i = 0; i < have->n; i++)
		any |= have->e[i].toe_abs == e.toe_abs;
	if (!any)
		eph_list_append(have, &e);
	save_cache(r);
}

/*
 * an ephemeris from words received once, checked against an older record
 * of the satellite (days-old broadcast orbits are within ~1.5 km and well
 * under a microsecond; one wrong bit in a significant field moves the
 * satellite much further)
 */
static int plausible(struct receiver *r, char sys, int prn, const struct eph *e)
{
	struct eph_list *old = eph_map_get(&r->eph, sys, prn);
	double now = gps_now(), ts[2];
	const struct eph *ref;
	int i;

	if (!old || !old->n || fabs(e->toe_abs - now) > 3 * 3600)
		return 0;
	ref = eph_best(old, now);
	if (fabs(now - ref->toe_abs) > STALE_CODE)	/* beyond 3 days the range drifts too far */
		return 0;
	ts[0] = now;
	ts[1] = now + 1800;
	for (i = 0; i < 2; i++) {
		double p1[3], p0[3], c1, c0, d[3];

		sat_pos(e, ts[i], p1, &c1);
		sat_pos(ref, ts[i], p0, &c0);
		d[0] = p1[0] - p0[0];
		d[1] = p1[1] - p0[1];
		d[2] = p1[2] - p0[2];
		if (norm3(d) > 3000 || fabs(c1 - c0) * C_LIGHT > 1000)
			return 0;
	}
	return 1;
}

/* navigation message frames: ephemerides and ionosphere */
static void navmsg(struct receiver *r, int mid, const unsigned char *p, size_t len)
{
	int week = (int)py_floordiv(gps_now(), 604800), gps = mid == 0xfe2b, prn;
	struct iono io = { 0 };

	prn = gps ? gps_nav_add(&r->gps, p, len) : bds_nav_add(&r->bds, p, len);
	if (prn) {
		char sys = gps ? 'G' : 'C';
		struct eph e;
		int ok, confirmed = 1;

		ok = gps ? gps_nav_ephemeris(&r->gps, prn, week, 1, &e)
			 : bds_nav_ephemeris(&r->bds, prn, week, 1, 0, &e);
		if (!ok) {
			struct eph c1, c2;
			int k1, k2 = 0;

			confirmed = 0;
			k1 = gps ? gps_nav_ephemeris(&r->gps, prn, week, 0, &c1)
				 : bds_nav_ephemeris(&r->bds, prn, week, 0, 0, &c1);
			if (!gps)
				k2 = bds_nav_ephemeris(&r->bds, prn, week, 0, 1, &c2);
			if (k1 && plausible(r, sys, prn, &c1)) {
				e = c1;
				ok = 1;
			} else if (k2 && plausible(r, sys, prn, &c2)) {
				e = c2;
				ok = 1;
			}
		}
		if (ok)
			decoded(r, sys, prn, e, confirmed);
	}
	if (r->gps.has_iono) {
		io.kind = 'G';
		memcpy(io.alpha, r->gps.iono_alpha, sizeof(io.alpha));
		memcpy(io.beta, r->gps.iono_beta, sizeof(io.beta));
	} else if (r->bds.has_iono && !(r->iono.kind == 'G')) {
		io.kind = 'C';
		memcpy(io.alpha, r->bds.iono_alpha, sizeof(io.alpha));
		memcpy(io.beta, r->bds.iono_beta, sizeof(io.beta));
	}
	if (io.kind && (io.kind != r->iono.kind || memcmp(io.alpha, r->iono.alpha, sizeof(io.alpha)) ||
			memcmp(io.beta, r->iono.beta, sizeof(io.beta)))) {
		r->iono = io;
		save_cache(r);
	}
}

static void put16(unsigned char *p, int v)
{
	p[0] = v;
	p[1] = v >> 8;
}

/*
 * time / frequency: s16 receiver clock offset (Hz at the link's carrier), u16
 * 0, u16 its uncertainty, u32 the previous FE00 tick (high half first), u16
 * status (0x0421; cold start 0x0400 with the stock software's uncertainty
 * 7092), u8 0, u16 GPS time of week in 1.5 s units (low bits), u8 0, u16 0
 */
static size_t fe01(struct receiver *r, int k, const char *link, int cold, unsigned char *p)
{
	long long tick = k ? 1014 + 1000LL * (k - 1) : 0;
	int unit = (int)py_floordiv(py_mod(gps_now(), 604800), 1.5) & 0xffff;

	put16(p, (int)py_round(r->clock_hz * carrier(link) / carrier("L1")));
	put16(p + 2, 0);
	put16(p + 4, cold ? 7092 : 4305);
	put16(p + 6, (tick >> 16) & 0xffff);
	put16(p + 8, tick & 0xffff);
	put16(p + 10, cold ? 0x0400 : 0x0421);
	p[12] = 0;
	put16(p + 13, unit);
	p[15] = 0;
	put16(p + 16, 0);
	return 18;
}

/*
 * velocity-aided smoothing (gain K per epoch; reset on a jump or a gap) and
 * speed / course from the velocity averaged over ~2 s: at rest indoors
 * single epochs read 0.35 m/s (95 %: 1.2 m/s), so below 0.5 m/s the speed is
 * 0 and the course follows only above 1.5 m/s
 */
static void smooth(struct receiver *r, struct fix *fix, int has_vel, const double vel[3])
{
	const double K = 0.25;
	double x[3], t = fix->t;
	int i;

	memcpy(x, fix->x, sizeof(x));
	if (r->have_track && t - r->track_t < 5) {
		double pred[3], d[3];

		for (i = 0; i < 3; i++)
			pred[i] = r->track_x[i] + (has_vel ? vel[i] : 0) * (t - r->track_t);
		for (i = 0; i < 3; i++)
			d[i] = x[i] - pred[i];
		if (norm3(d) < 150)
			for (i = 0; i < 3; i++)
				x[i] = pred[i] + K * (x[i] - pred[i]);
	}
	r->have_track = 1;
	memcpy(r->track_x, x, sizeof(x));
	r->track_t = t;
	ecef2geo(x, &fix->lat, &fix->lon, &fix->alt);
	memcpy(fix->xs, x, sizeof(x));
	fix->acc = fix->hdop * fmax(fix->rms, 3.0) * (has_vel ? 0.7 : 1.0);
	fix->has_speed = 0;
	if (has_vel) {
		if (!r->have_vel)
			memcpy(r->vel, vel, sizeof(r->vel));
		else
			for (i = 0; i < 3; i++)
				r->vel[i] = 0.5 * (r->vel[i] + vel[i]);
		r->have_vel = 1;
		r->vel_age = 0;
	} else if (++r->vel_age > 5) {	/* an epoch without enough Doppler: keep the last few s */
		r->have_vel = 0;
	}
	if (r->have_vel) {
		double e3[3][3], v[3], speed;

		enu(x, e3);
		for (i = 0; i < 3; i++)
			v[i] = e3[i][0] * r->vel[0] + e3[i][1] * r->vel[1] + e3[i][2] * r->vel[2];
		speed = hypot(v[0], v[1]);
		fix->has_speed = 1;
		fix->speed = speed >= 0.5 ? speed : 0.0;
		if (speed > 1.5) {
			r->course = py_mod(atan2(v[0], v[1]) * 180 / M_PI, 360);
			r->have_course = 1;
		}
	}
	fix->has_course = r->have_course;
	fix->course = r->course;
}

struct receiver *receiver_new(const char *nav_file, const double pos[3], double pos_unc,
			      double clock_hz, const char *firmware, const char *cache,
			      const char *mode, struct receiver_cb cb)
{
	struct receiver *r = calloc(1, sizeof(*r));

	if (!mode || !strcmp(mode, "balanced") || (strcmp(mode, "continuous") && strcmp(mode, "low")))
		r->mode = (struct mode){ 1, 60, 30, 5, 90 };
	else if (!strcmp(mode, "low"))
		r->mode = (struct mode){ 1, 30, 120, 3, 60 };
	r->nav = nav_file ? strdup(nav_file) : NULL;
	memcpy(r->pos, pos, sizeof(r->pos));
	r->clock_hz = clock_hz;
	r->pos_unc = pos_unc;
	r->firmware = strdup(firmware);
	r->cb = cb;
	if (cache) {
		if (asprintf(&r->cache, "%s/decoded.json", cache) < 0)
			r->cache = NULL;
	}
	load_cache(r);
	return r;
}

void receiver_result(struct receiver *r, double pos[3], double *pos_unc, double *clock_hz)
{
	memcpy(pos, r->pos, sizeof(r->pos));
	*pos_unc = r->pos_unc;
	*clock_hz = r->clock_hz;
}

void receiver_free(struct receiver *r)
{
	size_t i;

	eph_map_free(&r->eph);
	eph_map_free(&r->decoded);
	free(r->gps.s);
	free(r->gps.pending);
	for (i = 0; i < r->bds.n; i++) {
		int k, j;

		for (k = 0; k < 3; k++)
			for (j = 0; j < 10; j++)
				free(r->bds.s[i].votes[k][j].v);
	}
	free(r->bds.s);
	free(r->bds.pending);
	free(r->nav);
	free(r->firmware);
	free(r->cache);
	free(r);
}

/* component-wise median of n positions */
static void median3(double (*x)[3], int n, double out[3])
{
	double *v = malloc(sizeof(double) * (n ? n : 1));
	int i, k;

	for (k = 0; k < 3; k++) {
		for (i = 0; i < n; i++)
			v[i] = x[i][k];
		out[k] = median(v, n);
	}
	free(v);
}

struct still {
	double t;
	int has_speed;
	double speed, xs[3];
};

struct loop {
	struct receiver *r;
	struct doppler doppler;
	struct {
		long long tick;
		double t;
	} *epochs;
	size_t nepochs;
	int have_timed;
	long long timed_tick;
	double timed_t, timed_unc;
	/* the epoch being received */
	int have_epoch;
	long long ep_tick;
	double ep_t;
	struct obs ep_obs;
	struct {
		double t;
		int fresh;
	} *ep_rx;
	size_t ep_nrx;
	int have_fix, nlocked;
	struct fix fix;
	/* duty cycle */
	double idle_until;
	struct still *still;
	size_t nstill;
	int have_burst, burst_n, burst_fast;
	double burst_t, burst_x[3];
	double (*burst_xs)[3];
	int burst_nxs;
	int have_rest;
	double rest[3];
	struct eph_map *eph;
};

static void duty(struct loop *L, struct fix *fix)
{
	struct receiver *r = L->r;
	struct mode *m = &r->mode;
	size_t i;

	if (!m->on)
		return;
	if (L->have_burst) {
		char why[64] = "";

		/*
		 * moved: two fixes in a row faster than 1.5 m/s, or at the
		 * burst's end its median position away from the one before the
		 * pause, horizontally (indoors single fixes scatter 20..40 m,
		 * heights ~100 m)
		 */
		L->burst_fast = (fix->has_speed ? fix->speed : 0) > 1.5 ? L->burst_fast + 1 : 0;
		if (fix->hdop < 3 && fix->spare >= 2) {	/* right after reacquiring: few sats, weak geometry */
			L->burst_n++;
			L->burst_xs = realloc(L->burst_xs, (L->burst_nxs + 1) * sizeof(*L->burst_xs));
			memcpy(L->burst_xs[L->burst_nxs++], fix->xs, sizeof(double) * 3);
		}
		if (L->burst_fast >= 2)
			snprintf(why, sizeof(why), "%.1f m/s", fix->speed);
		if (!why[0] && L->burst_n >= m->burst) {
			double med[3], d3[3], e3[3][3], h[2], d;
			int k;

			median3(L->burst_xs, L->burst_nxs, med);
			for (k = 0; k < 3; k++)
				d3[k] = med[k] - L->burst_x[k];
			enu(L->burst_x, e3);
			for (k = 0; k < 2; k++)
				h[k] = e3[k][0] * d3[0] + e3[k][1] * d3[1] + e3[k][2] * d3[2];
			d = sqrt(h[0] * h[0] + h[1] * h[1]);	/* horizontal */
			if (d > fmax(75.0, 3 * fix->acc))
				snprintf(why, sizeof(why), "%.0f m away", d);
			if (!why[0]) {
				for (k = 0; k < 3; k++)
					L->rest[k] = 0.5 * (L->rest[k] + med[k]);
				L->have_burst = 0;
				L->idle_until = mono_s() + m->sleep;
			}
		}
		if (why[0]) {
			L->have_burst = 0;	/* moving: continuous */
			L->nstill = 0;
			state(r, "moving (%s): continuous tracking", why);
		}
		return;
	}
	/*
	 * at rest: the last STILL s of speeds, under 20 % of them 0.5 m/s or
	 * more (indoors single readings reach ~0.9 m/s), none 2 m/s
	 */
	L->still = realloc(L->still, (L->nstill + 1) * sizeof(*L->still));
	L->still[L->nstill].t = fix->t;
	L->still[L->nstill].has_speed = fix->has_speed;
	L->still[L->nstill].speed = fix->speed;
	memcpy(L->still[L->nstill].xs, fix->xs, sizeof(double) * 3);
	L->nstill++;
	while (L->nstill && fix->t - L->still[0].t > m->still) {
		memmove(L->still, L->still + 1, (L->nstill - 1) * sizeof(*L->still));
		L->nstill--;
	}
	{
		int none = 0, fast = 0;
		double vmax = -INFINITY;

		for (i = 0; i < L->nstill; i++) {
			if (!L->still[i].has_speed) {
				none = 1;
				continue;
			}
			fast += L->still[i].speed >= 0.5;
			vmax = fmax(vmax, L->still[i].speed);
		}
		if (L->nstill && fix->t - L->still[0].t >= m->still * 0.9 && !none &&
		    fast < 0.2 * L->nstill && vmax < 2.0) {
			double (*xs)[3] = malloc(L->nstill * sizeof(*xs));

			for (i = 0; i < L->nstill; i++)
				memcpy(xs[i], L->still[i].xs, sizeof(double) * 3);
			median3(xs, L->nstill, L->rest);	/* where it rests */
			free(xs);
			L->have_rest = 1;
			L->idle_until = mono_s() + m->sleep;
			L->nstill = 0;
			state(r, "at rest: tracking %.0f s off, then %d fixes", m->sleep, m->burst);
		}
	}
}

/*
 * an epoch's records are in (they follow its FE28 within ms): time it,
 * velocity / clock, position
 */
static void finish(struct loop *L)
{
	struct receiver *r = L->r;
	double hz, vel[3];
	int dv, i;

	if (L->ep_nrx) {	/* fresh orbits time the epoch to ~a chip, days-old ones to ~1.5 km */
		double *t = malloc(L->ep_nrx * sizeof(double));
		int nf = 0;

		for (i = 0; i < (int)L->ep_nrx; i++)
			if (L->ep_rx[i].fresh)
				t[nf++] = L->ep_rx[i].t;
		if (!nf)
			for (i = 0; i < (int)L->ep_nrx; i++)
				t[nf++] = L->ep_rx[i].t;
		L->have_timed = 1;
		L->timed_tick = L->ep_tick;
		L->timed_unc = 0;
		for (i = 0; i < (int)L->ep_nrx; i++)
			if (L->ep_rx[i].fresh)
				break;
		L->timed_unc = i < (int)L->ep_nrx ? 0.0 : 1500.0;
		L->timed_t = median(t, nf);
		free(t);
	}
	dv = doppler_update(&L->doppler, &hz, vel);
	if (dv)
		r->clock_hz = hz;
	L->nlocked = L->ep_obs.n;
	if (L->have_fix)
		free(L->fix.used);
	L->have_fix = L->ep_obs.n ? solve(L->eph, &L->ep_obs, r->pos, &r->iono, &L->fix) : 0;
	if (checked(&L->fix, L->have_fix, r->pos, r->pos_unc, 75)) {
		L->fix.clock_hz = r->clock_hz;
		smooth(r, &L->fix, dv == 2, vel);
		if (checked(&L->fix, 1, r->pos, r->pos_unc, 30)) {
			memcpy(r->pos, L->fix.xs, sizeof(r->pos));
			r->pos_unc = 30.0;
		}
		if (r->cb.on_fix)
			r->cb.on_fix(&L->fix, r->cb.data);
		duty(L, &L->fix);
	}
}

static void epoch_reset(struct loop *L)
{
	L->ep_obs.n = 0;
	L->ep_nrx = 0;
}

static void epochs_put(struct loop *L, long long tick, double t)
{
	size_t i;

	for (i = 0; i < L->nepochs; i++)
		if (L->epochs[i].tick == tick) {
			L->epochs[i].t = t;
			goto pop;
		}
	L->epochs = realloc(L->epochs, (L->nepochs + 1) * sizeof(*L->epochs));
	L->epochs[L->nepochs].tick = tick;
	L->epochs[L->nepochs++].t = t;
pop:
	for (i = 0; i < L->nepochs; i++)
		if (L->epochs[i].tick == tick - 5000) {
			memmove(L->epochs + i, L->epochs + i + 1, (L->nepochs - i - 1) * sizeof(*L->epochs));
			L->nepochs--;
			break;
		}
}

static int epochs_get(struct loop *L, long long tick, double *t)
{
	size_t i;

	for (i = 0; i < L->nepochs; i++)
		if (L->epochs[i].tick == tick) {
			*t = L->epochs[i].t;
			return 1;
		}
	return 0;
}

static void receiver_loop(struct receiver *r, struct link *l1, struct link *l5,
			  volatile int *stop, double duration)
{
	struct loop L = { .r = r };
	double start = mono_s();
	long long k = 0;

	L.eph = &r->eph;
	while (!*stop && (duration <= 0 || mono_s() - start < duration)) {
		double week0, t_ep, t_unc = 0, end;
		long long last;
		int known, idle, cold, nplan, li;
		struct plan *plan = NULL;

		if (k % 60 == 0)
			load_orbits(r);
		L.eph = &r->eph;
		week0 = py_floordiv((double)(long long)gps_now(), 604800) * 604800;	/* GPS weeks start at the epoch */
		/*
		 * GPS time of the DSP's last epoch (the previous FE00 tick, also
		 * in FE01), which the aiding refers to: from the last timed
		 * epoch, else from the system time its FE28 arrived at
		 */
		last = 1014 + 1000 * (k - 1);
		if (L.have_timed && 0 <= last - L.timed_tick && last - L.timed_tick < 900000) {
			/*
			 * the DSP's ms counter runs on the receiver oscillator (its
			 * offset ~1e-8: ~1 chip per 100 s since the timed epoch)
			 */
			double dt = (last - L.timed_tick) / 1000.0;

			t_ep = L.timed_t + dt;
			known = 1;
			t_unc = L.timed_unc + dt * (1e-8 + fabs(r->clock_hz) / 1575.42e6) * C_LIGHT;
		} else {
			if (!epochs_get(&L, last, &t_ep))
				t_ep = gps_now() - 0.1;
			known = 0;
			t_unc = 0.0;
		}
		idle = mono_s() < L.idle_until;
		if (!idle && L.idle_until && !L.have_burst) {
			L.have_burst = 1;
			L.burst_n = 0;
			L.burst_t = mono_s();
			memcpy(L.burst_x, L.rest, sizeof(L.rest));
			L.burst_nxs = 0;
			L.burst_fast = 0;
			L.idle_until = 0.0;
		}
		if (L.have_burst && mono_s() - L.burst_t > r->mode.burst_max) {
			L.have_burst = 0;	/* no fix: try later */
			L.idle_until = mono_s() + r->mode.sleep;
			idle = 1;
		}
		nplan = idle ? 0 : fe02(&r->eph, r->pos, t_ep, r->clock_hz, known, r->pos_unc + t_unc, &plan);
		cold = !nplan && !idle;	/* no orbit data at all (offline, nothing decoded yet) */
		for (li = 0; li < 2; li++) {
			struct link *ln = li ? l5 : l1;
			const char *name = li ? "L5" : "L1";
			long long tick_k = 1014 + 1000 * k;	/* DSP epoch tick, ms (32 bit, high half first) */
			unsigned char b[64];
			int j;

			send_hex(ln, 0xfe05, "3400000100");
			put16(b, (tick_k >> 16) & 0xffff);
			put16(b + 2, tick_k & 0xffff);
			link_send(ln, 0xfe00, b, 4);
			send_hex(ln, 0xfe10, li ? fe10_l5 : fe10_l1);
			send_hex(ln, 0xfe11, fe11);
			link_send(ln, 0xfe01, b, fe01(r, k, name, cold, b));
			if (!li) {
				unsigned char *list = malloc(2 + 2 * nplan);

				for (j = 0; j < nplan; j++)
					link_send(ln, 0xfe02, plan[j].payload, 20);
				list[0] = 0;
				list[1] = nplan;
				for (j = 0; j < nplan; j++) {
					list[2 + 2 * j] = plan[j].prn;
					list[3 + 2 * j] = sysinfo(plan[j].sys)->code;
				}
				link_send(ln, 0xfe03, list, 2 + 2 * nplan);
				free(list);
			}
			if (cold) {
				unsigned char bl[512];

				if (li) {
					unsigned char z[2] = { 0, 0 };

					link_send(ln, 0xfe03, z, 2);
				}
				link_send(ln, 0xfe03, bl, unhex(li ? blind_l5 : blind_l1, bl));
			} else if (!li) {	/* (idle: both lists empty) */
				unsigned char one[2] = { 1, 0 };

				link_send(ln, 0xfe03, one, 2);
			}
			link_send(ln, 0xfe1c, NULL, 0);
			send_hex(ln, 0xfe05, "2900");
		}
		k++;
		/*
		 * the DSP's reports until the next second: per epoch an FE28 (its
		 * tick) then its FE29 channel records; fully locked channels
		 * (state 20, byte 6 = 64) time the epoch and give the position
		 */
		end = start + k;
		while (mono_s() < end && !*stop) {
			struct frame *fr;
			size_t n, i;

			sleep_s(0.05);
			n = link_take(l5, &fr);
			for (i = 0; i < n; i++)
				frame_free(&fr[i]);
			free(fr);
			n = link_take(l1, &fr);
			for (i = 0; i < n; i++) {
				const unsigned char *p = fr[i].p;
				size_t len = fr[i].len;
				int mid = fr[i].mid;

				if (mid == 0xfe2b || mid == 0xfe4b) {
					navmsg(r, mid, p, len);
					continue;
				}
				if (mid == 0xfe28) {
					long long tick;

					if (len < 5)
						continue;
					if (L.have_epoch)
						finish(&L);
					tick = (p[3] | p[4] << 8) | (long long)(p[1] | p[2] << 8) << 16;
					epochs_put(&L, tick, gps_now());
					L.have_epoch = 1;
					L.ep_tick = tick;
					L.ep_t = mono_s();
					epoch_reset(&L);
					continue;
				}
				if (mid != 0xfe29 || len < 39 || !L.have_epoch)
					continue;
				{
					char sys = code_sys(p[2]);
					double ttx;
					const struct eph *e;
					struct eph_list *el;

					if (!sys || p[5] != 20 || p[6] != 64 || p[3] < MIN_CN0)
						continue;
					ttx = transmit_time(p, week0);
					if (ttx - t_ep > 302400)	/* week rollover between now and the record */
						ttx -= 604800;
					else if (t_ep - ttx > 302400)
						ttx += 604800;
					e = eph_usable(&r->eph, sys, p[1], ttx);
					if (e) {
						obs_set(&L.ep_obs, sys, p[1], ttx, p[3]);
					} else if ((el = eph_map_get(&r->eph, sys, p[1])) && el->n) {
						/* days-old orbits: Doppler / timing only */
						e = eph_best(el, ttx);
						if (fabs(ttx - e->toe_abs) > STALE || (long long)e->svh)
							continue;
					} else {
						continue;
					}
					if (p[3] >= 18) {
						doppler_record(&L.doppler, sys, p[1], e, p, ttx, r->pos);
						if (obs_has(&L.ep_obs, sys, p[1]) || fabs(ttx - e->toe_abs) < STALE_CODE) {
							double rho, rate, clk, s_[3];

							predicted(e, r->pos, ttx + 0.075, &rho, &rate, &clk, s_);
							L.ep_rx = realloc(L.ep_rx, (L.ep_nrx + 1) * sizeof(*L.ep_rx));
							L.ep_rx[L.ep_nrx].t = ttx + rho / C_LIGHT - clk;
							L.ep_rx[L.ep_nrx++].fresh = obs_has(&L.ep_obs, sys, p[1]);
						}
					}
				}
			}
			for (i = 0; i < n; i++)
				frame_free(&fr[i]);
			free(fr);
			if (L.have_epoch && mono_s() - L.ep_t > 0.3) {
				finish(&L);
				L.have_epoch = 0;
			}
		}
		if (k % 10 == 0) {
			char sol[64], what[32];

			if (L.have_fix)
				snprintf(sol, sizeof(sol), "rms %.0f m, %d spare", L.fix.rms, L.fix.spare);
			else
				snprintf(sol, sizeof(sol), "no solution");
			if (idle)
				snprintf(what, sizeof(what), "idle");
			else if (cold)
				snprintf(what, sizeof(what), "cold search");
			else
				snprintf(what, sizeof(what), "%d planned", nplan);
			state(r, "%lld s: %s, %d locked (%s), clock %+.0f Hz, epoch %s, %zu decoded", k, what,
			      L.nlocked, sol, r->clock_hz, known ? "timed" : "from system time", r->decoded.n);
		}
		free(plan);
	}
	if (L.have_fix)
		free(L.fix.used);
	free(L.doppler.prev);
	free(L.doppler.s);
	free(L.epochs);
	free(L.ep_obs.o);
	free(L.ep_rx);
	free(L.still);
	free(L.burst_xs);
}

int receiver_run(struct receiver *r, volatile int *stop, double duration)
{
	static const char *const names[] = { "L5", "L1" };	/* L5 must boot first */
	struct link links[2];
	int opened = 0, ok = 0, i;
	size_t s;

	load_orbits(r);
	for (i = 0; i < 2; i++) {
		char *path;
		FILE *f;
		unsigned char *image;
		long n;

		if (link_open(&links[i], names[i])) {
			state(r, "%s: cannot open the DSP link", names[i]);
			goto out;
		}
		opened++;
		if (asprintf(&path, "%s/gnss-%s.bin", r->firmware, i ? "l1" : "l5") < 0)
			goto out;
		f = fopen(path, "re");
		free(path);
		if (!f) {
			state(r, "%s: no DSP image", names[i]);
			goto out;
		}
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		image = malloc(n);
		n = fread(image, 1, n, f);
		fclose(f);
		if (!dsp_boot(&links[i], image, n)) {
			free(image);
			state(r, "%s DSP boot failed", names[i]);
			goto out;
		}
		free(image);
	}
	for (s = 0; s < sizeof(setup) / sizeof(*setup); s++)
		send_hex(&links[!strcmp(setup[s].link, "L1")], setup[s].mid, setup[s].hex);
	state(r, "DSPs running");
	receiver_loop(r, &links[1], &links[0], stop, duration);
	ok = 1;
out:
	for (i = 0; i < opened; i++)
		link_close(&links[i]);
	return ok;
}
