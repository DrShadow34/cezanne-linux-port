// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sunlight boost for the cezanne panel (MIUI threashold_sunlight_brightness):
 * in bright ambient light switch the panel to its high brightness mode, above
 * what the backlight range reaches. Only while GNOME's automatic brightness is
 * on, like MIUI. Ambient light from iio-sensor-proxy (system bus), claimed
 * whenever the service appears (udev starts it, after us at boot).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>

#define HBM	"/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm"
#define ON_LUX	1000		/* MIUI threshold */
#define OFF_LUX	700		/* hysteresis */
#define HOLD	3.0		/* seconds the level must hold */
#define USER	"arch"

static gboolean hbm, want;
static double since;
static GDBusProxy *proxy;

static double mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* GNOME's 'Automatic Screen Brightness' of the logged-in user */
static gboolean auto_brightness_on(void)
{
	struct passwd *pw = getpwnam(USER);
	char env[96], out[16] = "";
	int fd[2], n = 0, status;
	struct pollfd pfd;
	double end = mono() + 3;
	pid_t pid;

	if (!pw || pipe2(fd, O_CLOEXEC))
		return FALSE;
	snprintf(env, sizeof(env), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%u/bus",
		 (unsigned)pw->pw_uid);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return FALSE;
	}
	if (!pid) {
		int null = open("/dev/null", O_WRONLY);

		dup2(fd[1], 1);
		if (null >= 0)
			dup2(null, 2);
		execlp("sudo", "sudo", "-u", USER, env, "gsettings", "get",
		       "org.gnome.settings-daemon.plugins.power", "ambient-enabled",
		       (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	pfd = (struct pollfd){ .fd = fd[0], .events = POLLIN };
	for (;;) {
		int left = (end - mono()) * 1000, r;

		if (left <= 0 || (r = poll(&pfd, 1, left)) == 0) {
			kill(pid, SIGKILL);
			n = -1;
			break;
		}
		if (r < 0) {
			if (errno == EINTR)
				continue;
			n = -1;
			break;
		}
		r = read(fd[0], out + n, sizeof(out) - 1 - n);
		if (r <= 0)
			break;
		n += r;
		if (n == sizeof(out) - 1)
			break;
	}
	close(fd[0]);
	waitpid(pid, &status, 0);
	if (n < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
		return FALSE;
	out[n] = 0;
	return !strcmp(g_strstrip(out), "true");
}

static void set_hbm(gboolean on)
{
	char *cur = NULL;
	FILE *f;

	/* never override the fingerprint spot (2) */
	if (!g_file_get_contents(HBM, &cur, NULL, NULL)) {
		printf("hbm: %s\n", g_strerror(errno));
		return;
	}
	if (!strcmp(g_strstrip(cur), "2")) {
		g_free(cur);
		return;
	}
	g_free(cur);
	f = fopen(HBM, "w");
	if (!f || fputs(on ? "1" : "0", f) < 0 || fclose(f)) {
		printf("hbm: %s\n", g_strerror(errno));
		return;
	}
	hbm = on;
	printf("sunlight boost %s\n", on ? "on" : "off");
}

static void on_light(double lux)
{
	gboolean w = hbm;

	if (lux >= ON_LUX)
		w = TRUE;
	else if (lux <= OFF_LUX)
		w = FALSE;
	if (w != want) {
		want = w;
		since = mono();
	}
	if (w != hbm && mono() - since >= HOLD && (!w || auto_brightness_on()))
		set_hbm(w);
}

static gboolean poll_light(gpointer data)
{
	GVariant *v;

	if (!proxy)
		return G_SOURCE_CONTINUE;
	v = g_dbus_proxy_get_cached_property(proxy, "LightLevel");

	if (v) {
		if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
			on_light(g_variant_get_double(v));
		g_variant_unref(v);
	}
	return G_SOURCE_CONTINUE;
}

static void appeared(GDBusConnection *bus, const char *name, const char *owner,
		     gpointer data)
{
	GError *err = NULL;
	GVariant *r = NULL;

	g_clear_object(&proxy);
	proxy = g_dbus_proxy_new_sync(bus, G_DBUS_PROXY_FLAGS_NONE, NULL, name,
				      "/net/hadess/SensorProxy", "net.hadess.SensorProxy",
				      NULL, &err);
	if (proxy)
		r = g_dbus_proxy_call_sync(proxy, "ClaimLight", NULL, G_DBUS_CALL_FLAGS_NONE,
					   -1, NULL, &err);
	if (!r) {
		fprintf(stderr, "sensor proxy: %s\n", err->message);
		g_error_free(err);
		g_clear_object(&proxy);
		return;
	}
	g_variant_unref(r);
}

static void vanished(GDBusConnection *bus, const char *name, gpointer data)
{
	g_clear_object(&proxy);
}

int main(void)
{
	/* one malloc arena: glibc backs each thread arena with a 2 MiB THP */
	mallopt(M_ARENA_MAX, 1);
	setvbuf(stdout, NULL, _IOLBF, 0);
	g_bus_watch_name(G_BUS_TYPE_SYSTEM, "net.hadess.SensorProxy", G_BUS_NAME_WATCHER_FLAGS_NONE,
			 appeared, vanished, NULL, NULL);
	g_timeout_add(500, poll_light, NULL);
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
