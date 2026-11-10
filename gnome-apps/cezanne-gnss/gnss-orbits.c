// SPDX-License-Identifier: MIT
/*
 * Broadcast orbits: RINEX 3 navigation files (GPS, QZSS, Galileo, BeiDou
 * Kepler records), satellite position / clock, coordinates; the atmospheric
 * delays of a pseudorange; per-satellite acquisition aiding (FE02); NMEA
 * sentences; the small linear algebra the receiver needs. Arithmetic in the
 * order of the Python version (orbits.py, atmo.py, aid.py, nmea.py), whose
 * float semantics (floor division, modulo, round) py_* reproduce.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gnss.h"

int gnss_leap = 18;

/* ---- Python float semantics ---- */

/* CPython's float floor division and modulo (_float_div_mod) */
static void float_div_mod(double vx, double wx, double *floordiv, double *mod_out)
{
	double mod = fmod(vx, wx), div = (vx - mod) / wx, fd;

	if (mod) {
		if ((wx < 0) != (mod < 0)) {
			mod += wx;
			div -= 1.0;
		}
	} else {
		mod = copysign(0.0, wx);
	}
	if (div) {
		fd = floor(div);
		if (div - fd > 0.5)
			fd += 1.0;
	} else {
		fd = copysign(0.0, vx / wx);
	}
	*floordiv = fd;
	*mod_out = mod;
}

double py_floordiv(double a, double b)
{
	double d, m;

	float_div_mod(a, b, &d, &m);
	return d;
}

double py_mod(double a, double b)
{
	double d, m;

	float_div_mod(a, b, &d, &m);
	return m;
}

/* round(): halves to even */
long long py_round(double x)
{
	return (long long)nearbyint(x);
}

/* ---- the ephemeris map ---- */

struct eph_list *eph_map_get(struct eph_map *m, char sys, int prn)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		if (m->l[i].sys == sys && m->l[i].prn == prn)
			return &m->l[i];
	return NULL;
}

struct eph_list *eph_map_add(struct eph_map *m, char sys, int prn)
{
	struct eph_list *l = eph_map_get(m, sys, prn);

	if (l)
		return l;
	if (m->n == m->cap) {
		m->cap = m->cap ? 2 * m->cap : 64;
		m->l = realloc(m->l, m->cap * sizeof(*m->l));
	}
	l = &m->l[m->n++];
	memset(l, 0, sizeof(*l));
	l->sys = sys;
	l->prn = prn;
	return l;
}

void eph_list_append(struct eph_list *l, const struct eph *e)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? 2 * l->cap : 8;
		l->e = realloc(l->e, l->cap * sizeof(*l->e));
	}
	l->e[l->n++] = *e;
}

void eph_list_remove(struct eph_list *l, size_t i)
{
	memmove(l->e + i, l->e + i + 1, (l->n - i - 1) * sizeof(*l->e));
	l->n--;
}

void eph_map_free(struct eph_map *m)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		free(m->l[i].e);
	free(m->l);
	memset(m, 0, sizeof(*m));
}

void eph_map_copy(struct eph_map *dst, const struct eph_map *src)
{
	size_t i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < src->n; i++) {
		struct eph_list *l = eph_map_add(dst, src->l[i].sys, src->l[i].prn);
		size_t k;

		for (k = 0; k < src->l[i].n; k++)
			eph_list_append(l, &src->l[i].e[k]);
	}
}

/* ---- time ---- */

double real_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* GPS seconds since 1980-01-06 from the system clock */
double gps_now(void)
{
	return real_s() - 315964800 + gnss_leap;
}

/* days from 1970-01-01 of a proleptic Gregorian date */
static long long days_from_civil(long long y, unsigned m, unsigned d)
{
	long long era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long long)doe - 719468;
}

/* seconds since the GPS epoch (1980-01-06) of a calendar time */
double gps_seconds(int y, int mo, int d, int h, int mi, int s)
{
	long long days = days_from_civil(y, mo, d) - days_from_civil(1980, 1, 6);

	return (double)(days * 86400 + h * 3600 + mi * 60 + s);
}

/* ---- RINEX ---- */

static char *slice(const char *line, size_t len, size_t from, size_t to, char *buf, size_t size)
{
	size_t n = 0, i;

	for (i = from; i < to && i < len && n + 1 < size; i++)
		buf[n++] = line[i];
	buf[n] = 0;
	return buf;
}

static char *strip(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

/*
 * Kepler records of GPS (G), QZSS (J), Galileo (E), BeiDou (C) into out:
 * {(sys, prn): [eph, ...]}; times in GPS seconds (BeiDou converted from BDT:
 * weeks + 1356, + 14 s). 0, or -1 (the file could not be read)
 */
int rinex_nav(const char *path, struct eph_map *out)
{
	FILE *f = fopen(path, "re");
	char *text, **lines = NULL, *p;
	size_t nlines = 0, cap = 0, end, i, size;
	long sz;

	memset(out, 0, sizeof(*out));
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	text = malloc(sz + 1);
	size = fread(text, 1, sz, f);
	fclose(f);
	text[size] = 0;
	/* splitlines() */
	for (p = text; *p;) {
		char *e = p + strcspn(p, "\r\n");

		if (nlines == cap) {
			cap = cap ? 2 * cap : 4096;
			lines = realloc(lines, cap * sizeof(*lines));
		}
		lines[nlines++] = p;
		if (*e == '\r' && e[1] == '\n') {
			*e = 0;
			p = e + 2;
		} else if (*e) {
			*e = 0;
			p = e + 1;
		} else {
			p = e;
		}
	}
	for (end = 0; end < nlines && !strstr(lines[end], "END OF HEADER"); end++)
		;
	if (end == nlines) {		/* (Python: StopIteration) */
		free(lines);
		free(text);
		return -1;
	}
	for (i = 0; i < end; i++) {
		char b[8];

		if (strstr(lines[i], "LEAP SECONDS") && *strip(slice(lines[i], strlen(lines[i]), 0, 6, b, sizeof(b))))
			gnss_leap = atoi(b);
	}
	i = end + 1;
	while (i < nlines) {
		/*
		 * a record: its satellite line, then continuation lines
		 * (indented; their number depends on the system and the RINEX
		 * version)
		 */
		const char *l = lines[i];
		size_t j = i + 1, k, llen = strlen(l), nrec;
		char sys, b[32], *s;
		double vals[31];
		int nvals = 0, prn, y, mo, d, h, mi, sec;
		struct eph e;
		struct eph_list *lst;

		while (j < nlines && lines[j][0] == ' ')
			j++;
		nrec = j - i;
		sys = l[0];
		s = strip(slice(l, llen, 1, 3, b, sizeof(b)));
		if (!(sys == 0 || strchr("GJEC", sys)) || nrec < 8 || !*s ||
		    strspn(s, "0123456789") != strlen(s)) {
			i = j;
			continue;
		}
		for (k = 0; k < 8; k++) {
			const char *r = lines[i + k];
			size_t rlen = strlen(r), off = k ? 4 : 23, jj;

			for (jj = 0; jj < (k ? 76u : 57u); jj += 19) {
				char fb[24], *fv, *q;

				fv = strip(slice(r, rlen, off + jj, off + jj + 19, fb, sizeof(fb)));
				for (q = fv; *q; q++)
					if (*q == 'D')
						*q = 'E';
				vals[nvals++] = *fv ? strtod(fv, NULL) : 0.0;
			}
		}
		prn = atoi(slice(l, llen, 1, 3, b, sizeof(b)));
		if (sscanf(slice(l, llen, 4, 23, b, sizeof(b)), "%d %d %d %d %d %d",
			   &y, &mo, &d, &h, &mi, &sec) != 6) {
			i = j;
			continue;
		}
		i = j;
		memset(&e, 0, sizeof(e));
		e.confirmed = -1;
		{
			double *fp[] = {
#define X(f) &e.f,
				EPH_FIELDS(X)
#undef X
			};

			for (k = 0; k < 26; k++)
				*fp[k] = vals[k];
		}
		e.toc = gps_seconds(y, mo, d, h, mi, sec);
		if (sys == 'C') {
			if (prn <= 5 || prn >= 59)	/* GEO: other orbit model */
				continue;
			e.toc += 14;
			e.toe_abs = (e.week + 1356) * 604800 + e.toe + 14;
		} else {
			e.toe_abs = e.week * 604800 + e.toe;
		}
		if (sys == 'E' && !((long long)e.codes & 1))	/* Galileo: I/NAV (E1) records only */
			continue;
		lst = eph_map_add(out, sys, prn);
		if (!lst->n || lst->e[lst->n - 1].toe_abs != e.toe_abs)	/* one record per toe */
			eph_list_append(lst, &e);
	}
	free(lines);
	free(text);
	return 0;
}

const struct eph *eph_best(const struct eph_list *l, double t)
{
	const struct eph *b = NULL;
	size_t i;

	for (i = 0; i < l->n; i++)
		if (!b || fabs(t - l->e[i].toe_abs) < fabs(t - b->toe_abs))
			b = &l->e[i];
	return b;
}

/* healthy record within 4 h of t, or NULL */
const struct eph *eph_usable(struct eph_map *m, char sys, int prn, double t)
{
	struct eph_list *l = eph_map_get(m, sys, prn);
	const struct eph *e;

	if (!l || !l->n)
		return NULL;
	e = eph_best(l, t);
	return fabs(t - e->toe_abs) < 4 * 3600 && !(long long)e->svh ? e : NULL;
}

/* ECEF position and clock offset (s) at GPS time t (s since epoch) */
void sat_pos(const struct eph *e, double t, double pos[3], double *clk_out)
{
	double a = e->sqa * e->sqa, tk = t - e->toe_abs;
	double n = sqrt(MU / pow(a, 3)) + e->dn, m = e->m0 + n * tk, ea = m, nu, phi;
	double du, dr, di, u, r, i, x, y, om, dt, clk;
	int k;

	for (k = 0; k < 10; k++)
		ea = m + e->e * sin(ea);
	nu = atan2(sqrt(1 - e->e * e->e) * sin(ea), cos(ea) - e->e);
	phi = nu + e->w;
	du = e->cus * sin(2 * phi) + e->cuc * cos(2 * phi);
	dr = e->crs * sin(2 * phi) + e->crc * cos(2 * phi);
	di = e->cis * sin(2 * phi) + e->cic * cos(2 * phi);
	u = phi + du;
	r = a * (1 - e->e * cos(ea)) + dr;
	i = e->i0 + di + e->idot * tk;
	x = r * cos(u);
	y = r * sin(u);
	om = e->omg0 + (e->omgd - OMEGA_E) * tk - OMEGA_E * e->toe;	/* toe in the system's own time */
	pos[0] = x * cos(om) - y * cos(i) * sin(om);
	pos[1] = x * sin(om) + y * cos(i) * cos(om);
	pos[2] = y * sin(i);
	dt = t - e->toc;
	clk = e->af0 + e->af1 * dt + e->af2 * dt * dt - 4.442807633e-10 * e->e * e->sqa * sin(ea);
	*clk_out = clk - e->tgd;
}

double norm3(const double v[3])
{
	/* numpy's norm: sqrt of the dot product */
	return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static double dist3(const double a[3], const double b[3])
{
	double d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };

	return norm3(d);
}

/*
 * geometric range (with Earth rotation), range rate, satellite clock and
 * position for a signal received at x at GPS time t
 */
void predicted(const struct eph *e, const double x[3], double t, double *rho_out,
	       double *rate_out, double *clk_out, double s_out[3])
{
	double tau = 0.075, s[3], clk = 0, rho = 0, s2[3], s0[3], c2, c0;
	int k;

	for (k = 0; k < 3; k++) {
		double th, r0;

		sat_pos(e, t - tau, s, &clk);
		th = OMEGA_E * tau;
		r0 = s[0];
		s[0] = r0 * cos(th) + s[1] * sin(th);
		s[1] = -r0 * sin(th) + s[1] * cos(th);
		rho = dist3(s, x);
		tau = rho / C_LIGHT;
	}
	sat_pos(e, t - tau + 1, s2, &c2);
	sat_pos(e, t - tau, s0, &c0);
	*rate_out = dist3(s2, x) - dist3(s0, x);
	*rho_out = rho;
	*clk_out = clk;
	memcpy(s_out, s, sizeof(s));
}

void geo2ecef(double lat, double lon, double h, double out[3])
{
	double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f), n;

	lat = lat * M_PI / 180;
	lon = lon * M_PI / 180;
	n = a / sqrt(1 - e2 * sin(lat) * sin(lat));
	out[0] = (n + h) * cos(lat) * cos(lon);
	out[1] = (n + h) * cos(lat) * sin(lon);
	out[2] = (n * (1 - e2) + h) * sin(lat);
}

void ecef2geo(const double p[3], double *lat_out, double *lon_out, double *h_out)
{
	double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f);
	double lon = atan2(p[1], p[0]), r = hypot(p[0], p[1]), lat = atan2(p[2], r * (1 - e2)), h = 0.0;
	int k;

	for (k = 0; k < 6; k++) {
		double n = a / sqrt(1 - e2 * sin(lat) * sin(lat));

		h = r / cos(lat) - n;
		lat = atan2(p[2], r * (1 - e2 * n / (n + h)));
	}
	*lat_out = lat * 180 / M_PI;
	*lon_out = lon * 180 / M_PI;
	*h_out = h;
}

/* rows: east, north, up unit vectors at x */
void enu(const double x[3], double r[3][3])
{
	double lat, lon, h, la, lo;

	ecef2geo(x, &lat, &lon, &h);
	la = lat * M_PI / 180;
	lo = lon * M_PI / 180;
	r[0][0] = -sin(lo);
	r[0][1] = cos(lo);
	r[0][2] = 0;
	r[1][0] = -sin(la) * cos(lo);
	r[1][1] = -sin(la) * sin(lo);
	r[1][2] = cos(la);
	r[2][0] = cos(la) * cos(lo);
	r[2][1] = cos(la) * sin(lo);
	r[2][2] = sin(la);
}

/* ---- linear algebra ---- */

/*
 * least squares h (rows x cols, row major) x = y by Householder QR (numpy's
 * lstsq uses an SVD: the same solution for the full-rank systems here, to
 * rounding); 0, or -1 for a rank-deficient h
 */
int lstsq(const double *h, const double *y, int rows, int cols, double *x)
{
	double *a = malloc(sizeof(double) * rows * cols), *b = malloc(sizeof(double) * rows);
	int i, j, k, ret = 0;

	memcpy(a, h, sizeof(double) * rows * cols);
	memcpy(b, y, sizeof(double) * rows);
	for (k = 0; k < cols && k < rows; k++) {
		double nrm = 0, alpha, vk, s;

		for (i = k; i < rows; i++)
			nrm += a[i * cols + k] * a[i * cols + k];
		nrm = sqrt(nrm);
		if (nrm == 0) {
			ret = -1;
			continue;
		}
		alpha = a[k * cols + k] > 0 ? -nrm : nrm;
		vk = a[k * cols + k] - alpha;
		a[k * cols + k] = vk;
		/* v = (vk, a[k+1..rows-1][k]); H = I - 2 v v' / v'v */
		s = vk * vk;
		for (i = k + 1; i < rows; i++)
			s += a[i * cols + k] * a[i * cols + k];
		for (j = k + 1; j < cols; j++) {
			double dot = 0;

			for (i = k; i < rows; i++)
				dot += a[i * cols + k] * a[i * cols + j];
			dot = 2 * dot / s;
			for (i = k; i < rows; i++)
				a[i * cols + j] -= dot * a[i * cols + k];
		}
		{
			double dot = 0;

			for (i = k; i < rows; i++)
				dot += a[i * cols + k] * b[i];
			dot = 2 * dot / s;
			for (i = k; i < rows; i++)
				b[i] -= dot * a[i * cols + k];
		}
		a[k * cols + k] = alpha;
	}
	for (k = cols - 1; k >= 0; k--) {
		double s = b[k];

		if (k >= rows || a[k * cols + k] == 0) {
			x[k] = 0;
			ret = -1;
			continue;
		}
		for (j = k + 1; j < cols; j++)
			s -= a[k * cols + j] * x[j];
		x[k] = s / a[k * cols + k];
	}
	free(a);
	free(b);
	return ret;
}

/* in place (n x n, row major), Gauss-Jordan with partial pivoting */
int inverse(double *a, int n)
{
	double *inv = calloc(n * n, sizeof(double));
	int i, j, k;

	for (i = 0; i < n; i++)
		inv[i * n + i] = 1;
	for (k = 0; k < n; k++) {
		int p = k;
		double d;

		for (i = k + 1; i < n; i++)
			if (fabs(a[i * n + k]) > fabs(a[p * n + k]))
				p = i;
		if (a[p * n + k] == 0) {
			free(inv);
			return -1;
		}
		if (p != k)
			for (j = 0; j < n; j++) {
				double t = a[k * n + j];

				a[k * n + j] = a[p * n + j];
				a[p * n + j] = t;
				t = inv[k * n + j];
				inv[k * n + j] = inv[p * n + j];
				inv[p * n + j] = t;
			}
		d = a[k * n + k];
		for (j = 0; j < n; j++) {
			a[k * n + j] /= d;
			inv[k * n + j] /= d;
		}
		for (i = 0; i < n; i++) {
			double f = a[i * n + k];

			if (i == k || f == 0)
				continue;
			for (j = 0; j < n; j++) {
				a[i * n + j] -= f * a[k * n + j];
				inv[i * n + j] -= f * inv[k * n + j];
			}
		}
	}
	memcpy(a, inv, sizeof(double) * n * n);
	free(inv);
	return 0;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* numpy's median (sorts v) */
double median(double *v, int n)
{
	if (!n)
		return NAN;
	qsort(v, n, sizeof(*v), cmp_double);
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* ---- the atmosphere ---- */

/* L1 group delay (m); t = GPS seconds, lat / lon / az / el in rad */
static double iono_gps(const double *alpha, const double *beta, double t, double lat, double lon,
		       double az, double el)
{
	double e = el / M_PI, psi = 0.0137 / (e + 0.11) - 0.022, phi, lam, phim, tt, f, amp = 0, per = 0, x, d;
	int i;

	phi = lat / M_PI + psi * cos(az);
	phi = fmax(-0.416, fmin(0.416, phi));
	lam = lon / M_PI + psi * sin(az) / cos(phi * M_PI);
	phim = phi + 0.064 * cos((lam - 1.617) * M_PI);
	tt = py_mod(43200 * lam + t, 86400);
	f = 1 + 16 * pow(0.53 - e, 3);
	for (i = 0; i < 4; i++)
		amp += alpha[i] * pow(phim, i);
	for (i = 0; i < 4; i++)
		per += beta[i] * pow(phim, i);
	amp = fmax(0.0, amp);
	per = fmax(72000.0, per);
	x = 2 * M_PI * (tt - 50400) / per;
	d = 5e-9 + (fabs(x) < 1.57 ? amp * (1 - x * x / 2 + pow(x, 4) / 24) : 0);
	return C_LIGHT * f * d;
}

/*
 * B1I group delay (m), BeiDou's Klobuchar variant (geographic latitude of
 * the pierce point at 375 km, BDT seconds t)
 */
static double iono_bds(const double *alpha, const double *beta, double t, double lat, double lon,
		       double az, double el)
{
	double r = 6378.0 / (6378.0 + 375.0), psi, phim, lamm, tt, p, a2 = 0, a4 = 0, iz;
	int i;

	psi = M_PI / 2 - el - asin(r * cos(el));
	phim = asin(sin(lat) * cos(psi) + cos(lat) * sin(psi) * cos(az));
	lamm = lon + asin(sin(psi) * sin(az) / cos(phim));
	tt = py_mod(t + lamm * 43200 / M_PI, 86400);
	p = fabs(phim / M_PI);
	for (i = 0; i < 4; i++)
		a2 += alpha[i] * pow(p, i);
	for (i = 0; i < 4; i++)
		a4 += beta[i] * pow(p, i);
	a2 = fmax(0.0, a2);
	a4 = fmin(172800.0, fmax(72000.0, a4));
	iz = 5e-9 + (fabs(tt - 50400) < a4 / 4 ? a2 * cos(2 * M_PI * (tt - 50400) / a4) : 0);
	return C_LIGHT * iz / sqrt(1 - pow(r * cos(el), 2));
}

/* delay (m) at carrier freq */
double atmo_iono(const struct iono *p, double t, double lat, double lon, double az, double el,
		 double freq)
{
	if (!p || !p->kind || el <= 0)
		return 0.0;
	if (p->kind == 'G')
		return iono_gps(p->alpha, p->beta, t, lat, lon, az, el) * pow(1575.42e6 / freq, 2);
	return iono_bds(p->alpha, p->beta, t - 14, lat, lon, az, el) * pow(1561.098e6 / freq, 2);
}

/* slant delay (m), Saastamoinen with a standard atmosphere */
double atmo_tropo(double lat, double h, double el)
{
	double pres, temp, e, z, trph, trpw;

	if (el <= 0 || !(-100 < h && h < 1e4))
		return 0.0;
	h = fmax(h, 0.0);
	pres = 1013.25 * pow(1 - 2.2557e-5 * h, 5.2568);
	temp = 15.0 - 6.5e-3 * h + 273.16;
	e = 6.108 * 0.7 * exp((17.15 * temp - 4684.0) / (temp - 38.45));
	z = M_PI / 2 - el;
	trph = 0.0022768 * pres / (1 - 0.00266 * cos(2 * lat) - 0.00028 * h / 1e3) / cos(z);
	trpw = 0.002277 * (1255.0 / temp + 0.05) * e / cos(z);
	return trph + trpw;
}

/* ---- FE02 aiding ---- */

/*
 * constellation: FE02 / FE03 / FE29 code, carrier Hz, chip rate, FE02 time
 * unit s, offset of its time scale to GPS time s
 */
static const struct sysinfo systems[] = {
	{ 'G', 0x01, 1575.42e6, 1.023e6, 1.5, 0 },
	{ 'E', 0x04, 1575.42e6, 1.023e6, 1.0, 0 },
	{ 'C', 0x20, 1561.098e6, 2.046e6, 1.0, -14 },
};

const struct sysinfo *sysinfo(char sys)
{
	size_t i;

	for (i = 0; i < sizeof(systems) / sizeof(*systems); i++)
		if (systems[i].sys == sys)
			return &systems[i];
	return NULL;
}

/*
 * per constellation: u16 at 2, Doppler window (Hz), code window (chips),
 * flags, flags; before the epoch time is known (whole code searched) and
 * once it is (as the stock software does from its first fix)
 */
struct window {
	int a, dunc, cunc, f1, f2;
};

static struct window window(char sys, int narrow)
{
	static const struct window wide[] = { { 4, 260, 512, 1, 0x41 }, { 6, 260, 1023, 1, 0x41 },
					      { 6, 260, 2046, 1, 0x01 } };
	static const struct window nar[] = { { 4, 68, 4, 0x0301, 0x41 }, { 6, 68, 5, 0x0302, 0x41 },
					     { 6, 68, 5, 0x0301, 0x41 } };
	int i = sys == 'G' ? 0 : sys == 'C' ? 1 : 2;

	return narrow ? nar[i] : wide[i];
}

static void put16(unsigned char *p, int v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static int cmp_key(const void *a, const void *b)
{
	const struct eph_list *x = *(const struct eph_list *const *)a, *y = *(const struct eph_list *const *)b;

	if (x->sys != y->sys)
		return x->sys < y->sys ? -1 : 1;
	return x->prn - y->prn;
}

/*
 * FE02 payloads for the satellites above ~3 deg; t_rx: GPS time of the DSP
 * epoch the prediction is for, clock_hz: receiver clock offset at L1,
 * known: t_rx is known (narrow code / Doppler windows); pos_unc: the
 * uncertainty of pos (m), widens the code window once t_rx is known and the
 * Doppler window (~1 Hz / km) before. Returns the count (plan in *out).
 */
int fe02(struct eph_map *m, const double pos[3], double t_rx, double clock_hz, int known,
	 double pos_unc, struct plan **out)
{
	const struct eph_list **keys = malloc(m->n * sizeof(*keys));
	double up[3], np = norm3(pos);
	size_t i, nk = 0;
	int n = 0;

	*out = NULL;
	for (i = 0; i < 3; i++)
		up[i] = pos[i] / np;
	for (i = 0; i < m->n; i++)
		keys[nk++] = &m->l[i];
	qsort(keys, nk, sizeof(*keys), cmp_key);
	for (i = 0; i < nk; i++) {
		const struct eph_list *l = keys[i];
		const struct sysinfo *si = sysinfo(l->sys);
		const struct eph *e;
		double rho, rate, clk, s[3], dot, dopp, tt, chipsf;
		long long unit, chips, period;
		int fresh, near;
		struct window w;
		unsigned char *pl;

		if (!si || !l->n)
			continue;
		e = eph_usable(m, l->sys, l->prn, t_rx);
		fresh = e != NULL;
		if (!fresh) {	/* a stale (decoded) record: Doppler good, code not */
			e = eph_best(l, t_rx);
			if (fabs(t_rx - e->toe_abs) > STALE || (long long)e->svh)
				continue;
		}
		predicted(e, pos, t_rx, &rho, &rate, &clk, s);
		dot = ((s[0] - pos[0]) / rho) * up[0] + ((s[1] - pos[1]) / rho) * up[1] +
		      ((s[2] - pos[2]) / rho) * up[2];
		if (dot < 0.05)
			continue;
		dopp = -rate * si->freq / C_LIGHT + clock_hz * si->freq / 1575.42e6;
		tt = py_mod(t_rx - rho / C_LIGHT + clk + si->offset, 604800);	/* transmit time, satellite clock */
		unit = (long long)py_floordiv(tt, si->unit_s);
		chipsf = (tt - unit * si->unit_s) * si->chip_rate;
		period = (long long)(si->unit_s * si->chip_rate);
		chips = py_round(chipsf);
		chips = ((chips % period) + period) % period;
		near = fresh || fabs(t_rx - e->toe_abs) < STALE_CODE;
		w = window(l->sys, known && near);
		if (known && near) {	/* days-old orbits: range within ~1.5 km (measured), add 2 km */
			long long add = (long long)ceil((pos_unc + (fresh ? 0 : 2000)) * si->chip_rate / C_LIGHT);
			long long c = w.cunc + add, wide = window(l->sys, 0).cunc;

			w.cunc = c < wide ? c : wide;
		} else {
			long long d = w.dunc + (long long)(pos_unc / 1000);

			w.dunc = d < 1000 ? d : 1000;
		}
		*out = realloc(*out, (n + 1) * sizeof(**out));
		(*out)[n].sys = l->sys;
		(*out)[n].prn = l->prn;
		pl = (*out)[n].payload;
		pl[0] = l->prn;
		pl[1] = si->code;
		put16(pl + 2, w.a);
		put16(pl + 4, unit & 0xffff);
		put16(pl + 6, chips >> 16);
		put16(pl + 8, chips & 0xffff);
		put16(pl + 10, (int)py_round(dopp));
		put16(pl + 12, w.dunc);
		put16(pl + 14, w.cunc);
		put16(pl + 16, w.f1);
		put16(pl + 18, w.f2);
		n++;
	}
	free(keys);
	return n;
}

/* ---- NMEA 0183 sentences from a fix (GeoClue reads GGA and RMC) ---- */

static void sentence(const char *body, char *out, size_t size)
{
	unsigned cs = 0;
	const char *p;

	for (p = body; *p; p++)
		cs ^= (unsigned char)*p;
	snprintf(out, size, "$%s*%02X\r\n", body, cs);
}

static void nmea_utc(double t_gps, struct tm *tm, double *frac)
{
	char r[64];
	double t;
	time_t s;

	/* round(x, 2): correctly rounded, as printf rounds */
	snprintf(r, sizeof(r), "%.2f", t_gps + 315964800 - gnss_leap);
	t = strtod(r, NULL);
	s = (time_t)t;

	gmtime_r(&s, tm);
	*frac = t - (double)(long long)t;
}

static void latlon(double lat, double lon, char *out, size_t size)
{
	double la = fabs(lat), lo = fabs(lon);

	snprintf(out, size, "%02d%09.6f,%c,%03d%09.6f,%c", (int)la, (la - (int)la) * 60,
		 lat >= 0 ? 'N' : 'S', (int)lo, (lo - (int)lo) * 60, lon >= 0 ? 'E' : 'W');
}

/*
 * the HDOP field carries our accuracy estimate (m): GeoClue turns HDOP into
 * an accuracy by buckets (<= 1: 0 m, <= 2: 1 m, <= 5: 3 m, <= 10: 50 m, <=
 * 20: 100 m, else 300 m), so pick the bucket that fits
 */
static double geoclue_hdop(double acc)
{
	static const double lim[][2] = { { 2, 1.5 }, { 6, 4.0 }, { 75, 8.0 }, { 200, 15.0 } };
	int i;

	for (i = 0; i < 4; i++)
		if (acc < lim[i][0])
			return lim[i][1];
	return 25.0;
}

static void hms(const struct fix *f, char *out, size_t size, struct tm *tm)
{
	double frac;
	char fr[16];

	nmea_utc(f->t, tm, &frac);
	snprintf(fr, sizeof(fr), "%.2f", frac);
	snprintf(out, size, "%02d%02d%02d%s", tm->tm_hour, tm->tm_min, tm->tm_sec, fr + 1);
}

/*
 * position, satellites used, accuracy as HDOP (see geoclue_hdop), height
 * (ellipsoidal: no geoid model yet, so the separation field is 0)
 */
void nmea_gga(const struct fix *f, char *out, size_t size)
{
	char t[32], ll[64], body[200];
	struct tm tm;

	hms(f, t, sizeof(t), &tm);
	latlon(f->lat, f->lon, ll, sizeof(ll));
	snprintf(body, sizeof(body), "GPGGA,%s,%s,1,%02d,%.1f,%.1f,M,0.0,M,,", t, ll, f->nused,
		 geoclue_hdop(f->acc), f->alt);
	sentence(body, out, size);
}

/*
 * time, date, position, speed (knots) and course (deg) from the Doppler
 * velocity; empty fields make GeoClue derive them from position changes,
 * which indoors jitter by tens of m, so the course is always given (the
 * last one moving faster than 1 m/s, else 0)
 */
void nmea_rmc(const struct fix *f, char *out, size_t size)
{
	char t[32], ll[64], body[200], speed[32] = "", course[32] = "", date[16];
	struct tm tm;

	hms(f, t, sizeof(t), &tm);
	latlon(f->lat, f->lon, ll, sizeof(ll));
	if (f->has_speed) {
		snprintf(speed, sizeof(speed), "%.2f", f->speed * 1.943844);
		snprintf(course, sizeof(course), "%.1f", f->has_course && f->course ? f->course : 0.0);
	}
	strftime(date, sizeof(date), "%d%m%y", &tm);
	snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%s,%s,%s,,,A", t, ll, speed, course, date);
	sentence(body, out, size);
}
