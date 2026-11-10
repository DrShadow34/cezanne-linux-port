// SPDX-License-Identifier: MIT
/*
 * cezanne-usbd: what the USB-C port does when a computer is plugged in,
 * like a phone's "Use USB for" choice (Redmi K30 Ultra, cezanne port).
 *
 * Owns the configfs gadget (mtu3, high speed) and switches it by mode:
 *   none     cable in, nothing chosen yet: an empty card reader (mass
 *            storage, no medium), only so that a host enumerates us; that
 *            is how a computer is told from a charger (BC1.2 says SDP behind
 *            PD chargers as well, a charger never configures a gadget)
 *   charge   no gadget: charging only
 *   storage  MTP (umtprd on FunctionFS): the caller's home folder, files
 *            owned by them
 *   stream   UVC webcam, MJPEG; cezanne-usb-stream (user session) feeds it
 *            the screen or a camera
 *   network  RNDIS + DHCP on 172.16.42.1 (the former usb-gadget.service)
 * Unplugging (the Type-C partner goes) falls back to none. While storage,
 * stream or network serve a host a logind sleep block inhibitor is held: a
 * suspend drops the USB link (the AOD's own suspend honours it).
 *
 * System bus: org.cezanne.Usb at /org/cezanne/Usb. SetMode is for the
 * active local session (polkit org.cezanne.usb.set-mode); the GNOME Shell
 * extension cezanne-usb@arch-port asks the user and calls it.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>
#include <gudev/gudev.h>

#define BUS_NAME	"org.cezanne.Usb"
#define OBJ_PATH	"/org/cezanne/Usb"
#define ACTION		"org.cezanne.usb.set-mode"

#define CONFIGFS	"/sys/kernel/config/usb_gadget"
#define GADGET		CONFIGFS "/cezanne"
#define PARTNER		"/sys/class/typec/port0-partner"
#define RUN		"/run/cezanne-usb"
#define FFS		RUN "/ffs-mtp"
#define UMTPRD_CONF	RUN "/umtprd.conf"
#define NET_ADDR	"172.16.42.1/24"
/*
 * a host left undecided (no extension asking, or nobody answering) goes to
 * charge only: in none, every s2idle doze of a locked phone re-enumerates
 * the probe on the computer
 */
#define UNDECIDED_S	120

/*
 * Linux Foundation "Multifunction Composite Gadget"; bcdDevice differs per
 * mode so that hosts (Windows caches drivers by VID:PID:bcdDevice) never
 * mix them up
 */
#define VID		0x1d6b
#define PID		0x0104

/* usb-gadget.service (RNDIS) before this daemon */
static const char *const old_gadgets[] = { "g1", NULL };
static const char *const modes[] = { "none", "charge", "storage", "stream", "network", NULL };

static int bcd(const char *mode)
{
	return !strcmp(mode, "none") ? 0x100 : !strcmp(mode, "storage") ? 0x200 :
	       !strcmp(mode, "stream") ? 0x300 : 0x400;
}

/* Stream sources: id, label, libcamera camera id ('' = the screen) */
static const char *const sources[][3] = {
	{ "screen", "Phone screen", "" },
	{ "main", "Main camera", "/base/soc/i2c@11d01000/camera-sensor@1a" },
	{ "ultrawide", "Ultrawide camera", "/base/soc/i2c@11d02000/camera-sensor@20" },
	{ "macro", "Macro camera", "/base/soc/i2c@11d04000/camera-sensor@2d" },
	{ "front", "Front camera", "/base/soc/i2c@11d02000/camera-sensor@56" },
};
#define NSOURCES	(sizeof(sources) / sizeof(*sources))

/*
 * UVC frame sizes per function: the screen is portrait, the sensors
 * landscape. MJPEG (CPU jpegenc does ~33 fps at 1080x2400 / 1920x1080), and
 * one small YUYV frame, what a client that does not ask for MJPEG gets
 * (OBS's default showed nothing): w * h * 2 bytes a frame within the 8 MB/s.
 */
static const struct uvc_fn {
	const char *name;
	int frames[2][2];
	int yuyv[2], yuyv_interval;
} uvc_fns[] = {
	{ "uvc.screen", { { 1080, 2400 }, { 720, 1600 } }, { 360, 800 }, 1000000 },	/* 10 fps, 5.8 MB/s */
	{ "uvc.camera", { { 1920, 1080 }, { 1280, 720 } }, { 640, 360 }, 666666 },	/* 15 fps, 6.9 MB/s */
};
static const int uvc_intervals[] = { 333333, 666666 };	/* 30 and 15 fps, in 100 ns */

static const char iface_xml[] =
	"<node>"
	"  <interface name='org.cezanne.Usb'>"
	"    <method name='SetMode'>"
	"      <arg name='mode' type='s' direction='in'/>"
	"      <arg name='source' type='s' direction='in'/>"
	"    </method>"
	"    <property name='Plugged' type='b' access='read'/>"
	"    <property name='Host' type='b' access='read'/>"
	"    <property name='Mode' type='s' access='read'/>"
	"    <property name='Source' type='s' access='read'/>"
	"    <property name='Sources' type='a(sss)' access='read'/>"
	"    <property name='VideoDevice' type='s' access='read'/>"
	"    <property name='Error' type='s' access='read'/>"
	"  </interface>"
	"</node>";

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

/* ---- files: errors as GError (Python's OSError) ------------------------ */

static void set_errno_error(GError **err, const char *path)
{
	int e = errno;

	g_set_error(err, G_IO_ERROR, g_io_error_from_errno(e), "[Errno %d] %s: '%s'",
		    e, g_strerror(e), path);
}

static gboolean write_str(const char *path, const char *value, GError **err)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	ssize_t n;

	if (fd < 0) {
		set_errno_error(err, path);
		return FALSE;
	}
	/*
	 * nothing to write: no write() at all (configfs fails a zero-length
	 * write with EFAULT; unbinding is the config links' removal anyway)
	 */
	n = *value ? write(fd, value, strlen(value)) : 0;
	if (n < 0) {
		set_errno_error(err, path);
		close(fd);
		return FALSE;
	}
	if (close(fd)) {
		set_errno_error(err, path);
		return FALSE;
	}
	return TRUE;
}

static gboolean writef(GError **err, const char *value, const char *fmt, ...)
{
	va_list ap;
	char *path;
	gboolean ok;

	va_start(ap, fmt);
	path = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	ok = write_str(path, value, err);
	g_free(path);
	return ok;
}

static gboolean writei(GError **err, long long v, const char *fmt, ...)
{
	char value[32], *path;
	va_list ap;
	gboolean ok;

	snprintf(value, sizeof(value), "%lld", v);
	va_start(ap, fmt);
	path = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	ok = write_str(path, value, err);
	g_free(path);
	return ok;
}

/* the stripped contents, or default */
static char *read_str(const char *path, const char *def)
{
	char *s;

	if (!g_file_get_contents(path, &s, NULL, NULL))
		return g_strdup(def);
	return g_strstrip(s);
}

static gboolean makedirs(const char *path, GError **err)
{
	if (g_mkdir_with_parents(path, 0777)) {
		set_errno_error(err, path);
		return FALSE;
	}
	return TRUE;
}

static gboolean symlinkf(GError **err, const char *target, const char *fmt, ...)
{
	va_list ap;
	char *path;
	gboolean ok = TRUE;

	va_start(ap, fmt);
	path = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	if (symlink(target, path)) {
		set_errno_error(err, path);
		ok = FALSE;
	}
	g_free(path);
	return ok;
}

static gboolean is_dir(const char *p)
{
	struct stat st;

	return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static gboolean is_link(const char *p)
{
	struct stat st;

	return !lstat(p, &st) && S_ISLNK(st.st_mode);
}

/* os.path.ismount */
static gboolean is_mount(const char *p)
{
	struct stat st, up;
	char *parent = g_strconcat(p, "/..", NULL);
	gboolean r = FALSE;

	if (!lstat(p, &st) && !S_ISLNK(st.st_mode) && !lstat(parent, &up))
		r = st.st_dev != up.st_dev || st.st_ino == up.st_ino;
	g_free(parent);
	return r;
}

/* run a command and wait for it: its exit status, -1 if it did not run */
static int run(char *const argv[])
{
	pid_t pid = fork();
	int st;

	if (pid < 0)
		return -1;
	if (!pid) {
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* subprocess.run(check=True) */
static gboolean run_check(char *const argv[], GError **err)
{
	int r = run(argv);
	GString *cmd;
	int i;

	if (!r)
		return TRUE;
	cmd = g_string_new("[");
	for (i = 0; argv[i]; i++)
		g_string_append_printf(cmd, "%s'%s'", i ? ", " : "", argv[i]);
	g_string_append_c(cmd, ']');
	g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
		    "Command '%s' returned non-zero exit status %d.", cmd->str, r);
	g_string_free(cmd, TRUE);
	return FALSE;
}

/* ---- the gadget -------------------------------------------------------- */

/* unlink the symlinks below d (the deepest first) */
static void unlink_links(const char *d)
{
	DIR *dir = opendir(d);
	struct dirent *e;

	while (dir && (e = readdir(dir))) {
		char *p;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		p = g_build_filename(d, e->d_name, NULL);
		if (is_link(p))
			unlink(p);
		else if (is_dir(p))
			unlink_links(p);
		g_free(p);
	}
	if (dir)
		closedir(dir);
}

/* rmdir every directory below d, the deepest first (errors ignored) */
static void rmdirs(const char *d)
{
	DIR *dir = opendir(d);
	struct dirent *e;
	GPtrArray *subs = g_ptr_array_new_with_free_func(g_free);
	guint i;

	while (dir && (e = readdir(dir))) {
		char *p;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		p = g_build_filename(d, e->d_name, NULL);
		if (is_dir(p) && !is_link(p))
			g_ptr_array_add(subs, p);
		else
			g_free(p);
	}
	if (dir)
		closedir(dir);
	for (i = 0; i < subs->len; i++) {
		rmdirs(subs->pdata[i]);
		rmdir(subs->pdata[i]);
	}
	g_ptr_array_unref(subs);
}

/*
 * Unbind and remove a configfs gadget: symlinks first, then the directories
 * deepest first (default groups refuse rmdir and go with their parent).
 */
static void remove_gadget(const char *path)
{
	char *udc;

	if (!is_dir(path))
		return;
	udc = g_strconcat(path, "/UDC", NULL);
	write_str(udc, "", NULL);
	g_free(udc);
	unlink_links(path);
	rmdirs(path);
	if (rmdir(path))
		log_msg("cannot remove %s: [Errno %d] %s: '%s'", path, errno, g_strerror(errno), path);
}

static char *udc;		/* the UDC's name */
static gboolean have_uvc;

static gboolean gadget_uvc(const struct uvc_fn *u, GError **err)
{
	char f[256], d[320];
	int i;

	snprintf(f, sizeof(f), GADGET "/functions/%s", u->name);
	if (!makedirs(f, err) ||
	    /*
	     * one 1024 byte packet per microframe (8 MB/s): with 2 or 3 the
	     * mtu3 still sent one per microframe and the host read the rest as
	     * payload headers (NOTES "USB modes")
	     */
	    !writei(err, 1024, "%s/streaming_maxpacket", f) ||
	    !writei(err, 1, "%s/streaming_interval", f) ||
	    !writef(err, "Redmi K30 Ultra", "%s/function_name", f))
		return FALSE;
	for (i = 0; i < 2; i++) {
		int w = u->frames[i][0], h = u->frames[i][1];
		char iv[32];

		snprintf(d, sizeof(d), "%s/streaming/mjpeg/m/%dx%d", f, w, h);
		snprintf(iv, sizeof(iv), "%d\n%d", uvc_intervals[0], uvc_intervals[1]);
		if (!makedirs(d, err) ||
		    !writei(err, w, "%s/wWidth", d) || !writei(err, h, "%s/wHeight", d) ||
		    !writei(err, (long long)w * h * 2, "%s/dwMaxVideoFrameBufferSize", d) ||
		    !writei(err, uvc_intervals[0], "%s/dwDefaultFrameInterval", d) ||
		    !writef(err, iv, "%s/dwFrameInterval", d) ||
		    !writei(err, (long long)w * h * 8, "%s/dwMinBitRate", d) ||
		    !writei(err, (long long)w * h * 16 * 30, "%s/dwMaxBitRate", d))
			return FALSE;
	}
	{
		int w = u->yuyv[0], h = u->yuyv[1], ivl = u->yuyv_interval;
		long long rate = (long long)w * h * 16 * 10000000 / ivl;

		/* guidFormat defaults to YUY2 */
		snprintf(d, sizeof(d), "%s/streaming/uncompressed/u/%dx%d", f, w, h);
		if (!makedirs(d, err) ||
		    !writei(err, w, "%s/wWidth", d) || !writei(err, h, "%s/wHeight", d) ||
		    !writei(err, (long long)w * h * 2, "%s/dwMaxVideoFrameBufferSize", d) ||
		    !writei(err, ivl, "%s/dwDefaultFrameInterval", d) ||
		    !writei(err, ivl, "%s/dwFrameInterval", d) ||
		    !writei(err, rate, "%s/dwMinBitRate", d) ||
		    !writei(err, rate, "%s/dwMaxBitRate", d))
			return FALSE;
	}
	snprintf(d, sizeof(d), "%s/streaming/header/h", f);
	if (!makedirs(d, err))
		return FALSE;
	/* MJPEG first: format 1, the default */
	snprintf(d, sizeof(d), "%s/streaming/mjpeg/m", f);
	if (!symlinkf(err, d, "%s/streaming/header/h/m", f))
		return FALSE;
	snprintf(d, sizeof(d), "%s/streaming/uncompressed/u", f);
	if (!symlinkf(err, d, "%s/streaming/header/h/u", f))
		return FALSE;
	/*
	 * no camera terminal / processing unit controls: cezanne-usb-stream
	 * implements none (hosts query what is announced)
	 */
	{
		static const char *const attrs[] = { "terminal/camera/default/bmControls",
						     "processing/default/bmControls" };

		for (i = 0; i < 2; i++) {
			GError *e = NULL;

			if (!writei(&e, 0, "%s/control/%s", f, attrs[i])) {
				log_msg("%s %s: %s", u->name, attrs[i], e->message);
				g_error_free(e);
			}
		}
	}
	snprintf(d, sizeof(d), "%s/streaming/header/h", f);
	if (!symlinkf(err, d, "%s/streaming/class/fs/h", f) ||
	    !symlinkf(err, d, "%s/streaming/class/hs/h", f) ||
	    !symlinkf(err, d, "%s/streaming/class/ss/h", f))
		return FALSE;
	snprintf(d, sizeof(d), "%s/control/header/h", f);
	if (!makedirs(d, err) ||
	    !symlinkf(err, d, "%s/control/class/fs/h", f) ||
	    !symlinkf(err, d, "%s/control/class/ss/h", f))
		return FALSE;
	return TRUE;
}

/*
 * The configfs gadget with every function created once; a mode is the set
 * of functions linked into configs/c.1 while bound.
 */
static gboolean gadget_init(GError **err)
{
	const char *f = GADGET "/functions/mass_storage.probe";
	char *serial, *mid;
	GDir *d = g_dir_open("/sys/class/udc", 0, NULL);
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *n;
	int i;

	while (d && (n = g_dir_read_name(d)))
		g_ptr_array_add(names, g_strdup(n));
	if (d)
		g_dir_close(d);
	g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
	udc = names->len ? g_strdup(names->pdata[0]) : g_strdup("None");
	g_ptr_array_unref(names);

	for (i = 0; old_gadgets[i]; i++) {
		char *p = g_strdup_printf(CONFIGFS "/%s", old_gadgets[i]);

		remove_gadget(p);
		g_free(p);
	}
	/* a previous run's gadget: its functions may be outdated */
	remove_gadget(GADGET);
	mid = read_str("/etc/machine-id", "");
	serial = g_strndup(*mid ? mid : "cezanne", *mid ? 16 : 7);
	g_free(mid);
	if (!makedirs(GADGET "/strings/0x409", err) ||
	    !writef(err, "0x1d6b", GADGET "/idVendor") ||
	    !writef(err, "0x104", GADGET "/idProduct") ||
	    !writef(err, "0x0200", GADGET "/bcdUSB") ||
	    !writef(err, "Arch Linux ARM", GADGET "/strings/0x409/manufacturer") ||
	    !writef(err, "Redmi K30 Ultra", GADGET "/strings/0x409/product") ||
	    !writef(err, serial, GADGET "/strings/0x409/serialnumber") ||
	    !makedirs(GADGET "/configs/c.1/strings/0x409", err) ||
	    !writei(err, 500, GADGET "/configs/c.1/MaxPower")) {
		g_free(serial);
		return FALSE;
	}
	g_free(serial);
	/* the probe */
	if (!makedirs(f, err) || !writei(err, 1, "%s/stall", f) ||
	    !writei(err, 1, "%s/lun.0/removable", f) || !writei(err, 1, "%s/lun.0/ro", f) ||
	    !writef(err, "Linux   Redmi K30 Ultra 1.00", "%s/lun.0/inquiry_string", f))
		return FALSE;
	/* MTP */
	if (!makedirs(GADGET "/functions/ffs.mtp", err) || !makedirs(FFS, err))
		return FALSE;
	if (!is_mount(FFS) &&
	    !run_check((char *[]){ "mount", "-t", "functionfs", "mtp", FFS, NULL }, err))
		return FALSE;
	/* usb_f_uvc (CONFIG_USB_CONFIGFS_F_UVC, linux-cezanne >= 7.3.1-2) */
	have_uvc = TRUE;
	for (i = 0; i < 2 && have_uvc; i++) {
		GError *e = NULL;

		if (!gadget_uvc(&uvc_fns[i], &e)) {
			log_msg("no UVC gadget function: %s", e->message);
			g_error_free(e);
			have_uvc = FALSE;
		}
	}
	return makedirs(GADGET "/functions/rndis.usb0", err);
}

static gboolean gadget_unbind(GError **err)
{
	char *cur = read_str(GADGET "/UDC", "");
	GDir *d;
	const char *n;
	gboolean ok = TRUE;

	if (*cur)
		ok = write_str(GADGET "/UDC", "", err);
	g_free(cur);
	if (!ok)
		return FALSE;
	d = g_dir_open(GADGET "/configs/c.1", 0, NULL);
	while (d && (n = g_dir_read_name(d))) {
		char *p = g_build_filename(GADGET "/configs/c.1", n, NULL);

		if (is_link(p))
			unlink(p);
		g_free(p);
	}
	if (d)
		g_dir_close(d);
	return TRUE;
}

static gboolean gadget_bind(const char *mode, const char *function, const char *label,
			    GError **err)
{
	char b[16], target[256];

	snprintf(b, sizeof(b), "0x%x", bcd(mode));
	snprintf(target, sizeof(target), GADGET "/functions/%s", function);
	return writef(err, b, GADGET "/bcdDevice") &&
	       writef(err, label, GADGET "/configs/c.1/strings/0x409/configuration") &&
	       symlinkf(err, target, GADGET "/configs/c.1/%s", function) &&
	       writef(err, udc, GADGET "/UDC");
}

static void gadget_remove(void)
{
	remove_gadget(GADGET);
	if (is_mount(FFS))
		run((char *[]){ "umount", FFS, NULL });
}

/* ---- the daemon -------------------------------------------------------- */

static struct {
	GMainLoop *loop;
	gboolean plugged, host;
	char *mode, *source, *video, *error;
	struct passwd user;		/* the caller of SetMode */
	gboolean have_user;
	char *user_name, *user_dir;
	GArray *children;		/* pid_t */
	int inhibit_fd;
	guint undecided_id;
	GDBusConnection *conn;
	guint reg_id;
	int state_fd;
	GUdevClient *udev;
} d = { .inhibit_fd = -1, .state_fd = -1 };

static GVariant *sources_variant(void)
{
	GVariantBuilder b;
	size_t i;

	g_variant_builder_init(&b, G_VARIANT_TYPE("a(sss)"));
	for (i = 0; i < NSOURCES; i++)
		g_variant_builder_add(&b, "(sss)", sources[i][0], sources[i][1], sources[i][2]);
	return g_variant_builder_end(&b);
}

static GVariant *get_prop(const char *name)
{
	if (!strcmp(name, "Plugged"))
		return g_variant_new_boolean(d.plugged);
	if (!strcmp(name, "Host"))
		return g_variant_new_boolean(d.host);
	if (!strcmp(name, "Mode"))
		return g_variant_new_string(d.mode ? d.mode : "none");
	if (!strcmp(name, "Source"))
		return g_variant_new_string(d.source ? d.source : "");
	if (!strcmp(name, "Sources"))
		return sources_variant();
	if (!strcmp(name, "VideoDevice"))
		return g_variant_new_string(d.video ? d.video : "");
	if (!strcmp(name, "Error"))
		return g_variant_new_string(d.error ? d.error : "");
	return NULL;
}

/* PropertiesChanged with these properties ('NAME' ...), NULL-terminated */
static void emit(const char *first, ...)
{
	GVariantBuilder b;
	va_list ap;
	const char *n;

	if (!d.conn)
		return;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	va_start(ap, first);
	for (n = first; n; n = va_arg(ap, const char *))
		g_variant_builder_add(&b, "{sv}", n, get_prop(n));
	va_end(ap);
	g_dbus_connection_emit_signal(d.conn, NULL, OBJ_PATH, "org.freedesktop.DBus.Properties",
				      "PropertiesChanged",
				      g_variant_new("(sa{sv}as)", BUS_NAME, &b, NULL), NULL);
}

static void sync_inhibitor(void)
{
	gboolean want = d.plugged && d.host && d.mode &&
			(!strcmp(d.mode, "storage") || !strcmp(d.mode, "stream") ||
			 !strcmp(d.mode, "network"));

	if (want && d.inhibit_fd < 0) {
		GError *err = NULL;
		GUnixFDList *fds = NULL;
		GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
		char *why = g_strdup_printf("USB %s in use", d.mode);
		GVariant *r = bus ? g_dbus_connection_call_with_unix_fd_list_sync(
			bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "Inhibit",
			g_variant_new("(ssss)", "sleep", "cezanne-usb", why, "block"),
			G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &fds, NULL, &err) : NULL;

		g_free(why);
		if (r) {
			gint32 h;

			g_variant_get(r, "(h)", &h);
			d.inhibit_fd = g_unix_fd_list_get(fds, h, NULL);
			g_variant_unref(r);
			g_object_unref(fds);
		} else {
			log_msg("inhibit: %s", err->message);
			g_error_free(err);
		}
		if (bus)
			g_object_unref(bus);
	} else if (!want && d.inhibit_fd >= 0) {
		close(d.inhibit_fd);
		d.inhibit_fd = -1;
	}
}

static int apply(const char *mode, const char *source, char **error_out);

static gboolean on_undecided(gpointer data)
{
	d.undecided_id = 0;
	log_msg("no choice made: charge only");
	apply("charge", "", NULL);
	return G_SOURCE_REMOVE;
}

static void sync_undecided(void)
{
	gboolean want = d.host && d.mode && !strcmp(d.mode, "none");

	if (want && !d.undecided_id) {
		d.undecided_id = g_timeout_add_seconds(UNDECIDED_S, on_undecided, NULL);
	} else if (!want && d.undecided_id) {
		g_source_remove(d.undecided_id);
		d.undecided_id = 0;
	}
}

/* terminate, wait 3 s each, then kill */
static void stop_children(void)
{
	guint i;

	for (i = 0; i < d.children->len; i++)
		kill(g_array_index(d.children, pid_t, i), SIGTERM);
	for (i = 0; i < d.children->len; i++) {
		pid_t pid = g_array_index(d.children, pid_t, i);
		double end = mono() + 3;

		while (waitpid(pid, NULL, WNOHANG) == 0) {
			if (mono() > end) {
				kill(pid, SIGKILL);
				waitpid(pid, NULL, 0);
				break;
			}
			g_usleep(10000);
		}
	}
	g_array_set_size(d.children, 0);
}

static pid_t spawn(char *const argv[])
{
	pid_t pid = fork();

	if (!pid) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid > 0)
		g_array_append_val(d.children, pid);
	return pid;
}

static gboolean start_mtp(GError **err)
{
	const char *name = d.have_user ? d.user_name : "arch";
	struct passwd *pw;
	char *bin = g_find_program_in_path("umtprd"), *conf, *mid, *serial;
	gboolean ok;
	double deadline;
	pid_t pid;

	if (!bin) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "data storage needs the umtp-responder package");
		return FALSE;
	}
	g_free(bin);
	if (d.have_user) {
		pw = &d.user;
	} else if (!(pw = getpwnam(name))) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
			    "getpwnam(): name not found: '%s'", name);
		return FALSE;
	}
	mid = read_str("/etc/machine-id", "");
	serial = g_strndup(*mid ? mid : "cezanne", *mid ? 16 : 7);
	g_free(mid);
	conf = g_strdup_printf(
		"loop_on_disconnect 1\n"
		"storage \"%s\" \"Home\" \"rw,uid=%u,gid=%u\"\n"
		"manufacturer \"Arch Linux ARM\"\n"
		"product \"Redmi K30 Ultra\"\n"
		"serial \"%s\"\n"
		"firmware_version \"1.0\"\n"
		"interface \"MTP\"\n"
		"usb_vendor_id 0x%x\n"
		"usb_product_id 0x%x\n"
		"usb_class 0x6\n"
		"usb_subclass 0x1\n"
		"usb_protocol 0x1\n"
		"usb_dev_version 0x%x\n"
		"usb_functionfs_mode 0x1\n"
		"usb_dev_path \"" FFS "/ep0\"\n"
		"usb_epin_path \"" FFS "/ep1\"\n"
		"usb_epout_path \"" FFS "/ep2\"\n"
		"usb_epint_path \"" FFS "/ep3\"\n"
		"usb_max_packet_size 0x200\n",
		pw->pw_dir, (unsigned)pw->pw_uid, (unsigned)pw->pw_gid, serial, VID, PID,
		bcd("storage"));
	g_free(serial);
	ok = write_str(UMTPRD_CONF, conf, err);
	g_free(conf);
	if (!ok)
		return FALSE;
	pid = spawn((char *[]){ "umtprd", "-conf", UMTPRD_CONF, NULL });
	/*
	 * FunctionFS creates the endpoint files once umtprd wrote its
	 * descriptors; binding before that fails
	 */
	deadline = mono() + 5;
	while (access(FFS "/ep1", F_OK)) {
		if (pid < 0 || waitpid(pid, NULL, WNOHANG) == pid || mono() > deadline) {
			g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "umtprd did not start");
			return FALSE;
		}
		g_usleep(50000);
	}
	return TRUE;
}

static gboolean start_network(GError **err)
{
	char *iface = read_str(GADGET "/functions/rndis.usb0/ifname", ""), *dev, *opt;
	gboolean ok;

	dev = g_strdup_printf("/sys/class/net/%s", iface);
	if (!*iface || access(dev, F_OK)) {
		g_free(dev);
		g_free(iface);
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "no RNDIS interface");
		return FALSE;
	}
	g_free(dev);
	ok = run_check((char *[]){ "ip", "addr", "replace", NET_ADDR, "dev", iface, NULL }, err) &&
	     run_check((char *[]){ "ip", "link", "set", iface, "up", NULL }, err);
	if (ok) {
		opt = g_strdup_printf("--interface=%s", iface);
		spawn((char *[]){ "dnsmasq", "--no-daemon", "--port=0", opt, "--bind-interfaces",
				  "--dhcp-range=172.16.42.2,172.16.42.10,1h", "--dhcp-option=3",
				  "--dhcp-option=6",
				  "--dhcp-leasefile=/run/cezanne-usb/dnsmasq.leases", NULL });
		g_free(opt);
	}
	g_free(iface);
	return ok;
}

static char *find_video(void)
{
	const char *want = !strcmp(d.source, "screen") ? "uvc.screen" : "uvc.camera";
	int i;

	for (i = 0; i < 40; i++) {
		GDir *dir = g_dir_open("/sys/class/video4linux", 0, NULL);
		GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
		const char *n;
		char *found = NULL;
		guint k;

		while (dir && (n = g_dir_read_name(dir)))
			g_ptr_array_add(names, g_strdup(n));
		if (dir)
			g_dir_close(dir);
		g_ptr_array_sort_values(names, (GCompareFunc)strcmp);
		for (k = 0; k < names->len && !found; k++) {
			char *p = g_strdup_printf("/sys/class/video4linux/%s/function_name",
						  (char *)names->pdata[k]);
			char *v = read_str(p, "");

			if (!strcmp(v, want))
				found = g_strdup_printf("/dev/%s", (char *)names->pdata[k]);
			g_free(v);
			g_free(p);
		}
		g_ptr_array_unref(names);
		if (found)
			return found;
		g_usleep(50000);
	}
	return g_strdup("");
}

/* switch the gadget; 0, or -1 with the error string in *error_out */
static int apply(const char *mode_in, const char *source_in, char **error_out)
{
	char *mode = g_strdup(mode_in), *source = g_strdup(source_in), *error = g_strdup("");
	GError *err = NULL;
	gboolean ok = TRUE;

	if (*source)
		log_msg("mode %s %s", mode, source);
	else
		log_msg("mode %s", mode);
	gadget_unbind(NULL);
	stop_children();
	if (!access(UMTPRD_CONF, F_OK))
		unlink(UMTPRD_CONF);
	if (!strcmp(mode, "none")) {
		ok = gadget_bind(mode, "mass_storage.probe", "USB", &err);
	} else if (!strcmp(mode, "storage")) {
		ok = start_mtp(&err) && gadget_bind(mode, "ffs.mtp", "MTP", &err);
	} else if (!strcmp(mode, "stream")) {
		if (!have_uvc) {
			g_set_error(&err, G_IO_ERROR, G_IO_ERROR_FAILED,
				    "this kernel has no USB webcam function");
			ok = FALSE;
		} else {
			ok = gadget_bind(mode, !strcmp(source, "screen") ? "uvc.screen" : "uvc.camera",
					 "Webcam", &err);
		}
	} else if (!strcmp(mode, "network")) {
		ok = gadget_bind(mode, "rndis.usb0", "RNDIS", &err) && start_network(&err);
	}
	if (!ok) {
		g_free(error);
		error = g_strdup_printf("%s: %s", mode, err->message);
		g_error_free(err);
		log_msg("%s", error);
		gadget_unbind(NULL);
		stop_children();
		g_free(mode);
		g_free(source);
		mode = g_strdup("charge");
		source = g_strdup("");
	}
	g_free(d.mode);
	g_free(d.source);
	g_free(d.video);
	g_free(d.error);
	d.mode = mode;
	d.source = !strcmp(mode, "stream") ? source : (g_free(source), g_strdup(""));
	d.video = !strcmp(mode, "stream") ? find_video() : g_strdup("");
	d.error = error;
	emit("Mode", "Source", "VideoDevice", "Error", NULL);
	sync_inhibitor();
	sync_undecided();
	if (error_out)
		*error_out = g_strdup(error);
	return *error ? -1 : 0;
}

/* ---- cable and host ---------------------------------------------------- */

static void on_uevent(GUdevClient *client, const char *action, GUdevDevice *dev,
		      gpointer data)
{
	gboolean plugged;

	if (g_strcmp0(g_udev_device_get_devtype(dev), "typec_partner") ||
	    (strcmp(action, "add") && strcmp(action, "remove")))
		return;
	plugged = !strcmp(action, "add");
	if (plugged == d.plugged)
		return;
	log_msg("cable %s", plugged ? "in" : "out");
	d.plugged = plugged;
	if (!plugged) {
		d.host = FALSE;
		if (strcmp(d.mode, "none"))
			apply("none", "", NULL);
		emit("Plugged", "Host", NULL);
	} else {
		emit("Plugged", NULL);
	}
	sync_inhibitor();
}

static void on_udc_state(void)
{
	char buf[65];
	ssize_t n = pread(d.state_fd, buf, 64, 0);

	buf[n > 0 ? n : 0] = 0;
	g_strstrip(buf);
	/* without a Type-C event (a cable plugged before boot) */
	if (!strcmp(buf, "configured") && !d.plugged) {
		d.plugged = !access(PARTNER, F_OK);
		emit("Plugged", NULL);
	}
	if (!strcmp(buf, "configured") && !d.host) {
		log_msg("host configured the gadget");
		d.host = TRUE;
		emit("Host", NULL);
		sync_inhibitor();
		sync_undecided();
	}
}

static gboolean udc_state_cb(gint fd, GIOCondition cond, gpointer data)
{
	on_udc_state();
	return G_SOURCE_CONTINUE;
}

/* ---- D-Bus ------------------------------------------------------------- */

struct auth {
	char *sender, *mode, *source;
	GDBusMethodInvocation *inv;
};

static void on_authorized(GObject *obj, GAsyncResult *res, gpointer data)
{
	struct auth *a = data;
	GDBusConnection *conn = G_DBUS_CONNECTION(obj);
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_finish(conn, res, &err), *u;
	gboolean authorized = FALSE;
	char *error = NULL;
	guint32 uid;
	struct passwd *pw;

	if (!r) {
		g_dbus_method_invocation_return_gerror(a->inv, err);
		g_error_free(err);
		goto out;
	}
	g_variant_get(r, "((bb@a{ss}))", &authorized, NULL, NULL);
	g_variant_unref(r);
	if (!authorized) {
		g_dbus_method_invocation_return_dbus_error(a->inv,
			"org.freedesktop.DBus.Error.AccessDenied", "not allowed to change the USB mode");
		goto out;
	}
	u = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
					"org.freedesktop.DBus", "GetConnectionUnixUser",
					g_variant_new("(s)", a->sender), G_VARIANT_TYPE("(u)"),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	if (!u) {
		g_dbus_method_invocation_return_gerror(a->inv, err);
		g_error_free(err);
		goto out;
	}
	g_variant_get(u, "(u)", &uid);
	g_variant_unref(u);
	pw = getpwuid(uid);
	if (pw) {
		g_free(d.user_name);
		g_free(d.user_dir);
		d.user = *pw;
		d.user_name = g_strdup(pw->pw_name);
		d.user_dir = g_strdup(pw->pw_dir);
		d.user.pw_name = d.user_name;
		d.user.pw_dir = d.user_dir;
		d.have_user = TRUE;
	}
	if (!strcmp(a->mode, "none") || !d.plugged) {	/* nothing to serve without a cable */
		g_free(a->mode);
		g_free(a->source);
		a->mode = g_strdup("none");
		a->source = g_strdup("");
	}
	if (apply(a->mode, a->source, &error))
		g_dbus_method_invocation_return_dbus_error(a->inv, "org.cezanne.Usb.Error.Failed",
							   error);
	else
		g_dbus_method_invocation_return_value(a->inv, NULL);
	g_free(error);
out:
	g_free(a->sender);
	g_free(a->mode);
	g_free(a->source);
	g_free(a);
}

static void on_call(GDBusConnection *conn, const char *sender, const char *path,
		    const char *iface, const char *method, GVariant *params,
		    GDBusMethodInvocation *inv, gpointer data)
{
	const char *mode, *source;
	GVariantBuilder details;
	gboolean known = FALSE;
	struct auth *a;
	size_t i;

	if (strcmp(method, "SetMode")) {
		g_dbus_method_invocation_return_dbus_error(inv,
			"org.freedesktop.DBus.Error.UnknownMethod", method);
		return;
	}
	g_variant_get(params, "(&s&s)", &mode, &source);
	for (i = 0; modes[i]; i++)
		known |= !strcmp(modes[i], mode);
	if (known && !strcmp(mode, "stream")) {
		known = FALSE;
		for (i = 0; i < NSOURCES; i++)
			known |= !strcmp(sources[i][0], source);
	}
	if (!known) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
						      "unknown mode %s %s", mode, source);
		return;
	}
	a = g_new0(struct auth, 1);
	a->sender = g_strdup(sender);
	a->mode = g_strdup(mode);
	a->source = g_strdup(source);
	a->inv = inv;
	g_variant_builder_init(&details, G_VARIANT_TYPE("a{ss}"));
	g_dbus_connection_call(conn, "org.freedesktop.PolicyKit1",
			       "/org/freedesktop/PolicyKit1/Authority",
			       "org.freedesktop.PolicyKit1.Authority", "CheckAuthorization",
			       g_variant_new("((s@a{sv})sa{ss}us)", "system-bus-name",
					     g_variant_new_parsed("{'name': <%s>}", sender),
					     ACTION, &details, 0, ""),
			       G_VARIANT_TYPE("((bba{ss}))"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			       on_authorized, a);
}

static GVariant *on_get(GDBusConnection *conn, const char *sender, const char *path,
			const char *iface, const char *name, GError **err, gpointer data)
{
	GVariant *v = get_prop(name);

	if (!v)
		g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "%s", name);
	return v;
}

static const GDBusInterfaceVTable vtable = { on_call, on_get, NULL, { 0 } };

static void on_bus(GDBusConnection *conn, const char *name, gpointer data)
{
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(iface_xml, NULL);

	d.conn = conn;
	d.reg_id = g_dbus_connection_register_object(conn, OBJ_PATH, info->interfaces[0],
						     &vtable, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void shutdown_daemon(void)
{
	stop_children();
	gadget_remove();
	if (d.inhibit_fd >= 0)
		close(d.inhibit_fd);
	g_main_loop_quit(d.loop);
}

static void on_name_lost(GDBusConnection *conn, const char *name, gpointer data)
{
	shutdown_daemon();
}

static gboolean on_signal(gpointer data)
{
	shutdown_daemon();
	return G_SOURCE_REMOVE;
}

int main(void)
{
	static const char *const subsystems[] = { "typec", NULL };
	GError *err = NULL;
	char *state;

	/* one malloc arena: glibc backs each thread arena with a 2 MiB THP */
	mallopt(M_ARENA_MAX, 1);
	setvbuf(stdout, NULL, _IOLBF, 0);
	g_mkdir_with_parents(RUN, 0777);
	d.loop = g_main_loop_new(NULL, FALSE);
	d.children = g_array_new(FALSE, FALSE, sizeof(pid_t));
	if (!gadget_init(&err)) {
		fprintf(stderr, "gadget: %s\n", err->message);
		return 1;
	}
	d.plugged = !access(PARTNER, F_OK);
	d.udev = g_udev_client_new(subsystems);
	g_signal_connect(d.udev, "uevent", G_CALLBACK(on_uevent), NULL);
	/* the UDC's state attribute is sysfs_notify()ed on every change */
	state = g_strdup_printf("/sys/class/udc/%s/state", udc);
	d.state_fd = open(state, O_RDONLY | O_CLOEXEC);
	if (d.state_fd < 0) {
		fprintf(stderr, "%s: %s\n", state, g_strerror(errno));
		return 1;
	}
	g_free(state);
	on_udc_state();
	g_unix_fd_add(d.state_fd, G_IO_PRI | G_IO_ERR, udc_state_cb, NULL);
	apply("none", "", NULL);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus, NULL,
		       on_name_lost, NULL, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_main_loop_run(d.loop);
	return 0;
}
