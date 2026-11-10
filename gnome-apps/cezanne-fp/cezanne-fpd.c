// SPDX-License-Identifier: GPL-2.0-only
/*
 * cezanne-fpd: fingerprint service for the cezanne (Redmi K30 Ultra). It
 * speaks fprintd's D-Bus API (net.reactivated.Fprint on the system bus), so
 * pam_fprintd, GDM's lock screen and GNOME Settings work unchanged, on the
 * Goodix GF3658 under-display sensor. The matching runs in the Goodix TA in
 * the Microtrust TEE (/dev/teei_fp; cezanne-tee.service boots the TEE and
 * serves its files and RPMB). Own code: the TA command sequences come from
 * traces of MIUI's HAL (mainline/fingerprint/ref, NOTES "Fingerprint").
 *
 * Verify: the spot shows a fingerprint icon (org.cezanne.Fod State 1 on the
 * system bus, drawn by the cezanne-fod Shell extension); a touch on it
 * (touchscreen evdev) switches to 2: the extension dims the screen, lights
 * the sensor disc and puts the panel in FOD HBM, which is the "UI ready" we
 * wait for (panel hbm = 2); then capture and match as MIUI's HAL does.
 *
 * The fingers enrolled under MIUI live in the TA's storage; their ids get
 * fprintd finger names in /var/lib/cezanne-fpd/prints.json. Enrollment and
 * deleting prints are not implemented yet.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#define TEE_DEV		"/dev/teei_fp"
#define FP_DEV		"/dev/goodix_fp"
#define HBM		"/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm"

#define CMD_FP_CMD		0x5402		/* _IO('T', 2) */
#define GF_IOC_INIT		0x80016700	/* _IOR('g', 0, u8) */
#define GF_IOC_RESET		0x6702
#define GF_IOC_ENABLE_IRQ	0x6703
#define GF_IOC_ENABLE_SPI_CLK	0x40046705	/* _IOW('g', 5, u32) */
#define GF_IOC_DISABLE_SPI_CLK	0x6706
#define GF_IOC_ENABLE_POWER	0x6707
#define GF_BUF_LEN		0x96000

/* the spot, panel pixels = touchscreen units (MIUI's box (439,1732) 202 px) */
#define FOD_X	540
#define FOD_Y	1833
#define FOD_R	101

/* TA results */
#define GF_OK		0
#define GF_NOT_MATCH	1008

static const char *const finger_names[] = {
	"right-thumb", "left-thumb", "right-index-finger", "left-index-finger",
	"right-middle-finger", "left-middle-finger", "right-ring-finger",
	"left-ring-finger", "right-little-finger", "left-little-finger", NULL,
};

#define BUS_NAME	"net.reactivated.Fprint"
#define MANAGER_PATH	"/net/reactivated/Fprint/Manager"
#define DEVICE_PATH	"/net/reactivated/Fprint/Device/0"
#define FOD_PATH	"/org/cezanne/Fod"
#define ERR		"net.reactivated.Fprint.Error."
#define DEVICE_IFACE	"net.reactivated.Fprint.Device"

static const char xml[] =
	"<node>"
	"<interface name='net.reactivated.Fprint.Manager'>"
	"  <method name='GetDevices'><arg type='ao' name='devices' direction='out'/></method>"
	"  <method name='GetDefaultDevice'><arg type='o' name='device' direction='out'/></method>"
	"</interface>"
	"<interface name='net.reactivated.Fprint.Device'>"
	"  <method name='ListEnrolledFingers'><arg type='s' name='username' direction='in'/>"
	"    <arg type='as' name='enrolled_fingers' direction='out'/></method>"
	"  <method name='DeleteEnrolledFingers'><arg type='s' name='username' direction='in'/></method>"
	"  <method name='DeleteEnrolledFingers2'/>"
	"  <method name='DeleteEnrolledFinger'><arg type='s' name='finger_name' direction='in'/></method>"
	"  <method name='Claim'><arg type='s' name='username' direction='in'/></method>"
	"  <method name='Release'/>"
	"  <method name='VerifyStart'><arg type='s' name='finger_name' direction='in'/></method>"
	"  <method name='VerifyStop'/>"
	"  <signal name='VerifyFingerSelected'><arg type='s' name='finger_name'/></signal>"
	"  <signal name='VerifyStatus'><arg type='s' name='result'/><arg type='b' name='done'/></signal>"
	"  <method name='EnrollStart'><arg type='s' name='finger_name' direction='in'/></method>"
	"  <method name='EnrollStop'/>"
	"  <signal name='EnrollStatus'><arg type='s' name='result'/><arg type='b' name='done'/></signal>"
	"  <property name='name' type='s' access='read'/>"
	"  <property name='num-enroll-stages' type='i' access='read'/>"
	"  <property name='scan-type' type='s' access='read'/>"
	"  <property name='finger-present' type='b' access='read'/>"
	"  <property name='finger-needed' type='b' access='read'/>"
	"</interface>"
	"<interface name='org.cezanne.Fod'>"
	"  <property name='State' type='u' access='read'/>"
	"</interface>"
	"</node>";

static GMutex log_lock;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	g_mutex_lock(&log_lock);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	g_mutex_unlock(&log_lock);
}

static double mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void put_u32(unsigned char *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t get_u32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* OSError-like failures: errno-style messages in a GError */
static gboolean os_error(GError **err, const char *fmt, ...)
{
	va_list ap;
	char *msg;

	va_start(ap, fmt);
	msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_FAILED, msg);
	g_free(msg);
	return FALSE;
}

static gboolean ioctl_ok(int fd, unsigned long req, void *arg, GError **err)
{
	if (ioctl(fd, req, arg) < 0)
		return os_error(err, "[Errno %d] %s", errno, g_strerror(errno));
	return TRUE;
}

/* ---- the Goodix TA ----------------------------------------------------- */

/*
 * the Goodix TA: u32 status, u32, u32, u32 len, payload; every payload
 * starts with u8 reset flag, pad, u32 module, u32 command, i32 result
 */
static struct {
	int dev, tee;
	unsigned char *buf;
} ta = { -1, -1, NULL };

struct field {
	size_t off;
	uint32_t v;
};

/* the payload in buf + 16 out to the TA, its reply back in place */
static gboolean ta_raw(const unsigned char *payload, size_t length, GError **err)
{
	uint32_t one = 1;
	gboolean ok;

	memset(ta.buf, 0, 16 + length);
	put_u32(ta.buf + 12, length);
	memcpy(ta.buf + 16, payload, length);
	/* like MIUI's HAL: SPI clock on around each command */
	if (!ioctl_ok(ta.dev, GF_IOC_ENABLE_SPI_CLK, &one, err))
		return FALSE;
	ok = ioctl_ok(ta.tee, CMD_FP_CMD, ta.buf, err);
	if (!ioctl_ok(ta.dev, GF_IOC_DISABLE_SPI_CLK, NULL, ok ? err : NULL))
		return FALSE;
	return ok;
}

/*
 * send module/sub with a payload of length bytes, the given (offset, u32)
 * fields set; the code in *code, the reply payload at ta.buf + 16
 */
static gboolean ta_cmd(uint32_t mod, uint32_t sub, size_t length, const struct field *f,
		       int nf, int32_t *code, GError **err)
{
	unsigned char *p = g_malloc0(length);
	int32_t status;
	int i;

	put_u32(p + 4, mod);
	put_u32(p + 8, sub);
	for (i = 0; i < nf; i++)
		put_u32(p + f[i].off, f[i].v);
	if (!ta_raw(p, length, err)) {
		g_free(p);
		return FALSE;
	}
	g_free(p);
	status = (int32_t)get_u32(ta.buf);
	*code = status ? status : (int32_t)get_u32(ta.buf + 16 + 12);
	return TRUE;
}

#define TA(mod, sub, len, code, err, ...)					\
	ta_cmd(mod, sub, len, (const struct field[]){ { 0, 0 }, ##__VA_ARGS__ } + 1,	\
	       sizeof((const struct field[]){ { 0, 0 }, ##__VA_ARGS__ }) /	\
	       sizeof(struct field) - 1, code, err)

static const unsigned char *ta_reply(void)
{
	return ta.buf + 16;
}

static gboolean ta_open(GError **err)
{
	ta.dev = open(FP_DEV, O_RDWR | O_CLOEXEC);
	if (ta.dev < 0)
		return os_error(err, "[Errno %d] %s: '%s'", errno, g_strerror(errno), FP_DEV);
	ta.tee = open(TEE_DEV, O_RDWR | O_CLOEXEC);
	if (ta.tee < 0) {
		os_error(err, "[Errno %d] %s: '%s'", errno, g_strerror(errno), TEE_DEV);
		close(ta.dev);
		ta.dev = -1;
		return FALSE;
	}
	if (!ta.buf)
		ta.buf = g_malloc0(GF_BUF_LEN);
	return TRUE;
}

static void ta_close(void)
{
	if (ta.dev >= 0)
		close(ta.dev);
	if (ta.tee >= 0)
		close(ta.tee);
	ta.dev = ta.tee = -1;
}

static gboolean ta_enumerate(GArray *ids, GError **err)
{
	int32_t code;
	uint32_t n, i;

	if (!TA(1000, 7, 0x70, &code, err))
		return FALSE;
	if (code)
		return os_error(err, "enumerate: %d", code);
	n = get_u32(ta_reply() + 0x1c);
	n = n < 5 ? n : 5;
	g_array_set_size(ids, 0);
	for (i = 0; i < n; i++) {
		uint32_t id = get_u32(ta_reply() + 0x48 + 4 * i);

		g_array_append_val(ids, id);
	}
	return TRUE;
}

/* MIUI's mfp-daemon at start; the enrolled finger ids into ids */
static gboolean ta_init(GArray *ids, GError **err)
{
	static const struct {
		uint32_t mod, sub, n;
	} first[] = { { 1012, 0, 0x1c }, { 1002, 9, 0x20 }, { 1002, 0, 0x1c }, { 1002, 1, 0x2c } };
	unsigned char init = 0, group[0x120] = { 0 };
	int32_t code;
	size_t i;

	if (!ioctl_ok(ta.dev, GF_IOC_ENABLE_POWER, NULL, err) ||
	    !ioctl_ok(ta.dev, GF_IOC_RESET, NULL, err))
		return FALSE;
	for (i = 0; i < sizeof(first) / sizeof(*first); i++) {
		if (!ta_cmd(first[i].mod, first[i].sub, first[i].n, NULL, 0, &code, err))
			return FALSE;
		if (code)
			return os_error(err, "TA %u/%u: %d", first[i].mod, first[i].sub, code);
	}
	if (!ioctl_ok(ta.dev, GF_IOC_INIT, &init, err) ||
	    !ioctl_ok(ta.dev, GF_IOC_ENABLE_IRQ, NULL, err))
		return FALSE;
	if (!TA(1013, 0, 0x24, &code, err))	/* DSP init: 1001 on MIUI too */
		return FALSE;
	/* sensor init: Xiaomi's product id ("cezanne" = 4), panel id 8 */
	if (!TA(1002, 2, 0x1c0, &code, err, { 0x1b8, 4 }, { 0x1bc, 8 }))
		return FALSE;
	if (code)
		return os_error(err, "sensor init: %d", code);
	if (!TA(1001, 0, 0x230, &code, err, { 0x28, 1 }))
		return FALSE;
	if (code)
		return os_error(err, "algo init: %d", code);
	if (!TA(1002, 8, 0x1c, &code, err) ||		/* sleep */
	    !TA(1000, 13, 0x1c, &code, err))		/* init finish */
		return FALSE;
	put_u32(group + 4, 1000);
	put_u32(group + 8, 0);
	memcpy(group + 0x20, "/data/vendor/goodix/fpdata", 26);
	if (!ta_raw(group, sizeof(group), err))		/* setActiveGroup, group 0 */
		return FALSE;
	return ta_enumerate(ids, err);
}

static gboolean ta_verify_begin(GError **err)
{
	int32_t code;

	return TA(1000, 1, 0x28, &code, err) &&			/* authenticate */
	       TA(1000, 11, 0x24, &code, err, { 0x20, 1 });	/* screen on */
}

/* one touch: up to three captures; the code and finger id */
static gboolean ta_capture_match(int x, int y, int32_t *code_out, uint32_t *fid_out,
				 GError **err)
{
	int32_t code = -1, c;
	uint32_t fid = 0;
	int i;

	if (!TA(1002, 9, 0x20, &c, err) ||			/* wakeup */
	    !TA(1001, 20, 0x30, &c, err))
		return FALSE;
	for (i = 0; i < 3; i++) {
		if (i && !TA(1001, 4, 0x378, &c, err))
			return FALSE;
		if (!TA(1002, 3, 0x48, &c, err, { 0x20, 2 }, { 0x28, 1 }, { 0x3c, 1 }) ||
		    /* the TA reads the image here (~110 ms) */
		    !TA(1002, 101, 0x50, &c, err, { 0x20, 1 }, { 0x2c, x }, { 0x30, y },
			{ 0x34, FOD_X }, { 0x38, FOD_Y }, { 0x3c, 0x43844000 }, { 0x40, FOD_R }) ||
		    !TA(1001, 2, 0xe8, &code, err, { 0x24, 1 }, { 0x58, 2 }, { 0x88, 1 },
			{ 0x90, 0x00010001 }, { 0x94, 0x14 }))
			return FALSE;
		if (code == GF_OK) {
			fid = get_u32(ta_reply() + 0x30);
			break;
		}
		if (code == GF_NOT_MATCH)
			break;
	}
	if (!TA(1001, 4, 0x378, &c, err) ||
	    !TA(1001, 20, 0x30, &c, err, { 0x20, 1 }, { 0x28, 1 }) ||
	    !TA(1002, 8, 0x1c, &c, err))			/* sleep */
		return FALSE;
	if (code == GF_OK && !TA(1001, 21, 0x20, &c, err, { 0x1c, fid }))
		return FALSE;
	if (!TA(1001, 24, 0x1c, &c, err))
		return FALSE;
	*code_out = code;
	*fid_out = fid;
	return TRUE;
}

static gboolean ta_verify_end(GError **err)
{
	int32_t code;

	return TA(1000, 11, 0x24, &code, err, { 0x1c, 1 });
}

/* ---- the touchscreen --------------------------------------------------- */

static char *touch_path;	/* finger down on the spot (shared, read only) */

static void touch_find(void)
{
	GDir *d = g_dir_open("/sys/class/input", 0, NULL);
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *n;
	guint i;

	while (d && (n = g_dir_read_name(d)))
		g_ptr_array_add(names, g_strdup(n));
	if (d)
		g_dir_close(d);
	g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
	for (i = 0; i < names->len && !touch_path; i++) {
		char *p = g_strdup_printf("/sys/class/input/%s/device/name",
					  (char *)names->pdata[i]), *name;

		if (g_file_get_contents(p, &name, NULL, NULL)) {
			if (g_str_has_prefix(names->pdata[i], "event") && strstr(name, "Goodix"))
				touch_path = g_strdup_printf("/dev/input/%s", (char *)names->pdata[i]);
			g_free(name);
		}
		g_free(p);
	}
	g_ptr_array_unref(names);
}

struct input_ev {
	int64_t sec, usec;
	uint16_t type, code;
	int32_t value;
};

/*
 * a new touch whose first report lies on the spot (1.5 radii); FALSE when
 * stopped (or on an error, in err)
 */
static gboolean touch_wait_down(volatile gint *stop, int *xo, int *yo, GError **err)
{
	int fd, x = -1, y = -1;
	gboolean new = FALSE;

	fd = open(touch_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return os_error(err, "[Errno %d] %s: '%s'", errno, g_strerror(errno), touch_path);
	while (!g_atomic_int_get(stop)) {
		struct pollfd pfd = { fd, POLLIN, 0 };
		struct input_ev ev[64];
		ssize_t n;
		int i;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			os_error(err, "[Errno %d] %s", errno, g_strerror(errno));
			close(fd);
			return FALSE;
		}
		for (i = 0; i < n / (ssize_t)sizeof(*ev); i++) {
			if (ev[i].type == 1 && ev[i].code == 0x14a) {		/* BTN_TOUCH */
				new = ev[i].value == 1;
			} else if (ev[i].type == 3 && ev[i].code == 0x35) {	/* ABS_MT_POSITION_X */
				x = ev[i].value;
			} else if (ev[i].type == 3 && ev[i].code == 0x36) {
				y = ev[i].value;
			} else if (ev[i].type == 0 && new) {			/* SYN_REPORT */
				double dx = x - FOD_X, dy = y - FOD_Y;

				new = FALSE;
				if (dx * dx + dy * dy <= (1.5 * FOD_R) * (1.5 * FOD_R)) {
					*xo = x;
					*yo = y;
					close(fd);
					return TRUE;
				}
			}
		}
	}
	close(fd);
	return FALSE;
}

static char *hbm(void)
{
	char *s;

	if (!g_file_get_contents(HBM, &s, NULL, NULL))
		return g_strdup("");
	return g_strstrip(s);
}

/* ---- prints: fprintd finger names <-> TA finger ids, per user ---------- */

static JsonObject *users;	/* user -> { finger name -> id } */
static char *prints_path, *state_dir;

/* the desktop's user: GDM's automatic login, else the first one */
static char *default_user(void)
{
	char *conf, **lines, *best = NULL;
	uid_t best_uid = 0;
	struct passwd *pw;
	int i;

	if (g_file_get_contents("/etc/gdm/custom.conf", &conf, NULL, NULL)) {
		lines = g_strsplit(conf, "\n", -1);
		g_free(conf);
		for (i = 0; lines[i]; i++) {
			char *line = g_strstrip(lines[i]), *eq = strchr(line, '=');

			if (eq && eq - line == 14 && !strncmp(line, "AutomaticLogin", 14) && eq[1]) {
				char *u = g_strdup(eq + 1);

				g_strfreev(lines);
				return u;
			}
		}
		g_strfreev(lines);
	}
	setpwent();
	while ((pw = getpwent()))
		if (pw->pw_uid >= 1000 && pw->pw_uid < 60000 && g_str_has_prefix(pw->pw_dir, "/home/") &&
		    (!best || pw->pw_uid < best_uid ||
		     (pw->pw_uid == best_uid && strcmp(pw->pw_name, best) < 0))) {
			g_free(best);
			best = g_strdup(pw->pw_name);
			best_uid = pw->pw_uid;
		}
	endpwent();
	return best ? best : g_strdup("root");
}

/* a JSON string as Python's json.dumps (ensure_ascii) writes it */
static void json_str(GString *out, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	g_string_append_c(out, '"');
	while (*p) {
		gunichar c = g_utf8_get_char_validated((const char *)p, -1);

		if (c == (gunichar)-1 || c == (gunichar)-2) {
			c = *p;
			p++;
		} else {
			p = (const unsigned char *)g_utf8_next_char(p);
		}
		if (c == '"' || c == '\\')
			g_string_append_printf(out, "\\%c", c);
		else if (c == '\n')
			g_string_append(out, "\\n");
		else if (c == '\r')
			g_string_append(out, "\\r");
		else if (c == '\t')
			g_string_append(out, "\\t");
		else if (c == '\b')
			g_string_append(out, "\\b");
		else if (c == '\f')
			g_string_append(out, "\\f");
		else if (c < 0x20 || c > 0x7e) {
			if (c > 0xffff) {
				c -= 0x10000;
				g_string_append_printf(out, "\\u%04x\\u%04x", 0xd800 + (c >> 10),
						       0xdc00 + (c & 0x3ff));
			} else {
				g_string_append_printf(out, "\\u%04x", c);
			}
		} else {
			g_string_append_c(out, c);
		}
	}
	g_string_append_c(out, '"');
}

/* json.dump(users, f, indent=1) */
static void prints_save(void)
{
	GString *out = g_string_new("{");
	GList *ul = json_object_get_members(users), *u;
	char *tmp = g_strconcat(prints_path, ".new", NULL);

	for (u = ul; u; u = u->next) {
		JsonObject *f = json_object_get_object_member(users, u->data);
		GList *fl = f ? json_object_get_members(f) : NULL, *k;

		g_string_append(out, u == ul ? "\n " : ",\n ");
		json_str(out, u->data);
		g_string_append(out, ": {");
		for (k = fl; k; k = k->next) {
			g_string_append(out, k == fl ? "\n  " : ",\n  ");
			json_str(out, k->data);
			g_string_append_printf(out, ": %" G_GINT64_FORMAT,
					       json_object_get_int_member(f, k->data));
		}
		g_string_append(out, fl ? "\n }" : "}");
		g_list_free(fl);
	}
	g_string_append(out, ul ? "\n}" : "}");
	g_list_free(ul);
	g_mkdir_with_parents(state_dir, 0777);
	if (!g_file_set_contents(tmp, out->str, out->len, NULL) || rename(tmp, prints_path))
		log_msg("prints: cannot write %s", prints_path);
	g_free(tmp);
	g_string_free(out, TRUE);
}

static gboolean id_in(GArray *ids, gint64 v)
{
	guint i;

	for (i = 0; i < ids->len; i++)
		if (g_array_index(ids, uint32_t, i) == v)
			return TRUE;
	return FALSE;
}

static void prints_load(GArray *ta_ids)
{
	JsonParser *p = json_parser_new();
	JsonNode *root;
	GList *ul, *u;
	GArray *new = g_array_new(FALSE, FALSE, sizeof(uint32_t));
	guint i;

	users = NULL;
	if (json_parser_load_from_file(p, prints_path, NULL) && (root = json_parser_get_root(p)) &&
	    JSON_NODE_HOLDS_OBJECT(root))
		users = json_object_ref(json_node_get_object(root));
	g_object_unref(p);
	if (!users)
		users = json_object_new();
	/* the ids already named */
	for (i = 0; i < ta_ids->len; i++) {
		uint32_t id = g_array_index(ta_ids, uint32_t, i);
		gboolean known = FALSE;

		ul = json_object_get_members(users);
		for (u = ul; u && !known; u = u->next) {
			JsonObject *f = json_object_get_object_member(users, u->data);
			GList *fl = f ? json_object_get_members(f) : NULL, *k;

			for (k = fl; k && !known; k = k->next)
				known = json_object_get_int_member(f, k->data) == id;
			g_list_free(fl);
		}
		g_list_free(ul);
		if (!known)
			g_array_append_val(new, id);
	}
	if (new->len) {
		/* fingers enrolled under MIUI: the phone's (first) user's */
		char *user = default_user();
		JsonObject *f;
		GString *msg = g_string_new("");
		GList *fl, *k;
		int n = 0;

		if (!json_object_has_member(users, user))
			json_object_set_object_member(users, user, json_object_new());
		f = json_object_get_object_member(users, user);
		for (i = 0; i < new->len; i++) {
			while (finger_names[n] && json_object_has_member(f, finger_names[n]))
				n++;
			if (!finger_names[n])
				break;
			json_object_set_int_member(f, finger_names[n], g_array_index(new, uint32_t, i));
		}
		prints_save();
		fl = json_object_get_members(f);
		for (k = fl; k; k = k->next)
			g_string_append_printf(msg, "%s%s=0x%" G_GINT64_MODIFIER "x",
					       k == fl ? "" : ", ", (char *)k->data,
					       json_object_get_int_member(f, k->data));
		g_list_free(fl);
		log_msg("prints: %s gets %s", user, msg->str);
		g_string_free(msg, TRUE);
		g_free(user);
	}
	g_array_unref(new);
	/* forget ids the TA no longer has */
	ul = json_object_get_members(users);
	for (u = ul; u; u = u->next) {
		JsonObject *f = json_object_get_object_member(users, u->data);
		GList *fl = f ? json_object_get_members(f) : NULL, *k;

		for (k = fl; k; k = k->next)
			if (!id_in(ta_ids, json_object_get_int_member(f, k->data)))
				json_object_remove_member(f, k->data);
		g_list_free(fl);
	}
	g_list_free(ul);
}

/* the user's finger names, sorted (empty: none) */
static GPtrArray *fingers_of(const char *user)
{
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	JsonObject *f = json_object_has_member(users, user) ?
			json_object_get_object_member(users, user) : NULL;
	GList *fl = f ? json_object_get_members(f) : NULL, *k;

	for (k = fl; k; k = k->next)
		g_ptr_array_add(names, g_strdup(k->data));
	g_list_free(fl);
	g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
	return names;
}

static gint64 finger_id(const char *user, const char *name)
{
	JsonObject *f = json_object_get_object_member(users, user);

	return json_object_get_int_member(f, name);
}

/* ---- the service ------------------------------------------------------- */

static struct {
	GDBusConnection *conn;
	char *claim_sender, *claim_user;
	guint claim_watch;
	gint *stop;		/* the running verification's stop flag */
	gboolean finger_present, finger_needed;
	guint32 fod;
	GMutex ta_lock;
} svc;

struct verify {
	GArray *ids;
	gint *stop;
};

static void emit(const char *path, const char *iface, const char *name, GVariant *args)
{
	g_dbus_connection_emit_signal(svc.conn, NULL, path, iface, name, args, NULL);
}

static void set_prop(const char *name, gboolean value)
{
	gboolean *p = !strcmp(name, "finger-present") ? &svc.finger_present : &svc.finger_needed;
	GVariantBuilder b;

	if (*p == value)
		return;
	*p = value;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", name, g_variant_new_boolean(value));
	emit(DEVICE_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
	     g_variant_new("(sa{sv}as)", DEVICE_IFACE, &b, NULL));
}

static void set_fod(guint32 state)
{
	GVariantBuilder b;

	if (svc.fod == state)
		return;
	svc.fod = state;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "State", g_variant_new_uint32(state));
	emit(FOD_PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
	     g_variant_new("(sa{sv}as)", "org.cezanne.Fod", &b, NULL));
}

static GVariant *get_prop(GDBusConnection *conn, const char *sender, const char *path,
			  const char *iface, const char *prop, GError **err, gpointer data)
{
	if (!strcmp(iface, "org.cezanne.Fod"))
		return g_variant_new_uint32(svc.fod);
	if (!strcmp(prop, "name"))
		return g_variant_new_string("Goodix GF3658 (under display)");
	if (!strcmp(prop, "num-enroll-stages"))
		return g_variant_new_int32(30);
	if (!strcmp(prop, "scan-type"))
		return g_variant_new_string("press");
	if (!strcmp(prop, "finger-present"))
		return g_variant_new_boolean(svc.finger_present);
	if (!strcmp(prop, "finger-needed"))
		return g_variant_new_boolean(svc.finger_needed);
	g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "%s", prop);
	return NULL;
}

/* idle callbacks from the worker: UI state and status signals */
struct ui {
	int fod;		/* -1: unchanged */
	int needed, present;	/* -1: unchanged */
	char *status;
	gboolean done;
};

static gboolean ui_apply(gpointer data)
{
	struct ui *u = data;

	if (u->status) {
		emit(DEVICE_PATH, DEVICE_IFACE, "VerifyStatus",
		     g_variant_new("(sb)", u->status, u->done));
	} else {
		if (u->fod >= 0)
			set_fod(u->fod);
		if (u->needed >= 0)
			set_prop("finger-needed", u->needed);
		if (u->present >= 0)
			set_prop("finger-present", u->present);
	}
	g_free(u->status);
	g_free(u);
	return G_SOURCE_REMOVE;
}

/* the props in call order, as Python's ui(fod=..., finger_needed=..., ...) */
static void ui(int fod, int needed, int present)
{
	struct ui *u = g_new0(struct ui, 1);

	u->fod = fod;
	u->needed = needed;
	u->present = present;
	g_idle_add(ui_apply, u);
}

static void status(const char *result, gboolean done)
{
	struct ui *u = g_new0(struct ui, 1);

	log_msg("verify: %s%s", result, done ? " (done)" : "");
	u->status = g_strdup(result);
	u->done = done;
	g_idle_add(ui_apply, u);
}

static gpointer verify_thread(gpointer data)
{
	struct verify *v = data;
	GError *err = NULL;
	int x, y;

	g_mutex_lock(&svc.ta_lock);
	ui(1, TRUE, -1);
	if (!ta_verify_begin(&err))
		goto error;
	while (!g_atomic_int_get(v->stop)) {
		int32_t code;
		uint32_t fid;
		double t0;
		char *h;

		if (!touch_path)
			break;		/* (Python: an uncaught TypeError ends it) */
		if (!touch_wait_down(v->stop, &x, &y, &err)) {
			if (err)
				goto error;
			break;
		}
		ui(2, -1, TRUE);
		/*
		 * the extension lights the spot, then puts the panel in FOD
		 * HBM: our "UI ready" (MIUI captures ~200 ms after the touch)
		 */
		t0 = mono();
		while ((h = hbm()) && strcmp(h, "2") && mono() - t0 < 0.8) {
			g_free(h);
			g_usleep(10000);
		}
		g_free(h);
		h = hbm();
		if (strcmp(h, "2")) {
			FILE *f;

			log_msg("no FOD HBM from the shell, setting it");
			f = fopen(HBM, "we");
			if (!f || fputs("2", f) < 0 || fclose(f)) {
				os_error(&err, "[Errno %d] %s: '%s'", errno, g_strerror(errno), HBM);
				g_free(h);
				goto error;
			}
		}
		g_free(h);
		if (0.2 - (mono() - t0) > 0)
			g_usleep((0.2 - (mono() - t0)) * 1e6);
		if (!ta_capture_match(x, y, &code, &fid, &err))
			goto error;
		ui(1, -1, FALSE);
		log_msg("touch at %d,%d: TA %d, finger 0x%x", x, y, code, fid);
		if (code == GF_OK && id_in(v->ids, fid)) {
			status("verify-match", TRUE);
			break;
		}
		if (code == GF_OK || code == GF_NOT_MATCH) {
			status("verify-no-match", TRUE);
			break;
		}
		/* not live / bad image: try again with the next touch */
		status("verify-retry-scan", FALSE);
	}
	goto out;
error:
	log_msg("verify: %s", err->message);
	g_clear_error(&err);
	status("verify-unknown-error", TRUE);
out:
	ta_verify_end(NULL);
	ui(0, FALSE, FALSE);
	g_atomic_int_set(v->stop, 1);
	g_mutex_unlock(&svc.ta_lock);
	g_array_unref(v->ids);
	g_free(v);
	return NULL;
}

static void stop_verify(void)
{
	if (svc.stop)
		g_atomic_int_set(svc.stop, 1);
}

static void release(void)
{
	stop_verify();
	if (svc.claim_watch) {
		g_bus_unwatch_name(svc.claim_watch);
		svc.claim_watch = 0;
	}
	if (svc.claim_sender)
		log_msg("released by %s", svc.claim_sender);
	g_clear_pointer(&svc.claim_sender, g_free);
	g_clear_pointer(&svc.claim_user, g_free);
}

static void claim_vanished(GDBusConnection *conn, const char *name, gpointer data)
{
	release();
}

/* the caller's uid; FALSE (error returned) on failure */
static gboolean caller_uid(const char *sender, guint32 *uid, GError **err)
{
	GVariant *r = g_dbus_connection_call_sync(svc.conn, "org.freedesktop.DBus",
						  "/org/freedesktop/DBus", "org.freedesktop.DBus",
						  "GetConnectionUnixUser", g_variant_new("(s)", sender),
						  G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
						  NULL, err);

	if (!r)
		return FALSE;
	g_variant_get(r, "(u)", uid);
	g_variant_unref(r);
	return TRUE;
}

struct dbus_err {
	const char *name;
	char *msg;
	gboolean unexpected;	/* Python: not a DBusError (logged) */
};

static gboolean fail(struct dbus_err *e, const char *name, const char *fmt, ...);

/* an error outside the fprintd API's own (Python: any other exception) */
#define fail_unexpected(e, ...)	((e)->unexpected = TRUE, fail(e, "Internal", __VA_ARGS__))

static gboolean fail(struct dbus_err *e, const char *name, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	e->name = name;
	e->msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	return FALSE;
}

/* the user a request is about (allocated in *out) */
static gboolean user_for(const char *sender, const char *username, char **out,
			 struct dbus_err *e)
{
	GError *err = NULL;
	struct passwd *pw;
	guint32 uid;

	if (!caller_uid(sender, &uid, &err)) {
		fail_unexpected(e, "%s", err->message);
		g_error_free(err);
		return FALSE;
	}
	pw = getpwuid(uid);
	if (!pw)
		return fail_unexpected(e, "'getpwuid(): uid not found: %u'", uid);
	if (!*username)
		username = pw->pw_name;
	if (uid != 0 && strcmp(pw->pw_name, username))
		return fail(e, "PermissionDenied", "not your fingerprints");
	*out = g_strdup(username);
	return TRUE;
}

static gboolean check_claim(const char *sender, struct dbus_err *e)
{
	if (!svc.claim_sender || strcmp(svc.claim_sender, sender))
		return fail(e, "ClaimDevice", "device not claimed by you");
	return TRUE;
}

static GVariant *strv_variant(GPtrArray *a)
{
	GVariantBuilder b;
	guint i;

	g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
	for (i = 0; i < a->len; i++)
		g_variant_builder_add(&b, "s", (char *)a->pdata[i]);
	return g_variant_new("(@as)", g_variant_builder_end(&b));
}

/* a method: its return value in *ret, or FALSE with the error in e */
static gboolean call(const char *sender, const char *name, GVariant *params, GVariant **ret,
		     struct dbus_err *e)
{
	const char *arg = NULL;
	char *user = NULL;

	*ret = NULL;
	if (g_variant_is_of_type(params, G_VARIANT_TYPE("(s)")))
		g_variant_get(params, "(&s)", &arg);
	if (!strcmp(name, "GetDevices")) {
		const char *devs[] = { DEVICE_PATH, NULL };

		*ret = g_variant_new("(^ao)", devs);
	} else if (!strcmp(name, "GetDefaultDevice")) {
		*ret = g_variant_new("(o)", DEVICE_PATH);
	} else if (!strcmp(name, "ListEnrolledFingers")) {
		GPtrArray *names;

		if (!user_for(sender, arg, &user, e))
			return FALSE;
		names = fingers_of(user);
		if (!names->len) {
			g_ptr_array_unref(names);
			fail(e, "NoEnrolledPrints", "no prints for %s", user);
			g_free(user);
			return FALSE;
		}
		*ret = strv_variant(names);
		g_ptr_array_unref(names);
		g_free(user);
	} else if (!strcmp(name, "Claim")) {
		guint32 uid;
		GError *err = NULL;

		if (!user_for(sender, arg, &user, e))
			return FALSE;
		if (svc.claim_sender) {
			if (!caller_uid(sender, &uid, &err)) {
				fail_unexpected(e, "%s", err->message);
				g_error_free(err);
				g_free(user);
				return FALSE;
			}
			if (uid == 0) {
				/*
				 * GDM restarts its fingerprint conversation (lock
				 * screen reset) before the old worker let go: the new
				 * one takes over
				 */
				log_msg("claim by %s replaces %s", sender, svc.claim_sender);
				release();
			}
		}
		if (svc.claim_sender) {
			g_free(user);
			return fail(e, "AlreadyInUse", "device already claimed");
		}
		svc.claim_sender = g_strdup(sender);
		svc.claim_user = user;
		svc.claim_watch = g_bus_watch_name_on_connection(svc.conn, sender,
								 G_BUS_NAME_WATCHER_FLAGS_NONE,
								 NULL, claim_vanished, NULL, NULL);
		log_msg("claimed by %s for %s", sender, svc.claim_user);
	} else if (!strcmp(name, "Release")) {
		if (!check_claim(sender, e))
			return FALSE;
		release();
	} else if (!strcmp(name, "VerifyStart")) {
		GPtrArray *names;
		struct verify *v;
		const char *finger = arg;
		guint i;

		if (!check_claim(sender, e))
			return FALSE;
		if (svc.stop && !g_atomic_int_get(svc.stop))
			return fail(e, "AlreadyInUse", "verification already running");
		names = fingers_of(svc.claim_user);
		if (!names->len) {
			g_ptr_array_unref(names);
			return fail(e, "NoEnrolledPrints", "no prints for %s", svc.claim_user);
		}
		v = g_new0(struct verify, 1);
		v->ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
		if (!*finger || !strcmp(finger, "any")) {
			for (i = 0; i < names->len; i++) {
				uint32_t id = finger_id(svc.claim_user, names->pdata[i]);

				g_array_append_val(v->ids, id);
			}
			finger = "any";
		} else {
			gboolean found = FALSE;

			for (i = 0; i < names->len; i++)
				found |= !strcmp(names->pdata[i], finger);
			if (!found) {
				g_ptr_array_unref(names);
				g_array_unref(v->ids);
				g_free(v);
				return fail(e, "NoEnrolledPrints", "no print for %s", finger);
			}
			{
				uint32_t id = finger_id(svc.claim_user, finger);

				g_array_append_val(v->ids, id);
			}
		}
		g_ptr_array_unref(names);
		emit(DEVICE_PATH, DEVICE_IFACE, "VerifyFingerSelected", g_variant_new("(s)", finger));
		/* the flag outlives the thread: the next VerifyStart looks at it */
		svc.stop = g_new0(gint, 1);
		v->stop = svc.stop;
		g_thread_unref(g_thread_new("verify", verify_thread, v));
	} else if (!strcmp(name, "VerifyStop")) {
		if (!check_claim(sender, e))
			return FALSE;
		stop_verify();
	} else if (!strcmp(name, "EnrollStart")) {
		if (!check_claim(sender, e))
			return FALSE;
		return fail(e, "Internal", "enrollment is not supported yet");
	} else if (!strcmp(name, "EnrollStop")) {
		if (!check_claim(sender, e))
			return FALSE;
		return fail(e, "NoActionInProgress", "no enrollment running");
	} else if (!strcmp(name, "DeleteEnrolledFingers") || !strcmp(name, "DeleteEnrolledFingers2") ||
		   !strcmp(name, "DeleteEnrolledFinger")) {
		return fail(e, "PrintsNotDeleted", "deleting prints is not supported yet");
	} else {
		return fail_unexpected(e, "'Service' object has no attribute 'm_%s'", name);
	}
	return TRUE;
}

static void method(GDBusConnection *conn, const char *sender, const char *path,
		   const char *iface, const char *name, GVariant *params,
		   GDBusMethodInvocation *inv, gpointer data)
{
	struct dbus_err e = { NULL, NULL, FALSE };
	GVariant *ret;

	if (call(sender, name, params, &ret, &e)) {
		g_dbus_method_invocation_return_value(inv, ret);
		return;
	}
	if (e.unexpected)
		log_msg("%s: %s", name, e.msg);
	{
		char *full = g_strconcat(ERR, e.name, NULL);

		g_dbus_method_invocation_return_dbus_error(inv, full, e.msg);
		g_free(full);
	}
	g_free(e.msg);
}

static const GDBusInterfaceVTable vtable = { method, get_prop, NULL, { 0 } };

static void on_bus(GDBusConnection *conn, const char *name, gpointer data)
{
	static const char *const paths[] = { MANAGER_PATH, DEVICE_PATH, FOD_PATH };
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
	int i;

	svc.conn = conn;
	for (i = 0; i < 3; i++)
		g_dbus_connection_register_object(conn, paths[i], node->interfaces[i], &vtable,
						  NULL, NULL, NULL);
	g_dbus_node_info_unref(node);
}

static void on_acquired(GDBusConnection *conn, const char *name, gpointer data)
{
	log_msg("serving %s", name);
}

static void on_lost(GDBusConnection *conn, const char *name, gpointer data)
{
	log_msg("lost %s", name);
	g_main_loop_quit(data);
}

int main(void)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
	const char *dir = g_getenv("STATE_DIRECTORY");
	GMainLoop *loop;
	GString *list;
	gboolean ready = FALSE;
	guint i;

	/* one malloc arena: glibc backs each thread arena with a 2 MiB THP */
	mallopt(M_ARENA_MAX, 1);
	setvbuf(stdout, NULL, _IOLBF, 0);
	state_dir = g_strdup(dir ? dir : "/var/lib/cezanne-fpd");
	prints_path = g_build_filename(state_dir, "prints.json", NULL);
	/* the TEE boots in the background (cezanne-tee.service): retry the TA */
	for (i = 0; i < 60 && !ready; i++) {
		GError *err = NULL;

		if (ta_open(&err) && ta_init(ids, &err)) {
			ready = TRUE;
			break;
		}
		log_msg("TA not ready: %s", err->message);
		g_error_free(err);
		ta_close();
		g_usleep(1000000);
	}
	if (!ready) {
		fputs("no fingerprint TA\n", stderr);
		return 1;
	}
	list = g_string_new("");
	for (i = 0; i < ids->len; i++)
		g_string_append_printf(list, "%s0x%x", i ? ", " : "", g_array_index(ids, uint32_t, i));
	log_msg("TA ready, %u enrolled finger(s): %s", ids->len, list->str);
	g_string_free(list, TRUE);
	touch_find();
	prints_load(ids);
	loop = g_main_loop_new(NULL, FALSE);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus,
		       on_acquired, on_lost, loop, NULL);
	g_main_loop_run(loop);
	return 0;
}
