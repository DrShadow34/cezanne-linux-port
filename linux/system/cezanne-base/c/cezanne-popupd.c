// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Pop-up front camera daemon (Redmi K30 Ultra).
 *
 * Raises the camera while it is in use and retracts it otherwise, like MIUI's
 * mi.motor.daemon. Motor: popup-motor.ko (/sys/bus/platform/devices/
 * popup-camera/{move,state}); position: ak09970.ko IIO "popup_hall", read
 * against MIUI's per-unit calibration (persist:/sensors/hall_calidata.txt).
 *
 * "In use" =
 *   - a client holds a connection on /run/cezanne-popup.sock after sending
 *     "hold" (the camera stack opens it and keeps it; if the process exits or
 *     crashes the kernel closes the socket and the camera goes down), or
 *   - a front camera capture runs: STREAM_ATTR reads "1" (cezanne_camsv.ko
 *     sets it from stream-on to stream-off; open video nodes alone do not
 *     count, libcamera/PipeWire keep them open all the time).
 *
 * Safety: retract at start and stop, verify every move with the hall sensor
 * (retry, then report), retract when the camera is pushed in by hand or the
 * phone is in free fall, and rate-limit pop-ups (MIUI warns about frequent
 * use; a long burst triggers a cool-down that protects the motor).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MOTOR		"/sys/bus/platform/devices/popup-camera"
#define SOCK		"/run/cezanne-popup.sock"
#define CALI_FILE	"/var/lib/cezanne-popup/hall_calidata.txt"
/* where MIUI keeps it: copied once into CALI_FILE (mounted read only) */
#define PERSIST		"/dev/disk/by-partlabel/persist"
#define PERSIST_CALI	"sensors/hall_calidata.txt"
#define STREAM_ATTR	"/sys/bus/platform/devices/1a004000.camera/streaming"
#define RELEASE_DELAY	0.5	/* s: app switches should not bounce the camera */
#define WARN_COUNT	5	/* pop-ups per WARN_WINDOW s before notifying */
#define WARN_WINDOW	60
#define LIMIT_COUNT	10	/* ... per LIMIT_WINDOW s before a cool-down */
#define LIMIT_WINDOW	120
#define COOLDOWN	60
#define FREEFALL_MS2	3.0	/* |a| below this ... */
#define FREEFALL_SAMPLES 3	/* ... for this many 20 ms samples */
#define PRESS_POS	7.9	/* popped up (rests at 8.00-8.02) but below this ... */
#define PRESS_SAMPLES	2	/* ... for this many 20 ms samples: pushed in */
#define HALL_SLOW_S	0.3	/* a hall read slower than this: bus trouble ... */
#define HALL_RETRY_S	30	/* ... do without the sensor this long */
#define MAX_CLIENTS	64
#define MAX_POINTS	32

struct point {
	double pos, xyz[3];
};

/* this unit's factory data, as a fallback */
static const struct point factory[] = {
	{ 0, { -197, 663, -7336 } }, { 1, { -1108, 2084, -6935 } },
	{ 2, { -2557, 4518, -6157 } }, { 3, { -3936, 6833, -4246 } },
	{ 4, { -4529, 7838, -1270 } }, { 5, { -3886, 6765, 1395 } },
	{ 6, { -2798, 4920, 2550 } }, { 7, { -1809, 3333, 2731 } },
	{ 8, { -1171, 2136, 2396 } }, { 9, { -776, 1349, 1953 } },
};

static struct point table[MAX_POINTS];
static int npoints;
static char hall_dev[300], accel_dev[300];
static double broken_until;

static struct {
	int fd;
	int holder;
} clients[MAX_CLIENTS];
static int nclients;

static int up, manual, fall, press;
static double release_at = -1, cooldown_until, warned_at;
static double pops[64];
static int npops;
static volatile sig_atomic_t stop;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
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

/* the file's contents (NUL-terminated, up to size - 1 bytes), or -1 */
static int read_str(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return n;
}

/* Python int(): surrounding white space allowed, nothing else */
static int read_int(const char *path, long *v)
{
	char buf[64], *end;

	if (read_str(path, buf, sizeof(buf)) < 0)
		return 0;
	errno = 0;
	*v = strtol(buf, &end, 10);
	if (end == buf || errno)
		return 0;
	while (*end == ' ' || *end == '\n' || *end == '\t' || *end == '\r')
		end++;
	return !*end;
}

static int write_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), ok;

	if (fd < 0)
		return -errno;
	ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
	if (!ok) {
		int e = errno;

		close(fd);
		return -e;
	}
	return close(fd) ? -errno : 0;
}

static int find_iio(const char *name, char *out, size_t size)
{
	DIR *d = opendir("/sys/bus/iio/devices");
	struct dirent *e;
	char path[300], buf[64];

	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/iio/devices/%s/name", e->d_name);
		if (read_str(path, buf, sizeof(buf)) < 0)
			continue;
		buf[strcspn(buf, " \t\r\n")] = 0;
		if (!strcmp(buf, name)) {
			snprintf(out, size, "/sys/bus/iio/devices/%s", e->d_name);
			closedir(d);
			return 1;
		}
	}
	if (d)
		closedir(d);
	out[0] = 0;
	return 0;
}

/* run a command, wait for it; its exit status or -1 */
static int run(char *const argv[], int quiet)
{
	pid_t pid = fork();
	int st;

	if (pid < 0)
		return -1;
	if (!pid) {
		if (quiet) {
			int null = open("/dev/null", O_RDWR);

			dup2(null, 1);
			dup2(null, 2);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* the per-unit calibration out of MIUI's persist partition, once */
static void copy_persist_cali(void)
{
	char mnt[64], src[128], tmp[] = CALI_FILE ".new", buf[4096];
	int in, out = -1, n, ok = 1;

	if (!access(CALI_FILE, F_OK) || access(PERSIST, F_OK))
		return;
	snprintf(mnt, sizeof(mnt), "/run/cezanne-popup.persist.%d", getpid());
	mkdir(mnt, 0755);
	if (run((char *[]){ "mount", "-o", "ro", PERSIST, mnt, NULL }, 1)) {
		rmdir(mnt);
		return;
	}
	snprintf(src, sizeof(src), "%s/%s", mnt, PERSIST_CALI);
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in >= 0) {
		mkdir("/var/lib/cezanne-popup", 0755);
		out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	}
	if (out >= 0) {
		while ((n = read(in, buf, sizeof(buf))) > 0)
			if (write(out, buf, n) != n)
				ok = 0;
		if (n < 0)
			ok = 0;
		if (close(out))
			ok = 0;
		if (ok && !rename(tmp, CALI_FILE))
			log_msg("calibration copied from persist:/%s", PERSIST_CALI);
	}
	if (in >= 0)
		close(in);
	run((char *[]){ "umount", mnt, NULL }, 1);
	rmdir(mnt);
}

static int cmp_point(const void *a, const void *b)
{
	const struct point *p = a, *q = b;
	int i;

	if (p->pos != q->pos)
		return p->pos < q->pos ? -1 : 1;
	for (i = 0; i < 3; i++)
		if (p->xyz[i] != q->xyz[i])
			return p->xyz[i] < q->xyz[i] ? -1 : 1;
	return 0;
}

/* "pos : 0.0, mag (uT) : -197.000000, 663.000000, -7336.000000" */
static int parse_point(char *line, struct point *p)
{
	char *f1 = strchr(line, ':'), *last = strrchr(line, ':'), *end;
	int i;

	if (!f1 || f1 == last)
		return 0;
	p->pos = strtod(f1 + 1, &end);
	if (end == f1 + 1 || (*end != ',' && *end != ':' && *end != ' '))
		return 0;
	end = last + 1;
	for (i = 0; i < 3; i++) {
		char *s = end;

		p->xyz[i] = strtod(s, &end);
		if (end == s)
			return 0;
		while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
			end++;
		if (*end != (i < 2 ? ',' : 0))
			return 0;
		end++;
	}
	return 1;
}

static void load_table(void)
{
	FILE *f;
	char line[256];
	int ok = 1;

	copy_persist_cali();
	npoints = 0;
	f = fopen(CALI_FILE, "re");
	while (f && fgets(line, sizeof(line), f)) {
		if (strncmp(line, "pos", 3))
			continue;
		if (npoints == MAX_POINTS || !parse_point(line, &table[npoints])) {
			ok = 0;
			break;
		}
		npoints++;
	}
	if (f)
		fclose(f);
	if (f && ok && npoints >= 2) {
		qsort(table, npoints, sizeof(*table), cmp_point);
		return;
	}
	memcpy(table, factory, sizeof(factory));
	npoints = sizeof(factory) / sizeof(*factory);
}

static int hall_read(double xyz[3])
{
	char path[340];
	double now;
	long v[3];
	int i, ok = 1;

	if (!hall_dev[0] && !find_iio("popup_hall", hall_dev, sizeof(hall_dev)))
		return 0;
	/*
	 * A hung I2C bus makes every read block for seconds: then go without
	 * position feedback for a while (moves are timed anyway) instead of
	 * delaying every move and check.
	 */
	now = mono();
	if (now < broken_until)
		return 0;
	for (i = 0; i < 3; i++) {
		snprintf(path, sizeof(path), "%s/in_magn_%c_raw", hall_dev, "xyz"[i]);
		if (!read_int(path, &v[i]))
			ok = 0;
	}
	if (mono() - now > HALL_SLOW_S) {
		log_msg("hall sensor not responding, ignoring it for %d s", HALL_RETRY_S);
		broken_until = mono() + HALL_RETRY_S;
		return 0;
	}
	for (i = 0; i < 3; i++)
		xyz[i] = v[i];
	return ok;
}

/* position 0 (retracted) .. 9 from the 3-axis field, nearest neighbour
 * between MIUI's calibrated points (linear in between); NAN: unknown */
static double position_of(const double xyz[3])
{
	double best_d = INFINITY, best = NAN;
	int i, k;

	/* project onto the closest calibration segment */
	for (i = 0; i + 1 < npoints; i++) {
		const double *a = table[i].xyz, *b = table[i + 1].xyz;
		double ab[3], l2 = 0, dot = 0, t, d2 = 0;

		for (k = 0; k < 3; k++) {
			ab[k] = b[k] - a[k];
			l2 += ab[k] * ab[k];
			dot += (xyz[k] - a[k]) * ab[k];
		}
		if (!l2)
			l2 = 1;
		t = fmax(0.0, fmin(1.0, dot / l2));
		for (k = 0; k < 3; k++) {
			double p = a[k] + t * ab[k];

			d2 += (xyz[k] - p) * (xyz[k] - p);
		}
		if (sqrt(d2) < best_d) {
			best_d = sqrt(d2);
			best = table[i].pos + t * (table[i + 1].pos - table[i].pos);
		}
	}
	return best;
}

static double position(void)
{
	double xyz[3];

	return hall_read(xyz) ? position_of(xyz) : NAN;
}

/* Python's repr of round(x, 2) */
static const char *fmt_pos2(double pos, char *buf, size_t size)
{
	size_t n;

	if (isnan(pos))
		return "None";
	snprintf(buf, size, "%.2f", pos);
	n = strlen(buf);
	if (buf[n - 1] == '0')
		buf[--n] = 0;
	return buf;
}

/* Python's repr of a float: the shortest form that reads back the same */
static const char *fmt_repr(double pos, char *buf, size_t size)
{
	int prec;

	if (isnan(pos))
		return "None";
	for (prec = 1; prec <= 17; prec++) {
		snprintf(buf, size, "%.*g", prec, pos);
		if (strtod(buf, NULL) == pos)
			break;
	}
	if (!strpbrk(buf, ".en"))
		strncat(buf, ".0", size - strlen(buf) - 1);
	return buf;
}

static int motor_move(const char *cmd)
{
	char state[32];
	double t0;
	int err = write_str(MOTOR "/move", cmd);

	if (err) {
		log_msg("motor: %s", strerror(-err));
		return 0;
	}
	t0 = mono();
	while (mono() - t0 < 1.5) {
		if (read_str(MOTOR "/state", state, sizeof(state)) < 0)
			return 0;
		state[strcspn(state, " \t\r\n")] = 0;
		if (!strcmp(state, "idle"))
			return 1;
		sleep_s(0.02);
	}
	write_str(MOTOR "/move", "stop");
	return 0;
}

/* notify-send in seat0's session, not waited for */
static void notify(const char *title, const char *body, const char *urgency)
{
	char line[256], uid[16], user[64], seat[64], env[96];
	FILE *p = popen("loginctl list-sessions --no-legend", "re");
	pid_t pid;

	if (!p)
		return;
	user[0] = 0;
	while (fgets(line, sizeof(line), p))
		if (sscanf(line, "%*s %15s %63s %63s", uid, user, seat) == 3 &&
		    !strcmp(seat, "seat0"))
			break;
		else
			user[0] = 0;
	pclose(p);
	if (!user[0])
		return;
	snprintf(env, sizeof(env), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%ld/bus",
		 strtol(uid, NULL, 10));
	pid = fork();
	if (pid < 0)
		return;
	if (!pid) {
		/* the grandchild runs it, reparented: nothing to reap here */
		if (fork())
			_exit(0);
		int null = open("/dev/null", O_RDWR);

		dup2(null, 1);
		dup2(null, 2);
		putenv(env);
		execlp("runuser", "runuser", "-u", user, "--", "notify-send", "-a",
		       "Pop-up camera", "-i", "camera-web", "-u", urgency, title, body,
		       (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

/* ---- movement ---------------------------------------------------------- */

static int go(int want_up)
{
	const char *want = want_up ? "up" : "down";
	char buf[32];
	int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		double pos;
		int ok;

		motor_move(want);
		pos = position();
		ok = isnan(pos) || (want_up ? pos >= 7.5 : pos <= 0.8);
		log_msg("%s: position %s%s", want, fmt_pos2(pos, buf, sizeof(buf)),
			ok ? "" : " (not reached)");
		if (ok) {
			up = want_up;
			return 1;
		}
	}
	up = want_up;
	return 0;
}

static void retract(const char *reason)
{
	log_msg("retract: %s", reason);
	if (!go(0))
		notify("Pop-up camera", "Camera did not retract fully", "critical");
	release_at = -1;
	manual = 0;
}

static int pop(void)
{
	double now = mono();
	int i, n = 0, recent = 0;

	if (now < cooldown_until) {
		log_msg("pop refused: cooling down");
		return 0;
	}
	for (i = 0; i < npops; i++)
		if (now - pops[i] < LIMIT_WINDOW)
			pops[n++] = pops[i];
	if (n == sizeof(pops) / sizeof(*pops))	/* cannot happen: cool-downs space them */
		memmove(pops, pops + 1, --n * sizeof(*pops));
	pops[n++] = now;
	npops = n;
	for (i = 0; i < npops; i++)
		if (now - pops[i] < WARN_WINDOW)
			recent++;
	if (npops >= LIMIT_COUNT) {
		char body[64];

		cooldown_until = now + COOLDOWN;
		snprintf(body, sizeof(body), "Used too often, paused for %d s", COOLDOWN);
		notify("Pop-up camera", body, "critical");
		return 0;
	}
	if (recent >= WARN_COUNT && now - warned_at > WARN_WINDOW) {
		warned_at = now;
		notify("Pop-up camera", "Frequent use wears the motor", "normal");
	}
	if (!go(1)) {
		notify("Pop-up camera", "Camera is blocked", "critical");
		retract("pop-up blocked");
		return 0;
	}
	find_iio("accel", accel_dev, sizeof(accel_dev));
	fall = 0;
	return 1;
}

/* ---- usage ------------------------------------------------------------- */

static int holders(void)
{
	int i, n = 0;

	for (i = 0; i < nclients; i++)
		n += clients[i].holder;
	return n;
}

static int in_use(void)
{
	long v;

	return holders() || manual || (read_int(STREAM_ATTR, &v) && v == 1);
}

static void update(void)
{
	int busy = in_use();
	double now = mono();

	if (busy) {
		release_at = -1;
		if (!up && now >= cooldown_until)
			pop();
	} else if (up) {
		if (release_at < 0)
			release_at = now + RELEASE_DELAY;
		else if (now >= release_at)
			retract("camera released");
	}
}

/* ---- safety while up --------------------------------------------------- */

static void check_up(void)
{
	char path[340], reason[64];
	double pos;
	long a[3];
	int i, ok = 1;

	if (!up)
		return;
	if (accel_dev[0]) {
		for (i = 0; i < 3; i++) {
			snprintf(path, sizeof(path), "%s/in_accel_%c_raw", accel_dev, "xyz"[i]);
			if (!read_int(path, &a[i]))
				ok = 0;
		}
		if (ok) {
			double g = sqrt((double)a[0] * a[0] + (double)a[1] * a[1] +
					(double)a[2] * a[2]) / 1000.0;

			fall = g < FREEFALL_MS2 ? fall + 1 : 0;
			if (fall >= FREEFALL_SAMPLES) {
				snprintf(reason, sizeof(reason), "free fall (|a| %.1f m/s^2)", g);
				retract(reason);
				notify("Pop-up camera", "Fall detected, camera retracted", "normal");
				cooldown_until = mono() + 3;
				return;
			}
		}
	}
	pos = position();
	press = !isnan(pos) && pos < PRESS_POS ? press + 1 : 0;
	if (press >= PRESS_SAMPLES) {
		press = 0;
		snprintf(reason, sizeof(reason), "pushed in (position %.1f)", pos);
		retract(reason);
		notify("Pop-up camera", "Pushed in, camera retracted", "normal");
		cooldown_until = mono() + 2;
	}
}

/* ---- socket API -------------------------------------------------------- */

static int serve(void)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX, .sun_path = SOCK };
	int fd;

	unlink(SOCK);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    chmod(SOCK, 0666) || listen(fd, 8)) {
		log_msg("%s: %s", SOCK, strerror(errno));
		exit(1);
	}
	return fd;
}

static void reply(int fd, const char *text)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf), "%s\n", text);

	send(fd, buf, n, MSG_NOSIGNAL);	/* failures are the client's problem */
}

static void drop_client(int i)
{
	close(clients[i].fd);
	clients[i] = clients[--nclients];
}

/* returns 0 when client i was dropped */
static int client(int i)
{
	char data[257], *cmd, *save;
	int fd = clients[i].fd, n = recv(fd, data, sizeof(data) - 1, 0);

	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return 1;
		drop_client(i);
		return 0;
	}
	data[n] = 0;
	for (cmd = strtok_r(data, " \t\r\n\v\f", &save); cmd;
	     cmd = strtok_r(NULL, " \t\r\n\v\f", &save)) {
		if (!strcmp(cmd, "hold")) {
			clients[i].holder = 1;
			update();
			reply(fd, up ? "up" : "refused");
		} else if (!strcmp(cmd, "release")) {
			clients[i].holder = 0;
			reply(fd, "ok");
		} else if (!strcmp(cmd, "up")) {
			manual = 1;
			update();
			reply(fd, up ? "up" : "refused");
		} else if (!strcmp(cmd, "down")) {
			int k;

			manual = 0;
			for (k = 0; k < nclients; k++)
				clients[k].holder = 0;
			if (up)
				retract("requested");
			reply(fd, "down");
		} else if (!strcmp(cmd, "status")) {
			char text[128], p[16];
			double pos = position(), cd = fmax(0, cooldown_until - mono());

			if (isnan(pos))
				strcpy(p, "?");
			else
				snprintf(p, sizeof(p), "%.2f", pos);
			snprintf(text, sizeof(text), "%s position=%s holders=%d cooldown=%.0fs",
				 up ? "up" : "down", p, holders(), cd);
			reply(fd, text);
		} else {
			reply(fd, "commands: hold release up down status");
		}
	}
	return 1;
}

/* ---- main loop --------------------------------------------------------- */

static void on_signal(int sig)
{
	stop = 1;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct pollfd pfd[MAX_CLIENTS + 1];
	char buf[32];
	double pos;
	int srv, i;

	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	find_iio("popup_hall", hall_dev, sizeof(hall_dev));
	load_table();
	pos = position();
	log_msg("start: position %s", fmt_repr(pos, buf, sizeof(buf)));
	if (isnan(pos) || pos > 0.8)
		retract("start-up");
	srv = serve();
	while (!stop) {
		int n;

		pfd[0] = (struct pollfd){ .fd = srv, .events = POLLIN };
		for (i = 0; i < nclients; i++)
			pfd[i + 1] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };
		n = poll(pfd, nclients + 1, up ? 20 : 200);
		if (n > 0) {
			/* clients first, by fd (the array shifts as they drop) */
			for (i = nclients - 1; i >= 0; i--)
				if (pfd[i + 1].revents && pfd[i + 1].fd == clients[i].fd)
					client(i);
			if (pfd[0].revents & POLLIN) {
				int fd = accept4(srv, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

				if (fd >= 0 && nclients < MAX_CLIENTS)
					clients[nclients++] = (typeof(clients[0])){ .fd = fd };
				else if (fd >= 0)
					close(fd);
			}
		}
		update();
		check_up();
	}
	if (up || ((pos = position()), !isnan(pos) && pos > 0.8))
		retract("shutdown");
	unlink(SOCK);
	return 0;
}
