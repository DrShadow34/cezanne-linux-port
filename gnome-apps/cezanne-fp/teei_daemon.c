// SPDX-License-Identifier: GPL-2.0
/*
 * teei_daemon: native replacement for MIUI's /vendor/bin/teei_daemon +
 * libteei_daemon_vfs.so (Microtrust isee 4.0), reimplemented from their
 * disassembly. It boots the TEE (/dev/teei_config INIT + UNLOCK) and serves
 * the TEE's file requests on /dev/tz_vfs.
 *
 * VFS protocol (one request at a time, 512 KiB shared buffer):
 *   read(/dev/tz_vfs) -> u32 func, u32 cmd_size, args at +8, strings/data at
 *   +0x1000; write(/dev/tz_vfs) the reply: status (int or u64) at +0, data at
 *   +0x1000, length 8 or 0x1000 + data.
 * The TEE names Android paths; they are mapped onto ROOT (see map_path()).
 * The tz_vfs driver only accepts a process whose comm is "teei_daemon".
 * Its RPMB device /dev/rpmb0 is served here on the UFS RPMB LU (see rpmb_*).
 *
 *   teei_daemon [-v] [-r root]	serve the TEE
 *   teei_daemon -c			print the RPMB write counter (read only)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/bsg.h>
#include <scsi/sg.h>

#define VFS_SIZE	0x80000
#define PARAM_OFF	0x1000

#define MAX_DRV_UUIDS	30
#define UUID_LEN	32
struct init_param {
	char uuids[MAX_DRV_UUIDS][UUID_LEN + 1];
	uint32_t uuid_count;
	uint32_t flag;		/* bit i set: uuids[i] is a TA, clear: a driver */
};
#define TEEI_CONFIG_IOCTL_INIT_TEEI	_IOWR(0x54, 3, struct init_param)
#define TEEI_CONFIG_IOCTL_UNLOCK	_IOWR(0x54, 4, int)

enum {
	VFS_OPEN = 17, VFS_READ, VFS_WRITE, VFS_IOCTL, VFS_CLOSE, VFS_TRUNC,
	VFS_UNLINK, VFS_LSEEK, VFS_RENAME, VFS_COPY,
	VFS_MKDIR = 33, VFS_RMDIR, VFS_OPENDIR, VFS_READDIR, VFS_CLOSEDIR,
	VFS_PRONODE = 48, VFS_SETPROP, VFS_GETPROP,
	VFS_RPMB_OPEN = 0x300, VFS_RPMB_GET_CNT, VFS_RPMB_WRITE_KEY,
	VFS_RPMB_READ, VFS_RPMB_READ2, VFS_RPMB_WRITE, VFS_RPMB_CLOSE,
};

/* the vendor rc's -r (drivers) and -t (TAs) lists, then the default driver */
static const char *const drivers[] = {
	"020b0000000000000000000000000000", "020f0000000000000000000000000000",
	"06090000000000000000000000000000", "05120000000000000000000000000000",
	"40188311faf343488db888ad39496f9a", "07060000000000000000000000007169",
	"4be4f7dc1f2c11e5b5f7727283247c7f", "9073f03a9618383bb1856eb3f990babd",
	"08050000000000000000000000003419", "5020170115e016302017012521300000",
	"0f5eed3c3b5a47afacca69a84bf0efad", "07407000000000000000000000000000",
	"05160000000000000000000000000000", "91dba524a6e24909acc48abd7163121a",
	"57f02fe81d5a11ea8d23d7abe98b8462", "08070000000000000000000000008270",
	"09080000000000000000000000009381",
};
static const char *const tas[] = {
	"08030000000000000000000000000000", "85f630e0f0964c5fa2cc268ce04e3da3",
	"98fb95bcb4bf42d26473eae48690d7ea", "7b66512021214487ba710a51d7ea78fe",
	"09010000000000000000000000000000",
};
static const char default_driver[] = "93feffccd8ca11e796c7c7a21acb4932";

static const char *root = "/var/lib/cezanne-tee";
static int verbose;

static void logm(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
#define dbg(...) do { if (verbose) logm(__VA_ARGS__); } while (0)

/*
 * Android path -> local path. Everything the TEE may touch lives under
 * ROOT: vendor/thh (TAs, read-only copies), data/vendor (TEE storage,
 * fingerprint templates), persist (a copy of the persist partition: factory
 * calibration, virtual RPMB). Unknown absolute paths go under ROOT/other.
 */
/* the path as Android would resolve it: relative ones below /vendor/thh/,
 * "." and ".." resolved (".." stops at "/"). The TA's secure storage opens
 * "/data/vendor/thh/<uuid>./../../../../mnt/vendor/persist/goodix/..." and
 * "../../../../mnt/..." (relative): the ".." must apply in Android's tree,
 * before map_path() moves it under ROOT */
static void android_path(const char *in, char *out, size_t n)
{
	char tmp[PATH_MAX], *seg, *save;
	size_t len = 0;

	snprintf(tmp, sizeof(tmp), "%s%s", in[0] == '/' ? "" : "/vendor/thh/", in);
	out[0] = 0;
	for (seg = strtok_r(tmp, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
		if (!strcmp(seg, "."))
			continue;
		if (!strcmp(seg, "..")) {
			while (len && out[len - 1] != '/')
				len--;
			if (len)
				len--;
			out[len] = 0;
			continue;
		}
		len += snprintf(out + len, n - len, "/%s", seg);
		if (len >= n)
			len = n - 1;
	}
	if (!len)
		snprintf(out, n, "/");
}

static void map_path(const char *in, char *out, size_t n)
{
	static const struct { const char *android, *local; } map[] = {
		{ "/vendor/", "vendor/" },
		{ "/data/vendor/", "data/vendor/" },
		{ "/mnt/vendor/persist/", "persist/" },
		{ "/vendor/persist/", "persist/" },
		{ "/persist/", "persist/" },
		{ "/data/", "data/" },
	};
	size_t i;

	char abs[PATH_MAX];

	android_path(in, abs, sizeof(abs));
	in = abs;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		size_t l = strlen(map[i].android);

		if (!strncmp(in, map[i].android, l)) {
			snprintf(out, n, "%s/%s%s", root, map[i].local, in + l);
			return;
		}
	}
	snprintf(out, n, "%s/other%s", root, in);
}

/* ---- properties (the TEE's SETPROP/GETPROP; read_property() in the vendor lib) ---- */
#define MAX_PROPS 64
static struct { char key[92], val[92]; } props[MAX_PROPS] = {
	{ "ro.hardware", "mt6885" },
	{ "ro.product.device", "cezanne" },
};
static pthread_mutex_t prop_lock = PTHREAD_MUTEX_INITIALIZER;

static void prop_set(const char *key, const char *val)
{
	int i;

	pthread_mutex_lock(&prop_lock);
	for (i = 0; i < MAX_PROPS && props[i].key[0]; i++)
		if (!strcmp(props[i].key, key))
			break;
	if (i < MAX_PROPS) {
		snprintf(props[i].key, sizeof(props[i].key), "%s", key);
		snprintf(props[i].val, sizeof(props[i].val), "%s", val);
	}
	pthread_mutex_unlock(&prop_lock);
}

static int prop_get(const char *key, char *val, size_t n)
{
	int i, ret = 0;

	val[0] = 0;
	pthread_mutex_lock(&prop_lock);
	for (i = 0; i < MAX_PROPS && props[i].key[0]; i++)
		if (!strcmp(props[i].key, key)) {
			snprintf(val, n, "%s", props[i].val);
			ret = strlen(val);
			break;
		}
	pthread_mutex_unlock(&prop_lock);
	return ret;
}

/* ---- helpers ---- */
static int mkdirs(const char *path, mode_t mode)
{
	char tmp[PATH_MAX], *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, mode);
			*p = '/';
		}
	return mkdir(tmp, mode);
}

static long copy_file(const char *src, const char *dst)
{
	char buf[65536];
	int in, out;
	ssize_t n;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -errno;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		n = -errno;
		close(in);
		return n;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n) {
			n = -EIO;
			break;
		}
	close(in);
	close(out);
	return n < 0 ? n : 0;
}

#define ARG32(b, i)	(*(int32_t *)((b) + 8 + 4 * (i)))
#define ARG64(b)	(*(uint64_t *)((b) + 8))

/* returns the reply length; reply status in *(int *)buf or *(u64 *)buf */
/*
 * RPMB. MIUI's vendor kernel provides /dev/rpmb0 (rpmb-mtk.c, GPL) and the
 * TEE drives it with VFS open + ioctl, the "SOTER" commands below; the ioctl
 * argument (at +0x1000) is u32 size + 512-byte RPMB frames, built and
 * authenticated (nonce, MAC) by the TEE. The same requests go to the UFS
 * RPMB well-known LU here (SCSI SECURITY PROTOCOL OUT / IN, protocol 0xec,
 * SPSP 1, through its bsg node), in the vendor driver's sequences:
 *   write data:  the frames (reliable write), a result read request, the
 *                response frame into frame 0 (the TEE checks it there)
 *   read data:   block count filled in (the TEE leaves it 0, as for eMMC),
 *                the request frame, then size / 512 frames back in place
 *   get counter: an own read counter request (random nonce), u32 back
 *   get wr size: the device's reliable write size in frames
 * MIUI's daemon library has the same operations as VFS funcs 0x300-0x306.
 * Programming the key is refused: the factory set it, once.
 */
#define RPMB_IOCTL_SOTER_WRITE_DATA	5
#define RPMB_IOCTL_SOTER_READ_DATA	6
#define RPMB_IOCTL_SOTER_GET_CNT	7
#define RPMB_IOCTL_SOTER_GET_WR_SIZE	8
#define RPMB_IOCTL_SOTER_SET_KEY	9
#define RPMB_BSG	"/dev/bsg/0:0:0:49476"	/* UFS W-LUN 0xc4 */
#define RPMB_RW_SIZE	"/sys/devices/platform/soc/11270000.ufshci/geometry_descriptor/rpmb_rw_size"
#define RPMB_FRAME	512
#define RPMB_MAX_DATA	(VFS_SIZE - PARAM_OFF - 4)
#define RPMB_REQ_COUNTER	0x0002
#define RPMB_REQ_RESULT		0x0005
/* big endian frame fields */
#define F_NONCE		484
#define F_WCOUNT	500
#define F_ADDR		504
#define F_BLOCKS	506
#define F_RESULT	508
#define F_REQ		510

static unsigned int get16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static void put16(uint8_t *p, unsigned int v) { p[0] = v >> 8; p[1] = v; }

static int is_rpmb(int fd)
{
	struct stat a, b;

	return !fstat(fd, &a) && !stat(RPMB_BSG, &b) && S_ISCHR(a.st_mode) &&
	       a.st_rdev == b.st_rdev;
}

static int rpmb_sec(int fd, int out, uint8_t *buf, uint32_t len)
{
	uint8_t cdb[12] = { out ? 0xb5 : 0xa2, 0xec, 0x00, 0x01, 0, 0,
			    len >> 24, len >> 16, len >> 8, len, 0, 0 };
	uint8_t sense[32] = { 0 };
	struct sg_io_v4 io = {
		.guard = 'Q',
		.protocol = BSG_PROTOCOL_SCSI,
		.subprotocol = BSG_SUB_PROTOCOL_SCSI_CMD,
		.request_len = sizeof(cdb),
		.request = (uintptr_t)cdb,
		.max_response_len = sizeof(sense),
		.response = (uintptr_t)sense,
		.timeout = 10000,
	};

	if (out) {
		io.dout_xfer_len = len;
		io.dout_xferp = (uintptr_t)buf;
	} else {
		io.din_xfer_len = len;
		io.din_xferp = (uintptr_t)buf;
	}
	for (int tries = 0; ; tries++) {
		if (ioctl(fd, SG_IO, &io) < 0)
			return -errno;
		/* the LU reports a unit attention (power on, reset: 6/29) once;
		 * the command was not executed, so it is safe to send it again */
		unsigned int key = (sense[0] & 0x7f) >= 0x72 ? sense[1] & 0xf : sense[2] & 0xf;

		if (io.device_status != 2 || key != 6 || tries == 3)
			break;
		memset(sense, 0, sizeof(sense));
	}
	if (io.device_status || io.transport_status || io.driver_status) {
		logm("rpmb: SECURITY PROTOCOL %s %u: status %#x/%#x/%#x sense %02x/%02x/%02x",
		     out ? "OUT" : "IN", len, io.device_status, io.transport_status,
		     io.driver_status, sense[2] & 0xf, sense[12], sense[13]);
		return -EIO;
	}
	return 0;
}

static int rpmb_counter(int fd, uint32_t *wc)
{
	uint8_t req[RPMB_FRAME] = { 0 }, res[RPMB_FRAME];
	int ret;

	if (getrandom(req + F_NONCE, 16, 0) != 16)
		return -EIO;
	put16(req + F_REQ, RPMB_REQ_COUNTER);
	ret = rpmb_sec(fd, 1, req, sizeof(req));
	if (!ret)
		ret = rpmb_sec(fd, 0, res, sizeof(res));
	if (ret)
		return ret;
	if ((get16(res + F_RESULT) & 0x7f) || memcmp(res + F_NONCE, req + F_NONCE, 16)) {
		logm("rpmb: counter read: result %#x", get16(res + F_RESULT));
		return -EIO;
	}
	*wc = (uint32_t)res[F_WCOUNT] << 24 | res[F_WCOUNT + 1] << 16 |
	      res[F_WCOUNT + 2] << 8 | res[F_WCOUNT + 3];
	return 0;
}

/* frames = size / 512 frames; returns 0 or -errno (RPMB results stay in the frames) */
static int rpmb_write(int fd, uint8_t *frames, uint32_t size)
{
	uint8_t res[RPMB_FRAME] = { 0 };
	int ret;

	ret = rpmb_sec(fd, 1, frames, size);
	put16(res + F_REQ, RPMB_REQ_RESULT);
	if (!ret)
		ret = rpmb_sec(fd, 1, res, sizeof(res));
	if (!ret)
		ret = rpmb_sec(fd, 0, res, sizeof(res));
	logm("rpmb: write %u blocks at %u -> %d, result %#x", get16(frames + F_BLOCKS),
	     get16(frames + F_ADDR), ret, get16(res + F_RESULT));
	memcpy(frames, res, sizeof(res));
	return ret;
}

static int rpmb_read(int fd, uint8_t *frames, uint32_t size)
{
	unsigned int addr = get16(frames + F_ADDR);
	int ret;

	put16(frames + F_BLOCKS, size / RPMB_FRAME);
	ret = rpmb_sec(fd, 1, frames, RPMB_FRAME);
	if (!ret)
		ret = rpmb_sec(fd, 0, frames, size);
	dbg("rpmb: read %u blocks at %u -> %d, result %#x", size / RPMB_FRAME, addr, ret,
	    get16(frames + size - RPMB_FRAME + F_RESULT));
	return ret;
}

static long rpmb_ioctl(int fd, unsigned int cmd, uint8_t *param)
{
	uint32_t size = *(uint32_t *)param, v = 0;
	char str[16] = "";
	int ret, f;

	if ((cmd == RPMB_IOCTL_SOTER_WRITE_DATA || cmd == RPMB_IOCTL_SOTER_READ_DATA) &&
	    (!size || size % RPMB_FRAME || size > RPMB_MAX_DATA))
		return -EINVAL;
	switch (cmd) {
	case RPMB_IOCTL_SOTER_WRITE_DATA:
		return rpmb_write(fd, param + 4, size);
	case RPMB_IOCTL_SOTER_READ_DATA:
		return rpmb_read(fd, param + 4, size);
	case RPMB_IOCTL_SOTER_GET_CNT:
		ret = rpmb_counter(fd, &v);
		if (!ret)
			memcpy(param, &v, 4);
		dbg("rpmb: write counter %u -> %d", v, ret);
		return ret;
	case RPMB_IOCTL_SOTER_GET_WR_SIZE:
		f = open(RPMB_RW_SIZE, O_RDONLY);
		if (f < 0 || read(f, str, sizeof(str) - 1) <= 0)
			str[0] = 0;
		if (f >= 0)
			close(f);
		v = strtoul(str, NULL, 0);
		if (!v)
			v = 1;
		memcpy(param, &v, 4);
		return 0;
	case RPMB_IOCTL_SOTER_SET_KEY:
		logm("rpmb: refusing to program the RPMB key");
		return -EPERM;
	}
	logm("rpmb: ioctl %u unsupported", cmd);
	return -ENOTTY;
}

static int handle(char *buf)
{
	uint32_t func = *(uint32_t *)buf;
	char *param = buf + PARAM_OFF;
	char path[PATH_MAX], path2[PATH_MAX];
	long ret = 0;
	int len = 8;

	dbg("req func %u size %d args %#x %#x %#x %#x", func, *(int32_t *)(buf + 4),
	    ARG32(buf, 0), ARG32(buf, 1), ARG32(buf, 2), ARG32(buf, 3));
	switch (func) {
	case VFS_OPEN: {
		int flags = ARG32(buf, 0), mode = ARG32(buf, 1);

		if (!strcmp(param, "/dev/rpmb0")) {
			ret = open(RPMB_BSG, O_RDWR | O_CLOEXEC);
			if (ret < 0)
				ret = -errno;
			logm("open /dev/rpmb0 (%s) -> %ld", RPMB_BSG, ret);
			break;
		}
		map_path(param, path, sizeof(path));
		if (flags & O_CREAT) {
			char *slash;

			snprintf(path2, sizeof(path2), "%s", path);
			slash = strrchr(path2, '/');
			if (slash) {
				*slash = 0;
				mkdirs(path2, 0700);
			}
		}
		ret = open(path, flags | O_CLOEXEC, mode);
		if (ret < 0)
			ret = -errno;
		dbg("open %s flags %#x mode %#o -> %ld", path, flags, mode, ret);
		break;
	}
	case VFS_READ:
		ret = read(ARG32(buf, 0), param, ARG32(buf, 1));
		if (ret < 0)
			ret = -errno;
		dbg("read fd %d count %d -> %ld", ARG32(buf, 0), ARG32(buf, 1), ret);
		len = PARAM_OFF + (ret > 0 ? ret : 0);
		break;
	case VFS_WRITE:
		ret = write(ARG32(buf, 0), param, ARG32(buf, 1));
		if (ret < 0)
			ret = -errno;
		dbg("write fd %d count %d -> %ld", ARG32(buf, 0), ARG32(buf, 1), ret);
		break;
	case VFS_IOCTL:
		if (is_rpmb(ARG32(buf, 0))) {
			ret = rpmb_ioctl(ARG32(buf, 0), ARG32(buf, 1), (uint8_t *)param);
			len = 0xa000;
			break;
		}
		/* 0x70/0x71/0x75: DRM/keybox notifications to the vendor kernel */
		ret = -ENOTTY;
		logm("ioctl fd %d cmd %#x arg %#x: unsupported", ARG32(buf, 0),
		     ARG32(buf, 1), ARG32(buf, 2));
		break;
	case VFS_CLOSE:
		ret = close(ARG32(buf, 0)) ? -errno : 0;
		dbg("close fd %d -> %ld", ARG32(buf, 0), ret);
		break;
	case VFS_TRUNC:
		ret = ftruncate(ARG32(buf, 0), ARG32(buf, 1)) ? -errno : 0;
		break;
	case VFS_UNLINK:
		map_path(param, path, sizeof(path));
		ret = unlink(path) ? -errno : 0;
		dbg("unlink %s -> %ld", path, ret);
		break;
	case VFS_LSEEK:
		ret = lseek(ARG32(buf, 0), ARG32(buf, 1), ARG32(buf, 2));
		if (ret < 0)
			ret = -errno;
		dbg("lseek fd %d off %d whence %d -> %ld", ARG32(buf, 0),
		    ARG32(buf, 1), ARG32(buf, 2), ret);
		break;
	case VFS_RENAME:
	case VFS_COPY:
		map_path(param, path, sizeof(path));
		map_path(param + strlen(param) + 1, path2, sizeof(path2));
		if (func == VFS_RENAME)
			ret = rename(path, path2) ? -errno : 0;
		else
			ret = copy_file(path, path2);
		dbg("%s %s %s -> %ld", func == VFS_RENAME ? "rename" : "copy",
		    path, path2, ret);
		break;
	case VFS_MKDIR:
		map_path(param, path, sizeof(path));
		ret = mkdirs(path, ARG32(buf, 0)) && errno != EEXIST ? -errno : 0;
		dbg("mkdir %s -> %ld", path, ret);
		break;
	case VFS_RMDIR:
		map_path(param, path, sizeof(path));
		ret = rmdir(path) ? -errno : 0;
		break;
	case VFS_OPENDIR: {
		DIR *d;

		map_path(param, path, sizeof(path));
		d = opendir(path);
		dbg("opendir %s -> %p", path, (void *)d);
		memset(buf, 0, 8);
		*(uint64_t *)buf = (uintptr_t)d;
		return 8;
	}
	case VFS_READDIR: {
		/* struct dirent64 records (280 bytes, same as bionic) at +0x1000 */
		DIR *d = (DIR *)(uintptr_t)ARG64(buf);
		uint32_t want = *(uint32_t *)(buf + 16), n = 0;
		struct dirent64 *e;

		while (d && n < want && (n + 1) * sizeof(*e) <= VFS_SIZE - PARAM_OFF &&
		       (e = readdir64(d)))
			memcpy(param + n++ * sizeof(*e), e, sizeof(*e));
		ret = n;
		len = PARAM_OFF + n * sizeof(*e);
		break;
	}
	case VFS_CLOSEDIR:
		ret = ARG64(buf) ? closedir((DIR *)(uintptr_t)ARG64(buf)) : -EBADF;
		break;
	case VFS_PRONODE: {
		/* where the TEE reads proinfo (device serial etc.) from */
		static const char node[] = "/dev/disk/by-partlabel/proinfo";

		memcpy(param, node, sizeof(node));
		ret = sizeof(node) - 1;
		len = PARAM_OFF + ret;
		break;
	}
	case VFS_SETPROP: {
		char key[128];

		snprintf(key, sizeof(key), "vendor.soter.teei.%s", param);
		prop_set(key, param + strlen(param) + 1);
		dbg("setprop %s=%s", key, param + strlen(param) + 1);
		ret = 0;
		break;
	}
	case VFS_GETPROP: {
		char val[92], key[92];

		snprintf(key, sizeof(key), "%s", param);
		ret = prop_get(key, val, sizeof(val));
		memset(param, 0, sizeof(val));
		memcpy(param, val, ret);
		dbg("getprop %s -> %s", key, val);
		len = PARAM_OFF + ret;
		break;
	}
	case VFS_RPMB_OPEN:
		ret = open(RPMB_BSG, O_RDWR | O_CLOEXEC);
		if (ret < 0)
			ret = -errno;
		logm("rpmb open -> %ld", ret);
		break;
	case VFS_RPMB_GET_CNT: {
		uint32_t wc = 0;

		ret = rpmb_counter(ARG32(buf, 0), &wc);
		memcpy(param, &wc, 4);
		len = 0xa000;
		break;
	}
	case VFS_RPMB_WRITE_KEY:
		logm("rpmb: refusing to program the RPMB key");
		ret = -EPERM;
		len = 0xa000;
		break;
	case VFS_RPMB_READ2: {
		uint32_t size = *(uint32_t *)param;

		ret = size && !(size % RPMB_FRAME) && size <= RPMB_MAX_DATA ?
		      rpmb_read(ARG32(buf, 0), (uint8_t *)param + 4, size) : -EINVAL;
		len = 0xa000;
		break;
	}
	case VFS_RPMB_WRITE: {
		/* the frame count is the first frame's block count */
		uint32_t size = get16((uint8_t *)param + 4 + F_BLOCKS) * RPMB_FRAME;

		ret = size && size <= RPMB_MAX_DATA ?
		      rpmb_write(ARG32(buf, 0), (uint8_t *)param + 4, size) : -EINVAL;
		len = 0xa000;
		break;
	}
	case VFS_RPMB_READ:	/* a no-op in MIUI's library, no reply data */
		ret = 0;
		break;
	case VFS_RPMB_CLOSE:
		ret = close(ARG32(buf, 0)) ? -errno : 0;
		break;
	default:
		logm("vfs func %#x (size %d): unsupported", func, *(int *)(buf + 4));
		ret = -ENOSYS;
		break;
	}
	memset(buf, 0, 8);
	*(int32_t *)buf = ret;
	dbg("  reply %ld len %d", ret, len);
	return len;
}

static void *init_thread(void *arg)
{
	struct init_param p;
	unsigned int i, n = 0;
	int fd, ret;

	(void)arg;
	memset(&p, 0, sizeof(p));
	for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++)
		memcpy(p.uuids[n++], drivers[i], UUID_LEN);
	for (i = 0; i < sizeof(tas) / sizeof(tas[0]); i++) {
		p.flag |= 1u << n;
		memcpy(p.uuids[n++], tas[i], UUID_LEN);
	}
	memcpy(p.uuids[n++], default_driver, UUID_LEN);
	p.uuid_count = n;

	fd = open("/dev/teei_config", O_RDWR);
	if (fd < 0) {
		logm("open /dev/teei_config: %s", strerror(errno));
		return NULL;
	}
	logm("booting the TEE, %u drivers/TAs", n);
	ret = ioctl(fd, TEEI_CONFIG_IOCTL_INIT_TEEI, &p);
	logm("TEE init: ret %d, flag %u", ret, p.flag);
	prop_set("vendor.soter.teei.init", ret ? "INIT_FAIL" : "INIT_OK");
	close(fd);
	return NULL;
}

/* MIUI waits for vold to decrypt /data first; our storage is always there */
static void *unlock_thread(void *arg)
{
	int fd, zero = 0;

	(void)arg;
	fd = open("/dev/teei_config", O_RDWR);
	if (fd < 0)
		return NULL;
	if (ioctl(fd, TEEI_CONFIG_IOCTL_UNLOCK, &zero))
		logm("unlock: %s", strerror(errno));
	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t1, t2;
	char *buf;
	int opt, fd;

	while ((opt = getopt(argc, argv, "vr:c")) != -1) {
		if (opt == 'v')
			verbose = 1;
		else if (opt == 'r')
			root = optarg;
		else if (opt == 'c') {
			uint32_t wc;
			int r = open(RPMB_BSG, O_RDWR);

			if (r < 0 || rpmb_counter(r, &wc)) {
				logm("rpmb: no write counter from %s", RPMB_BSG);
				return 1;
			}
			printf("RPMB write counter %u\n", wc);
			return 0;
		} else {
			fprintf(stderr, "usage: %s [-v] [-r root] | -c\n", argv[0]);
			return 1;
		}
	}
	prctl(PR_SET_NAME, "teei_daemon");	/* tz_vfs checks current->comm */
	buf = malloc(VFS_SIZE);
	if (!buf)
		return 1;
	fd = open("/dev/tz_vfs", O_RDWR);
	if (fd < 0) {
		logm("open /dev/tz_vfs: %s", strerror(errno));
		return 1;
	}
	pthread_create(&t1, NULL, init_thread, NULL);
	pthread_create(&t2, NULL, unlock_thread, NULL);

	for (;;) {
		ssize_t n = read(fd, buf, VFS_SIZE);
		int len;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			logm("read /dev/tz_vfs: %s", strerror(errno));
			usleep(10000);
			continue;
		}
		len = handle(buf);
		if (write(fd, buf, len) < 0)
			logm("write /dev/tz_vfs: %s", strerror(errno));
	}
}
