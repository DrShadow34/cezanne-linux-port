/* SPDX-License-Identifier: MIT */
/*
 * cezanne-gnssd (C version of the Python receiver of the same name): shared
 * types. Files: gnss-orbits.c (broadcast orbits, coordinates, atmosphere,
 * FE02 aiding, NMEA, small linear algebra), gnss-nav.c (navigation messages),
 * gnss-dsp.c (the DSP links), gnss-receiver.c (the receiver),
 * cezanne-gnssd.c (the service).
 */
#ifndef GNSS_H
#define GNSS_H

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define C_LIGHT		299792458.0
#define MU		3.986005e14
#define OMEGA_E		7.2921151467e-5

extern int gnss_leap;		/* GPS - UTC, s (updated from the RINEX header) */

/* ---- a broadcast ephemeris (orbits.py record) ---- */

/* the RINEX record's fields in order (orbits.FIELDS) */
#define EPH_FIELDS(X) X(af0) X(af1) X(af2) X(iode) X(crs) X(dn) X(m0) X(cuc) X(e) \
	X(cus) X(sqa) X(toe) X(cic) X(omg0) X(cis) X(i0) X(crc) X(w) X(omgd) X(idot) \
	X(codes) X(week) X(l2p) X(sva) X(svh) X(tgd)

struct eph {
#define X(f) double f;
	EPH_FIELDS(X)
#undef X
	double toc, toe_abs;
	double iodc;		/* decoded records only */
	int has_iodc;
	int confirmed;		/* decoded: 1 / 0; -1: no such key (downloaded) */
};

/* per satellite: its records; key: system letter (G, J, E, C) and PRN */
struct eph_list {
	char sys;
	int prn;
	struct eph *e;
	size_t n, cap;
};

/* {(sys, prn): [eph, ...]} in insertion order */
struct eph_map {
	struct eph_list *l;
	size_t n, cap;
};

struct eph_list *eph_map_get(struct eph_map *m, char sys, int prn);
struct eph_list *eph_map_add(struct eph_map *m, char sys, int prn);
void eph_list_append(struct eph_list *l, const struct eph *e);
void eph_list_remove(struct eph_list *l, size_t i);
void eph_map_free(struct eph_map *m);
void eph_map_copy(struct eph_map *dst, const struct eph_map *src);

double gps_now(void);
double gps_seconds(int y, int mo, int d, int h, int mi, int s);
int rinex_nav(const char *path, struct eph_map *out);
const struct eph *eph_best(const struct eph_list *l, double t);
const struct eph *eph_usable(struct eph_map *m, char sys, int prn, double t);
void sat_pos(const struct eph *e, double t, double pos[3], double *clk);
void predicted(const struct eph *e, const double x[3], double t, double *rho, double *rate,
	       double *clk, double s[3]);
void geo2ecef(double lat, double lon, double h, double out[3]);
void ecef2geo(const double p[3], double *lat, double *lon, double *h);
void enu(const double x[3], double r[3][3]);

/* ---- vectors and least squares ---- */
double norm3(const double v[3]);
int lstsq(const double *h, const double *y, int rows, int cols, double *x);
int inverse(double *a, int n);
double median(double *v, int n);
double py_floordiv(double a, double b);
double py_mod(double a, double b);
long long py_round(double x);

/* ---- atmosphere ---- */
struct iono {
	char kind;		/* 0: none, 'G' or 'C' */
	double alpha[4], beta[4];
};
double atmo_iono(const struct iono *p, double t, double lat, double lon, double az, double el,
		 double freq);
double atmo_tropo(double lat, double h, double el);

/* ---- FE02 aiding ---- */
#define STALE		(7 * 86400)
#define STALE_CODE	(3 * 86400)
struct sysinfo {
	char sys;
	int code;
	double freq, chip_rate, unit_s, offset;
};
const struct sysinfo *sysinfo(char sys);
struct plan {
	char sys;
	int prn;
	unsigned char payload[20];
};
int fe02(struct eph_map *m, const double pos[3], double t_rx, double clock_hz, int known,
	 double pos_unc, struct plan **out);

/* ---- NMEA ---- */
struct sat_cn {
	char sys;
	int prn, cn;
};
struct fix {
	double t, x[3], xs[3], lat, lon, alt;
	struct sat_cn *used;	/* sorted */
	int nused, spare;
	double rms, hdop, vdop, acc, clock_hz;
	int has_speed, has_course;
	double speed, course;
};
void nmea_gga(const struct fix *f, char *out, size_t size);
void nmea_rmc(const struct fix *f, char *out, size_t size);

/* ---- navigation messages ---- */
struct gps_word {
	uint32_t w;
	int set, confirmed;
};
struct gps_sat {
	int prn;
	struct gps_word sub[3][10];	/* subframes 1..3 */
	int have[3];
};
struct gps_nav {
	struct gps_sat *s;
	size_t n;
	struct { int prn, sid, v; } *pending;
	size_t npending;
	int has_iono;
	double iono_alpha[4], iono_beta[4];
	int has_utc;
	double utc_a0, utc_a1, utc_tot, utc_wnt, utc_dtls;
};
int gps_nav_add(struct gps_nav *g, const unsigned char *p, size_t len);
int gps_nav_ephemeris(struct gps_nav *g, int prn, int week_now, int confirmed, struct eph *e);

struct bds_vote {
	uint32_t v;
	int score;
};
struct bds_word {
	uint32_t v;
	int set, confirmed;
};
struct bds_votes {
	int set;		/* the dict exists */
	struct bds_vote *v;
	size_t n;
	uint32_t latest;
	int has_latest;
};
struct bds_sat {
	int prn;
	struct bds_word sub[3][10];
	int have[3];
	struct bds_votes votes[3][10];
	int votes_set[3];
};
struct bds_nav {
	struct bds_sat *s;
	size_t n;
	struct { int prn, fra; uint32_t v; } *pending;
	size_t npending;
	int has_iono;
	double iono_alpha[4], iono_beta[4];
};
int bds_nav_add(struct bds_nav *b, const unsigned char *p, size_t len);
int bds_nav_ephemeris(struct bds_nav *b, int prn, int week_now, int confirmed, int latest,
		      struct eph *e);

/* ---- the DSP links ---- */
struct frame {
	double t;		/* s since open */
	int mid, seq;
	unsigned char *p;
	size_t len;
};
struct link {
	const char *name;
	int fd;
	unsigned char *buf;
	size_t blen, bcap;
	struct frame *frames;
	size_t nframes, cap;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	double t0;
	volatile int alive, reader_done;
	pthread_t reader;
};
extern FILE *dsp_log;
int link_open(struct link *l, const char *name);
void link_close(struct link *l);
int link_send(struct link *l, int mid, const unsigned char *p, size_t len);
size_t link_take(struct link *l, struct frame **out);
int link_wait(struct link *l, int mid, double timeout, const unsigned char *match, size_t mlen,
	      struct frame *out);
void frame_free(struct frame *f);
int dsp_boot(struct link *l, const unsigned char *image, size_t len);
double mono_s(void);
double real_s(void);
void sleep_s(double s);

/* ---- the receiver ---- */
struct receiver_cb {
	void (*on_fix)(struct fix *f, void *data);
	void (*on_state)(const char *text, void *data);
	void *data;
};
struct receiver;
struct receiver *receiver_new(const char *nav_file, const double pos[3], double pos_unc,
			      double clock_hz, const char *firmware, const char *cache,
			      const char *mode, struct receiver_cb cb);
int receiver_run(struct receiver *r, volatile int *stop, double duration);
void receiver_result(struct receiver *r, double pos[3], double *pos_unc, double *clock_hz);
void receiver_free(struct receiver *r);

#endif
