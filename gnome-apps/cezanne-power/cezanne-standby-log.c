// SPDX-License-Identifier: MIT
/*
 * Battery use while the phone is locked, for the cezanne-power app.
 *
 * Each lock of the seat0 session (logind LockedHint) starts a period, the
 * unlock ends it. A period records the gauge's charge counter (the MT6359
 * coulomb counter, which counts through suspend), real time, time awake
 * (CLOCK_MONOTONIC stops in suspend, CLOCK_BOOTTIME does not) and the number
 * of suspends. The gauge driver updates the counter every 10 s in freezable
 * work, so both ends wait for a fresh value. Periods appear in
 * $STATE_DIRECTORY/standby.json (world-readable), newest last, in the
 * format of Python's json.dump (the app reads it with json.load).
 */
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <gio/gio.h>

#define BATTERY		"/sys/class/power_supply/battery"
#define SUSPENDS	"/sys/power/suspend_stats/success"
#define LOGIN1		"org.freedesktop.login1"
#define SESSION_IFACE	"org.freedesktop.login1.Session"
#define PROPS_IFACE	"org.freedesktop.DBus.Properties"
#define FRESH_WAIT_S	12	/* the counter changes at least every 10 s awake */
#define MIN_PERIOD_S	120
#define KEEP		200

struct clocks {
	double time, boot, mono;
	long long suspends;
};

struct period {
	gboolean pending, plugged;
	long long counter;
	struct clocks c;
};

/* a fresh-counter wait: calls then(counter or NULL, clocks) once */
struct fresh {
	long long last;
	gboolean have_last;
	double deadline;
	struct period *period;
	void (*then)(struct period *p, const long long *counter, const struct clocks *c);
};

static GDBusConnection *bus;
static struct period *cur;		/* open period */
static guint waiting;			/* GLib source of a pending fresh wait */
static char *log_path;

static double clock_s(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* the file's stripped contents, or NULL */
static char *read_str(const char *path)
{
	char *s;

	if (!g_file_get_contents(path, &s, NULL, NULL))
		return NULL;
	return g_strstrip(s);
}

static gboolean read_int(const char *path, long long *v)
{
	char *s = read_str(path), *end;
	gboolean ok = FALSE;

	if (s && *s) {
		errno = 0;
		*v = strtoll(s, &end, 10);
		ok = !errno && !*end;
	}
	g_free(s);
	return ok;
}

static gboolean str_is(const char *path, const char *a, const char *b)
{
	char *s = read_str(path);
	gboolean r = !g_strcmp0(s ? s : "", a) || (b && !g_strcmp0(s ? s : "", b));

	g_free(s);
	return r;
}

/* a charger or cable present (any USB / AC supply online) */
static gboolean plugged(void)
{
	const char *ps = "/sys/class/power_supply";
	DIR *d = opendir(ps);
	struct dirent *e;
	gboolean on = FALSE;

	while (d && !on && (e = readdir(d))) {
		char *type, *online;

		if (e->d_name[0] == '.')
			continue;
		type = g_strdup_printf("%s/%s/type", ps, e->d_name);
		online = g_strdup_printf("%s/%s/online", ps, e->d_name);
		on = str_is(type, "USB", "Mains") && !str_is(online, "", "0");
		g_free(type);
		g_free(online);
	}
	if (d)
		closedir(d);
	return on || !str_is(BATTERY "/status", "Discharging", "Not charging");
}

static void clocks(struct clocks *c)
{
	c->time = clock_s(CLOCK_REALTIME);
	c->boot = clock_s(CLOCK_BOOTTIME);
	c->mono = clock_s(CLOCK_MONOTONIC);
	if (!read_int(SUSPENDS, &c->suspends))
		c->suspends = 0;
}

static GVariant *call(const char *path, const char *iface, const char *method,
		      GVariant *args, const char *reply)
{
	return g_dbus_connection_call_sync(bus, LOGIN1, path, iface, method, args,
					   reply ? G_VARIANT_TYPE(reply) : NULL,
					   G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
}

static char *active_session(void)
{
	GVariant *v = call("/org/freedesktop/login1/seat/seat0", PROPS_IFACE, "Get",
			   g_variant_new("(ss)", "org.freedesktop.login1.Seat",
					 "ActiveSession"), "(v)");
	GVariant *inner;
	char *path = NULL;

	if (!v)
		return NULL;
	g_variant_get(v, "(v)", &inner);
	if (g_variant_is_of_type(inner, G_VARIANT_TYPE("(so)")))
		g_variant_get(inner, "(so)", NULL, &path);
	g_variant_unref(inner);
	g_variant_unref(v);
	return path;
}

static gboolean session_locked(const char *path)
{
	GVariant *v = call(path, PROPS_IFACE, "Get",
			   g_variant_new("(ss)", SESSION_IFACE, "LockedHint"), "(v)");
	GVariant *inner;
	gboolean locked = FALSE;

	if (!v)
		return FALSE;
	g_variant_get(v, "(v)", &inner);
	if (g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN))
		locked = g_variant_get_boolean(inner);
	g_variant_unref(inner);
	g_variant_unref(v);
	return locked;
}

static void cancel_wait(void)
{
	if (waiting) {
		g_source_remove(waiting);
		waiting = 0;
	}
}

static gboolean poll_fresh(gpointer data)
{
	struct fresh *f = data;
	long long c;
	gboolean have = read_int(BATTERY "/charge_counter", &c);
	struct clocks now;

	if ((have != f->have_last || (have && c != f->last)) ||
	    clock_s(CLOCK_MONOTONIC) > f->deadline) {
		waiting = 0;
		clocks(&now);
		f->then(f->period, have ? &c : NULL, &now);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* a wait owns its period unless that is still the open one */
static void fresh_free(gpointer data)
{
	struct fresh *f = data;

	if (f->period != cur)
		g_free(f->period);
	g_free(f);
}

static void fresh(struct period *p,
		  void (*then)(struct period *, const long long *, const struct clocks *))
{
	struct fresh *f = g_new0(struct fresh, 1);

	cancel_wait();
	f->have_last = read_int(BATTERY "/charge_counter", &f->last);
	f->deadline = clock_s(CLOCK_MONOTONIC) + FRESH_WAIT_S;
	f->period = p;
	f->then = then;
	waiting = g_timeout_add_full(G_PRIORITY_DEFAULT, 500, poll_fresh, f, fresh_free);
}

/*
 * The log's top-level array elements as text (kept verbatim), or an empty
 * list if the file is missing or not a JSON array of objects.
 */
static GPtrArray *load_entries(void)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	char *s, *p, *start = NULL;
	int depth = 0;
	gboolean in_str = FALSE, ok = FALSE;

	if (!g_file_get_contents(log_path, &s, NULL, NULL))
		return a;
	p = s + strspn(s, " \t\r\n");
	if (*p++ != '[')
		goto out;
	for (; *p; p++) {
		if (in_str) {
			if (*p == '\\' && p[1])
				p++;
			else if (*p == '"')
				in_str = FALSE;
			continue;
		}
		if (*p == '"') {
			in_str = TRUE;
		} else if (*p == '{') {
			if (!depth++)
				start = p;
		} else if (*p == '}') {
			if (--depth < 0)
				break;
			if (!depth)
				g_ptr_array_add(a, g_strndup(start, p + 1 - start));
		} else if (!depth && *p == ']') {
			ok = !p[1 + strspn(p + 1, " \t\r\n")];
			break;
		} else if (!depth && !strchr(", \t\r\n", *p)) {
			break;	/* not an array of objects */
		}
	}
out:
	if (!ok)
		g_ptr_array_set_size(a, 0);
	g_free(s);
	return a;
}

static void append(char *entry)
{
	GPtrArray *log = load_entries();
	GString *out = g_string_new("[");
	char *tmp = g_strconcat(log_path, ".tmp", NULL);
	guint i, first;

	g_ptr_array_add(log, entry);
	first = log->len > KEEP ? log->len - KEEP : 0;
	for (i = first; i < log->len; i++)
		g_string_append_printf(out, "%s%s", i > first ? ", " : "",
				       (char *)log->pdata[i]);
	g_string_append_c(out, ']');
	if (g_file_set_contents(tmp, out->str, out->len, NULL)) {
		chmod(tmp, 0644);
		if (rename(tmp, log_path))
			g_warning("rename %s: %s", tmp, g_strerror(errno));
	} else {
		g_warning("cannot write %s", tmp);
	}
	g_string_free(out, TRUE);
	g_free(tmp);
	g_ptr_array_unref(log);
}

/* Python's round(): halves to even */
static long long round_even(double x)
{
	return (long long)nearbyint(x);
}

static void start(struct period *p, const long long *counter, const struct clocks *c)
{
	if (cur == p && counter) {
		p->c = *c;
		p->counter = *counter;
		p->pending = FALSE;
	}
}

static void stop(struct period *p, const long long *counter, const struct clocks *c)
{
	double seconds, awake;
	long long uah;
	char ma[32];

	if (!counter)
		return;
	seconds = c->time - p->c.time;
	if (seconds < MIN_PERIOD_S)
		return;
	uah = p->counter - *counter;
	awake = c->mono - p->c.mono;
	g_ascii_formatd(ma, sizeof(ma), "%.1f", nearbyint(uah * 3.6 / seconds * 10) / 10);
	append(g_strdup_printf("{\"start\": %lld, \"end\": %lld, \"seconds\": %lld, "
			       "\"asleep\": %lld, \"uah\": %lld, \"ma\": %s, "
			       "\"suspends\": %lld, \"plugged\": %s}",
			       round_even(p->c.time), round_even(c->time),
			       round_even(seconds),
			       round_even(MAX(0.0, c->boot - p->c.boot - awake)),
			       uah, ma, c->suspends - p->c.suspends,
			       p->plugged || plugged() || uah <= 0 ? "true" : "false"));
}

static void begin(void)
{
	/* the old open period: freed here, or by its cancelled start wait */
	if (cur && !waiting)
		g_free(cur);
	cur = g_new0(struct period, 1);
	cur->pending = TRUE;
	cur->plugged = plugged();
	fresh(cur, start);
}

static void end(void)
{
	struct period *p = cur;

	cur = NULL;
	if (!p)
		return;
	if (p->pending) {	/* unlocked within the start wait */
		if (waiting)
			cancel_wait();	/* frees p */
		else
			g_free(p);
		return;
	}
	fresh(p, stop);
}

static void on_props(GDBusConnection *conn, const char *sender, const char *path,
		     const char *iface, const char *signal, GVariant *params,
		     gpointer data)
{
	GVariant *changed, *locked;
	char *active;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv}as)")))
		return;
	g_variant_get(params, "(&s@a{sv}@as)", NULL, &changed, NULL);
	locked = g_variant_lookup_value(changed, "LockedHint", G_VARIANT_TYPE_BOOLEAN);
	g_variant_unref(changed);
	if (!locked)
		return;
	active = active_session();
	if (!g_strcmp0(path, active)) {
		if (g_variant_get_boolean(locked))
			begin();
		else
			end();
	}
	g_free(active);
	g_variant_unref(locked);
}

int main(void)
{
	const char *dir = g_getenv("STATE_DIRECTORY");
	GError *err = NULL;
	char *path;

	/*
	 * one malloc arena for GLib's threads: glibc gives each thread arena a
	 * whole transparent huge page (2 MiB each, most of this daemon's RSS)
	 */
	mallopt(M_ARENA_MAX, 1);
	if (!dir)
		dir = "/var/lib/cezanne-power";
	g_mkdir_with_parents(dir, 0755);
	log_path = g_build_filename(dir, "standby.json", NULL);

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		g_printerr("system bus: %s\n", err->message);
		return 1;
	}
	g_dbus_connection_signal_subscribe(bus, LOGIN1, PROPS_IFACE, "PropertiesChanged",
					   NULL, SESSION_IFACE, G_DBUS_SIGNAL_FLAGS_NONE,
					   on_props, NULL, NULL);
	path = active_session();
	if (path && session_locked(path))
		begin();
	g_free(path);
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
