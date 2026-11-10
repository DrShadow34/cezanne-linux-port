// SPDX-License-Identifier: MIT
/*
 * cezanne-gnssd: GNSS location service for the Redmi K30 Ultra (cezanne).
 * Serves NMEA (GGA + RMC, one per fix) on a Unix socket that GeoClue reads
 * ([network-nmea] nmea-socket=); the GNSS DSPs run only while a client is
 * connected (switched off IDLE s after the last one leaves). Orbits: the
 * satellites' own navigation messages (decoded, cached), the IGS download
 * where there is network. GeoClue only connects when it starts, so its
 * connection is kept in systemd's file descriptor store and taken back
 * after a restart of this service. Broadcast orbits come from the IGS (BKG
 * mirror, cached); the rough start position is the last fix, else the
 * system time zone's coordinates (zone1970.tab).
 *   cezanne-gnssd            service
 *   cezanne-gnssd --run N    receiver in the foreground for N s, fixes and
 *                            NMEA on stdout (testing; no socket)
 *   ... --offline            no download: the cached orbit file (if any, used
 *                            for aiding even when days old) and the
 *                            satellites' own messages
 *   ... --log FILE           every DSP frame (utc link dir id hex)
 *   ... --mode MODE          continuous / balanced / low (else the config)
 * /etc/cezanne-gnss.conf: [receiver] mode = balanced (default; at rest the
 * receiver tracks in short bursts), low (longer pauses) or continuous.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <malloc.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <zlib.h>

#include "gnss.h"

#define CONF		"/etc/cezanne-gnss.conf"
#define SOCK		"/run/cezanne-gnss/nmea.sock"
#define STATE		"/var/lib/cezanne-gnss/last-fix.json"
#define CACHE		"/var/cache/cezanne-gnss"
#define FIRMWARE	"/usr/lib/firmware/mediatek/cezanne"
#define IDLE		30	/* s without clients before the DSPs go off */
#define SPEED		30.0	/* m/s assumed travel since the last fix (start uncertainty) */
#define URL		"https://igs.bkg.bund.de/root_ftp/IGS/BRDC/%d/%03d/BRDC00WRD_S_%d%03d0000_01D_MN.rnx.gz"
#define MAX_AGE		3600	/* s before the orbit file is fetched again */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	pthread_mutex_lock(&log_lock);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	pthread_mutex_unlock(&log_lock);
}

static void on_state(const char *text, void *data)
{
	log_msg("%s", text);
}

/* [receiver] mode of the config, else balanced */
static char *conf_mode(void)
{
	FILE *f = fopen(CONF, "re");
	char line[256], *mode = NULL;
	int in = 0;

	while (f && fgets(line, sizeof(line), f)) {
		char *s = line, *e, *v;

		while (*s == ' ' || *s == '\t')
			s++;
		s[strcspn(s, "\r\n")] = 0;
		if (*s == '#' || *s == ';' || !*s)
			continue;
		if (*s == '[') {
			e = strchr(s, ']');
			in = e && e - s - 1 == 8 && !strncmp(s + 1, "receiver", 8);
			continue;
		}
		if (!in)
			continue;
		v = s + strcspn(s, "=:");
		if (!*v)
			continue;
		e = v;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		if (e - s == 4 && !strncasecmp(s, "mode", 4)) {
			v++;
			while (*v == ' ' || *v == '\t')
				v++;
			e = v + strlen(v);
			while (e > v && (e[-1] == ' ' || e[-1] == '\t'))
				*--e = 0;
			free(mode);
			mode = strdup(v);
		}
	}
	if (f)
		fclose(f);
	return mode ? mode : strdup("balanced");
}

/* (lat, lon) of the system time zone from zone1970.tab: 1, or 0 */
static int tz_position(double *lat, double *lon)
{
	char *real = realpath("/etc/localtime", NULL), *zone, line[512];
	FILE *f;
	int found = 0;

	if (!real || !(zone = strstr(real, "/zoneinfo/"))) {
		free(real);
		return 0;
	}
	zone += 10;
	f = fopen("/usr/share/zoneinfo/zone1970.tab", "re");
	while (f && !found && fgets(line, sizeof(line), f)) {
		char *fld[4] = { 0 }, *p = line, *c, *z;
		int n = 0, k;

		while (n < 4 && p) {
			fld[n++] = p;
			p = strchr(p, '\t');
			if (p)
				*p++ = 0;
		}
		if (n < 3)
			continue;
		z = fld[2];
		z[strcspn(z, " \t\r\n")] = 0;
		if (strcmp(z, zone))
			continue;
		c = fld[1];
		for (k = 1; c[k] && c[k] != '+' && c[k] != '-'; k++)
			;
		if (!c[k] || k < 5 || strlen(c + k) < 6)
			break;
		{
			int la = (c[1] - '0') * 10 + (c[2] - '0'), lam = (c[3] - '0') * 10 + (c[4] - '0');
			int lo = (c[k + 1] - '0') * 100 + (c[k + 2] - '0') * 10 + (c[k + 3] - '0');
			int lom = (c[k + 4] - '0') * 10 + (c[k + 5] - '0');

			*lat = (c[0] == '-' ? -1 : 1) * (la + lam / 60.0);
			*lon = (c[k] == '-' ? -1 : 1) * (lo + lom / 60.0);
			found = 1;
		}
	}
	if (f)
		fclose(f);
	free(real);
	return found;
}

/* (ECEF, uncertainty m, clock Hz) to start from: 1, or 0 */
static int start_position(double pos[3], double *unc, double *clock)
{
	JsonParser *p = json_parser_new();
	JsonNode *root;
	double lat, lon;
	int ok = 0;

	if (json_parser_load_from_file(p, STATE, NULL) && (root = json_parser_get_root(p)) &&
	    JSON_NODE_HOLDS_OBJECT(root)) {
		JsonObject *s = json_node_get_object(root);

		if (json_object_has_member(s, "lat") && json_object_has_member(s, "lon") &&
		    json_object_has_member(s, "alt") && json_object_has_member(s, "time")) {
			double t = json_object_get_double_member(s, "time");

			*unc = fmin(2e6, 30 + SPEED * fmax(0, real_s() - t));
			geo2ecef(json_object_get_double_member(s, "lat"), json_object_get_double_member(s, "lon"),
				 json_object_get_double_member(s, "alt"), pos);
			*clock = json_object_has_member(s, "clock_hz") ?
				 json_object_get_double_member(s, "clock_hz") : 0.0;
			ok = 1;
		}
	}
	g_object_unref(p);
	if (ok)
		return 1;
	if (tz_position(&lat, &lon)) {
		geo2ecef(lat, lon, 200.0, pos);
		*unc = 300e3;
		*clock = 0.0;
		return 1;
	}
	return 0;
}

static void save(double lat, double lon, double alt, double clock_hz)
{
	char *s;

	mkdir("/var/lib/cezanne-gnss", 0777);
	if (asprintf(&s, "{\"lat\": %.17g, \"lon\": %.17g, \"alt\": %.17g, \"time\": %.17g, "
		     "\"clock_hz\": %.17g}", lat, lon, alt, real_s(), clock_hz) < 0)
		return;
	if (g_file_set_contents(STATE ".new", s, -1, NULL))
		rename(STATE ".new", STATE);
	free(s);
}

/* ---- broadcast orbits from the IGS (BKG mirror) ---- */

static size_t grow(void *data, size_t size, size_t n, void *arg)
{
	GByteArray *b = arg;

	g_byte_array_append(b, data, size * n);
	return size * n;
}

/* the day's RINEX text (ASCII, others replaced) or NULL */
static char *fetch(time_t day, int timeout)
{
	struct tm tm;
	char url[256];
	CURL *c = curl_easy_init();
	GByteArray *gz = g_byte_array_new();
	GString *out = NULL;
	z_stream z = { 0 };
	unsigned char buf[65536];
	int ret;

	gmtime_r(&day, &tm);
	snprintf(url, sizeof(url), URL, tm.tm_year + 1900, tm.tm_yday + 1, tm.tm_year + 1900, tm.tm_yday + 1);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, grow);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, gz);
	ret = curl_easy_perform(c);
	curl_easy_cleanup(c);
	if (ret != CURLE_OK || !gz->len)
		goto out;
	/* gzip.decompress: every member */
	out = g_string_new("");
	z.next_in = gz->data;
	z.avail_in = gz->len;
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
		goto fail;
	for (;;) {
		size_t i, n;

		z.next_out = buf;
		z.avail_out = sizeof(buf);
		ret = inflate(&z, Z_NO_FLUSH);
		n = sizeof(buf) - z.avail_out;
		for (i = 0; i < n; i++)
			if (buf[i] < 0x80)
				g_string_append_c(out, buf[i]);
			else
				g_string_append(out, "\xef\xbf\xbd");
		if (ret == Z_STREAM_END) {
			if (!z.avail_in)
				break;
			inflateReset(&z);
			continue;
		}
		if (ret != Z_OK && !(ret == Z_BUF_ERROR && !z.avail_in && n))
			goto fail;
		if (!z.avail_in && !n)
			goto fail;	/* truncated */
	}
	inflateEnd(&z);
	g_byte_array_unref(gz);
	return g_string_free(out, FALSE);
fail:
	inflateEnd(&z);
	g_string_free(out, TRUE);
out:
	g_byte_array_unref(gz);
	return NULL;
}

/* the cached file without fetching, or NULL */
static char *orbits_cached(void)
{
	return access(CACHE "/brdc.rnx", F_OK) ? NULL : strdup(CACHE "/brdc.rnx");
}

/*
 * cache/brdc.rnx = yesterday's + today's records (UTC days) with today's
 * header; returns its path, or NULL if there is no file at all
 */
static char *orbits_refresh(void)
{
	struct stat st;
	time_t now = time(NULL), today = now - now % 86400;
	char *texts[2];
	int n = 0, i;

	if (!stat(CACHE "/brdc.rnx", &st) &&
	    real_s() - (st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9) < MAX_AGE)
		return strdup(CACHE "/brdc.rnx");
	for (i = 0; i < 2; i++) {
		char *t = fetch(today - (1 - i) * 86400, 30);

		if (t)
			texts[n++] = t;
	}
	if (n) {
		const char *last = texts[n - 1], *eoh = strstr(last, "END OF HEADER");
		GString *out = g_string_new("");

		if (eoh) {
			const char *nl = strchr(eoh, '\n');

			g_string_append_len(out, last, nl ? nl - last : (gssize)strlen(last));
			g_string_append_c(out, '\n');
		} else {
			g_string_append(out, last);
			g_string_append_c(out, '\n');
		}
		for (i = 0; i < n; i++) {
			const char *h = strstr(texts[i], "END OF HEADER"), *nl = h ? strchr(h, '\n') : NULL;

			if (nl)
				g_string_append(out, nl + 1);
		}
		mkdir(CACHE, 0777);
		if (g_file_set_contents(CACHE "/brdc.rnx.new", out->str, out->len, NULL))
			rename(CACHE "/brdc.rnx.new", CACHE "/brdc.rnx");
		g_string_free(out, TRUE);
		for (i = 0; i < n; i++)
			g_free(texts[i]);
	}
	return orbits_cached();
}

/* ---- the service ---- */

struct client {
	int fd;
	char name[32];
};

static struct {
	pthread_mutex_t lock;
	struct client *c;
	int n;
} clients = { PTHREAD_MUTEX_INITIALIZER, NULL, 0 };

/* sd_notify, with file descriptors for the store */
static void notify(const char *text, const int *fds, int nfds)
{
	const char *addr = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct iovec iov = { (void *)text, strlen(text) };
	struct msghdr msg = { .msg_name = &sa, .msg_iov = &iov, .msg_iovlen = 1 };
	char cbuf[CMSG_SPACE(sizeof(int) * 8)];
	socklen_t len;
	int s;

	if (!addr || !*addr)
		return;
	strncpy(sa.sun_path, addr, sizeof(sa.sun_path) - 1);
	if (addr[0] == '@')
		sa.sun_path[0] = 0;
	len = offsetof(struct sockaddr_un, sun_path) + strlen(addr);
	msg.msg_namelen = len;
	if (nfds) {
		struct cmsghdr *cm;

		msg.msg_control = cbuf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
		cm = CMSG_FIRSTHDR(&msg);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
		memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfds);
	}
	s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return;
	sendmsg(s, &msg, MSG_NOSIGNAL);
	close(s);
}

static void drop(int fd)
{
	char name[32] = "";
	int i, n;

	pthread_mutex_lock(&clients.lock);
	for (i = 0; i < clients.n; i++)
		if (clients.c[i].fd == fd) {
			snprintf(name, sizeof(name), "%s", clients.c[i].name);
			clients.c[i] = clients.c[--clients.n];
			break;
		}
	n = clients.n;
	pthread_mutex_unlock(&clients.lock);
	if (name[0]) {
		char msg[64];

		snprintf(msg, sizeof(msg), "FDSTOREREMOVE=1\nFDNAME=%s", name);
		notify(msg, NULL, 0);
		close(fd);
		log_msg("client left (%d)", n);
	}
}

/*
 * one receiver run in a thread; orbits refreshed in the background (the
 * receiver reloads the file when it changes)
 */
struct session {
	volatile int stop;
	pthread_t thread, orbits;
	int have_fix, orbits_started;
	double lat, lon, alt, clock_hz;
	double last_save, t0;
	pthread_mutex_t wait_lock;
	pthread_cond_t wait_cond;
};

static void send_fix(struct fix *fix)
{
	char gga[256], rmc[256], data[512];
	int *fds, n, i;

	nmea_gga(fix, gga, sizeof(gga));
	nmea_rmc(fix, rmc, sizeof(rmc));
	snprintf(data, sizeof(data), "%s%s", gga, rmc);
	pthread_mutex_lock(&clients.lock);
	n = clients.n;
	fds = malloc(sizeof(int) * (n ? n : 1));
	for (i = 0; i < n; i++)
		fds[i] = clients.c[i].fd;
	pthread_mutex_unlock(&clients.lock);
	for (i = 0; i < n; i++) {
		size_t off = 0, len = strlen(data);

		while (off < len) {
			ssize_t w = send(fds[i], data + off, len - off, MSG_NOSIGNAL);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				drop(fds[i]);
				break;
			}
			off += w;
		}
	}
	free(fds);
}

static void session_fix(struct fix *fix, void *data)
{
	struct session *s = data;

	if (!s->have_fix)
		log_msg("first fix after %.0f s, %d satellites", mono_s() - s->t0, fix->nused);
	if (mono_s() - s->last_save > 60) {
		save(fix->lat, fix->lon, fix->alt, fix->clock_hz);
		s->last_save = mono_s();
	}
	s->have_fix = 1;
	s->lat = fix->lat;
	s->lon = fix->lon;
	s->alt = fix->alt;
	s->clock_hz = fix->clock_hz;
	send_fix(fix);
}

static void *session_orbits(void *arg)
{
	struct session *s = arg;

	for (;;) {
		struct timespec ts;
		int stopped;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 600;
		pthread_mutex_lock(&s->wait_lock);
		while (!s->stop)
			if (pthread_cond_timedwait(&s->wait_cond, &s->wait_lock, &ts) == ETIMEDOUT)
				break;
		stopped = s->stop;
		pthread_mutex_unlock(&s->wait_lock);
		if (stopped)
			return NULL;
		free(orbits_refresh());
	}
}

static void *session_run(void *arg)
{
	struct session *s = arg;
	char *nav = orbits_refresh(), *mode;
	double pos[3], unc, clock, lat, lon, h;
	struct receiver *r;
	struct receiver_cb cb = { session_fix, on_state, s };

	if (!start_position(pos, &unc, &clock)) {
		log_msg("no start position");
		free(nav);
		return NULL;
	}
	ecef2geo(pos, &lat, &lon, &h);
	log_msg("receiver on: start %.2f %.2f +- %.0f km, clock %+.0f Hz", lat, lon, unc / 1000, clock);
	if (!pthread_create(&s->orbits, NULL, session_orbits, s))
		s->orbits_started = 1;
	if (!nav)
		log_msg("no downloaded orbits: from the satellites only");
	mode = conf_mode();
	r = receiver_new(nav, pos, unc, clock, FIRMWARE, CACHE, mode, cb);
	receiver_run(r, &s->stop, 0);
	receiver_free(r);
	free(mode);
	free(nav);
	if (s->have_fix)
		save(s->lat, s->lon, s->alt, s->clock_hz);
	log_msg("receiver off");
	return NULL;
}

static struct session *session_new(void)
{
	struct session *s = calloc(1, sizeof(*s));

	s->t0 = mono_s();
	pthread_mutex_init(&s->wait_lock, NULL);
	pthread_cond_init(&s->wait_cond, NULL);
	pthread_create(&s->thread, NULL, session_run, s);
	return s;
}

static void session_close(struct session *s)
{
	pthread_mutex_lock(&s->wait_lock);
	s->stop = 1;
	pthread_cond_broadcast(&s->wait_cond);
	pthread_mutex_unlock(&s->wait_lock);
	pthread_join(s->thread, NULL);
	if (s->orbits_started)
		pthread_join(s->orbits, NULL);
	free(s);
}

/* client connections handed back by systemd's fd store */
static void stored(void)
{
	const char *pid = getenv("LISTEN_PID"), *fdn = getenv("LISTEN_FDS"), *names = getenv("LISTEN_FDNAMES");
	char **nv, buf[16];
	int n, i, nn;

	snprintf(buf, sizeof(buf), "%d", getpid());
	if (!pid || strcmp(pid, buf))
		return;
	n = fdn ? atoi(fdn) : 0;
	nv = g_strsplit(names ? names : "", ":", -1);
	nn = g_strv_length(nv);
	for (i = 0; i < n; i++)
		if (i < nn && g_str_has_prefix(nv[i], "client")) {
			clients.c = realloc(clients.c, (clients.n + 1) * sizeof(*clients.c));
			clients.c[clients.n].fd = 3 + i;
			snprintf(clients.c[clients.n].name, sizeof(clients.c[0].name), "%s", nv[i]);
			clients.n++;
		}
	g_strfreev(nv);
}

static int serve(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = SOCK };
	struct session *session = NULL;
	double idle_since = 0;
	long long serial = (long long)time(NULL);
	int srv, have_idle = 0;

	mkdir("/run/cezanne-gnss", 0777);
	unlink(SOCK);
	srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (srv < 0 || bind(srv, (struct sockaddr *)&sa, sizeof(sa)) || chmod(SOCK, 0666) ||
	    listen(srv, 4)) {
		fprintf(stderr, "%s: %s\n", SOCK, strerror(errno));
		return 1;
	}
	stored();
	if (clients.n)
		log_msg("listening on %s, %d client(s) kept over the restart", SOCK, clients.n);
	else
		log_msg("listening on %s", SOCK);
	for (;;) {
		struct timeval tv = { 1, 0 };
		fd_set rd;
		int maxfd = srv, i, n, *fds;

		FD_ZERO(&rd);
		FD_SET(srv, &rd);
		pthread_mutex_lock(&clients.lock);
		n = clients.n;
		fds = malloc(sizeof(int) * (n ? n : 1));
		for (i = 0; i < n; i++) {
			fds[i] = clients.c[i].fd;
			FD_SET(fds[i], &rd);
			if (fds[i] > maxfd)
				maxfd = fds[i];
		}
		pthread_mutex_unlock(&clients.lock);
		if (select(maxfd + 1, &rd, NULL, NULL, &tv) > 0) {
			if (FD_ISSET(srv, &rd)) {
				int c = accept4(srv, NULL, NULL, SOCK_CLOEXEC);

				if (c >= 0) {
					char name[32], msg[64];
					int total;

					snprintf(name, sizeof(name), "client%lld", serial++);
					pthread_mutex_lock(&clients.lock);
					clients.c = realloc(clients.c, (clients.n + 1) * sizeof(*clients.c));
					clients.c[clients.n].fd = c;
					snprintf(clients.c[clients.n].name, sizeof(clients.c[0].name), "%s", name);
					total = ++clients.n;
					pthread_mutex_unlock(&clients.lock);
					snprintf(msg, sizeof(msg), "FDSTORE=1\nFDNAME=%s", name);
					notify(msg, &c, 1);
					log_msg("client connected (%d)", total);
				}
			}
			for (i = 0; i < n; i++) {
				char b[256];
				ssize_t r;

				if (!FD_ISSET(fds[i], &rd))
					continue;
				r = recv(fds[i], b, sizeof(b), MSG_DONTWAIT);
				if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR))
					drop(fds[i]);
			}
		}
		free(fds);
		pthread_mutex_lock(&clients.lock);
		n = clients.n;
		pthread_mutex_unlock(&clients.lock);
		if (n && !session) {
			session = session_new();
			have_idle = 0;
		} else if (n) {
			have_idle = 0;
		} else if (session) {
			if (!have_idle) {
				idle_since = mono_s();
				have_idle = 1;
			}
			if (mono_s() - idle_since > IDLE) {
				session_close(session);
				session = NULL;
			}
		}
	}
}

/* ---- the foreground test run ---- */

static double run_t0;

static void run_fix(struct fix *f, void *data)
{
	GString *sats = g_string_new("");
	char speed[32], gga[256], rmc[256];
	int i;

	for (i = 0; i < f->nused; i++)
		g_string_append_printf(sats, "%s%c%d/%d", i ? " " : "", f->used[i].sys, f->used[i].prn,
				       f->used[i].cn);
	if (f->has_speed)
		snprintf(speed, sizeof(speed), "speed %.1f m/s", f->speed);
	else
		snprintf(speed, sizeof(speed), "speed -");
	log_msg("%5.1f s  %.6f %.6f %.0f m  acc %.0f m  rms %.0f m  %s  %s", mono_s() - run_t0, f->lat,
		f->lon, f->alt, f->acc, f->rms, speed, sats->str);
	nmea_gga(f, gga, sizeof(gga));
	nmea_rmc(f, rmc, sizeof(rmc));
	pthread_mutex_lock(&log_lock);
	fputs(gga, stdout);
	fputs(rmc, stdout);
	fflush(stdout);
	pthread_mutex_unlock(&log_lock);
	g_string_free(sats, TRUE);
}

static int run(double seconds, int offline, const char *log_file, const char *mode_name)
{
	char *nav = offline ? orbits_cached() : orbits_refresh(), *mode;
	double pos[3], unc, clock, lat, lon, alt;
	struct receiver_cb cb = { run_fix, on_state, NULL };
	struct receiver *r;
	volatile int stop = 0;

	if (log_file) {
		dsp_log = fopen(log_file, "we");
		if (dsp_log)
			setvbuf(dsp_log, NULL, _IOLBF, 0);
	}
	if (!start_position(pos, &unc, &clock)) {
		log_msg("no start position");
		free(nav);
		return 1;
	}
	run_t0 = mono_s();
	mode = mode_name ? strdup(mode_name) : conf_mode();
	r = receiver_new(nav, pos, unc, clock, FIRMWARE, CACHE, mode, cb);
	receiver_run(r, &stop, seconds);
	receiver_result(r, pos, &unc, &clock);
	if (unc < 100) {	/* had a fix */
		ecef2geo(pos, &lat, &lon, &alt);
		save(lat, lon, alt, clock);
	}
	receiver_free(r);
	free(mode);
	free(nav);
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "run", required_argument, NULL, 'r' },
		{ "offline", no_argument, NULL, 'o' },
		{ "log", required_argument, NULL, 'l' },
		{ "mode", required_argument, NULL, 'm' },
		{ NULL, 0, NULL, 0 },
	};
	double seconds = 0;
	int offline = 0, c;
	const char *log_file = NULL, *mode = NULL;

	/*
	 * fewer malloc arenas: glibc backs each thread arena with a 2 MiB THP;
	 * a fixed mmap threshold: the orbit file's buffers (~15 MB, parsed every
	 * hour) go back to the system when freed, not into the (THP) heap
	 */
	mallopt(M_ARENA_MAX, 1);
	mallopt(M_MMAP_THRESHOLD, 256 * 1024);
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'r': seconds = strtod(optarg, NULL); break;
		case 'o': offline = 1; break;
		case 'l': log_file = optarg; break;
		case 'm':
			if (strcmp(optarg, "continuous") && strcmp(optarg, "balanced") && strcmp(optarg, "low")) {
				fprintf(stderr, "cezanne-gnssd: error: argument --mode: invalid choice: '%s'\n", optarg);
				return 2;
			}
			mode = optarg;
			break;
		default:
			fputs("usage: cezanne-gnssd [-h] [--run SECONDS] [--offline] [--log LOG] "
			      "[--mode {continuous,balanced,low}]\n", stderr);
			return 2;
		}
	}
	return seconds ? run(seconds, offline, log_file, mode) : serve();
}
