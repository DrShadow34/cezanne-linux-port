// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Direct (charge pump) charging from a USB PD PPS adapter.
 *
 * The BQ25970 / BQ25968 halves VBUS into the battery and does not regulate, so
 * the battery current is set by the adapter voltage: request ~2 x VBAT, then
 * step the PPS voltage by 20 mV on the battery voltage / battery current / bus
 * current loops. Rules from the vendor pd_policy_manager (cezanne DT values):
 *
 * - start: PPS adapter, battery 10..48 C, VBAT < 4.30 V (non-FFC 4.45 V
 *   limit - 150 mV), MT6360 input cut to 100 mA while measuring
 * - battery current limit: 6 A, 4.8 A for a non-Xiaomi ("unverified") PPS
 *   adapter, 5.4 A below 4.25 V / 3.52 A above (vendor step charging), 3.52 A
 *   at 10..15 C; bus current = half + 50 mA, at most 3 A
 * - VBAT within 10 mV of 4.45 V: lower the current limit 100 mA per second
 * - done: VBAT > 4.37 V and IBAT < 1.5 A for 10 s -> back to the MT6360,
 *   which finishes the CV phase (no retry until the adapter is replugged)
 * - stop on any pump fault, temperature leaving the range, adapter removal
 * - charge limit / temperature guard (the gauge, mt6359-gauge): while the
 *   MT6360's charge_behaviour is inhibit-charge the pump neither starts nor
 *   runs (the battery's charge_control_end_threshold stops charging there,
 *   charging resumes at the start threshold)
 *
 * QC chargers (no PD; type from the xmusb350 driver): 9 V through QC 2.0
 * mode select for the MT6360 (mt6360-chg-setup then allows 2 A input, 3 A
 * charge), once the charger has been attached QC_SETTLE s without a PD
 * contract (a USB-C charger can do both; TCPM gives up PD within ~3 s); back
 * to 5 V while the battery is >= 48 C.
 *
 * QC 3 / 3+ direct charging (vendor cp_qc30, limits from the cezanne DT):
 * same start window as PPS; QC 5 V, continuous mode, D+ steps up to
 * 2 x VBAT + 300 mV (QC 3+: 10 steps at a time below 2 x VBAT), pump on, then
 * one step per period on the vendor's thresholds (QC 3.0 steps are 200 mV,
 * ~1 A of battery current: +-500..600 mA hysteresis). Off while
 * /run/cezanne-dcharge-no-qc-pump exists.
 *
 * While running, /run/cezanne-dcharge holds our pid and mt6360-chg-setup
 * leaves the MT6360 alone. The pump stops by itself 5 s after the last
 * keepalive (bq25970 driver) if this program dies.
 *
 * File errors inside a session end it ("error: ..."), like the Python
 * version's OSError handling (try_call below).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glob.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* the tests' fake sysfs (empty: the real one) */
#ifndef SYSROOT
#define SYSROOT		""
#endif

#define PS		SYSROOT "/sys/class/power_supply"
#define CP		PS "/bq25970"
#define BAT		PS "/battery"
#define FLAG		SYSROOT "/run/cezanne-dcharge"

#define VBAT_MAX	4450	/* mV, non-FFC */
#define VBAT_START_MAX	(VBAT_MAX - 150)
#define VBAT_MIN	3500
#define FCC_MAX		6000	/* mA */
#define FCC_UNVERIFIED	4800
#define STEP_VBAT	4250
#define STEP_FCC_HI	5400
#define STEP_FCC_LO	3520
#define IBUS_MAX	3000
#define TAPER_HYS	10
#define TAPER_DONE_HYS	80
#define TAPER_DONE_IBAT	1500
#define T_MIN		100	/* 0.1 C */
#define T_MAX		480
#define T_HYS		20
#define T_COOL		150
#define FCC_COOL	3520
#define PPS_STEP	20	/* mV */
#define PPS_VMAX	10000	/* vendor: APDO 11 V max - 1 V */
#define PERIOD		0.5
#define PPS_REFRESH	5	/* s; sources hard reset without a request in 15 s */

#define XM		PS "/xmusb350"
#define QC_SETTLE	5	/* s */
#define QC_VBUS_LOST	7000	/* mV: 9 V requested, VBUS below this = out of QC */
#define QC_TRIES	3	/* 9 V requests per attach */
#define QC_T_HOT	480	/* 0.1 C */
#define QC_T_OK		450
#define QC_VBUS_ALARM	9500	/* mV, vendor bus OVP alarm threshold (QC 3 / 3+) */
#define QC_INIT_UP	300	/* mV over 2 x VBAT before the pump starts */
#define QC_PUMP_OFF	SYSROOT "/run/cezanne-dcharge-no-qc-pump"
#define INPUT_CAP	SYSROOT "/run/cezanne-aicr-max"	/* mt6360-chg-setup's input cap: no pump either */
#define CHG_SETUP	SYSROOT "/usr/local/bin/mt6360-chg-setup"

/* bq25970 state register bits */
#define CONV_VBUS_LO	0x20
#define CONV_VBUS_HI	0x10
#define CONV_SWITCHING	0x04
#define INT_BUS_OVP_ALM	0x20
#define INT_BUS_OCP_ALM	0x10
#define REG_VBAT_ACTIVE	0x08

static const char *const qc_types[] = {
	"qc2", "qc3", "qc3-18", "qc3-27", "qc3p-18", "qc3p-27", NULL
};

/*
 * QC direct charging: (IBAT, IBUS) mA per type; plain "qc3" (class not
 * measured) as class A 18 W
 */
static const struct {
	const char *type;
	int fcc, ibus;
} qc_pump_limits[] = {
	{ "qc3", 3200, 1700 }, { "qc3-18", 3200, 1700 }, { "qc3-27", 5400, 2700 },
	{ "qc3p-18", 4500, 2300 }, { "qc3p-27", 5600, 2700 }, { NULL, 0, 0 },
};

static volatile sig_atomic_t running = 1;

/* ---- errors: OSError (and bad numbers) as a jump to the innermost try_call */

static jmp_buf *err_jmp;
static char err_msg[384];

static void __attribute__((noreturn)) raise_err(int err, const char *path)
{
	snprintf(err_msg, sizeof(err_msg), "[Errno %d] %s: '%s'", err, strerror(err), path);
	longjmp(*err_jmp, 1);
}

/* fn(arg) with file errors caught: 0, or -1 with err_msg set */
static int try_call(void (*fn)(void *), void *arg)
{
	jmp_buf buf, *prev = err_jmp;

	if (setjmp(buf)) {
		err_jmp = prev;
		return -1;
	}
	err_jmp = &buf;
	fn(arg);
	err_jmp = prev;
	return 0;
}

struct rd_ctx {
	const char *path;
	char *buf;
	size_t size;
};

static char *rd(const char *path, char *buf, size_t size);
static void wr(const char *path, const char *val);

static void rd_fn(void *arg)
{
	struct rd_ctx *c = arg;

	rd(c->path, c->buf, c->size);
}

/* rd() without raising: 0 or -1 */
static int try_rd(const char *path, char *buf, size_t size)
{
	struct rd_ctx c = { path, buf, size };

	return try_call(rd_fn, &c);
}

struct wr_ctx {
	const char *path, *val;
};

static void wr_fn(void *arg)
{
	struct wr_ctx *c = arg;

	wr(c->path, c->val);
}

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static double mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_s(double s)
{
	struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* Python's // (floor division) */
static long fdiv(long a, long b)
{
	long q = a / b;

	return (a % b && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int exists(const char *path)
{
	return !access(path, F_OK);
}

static int is_dir(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

/* the stripped file contents into buf; raises on errors */
static char *rd(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "re");
	size_t n;
	char *s;

	if (!f)
		raise_err(errno, path);
	n = fread(buf, 1, size - 1, f);
	if (ferror(f)) {
		int e = errno;

		fclose(f);
		raise_err(e, path);
	}
	fclose(f);
	buf[n] = 0;
	while (n && strchr(" \t\r\n", buf[n - 1]))
		buf[--n] = 0;
	for (s = buf; *s && strchr(" \t\r\n", *s); s++)
		;
	memmove(buf, s, strlen(s) + 1);
	return buf;
}

static long rdi(const char *path)
{
	char buf[64], *end;
	long v;

	rd(path, buf, sizeof(buf));
	errno = 0;
	v = strtol(buf, &end, 10);
	if (end == buf || *end || errno) {
		snprintf(err_msg, sizeof(err_msg), "invalid literal for int(): '%s' (%s)", buf, path);
		longjmp(*err_jmp, 1);
	}
	return v;
}

static void wr(const char *path, const char *val)
{
	FILE *f = fopen(path, "we");

	if (!f)
		raise_err(errno, path);
	if (fputs(val, f) < 0) {
		int e = errno;

		fclose(f);
		raise_err(e, path);
	}
	if (fclose(f))
		raise_err(errno, path);
}

static void wri(const char *path, long val)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld", val);
	wr(path, buf);
}

static void pathf(char *buf, size_t size, const char *dir, const char *attr)
{
	snprintf(buf, size, "%s/%s", dir, attr);
}

/* the first online tcpm source, or 0 (path in out) */
static int tcpm(char *out, size_t size)
{
	glob_t g;
	char p[300], v[16];
	size_t i;
	int found = 0;

	if (glob(PS "/tcpm-source-psy-*", GLOB_NOSORT, NULL, &g))
		return 0;
	for (i = 0; i < g.gl_pathc && !found; i++) {
		pathf(p, sizeof(p), g.gl_pathv[i], "online");
		if (try_rd(p, v, sizeof(v))) {
			globfree(&g);
			longjmp(*err_jmp, 1);	/* err_msg is set: the caller's error */
		}
		if (strcmp(v, "0")) {
			snprintf(out, size, "%s", g.gl_pathv[i]);
			found = 1;
		}
	}
	globfree(&g);
	return found;
}

static int mt6360(char *out, size_t size)
{
	glob_t g;
	int found = 0;

	if (!glob(PS "/mt6360-chg*", 0, NULL, &g)) {
		snprintf(out, size, "%s", g.gl_pathv[0]);
		found = 1;
		globfree(&g);
	}
	return found;
}

/* the gauge holds the MT6360's charging off (charge limit or temperature):
 * the pump must not charge either */
static int charge_inhibited(void)
{
	char chg[256], p[300], buf[256];

	if (!mt6360(chg, sizeof(chg)))
		return 0;
	pathf(p, sizeof(p), chg, "charge_behaviour");
	return !try_rd(p, buf, sizeof(buf)) && strstr(buf, "[inhibit-charge]");
}

struct sample {
	long vbat, ibat, vbus, ibus, vac, temp, cap;
	int conv, intr, fault, reg;
};

/* "conv %02x int %02x fault %02x reg %02x" */
static void cp_state(struct sample *s)
{
	char buf[128], key[16];
	char *p = rd(CP "/state", buf, sizeof(buf));
	unsigned int v;
	int n;

	while (sscanf(p, "%15s %x%n", key, &v, &n) == 2) {
		if (!strcmp(key, "conv"))
			s->conv = v;
		else if (!strcmp(key, "int"))
			s->intr = v;
		else if (!strcmp(key, "fault"))
			s->fault = v;
		else if (!strcmp(key, "reg"))
			s->reg = v;
		p += n;
	}
}

static void sample(struct sample *s)
{
	memset(s, 0, sizeof(*s));
	s->vbat = fdiv(rdi(CP "/battery_voltage"), 1000);
	s->ibat = fdiv(rdi(CP "/battery_current"), 1000);
	s->vbus = fdiv(rdi(CP "/voltage_now"), 1000);
	s->ibus = fdiv(rdi(CP "/current_now"), 1000);
	s->vac = fdiv(rdi(CP "/ac_voltage"), 1000);
	s->temp = rdi(BAT "/temp");
	s->cap = rdi(BAT "/capacity");
	cp_state(s);
}

/* Python's repr of the sample dict */
static const char *sample_str(const struct sample *s, char *buf, size_t size)
{
	snprintf(buf, size, "{'vbat': %ld, 'ibat': %ld, 'vbus': %ld, 'ibus': %ld, "
		 "'vac': %ld, 'temp': %ld, 'cap': %ld, 'conv': %d, 'int': %d, "
		 "'fault': %d, 'reg': %d}", s->vbat, s->ibat, s->vbus, s->ibus,
		 s->vac, s->temp, s->cap, s->conv, s->intr, s->fault, s->reg);
	return buf;
}

/* ---- sessions ---------------------------------------------------------- */

struct session {
	int qc;			/* 0: PPS (Session), 1: QC 3 / 3+ (QcSession) */
	char src[256], chg[256];
	long req_mv;
	double req_t;
	long fcc_taper;
	double taper_t, done_t;
	/* QC */
	char qc_type[64];
	int plus;
	long fcc_max, ibus_max;
	long backoff;		/* vendor "value": IBUS margin while VBAT regulates */
};

static void session_init(struct session *ses, const char *src, const char *chg)
{
	memset(ses, 0, sizeof(*ses));
	snprintf(ses->src, sizeof(ses->src), "%s", src ? src : "");
	snprintf(ses->chg, sizeof(ses->chg), "%s", chg ? chg : "");
	ses->fcc_taper = FCC_MAX;
}

static int qc_limits(const char *type, long *fcc, long *ibus)
{
	int i;

	for (i = 0; qc_pump_limits[i].type; i++)
		if (!strcmp(qc_pump_limits[i].type, type)) {
			*fcc = qc_pump_limits[i].fcc;
			*ibus = qc_pump_limits[i].ibus;
			return 1;
		}
	return 0;
}

static void qc_session_init(struct session *ses, const char *type, const char *chg)
{
	session_init(ses, NULL, chg);
	ses->qc = 1;
	snprintf(ses->qc_type, sizeof(ses->qc_type), "%s", type);
	ses->plus = !strncmp(type, "qc3p", 4);
	qc_limits(type, &ses->fcc_max, &ses->ibus_max);
}

static int present(struct session *ses)
{
	char buf[64], src[256];

	if (ses->qc)
		return !try_rd(XM "/qc_type", buf, sizeof(buf)) && !strcmp(buf, ses->qc_type);
	return tcpm(src, sizeof(src));	/* errors: the session's */
}

static void request(struct session *ses, long mv, int force)
{
	char p[300];
	double now;

	mv = mv - mv % PPS_STEP;
	mv = mv > PPS_VMAX ? PPS_VMAX : mv;
	mv = mv < 5000 ? 5000 : mv;
	now = mono();
	if (mv == ses->req_mv && !force && now - ses->req_t < PPS_REFRESH)
		return;
	pathf(p, sizeof(p), ses->src, "voltage_now");
	wri(p, mv * 1000);
	ses->req_mv = mv;
	ses->req_t = now;
}

static long fcc(struct session *ses, const struct sample *s)
{
	long f = FCC_MAX;

	f = f < FCC_UNVERIFIED ? f : FCC_UNVERIFIED;
	f = f < ses->fcc_taper ? f : ses->fcc_taper;
	if (s->vbat < STEP_VBAT)
		f = f < STEP_FCC_HI ? f : STEP_FCC_HI;
	else
		f = f < STEP_FCC_LO ? f : STEP_FCC_LO;
	if (s->temp < T_COOL)
		f = f < FCC_COOL ? f : FCC_COOL;
	if (ses->qc)
		f = f < ses->fcc_max ? f : ses->fcc_max;
	return f;
}

static long qc_vbus(void)
{
	return fdiv(rdi(CP "/voltage_now"), 1000);
}

static void pulse(struct session *ses, int n)
{
	wri(XM "/pulses", n);
	sleep_s((ses->plus ? 0.05 : 0.1) * abs(n));
	ses->req_mv = qc_vbus();
}

static void write_flag(void)
{
	wri(FLAG, getpid());
}

/* NULL: the pump runs; else why not (in buf) */
static const char *pps_start(struct session *ses, char *buf, size_t size)
{
	char p[300], st[128], sm[256];
	struct sample s;
	long apdo_ma;
	int i, ok;

	write_flag();
	pathf(p, sizeof(p), ses->chg, "input_current_limit");
	wri(p, 100000);
	sleep_s(1);
	sample(&s);
	if (!(VBAT_MIN <= s.vbat && s.vbat < VBAT_START_MAX)) {
		snprintf(buf, size, "vbat %ld mV out of range", s.vbat);
		return buf;
	}
	/*
	 * PPS at the current fixed contract, then the adapter current limit
	 * (each PPS request must stay >= op-sink-microwatt: current first)
	 */
	pathf(p, sizeof(p), ses->src, "online");
	if (try_call(wr_fn, &(struct wr_ctx){ p, "2" })) {
		snprintf(buf, size, "no pps: %s", err_msg);
		return buf;
	}
	pathf(p, sizeof(p), ses->src, "current_max");
	apdo_ma = fdiv(rdi(p), 1000);
	log_msg("pps on, apdo current %ld mA", apdo_ma);
	pathf(p, sizeof(p), ses->src, "current_now");
	wri(p, (apdo_ma < IBUS_MAX ? apdo_ma : IBUS_MAX) * 1000);
	request(ses, 2 * s.vbat + 300, 1);
	/* VBUS into the pump's window (VBUS_ERRORLO / HI) */
	for (i = 0, ok = 0; i < 50 && !ok; i++) {
		struct sample t = { 0 };

		sleep_s(0.1);
		cp_state(&t);
		if (t.conv & CONV_VBUS_LO)
			request(ses, ses->req_mv + PPS_STEP, 0);
		else if (t.conv & CONV_VBUS_HI)
			request(ses, ses->req_mv - PPS_STEP, 0);
		else
			ok = 1;
	}
	if (!ok)
		return "vbus never in range";
	pathf(p, sizeof(p), ses->chg, "online");
	wr(p, "0");
	wr(CP "/online", "1");
	for (i = 0, ok = 0; i < 10 && !ok; i++) {
		struct sample t = { 0 };

		sleep_s(0.1);
		cp_state(&t);
		ok = !!(t.conv & CONV_SWITCHING);
	}
	if (!ok) {
		snprintf(buf, size, "pump not switching: %s", rd(CP "/state", st, sizeof(st)));
		return buf;
	}
	sample(&s);
	log_msg("pump on at %ld mV, %s", ses->req_mv, sample_str(&s, sm, sizeof(sm)));
	return NULL;
}

static const char *qc_start(struct session *ses, char *buf, size_t size)
{
	char p[300], st[128], sm[256];
	struct sample s;
	long target;
	int i, ok, n;

	write_flag();
	pathf(p, sizeof(p), ses->chg, "input_current_limit");
	wri(p, 100000);
	wr(XM "/qc_mode", "qc2-5v");
	for (i = 0, ok = 0; i < 30 && !ok; i++) {
		sleep_s(0.2);
		ok = qc_vbus() < 5300;
	}
	if (!ok) {
		snprintf(buf, size, "vbus %ld mV, not back at 5 V", qc_vbus());
		return buf;
	}
	wr(XM "/qc_mode", ses->plus ? "qc3p" : "qc3");
	sleep_s(0.5);
	sample(&s);
	if (!(VBAT_MIN <= s.vbat && s.vbat < VBAT_START_MAX)) {
		snprintf(buf, size, "vbat %ld mV out of range", s.vbat);
		return buf;
	}
	target = 2 * s.vbat + QC_INIT_UP;
	for (i = 0, ok = 0; i < (ses->plus ? 250 : 30) && !ok; i++) {
		long v = qc_vbus();

		if (v >= target - 50) {
			ok = 1;
			break;
		}
		if (v > QC_VBUS_ALARM) {
			snprintf(buf, size, "vbus %ld mV over the alarm threshold", v);
			return buf;
		}
		pulse(ses, ses->plus && v < 2 * s.vbat && v < 8300 ? 10 : 1);
	}
	if (!ok) {
		snprintf(buf, size, "vbus stuck at %ld mV (target %ld)", qc_vbus(), target);
		return buf;
	}
	/* into the pump's window (VBUS_ERRORLO / HI) */
	n = ses->plus ? 4 : 1;
	for (i = 0, ok = 0; i < (ses->plus ? 10 : 5) && !ok; i++) {
		struct sample t = { 0 };

		cp_state(&t);
		if (t.conv & CONV_VBUS_LO)
			pulse(ses, n);
		else if (t.conv & CONV_VBUS_HI)
			pulse(ses, -n);
		else
			ok = 1;
	}
	if (!ok) {
		snprintf(buf, size, "vbus not in the pump window: %s",
			 rd(CP "/state", st, sizeof(st)));
		return buf;
	}
	pathf(p, sizeof(p), ses->chg, "online");
	wr(p, "0");
	wr(CP "/online", "1");
	for (i = 0, ok = 0; i < 10 && !ok; i++) {
		struct sample t = { 0 };

		sleep_s(0.1);
		cp_state(&t);
		ok = !!(t.conv & CONV_SWITCHING);
	}
	if (!ok) {
		snprintf(buf, size, "pump not switching: %s", rd(CP "/state", st, sizeof(st)));
		return buf;
	}
	sample(&s);
	log_msg("qc pump on (%s) at %ld mV, %s", ses->qc_type, qc_vbus(),
		sample_str(&s, sm, sizeof(sm)));
	return NULL;
}

/* the checks both kinds of session share: a stop reason or NULL */
static const char *common_step(struct session *ses, const struct sample *s,
			       char *buf, size_t size)
{
	char st[128];
	double now;

	if (s->fault || !(s->conv & CONV_SWITCHING)) {
		snprintf(buf, size, "pump stopped: %s", rd(CP "/state", st, sizeof(st)));
		return buf;
	}
	if (!(T_MIN <= s->temp && s->temp < T_MAX)) {
		snprintf(buf, size, "battery %.1f C", s->temp / 10.0);
		return buf;
	}
	if (charge_inhibited()) {
		snprintf(buf, size, "charge limit at %ld%%", s->cap);
		return buf;
	}
	now = mono();
	/* CV: walk the current limit down near the voltage limit */
	if (s->vbat > VBAT_MAX - TAPER_HYS && now - ses->taper_t >= 1) {
		long f = fcc(ses, s);

		ses->fcc_taper = (ses->fcc_taper < f ? ses->fcc_taper : f) - 100;
		ses->taper_t = now;
	}
	if (s->vbat > VBAT_MAX - TAPER_DONE_HYS && s->ibat < TAPER_DONE_IBAT) {
		if (!ses->done_t)
			ses->done_t = now;
		if (now - ses->done_t >= 10)
			return "done";
	} else {
		ses->done_t = 0;
	}
	return NULL;
}

/* one control period; a stop reason or NULL */
static const char *pps_step(struct session *ses, const struct sample *s,
			    char *buf, size_t size)
{
	const char *r = common_step(ses, s, buf, size);
	long f, ibus_lim, steps[5], m;
	int i;

	if (r)
		return r;
	f = fcc(ses, s);
	ibus_lim = fdiv(f, 2) + 50;
	ibus_lim = ibus_lim < IBUS_MAX ? ibus_lim : IBUS_MAX;
	steps[0] = s->vbat > VBAT_MAX ? -1 : s->vbat < VBAT_MAX - 10 ? 1 : 0;
	steps[1] = s->ibat < f - 50 ? 1 : s->ibat > f ? -1 : 0;
	steps[2] = s->ibus < ibus_lim ? 1 : s->ibus > ibus_lim + 50 ? -1 : 0;
	steps[3] = s->reg & REG_VBAT_ACTIVE ? -3 : 1;
	steps[4] = s->intr & (INT_BUS_OVP_ALM | INT_BUS_OCP_ALM) ? -1 : 1;
	for (i = 1, m = steps[0]; i < 5; i++)
		m = steps[i] < m ? steps[i] : m;
	request(ses, ses->req_mv + m * PPS_STEP, 0);
	wr(CP "/online", "1");
	return NULL;
}

static const char *qc_step(struct session *ses, const struct sample *s,
			   char *buf, size_t size)
{
	const char *r = common_step(ses, s, buf, size);
	long f, ibus_lim, b = ses->backoff;
	int alarm, vreg, up, down;

	if (r)
		return r;
	f = fcc(ses, s);
	ibus_lim = fdiv(f, 2);
	ibus_lim = ibus_lim < ses->ibus_max ? ibus_lim : ses->ibus_max;
	alarm = s->intr & (INT_BUS_OVP_ALM | INT_BUS_OCP_ALM);
	vreg = s->reg & REG_VBAT_ACTIVE;
	if (ses->plus) {
		up = s->vbus <= QC_VBUS_ALARM && s->ibus < ibus_lim - b + 50 &&
		     s->vbat < VBAT_MAX - 20 && s->ibat < f;
		down = vreg || s->vbat > VBAT_MAX - 10 || s->ibat > f + 50 ||
		       s->ibus > ibus_lim - b + 100;
	} else {
		up = s->vbus <= QC_VBUS_ALARM && s->ibus < ibus_lim - b &&
		     s->vbat < VBAT_MAX - 40 && s->ibat < f - 600;
		down = vreg || s->vbat > VBAT_MAX || s->ibat > f + 500 ||
		       s->ibus > ibus_lim + 400 - b;
	}
	if (alarm || down)
		pulse(ses, -1);
	else if (up)
		pulse(ses, 1);
	if (vreg)
		ses->backoff += 100;
	else if (s->vbat < VBAT_MAX - 150)
		ses->backoff = 0;
	wr(CP "/online", "1");
	return NULL;
}

/* each step on its own: an error is logged and the next one still runs */
static void try_wr(const char *path, const char *val)
{
	if (try_call(wr_fn, &(struct wr_ctx){ path, val }))
		log_msg("stop: %s", err_msg);
}

struct tcpm_ctx {
	char *out;
	size_t size;
	int found;
};

static void tcpm_fn(void *arg)
{
	struct tcpm_ctx *c = arg;

	c->found = tcpm(c->out, c->size);
}

static void session_stop(struct session *ses)
{
	char p[300], src[256];

	try_wr(CP "/online", "0");
	pathf(p, sizeof(p), ses->chg, "online");
	try_wr(p, "1");
	if (ses->qc) {
		try_wr(XM "/qc_mode", "qc2-5v");
	} else {
		struct tcpm_ctx c = { src, sizeof(src), 0 };

		if (try_call(tcpm_fn, &c))
			log_msg("stop: %s", err_msg);
		if (c.found) {
			pathf(p, sizeof(p), ses->src, "online");
			try_wr(p, "1");
		}
	}
	unlink(FLAG);
	/* back to the MT6360 policy (input limit, JEITA) */
	if (system(CHG_SETUP) == -1)
		log_msg("stop: %s", strerror(errno));
}

/* ---- QC 9 V for the switching charger ---------------------------------- */

struct qc {
	double since;		/* < 0: not settling */
	int tries, hot, low;
	int chip_reset;		/* once per plug: QC_TRIES failed -> redetect */
};

static void qc_reset(struct qc *qc)
{
	qc->since = -1;
	qc->tries = qc->hot = qc->low = 0;
}

/* TCPM has a PD contract (usb_type other than plain Type-C) */
static int pd_contract(void)
{
	char src[256], p[300], buf[128];

	if (!tcpm(src, sizeof(src)))
		return 0;
	pathf(p, sizeof(p), src, "usb_type");
	return !strstr(rd(p, buf, sizeof(buf)), "[C]");
}

static int str_in(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(s, *list))
			return 1;
	return 0;
}

static void qc_poll(struct qc *qc)
{
	char t[64];
	double now;
	long temp, vbus;

	if (try_rd(XM "/qc_type", t, sizeof(t)))
		strcpy(t, "none");
	if (!strcmp(t, "none"))
		qc->chip_reset = 0;
	if (!str_in(t, qc_types) || pd_contract()) {
		qc_reset(qc);
		return;
	}
	now = mono();
	if (qc->since < 0) {
		qc->since = now;
		log_msg("qc charger: %s", t);
	}
	if (now - qc->since < QC_SETTLE)
		return;
	temp = rdi(BAT "/temp");
	if (temp >= QC_T_HOT || (qc->hot && temp >= QC_T_OK)) {
		if (!qc->hot) {
			log_msg("qc: battery %.1f C, back to 5 V", temp / 10.0);
			wr(XM "/qc_mode", "qc2-5v");
			qc->hot = 1;
			if (system(CHG_SETUP) == -1)
				log_msg("qc: %s", strerror(errno));
		}
		return;
	}
	qc->hot = 0;
	/* requested 9 V and VBUS not collapsed (it sags ~0.5 V at 2 A) */
	if (rdi(XM "/qc_voltage") >= 8500) {
		if (fdiv(rdi(CP "/voltage_now"), 1000) >= QC_VBUS_LOST) {
			qc->low = 0;
			return;
		}
		qc->low++;
		if (qc->low < 2)
			return;
		log_msg("qc: vbus collapsed, charger left QC");
	}
	qc->low = 0;
	if (qc->tries >= QC_TRIES) {
		if (!qc->chip_reset) {
			/* the charger left QC mode behind the chip's back: handshake again */
			log_msg("qc: no 9 V, resetting the xmusb350");
			qc->chip_reset = 1;
			wr(XM "/reset", "1");
			qc_reset(qc);
		}
		return;
	}
	qc->tries++;
	/* QC 2.0 fixed 9 V (QC 3 chargers too); from the 5 V mode only */
	wr(XM "/qc_mode", "qc2-5v");
	wr(XM "/qc_mode", "qc2-9v");
	sleep_s(0.5);
	vbus = fdiv(rdi(CP "/voltage_now"), 1000);
	log_msg("qc: 9 V requested (%s), vbus %ld mV", t, vbus);
	if (system(CHG_SETUP) == -1)
		log_msg("qc: %s", strerror(errno));
}

/* ---- eligibility (file errors: not eligible) --------------------------- */

static int battery_ok(void)
{
	long t = rdi(BAT "/temp");

	if (!(T_MIN + T_HYS <= t && t < T_MAX - T_HYS))
		return 0;
	return rdi(BAT "/capacity") < 100 && !charge_inhibited();
}

struct elig_ctx {
	struct qc *qc;
	char *out;
	size_t size;
	int ok;
};

static void qc_pump_eligible_fn(void *arg)
{
	struct elig_ctx *c = arg;
	long fcc_max, ibus_max;

	rd(XM "/qc_type", c->out, c->size);
	c->ok = qc_limits(c->out, &fcc_max, &ibus_max) && !pd_contract() &&
		c->qc->since >= 0 && mono() - c->qc->since >= QC_SETTLE && battery_ok() &&
		/* charging current lifts it; the session measures again with the input cut */
		fdiv(rdi(BAT "/voltage_now"), 1000) < VBAT_MAX - 50;
}

/* QC 3 / 3+ charger settled (see qc_poll) and the battery in range */
static int qc_pump_eligible(struct qc *qc, char *type, size_t size)
{
	struct elig_ctx c = { qc, type, size, 0 };

	if (exists(QC_PUMP_OFF) || exists(INPUT_CAP) || !is_dir(CP))
		return 0;
	return !try_call(qc_pump_eligible_fn, &c) && c.ok;
}

static void eligible_fn(void *arg)
{
	struct elig_ctx *c = arg;
	glob_t g;

	c->ok = 0;
	if (!tcpm(c->out, c->size) || !is_dir(CP) || exists(INPUT_CAP))
		return;
	/* the adapter's advertised capabilities (no renegotiation to probe) */
	if (glob(SYSROOT "/sys/class/typec/port*-partner/usb_power_delivery/"
		 "source-capabilities/*:programmable_supply", 0, NULL, &g))
		return;
	globfree(&g);
	c->ok = battery_ok();
}

/* PPS adapter attached and the battery in the direct charging range */
static int eligible(char *src, size_t size)
{
	struct elig_ctx c = { NULL, src, size, 0 };

	return !try_call(eligible_fn, &c) && c.ok;
}

/* ---- main -------------------------------------------------------------- */

struct run_ctx {
	struct session *ses;
	char *buf;
	size_t size;
	const char *reason;
};

/* start and run a session until it stops; errors: the caller's */
static void run_fn(void *arg)
{
	struct run_ctx *c = arg;
	struct session *ses = c->ses;
	double last_log = 0;

	c->reason = ses->qc ? qc_start(ses, c->buf, c->size) : pps_start(ses, c->buf, c->size);
	while (running && !c->reason) {
		struct sample s;

		sleep_s(PERIOD);
		if (!present(ses)) {
			c->reason = "adapter removed";
			break;
		}
		sample(&s);
		c->reason = ses->qc ? qc_step(ses, &s, c->buf, c->size)
				    : pps_step(ses, &s, c->buf, c->size);
		if (mono() - last_log >= 10) {
			last_log = mono();
			log_msg("req %ld mV fcc %ld | vbus %ld ibus %ld vbat %ld ibat %ld "
				"T %.1f cap %ld%%", ses->req_mv, fcc(ses, &s), s.vbus, s.ibus,
				s.vbat, s.ibat, s.temp / 10.0, s.cap);
		}
	}
}

static void qc_poll_fn(void *arg)
{
	qc_poll(arg);
}

static struct {
	char attached[256];
	int have_attached, finished;
	double retry_at;
	struct qc qc;
} st;

static void loop_fn(void *arg)
{
	char src[256], chg[256], type[64], rbuf[400];
	struct session ses;
	struct run_ctx run = { &ses, rbuf, sizeof(rbuf), NULL };
	int have_ses = 0, have_src = tcpm(src, sizeof(src));

	if (have_src != st.have_attached || (have_src && strcmp(src, st.attached))) {
		st.have_attached = have_src;
		snprintf(st.attached, sizeof(st.attached), "%s", have_src ? src : "");
		st.finished = 0;
		st.retry_at = 0;
	}
	if (!st.finished && mono() >= st.retry_at) {
		if (eligible(src, sizeof(src))) {
			session_init(&ses, src, mt6360(chg, sizeof(chg)) ? chg : NULL);
			have_ses = 1;
		} else if (qc_pump_eligible(&st.qc, type, sizeof(type))) {
			qc_session_init(&ses, type, mt6360(chg, sizeof(chg)) ? chg : NULL);
			have_ses = 1;
		}
	}
	if (!have_ses) {
		if (try_call(qc_poll_fn, &st.qc))
			log_msg("qc: %s", err_msg);
		sleep_s(2);
		return;
	}
	if (try_call(run_fn, &run)) {
		snprintf(rbuf, sizeof(rbuf), "error: %s", err_msg);
		run.reason = rbuf;
	}
	session_stop(&ses);
	log_msg("stop: %s", run.reason ? run.reason : "terminated");
	if (ses.qc)
		qc_reset(&st.qc);	/* back to the 9 V switching path */
	if (run.reason && (!strcmp(run.reason, "done") || !strncmp(run.reason, "vbat", 4) ||
			   !strncmp(run.reason, "no pps", 6)))
		st.finished = 1;
	else
		st.retry_at = mono() + 60;
}

static void term(int sig)
{
	running = 0;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = term };

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	qc_reset(&st.qc);
	while (running) {
		/* an error outside a session (the adapter's or battery's files) */
		if (try_call(loop_fn, NULL)) {
			log_msg("error: %s", err_msg);
			sleep_s(2);
		}
	}
	return 0;
}
