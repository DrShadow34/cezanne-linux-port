// SPDX-License-Identifier: MIT
/*
 * cezanne-modemd: userspace side of the cezanne modem (kernel driver
 * cezanne-ccci). Serves the MD's NVRAM file service and RPC requests, then
 * boots the MD and keeps serving; logs system / status messages. Once the MD
 * is ready it powers the SIM (vendor init + AT+CFUN=4) on the AT port and
 * then hands that port and the data links to ModemManager (udev rule
 * 78-cezanne-modem.rules).
 *
 *   cezanne-modemd [--root DIR] [--no-boot] [--run SECONDS] [-v]
 *
 * CCCI packets as the cezanne-ccci driver passes them on its user-header
 * ports: a 16-byte header (u32 data[0], u32 length, u16 channel, u16 seq,
 * u32 reserved) and the payload. Requests and replies of the FS and RPC
 * services carry an op word and a list of arguments, each u32 length +
 * data padded to 4 bytes.
 *
 * The file service (CCCI channels 14 / 15, /dev/ccci_fs): the MD keeps its
 * NVRAM (calibration, IMEI, settings) and configuration files on the AP and
 * reaches them with FAT-style calls on drive letters. Behaviour (paths,
 * flags, error codes, reply layouts) follows MediaTek's ccci_fsd as seen in
 * MIUI 14's ccci_mdinit; see NOTES.md "Modem file service". Request: header
 * (reserved = request slot), u32 op, u32 argc, arguments. Reply: u32 op |
 * 0xffff0000, u32 count, results. Payloads longer than one packet are split
 * in 0xd80-byte chunks, each packet carrying the op word again, data[0]
 * bit 31 set on all but the last.
 *
 * The RPC requests (CCCI channels 32 / 33, /dev/ccci_rpc): board facts (GPIO
 * numbers and levels, SIM hot-plug EINT setup, device tree attributes,
 * system properties), the same layout; reply op | 0xffff0000. Semantics
 * follow MediaTek's port_rpc.c (kernel side ops) and ccci_rpcd (0x400f /
 * 0x4010).
 *
 * This is the C version of the Python daemon of the same name (same
 * replies byte for byte, same files, same log lines); a request too short to
 * hold its op word, which crashed the Python one, is logged and dropped.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define SYS		"/sys/devices/platform/cezanne-ccci"
#define AT_PORT		"/dev/wwan0at0"
#define AT_READY	"/run/cezanne-modem/at-ready"
#define AT_TIMEOUT	10
#define MORE		0x80000000u	/* data[0]: more fragments follow */
#define CHUNK		0xd80
#define FW		"/usr/lib/firmware/mediatek/cezanne/"
#define MAX_FILES	128
#define MAX_PATH	0x1000

/* FS error codes (ccci_fsd FS_ErrorConv and friends) */
#define E_PARAM		-2
#define E_TOO_MANY	-5
#define E_NO_MORE	-6
#define E_NOT_FOUND	-9
#define E_HANDLE	-10
#define E_ACCESS	-16
#define E_GENERIC	-18
#define E_DRIVE		-19
#define E_PATH_LONG	-49

/* RPC */
#define FS_NO_OP	-1
#define FS_PARAM_ERROR	-2
#define FS_FUNC_FAIL	-5
#define ERR_QUERY_STRING -11
#define NO_ATTR		-12

/*
 * MIUI's RIL sends ESIMS / EBOOT before powering the radio; CFUN=4 then
 * brings the SIM up with RF off, the low-power state ModemManager expects.
 * (command, seconds to keep retrying it): the AT task starts a moment after
 * the handshake, the SMS task (CMGF) only after the SIM is up.
 */
static const struct {
	const char *cmd;
	int retry;
} at_init[] = {
	{ "AT", 30 }, { "AT+CMEE=1", 0 }, { "AT+ESIMS=1", 0 }, { "AT+EBOOT=0", 0 },
	{ "AT+CFUN=4", 0 }, { "AT+CMGF=0", 30 },
};
#define AT_STEPS	(int)(sizeof(at_init) / sizeof(*at_init))

static int verbose;

static void log_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);	/* Python time.time() */
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- byte buffers ------------------------------------------------------ */

struct buf {
	unsigned char *p;
	size_t len, cap;
};

static void buf_grow(struct buf *b, size_t n)
{
	if (b->len + n <= b->cap)
		return;
	b->cap = (b->len + n) * 2 + 64;
	b->p = realloc(b->p, b->cap);
	if (!b->p) {
		perror("realloc");
		exit(1);
	}
}

static void buf_add(struct buf *b, const void *p, size_t n)
{
	buf_grow(b, n);
	if (n)
		memcpy(b->p + b->len, p, n);
	b->len += n;
}

static void buf_u32(struct buf *b, uint32_t v)
{
	unsigned char x[4] = { v, v >> 8, v >> 16, v >> 24 };

	buf_add(b, x, 4);
}

static uint32_t get_u32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_u32(unsigned char *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

/* an argument or a result: bytes (NULL p with len 0 is b'') */
struct arg {
	const unsigned char *p;
	size_t len;
};

/* results of one reply: copies of the bytes (buffers kept for reuse) */
struct res {
	struct buf *items;
	int n, cap;
};

static struct buf *res_new(struct res *r)
{
	struct buf *b;

	if (r->n == r->cap) {
		r->items = realloc(r->items, (r->cap + 8) * sizeof(*r->items));
		if (!r->items) {
			perror("realloc");
			exit(1);
		}
		memset(r->items + r->cap, 0, 8 * sizeof(*r->items));
		r->cap += 8;
	}
	b = &r->items[r->n++];
	b->len = 0;
	return b;
}

static void res_u32(struct res *r, int64_t v)
{
	buf_u32(res_new(r), (uint32_t)v);
}

static void res_bytes(struct res *r, const void *p, size_t n)
{
	buf_add(res_new(r), p, n);
}

static void res_clear(struct res *r)
{
	r->n = 0;
}

/* ---- "exceptions": a bad argument ends the op (Python TypeError etc.) --- */

static jmp_buf *bad_jmp;
static char bad_msg[160];

static void __attribute__((noreturn)) bad_args(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(bad_msg, sizeof(bad_msg), fmt, ap);
	va_end(ap);
	longjmp(*bad_jmp, 1);
}

/* i32(b): the first 4 bytes, signed */
static int32_t i32(const struct arg *a)
{
	if (a->len < 4)
		bad_args("unpack requires a buffer of 4 bytes");
	return (int32_t)get_u32(a->p);
}

/* argument i, or the op's TypeError */
#define ARG(i)	((i) < argc ? &args[i] : (bad_args("missing argument %d", (i) + 1), &args[0]))

/* ---- the argument list ------------------------------------------------- */

/* count arguments from buf at off; NULL (msg set) when truncated */
static struct arg *lv_parse(const unsigned char *buf, size_t len, uint32_t count,
			    size_t off, int *argc, const char **msg)
{
	struct arg *a = NULL;
	uint32_t i;

	*argc = 0;
	for (i = 0; i < count; i++) {
		uint32_t n;

		if (off + 4 > len) {
			*msg = "argument list truncated";
			free(a);
			return NULL;
		}
		n = get_u32(buf + off);
		if (off + 4 + (uint64_t)n > len) {
			*msg = "argument longer than packet";
			free(a);
			return NULL;
		}
		a = realloc(a, (i + 1) * sizeof(*a));
		a[i] = (struct arg){ buf + off + 4, n };
		off += 4 + (((uint64_t)n + 3) & ~(uint64_t)3);
	}
	*argc = count;
	return a ? a : malloc(1);
}

static void lv_pack(struct buf *out, const struct res *r)
{
	static const unsigned char zero[4];
	int i;

	for (i = 0; i < r->n; i++) {
		buf_u32(out, r->items[i].len);
		buf_add(out, r->items[i].p, r->items[i].len);
		buf_add(out, zero, -r->items[i].len & 3);
	}
}

/* a packet: header (the driver fills in length and channel) + payload */
static void make_pkt(struct buf *out, uint32_t d0, uint32_t rsv, const void *p, size_t n)
{
	unsigned char h[16];

	put_u32(h, d0);
	put_u32(h + 4, 16 + n);
	h[8] = h[9] = h[10] = h[11] = 0;	/* channel 0, seq 0 */
	put_u32(h + 12, rsv);
	out->len = 0;
	buf_add(out, h, 16);
	buf_add(out, p, n);
}

/* ---- text: Python's decodings ------------------------------------------ */

/* append code point c as UTF-8 */
static void utf8_put(struct buf *b, uint32_t c)
{
	unsigned char x[4];
	int n;

	if (c < 0x80) {
		x[0] = c;
		n = 1;
	} else if (c < 0x800) {
		x[0] = 0xc0 | c >> 6;
		x[1] = 0x80 | (c & 0x3f);
		n = 2;
	} else if (c < 0x10000) {
		x[0] = 0xe0 | c >> 12;
		x[1] = 0x80 | (c >> 6 & 0x3f);
		x[2] = 0x80 | (c & 0x3f);
		n = 3;
	} else {
		x[0] = 0xf0 | c >> 18;
		x[1] = 0x80 | (c >> 12 & 0x3f);
		x[2] = 0x80 | (c >> 6 & 0x3f);
		x[3] = 0x80 | (c & 0x3f);
		n = 4;
	}
	buf_add(b, x, n);
}

/*
 * wstr(): the MD's UTF-16LE path argument, decoded with errors='replace'
 * and cut at the first NUL; code points into cp (count returned)
 */
static size_t utf16_decode(const struct arg *a, uint32_t *cp, size_t max)
{
	size_t i = 0, n = 0;

	while (i < a->len && n < max) {
		uint32_t u, v;

		if (i + 1 >= a->len) {		/* a lone trailing byte */
			cp[n++] = 0xfffd;
			break;
		}
		u = a->p[i] | a->p[i + 1] << 8;
		i += 2;
		if (u >= 0xd800 && u < 0xdc00) {
			if (i + 1 < a->len) {
				v = a->p[i] | a->p[i + 1] << 8;
				if (v >= 0xdc00 && v < 0xe000) {
					i += 2;
					u = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
				} else {
					u = 0xfffd;
				}
			} else {
				u = 0xfffd;
				/* the odd byte after it is replaced too */
				if (i < a->len) {
					cp[n++] = u;
					u = 0xfffd;
					i = a->len;
				}
			}
		} else if (u >= 0xdc00 && u < 0xe000) {
			u = 0xfffd;
		}
		if (!u)
			break;
		cp[n++] = u;
	}
	return n;
}

static void cps_to_utf8(struct buf *b, const uint32_t *cp, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		utf8_put(b, cp[i]);
}

/* a file name from readdir as code points (surrogateescape: bytes >= 0x80
 * that are no valid UTF-8 become U+DC80..U+DCFF) */
static size_t fsname_decode(const char *s, uint32_t *cp, size_t max)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t n = 0;

	while (*p && n < max) {
		uint32_t c = *p;
		int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 :
			(c & 0xf8) == 0xf0 ? 3 : -1, j;
		uint32_t v;

		if (k > 0) {
			v = c & (0x3f >> k);
			for (j = 1; j <= k; j++) {
				if ((p[j] & 0xc0) != 0x80)
					break;
				v = v << 6 | (p[j] & 0x3f);
			}
			if (j > k && !((k == 1 && v < 0x80) || (k == 2 && v < 0x800) ||
				       (k == 3 && (v < 0x10000 || v > 0x10ffff)) ||
				       (v >= 0xd800 && v < 0xe000))) {
				cp[n++] = v;
				p += k + 1;
				continue;
			}
			k = -1;
		}
		if (k < 0) {
			cp[n++] = 0xdc00 + *p++;
			continue;
		}
		cp[n++] = c;
		p++;
	}
	return n;
}

/* str.upper() for what NVRAM names hold (ASCII, Latin-1 letters) */
static uint32_t cp_upper(uint32_t c)
{
	if (c >= 'a' && c <= 'z')
		return c - 32;
	if ((c >= 0xe0 && c <= 0xfe && c != 0xf7))
		return c - 32;
	if (c == 0xff)
		return 0x178;
	if (c == 0xb5)
		return 0x39c;
	return c;
}

/* cstr(): up to the first NUL, decoded as ASCII with errors='replace' */
static void cstr(const struct arg *a, size_t from, size_t to, char *out, size_t size)
{
	struct buf b = { 0 };
	size_t i;

	for (i = from; i < to && i < a->len && a->p[i]; i++) {
		if (a->p[i] < 0x80)
			buf_add(&b, &a->p[i], 1);
		else
			utf8_put(&b, 0xfffd);
	}
	buf_add(&b, "", 1);
	snprintf(out, size, "%s", (char *)b.p);
	free(b.p);
}

/* ---- the board (stock device tree of the Redmi K30 Ultra) -------------- */

/*
 * What the MD asks the AP about the board (RPC), as the stock device tree
 * describes it (nodes gpio_usage_mapping, md_attr_node,
 * md1_sim*_hot_plug_eint of MIUI 14's FDT).
 *
 * GPIO name -> (AP pin, MD view id); the MD gets the MD view id
 */
static const struct {
	const char *name;
	int pin, md;
} gpios[] = {
	{ "GPIO_SIM1_HOT_PLUG", 43, 43 },
	{ "GPIO_SIM2_SCLK", 45, 45 },
	{ "GPIO_SIM2_SRST", 46, 46 },
	{ "GPIO_SIM2_SIO", 47, 47 },
	{ "GPIO_SIM1_SIO", 48, 48 },
	{ "GPIO_SIM1_SRST", 49, 49 },
	{ "GPIO_SIM1_SCLK", 50, 50 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_1ST_PIN", 63, 63 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_2ND_PIN", 64, 64 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_3RD_PIN", 65, 65 },
	{ "GPIO_FDD_BAND_SUPPORT_DETECT_4TH_PIN", 66, 66 },
	{ NULL, 0, 0 },
};

/* names the MD uses for some of them (BSP port_rpc.c gpio_mapping_table) */
static const char *gpio_alias(const char *name, char *buf, size_t size)
{
	static const char *const ord[] = { "1ST", "2ND", "3RD", "4TH", "5TH", "6TH",
					   "7TH", "8TH", "9TH", "ATH" };
	static const char keys[] = "123456789A";
	const char *pre = "GPIO_FDD_Band_Support_Detection_";
	size_t n = strlen(pre);

	if (!strncmp(name, pre, n) && name[n] && !name[n + 1] && strchr(keys, name[n])) {
		snprintf(buf, size, "GPIO_FDD_BAND_SUPPORT_DETECT_%s_PIN",
			 ord[strchr(keys, name[n]) - keys]);
		return buf;
	}
	return name;
}

static int gpio_lookup(const char *name, int *pin, int *md)
{
	char buf[64];
	int i;

	name = gpio_alias(name, buf, sizeof(buf));
	for (i = 0; gpios[i].name; i++)
		if (!strcmp(gpios[i].name, name)) {
			*pin = gpios[i].pin;
			*md = gpios[i].md;
			return 1;
		}
	return 0;
}

/*
 * SIM hot-plug EINT attributes by query type: number, debounce (ms),
 * polarity, sensitivity, socket type, dedicated, source pin. SIM2's node
 * exists without properties (every query fails with -12).
 */
static const struct {
	const char *name;
	int v[7];
} eints[] = {
	{ "MD1_SIM1_HOT_PLUG_EINT", { 0, 50, 0, 0, 0, 0, 1 } },
	{ "MD1_SIM2_HOT_PLUG_EINT", { NO_ATTR, NO_ATTR, NO_ATTR, NO_ATTR, NO_ATTR,
				      NO_ATTR, NO_ATTR } },
	{ NULL, { 0 } },
};

/* md_attr_node */
static const struct {
	const char *name;
	uint32_t v;
} dtsi[] = {
	{ "mediatek,md_product_name_model_id", 0x0f0f0f0f },
	{ "mediatek,md_drdi_rf_set_idx", 0x0c },
	{ NULL, 0 },
};

/* Android system properties the MD may ask for (IPC_RPC_QUERY_AP_SYS_PROPERTY) */
static const struct {
	const char *key, *v;
} properties[] = {
	{ "ro.product.vendor.name", "cezanne" },
	{ "ro.product.vendor.device", "cezanne" },
	{ "ro.product.name", "cezanne" },
	{ "ro.product.device", "cezanne" },
	{ "ro.product.model", "M2006J10C" },
	{ "ro.product.brand", "Redmi" },
	{ "ro.product.manufacturer", "Xiaomi" },
	{ NULL, NULL },
};

/*
 * input level of an AP pin from the pinctrl debugfs, or -1: the first line
 * matching \s*(\d+):.*?din\s*[:=]?\s*(\d) for the pin
 */
static int gpio_level(long long pin)
{
	glob_t g;
	size_t i;
	int level = -1;

	if (glob("/sys/kernel/debug/pinctrl/*pinctrl*/pins", 0, NULL, &g))
		return -1;
	for (i = 0; i < g.gl_pathc && level < 0; i++) {
		FILE *f = fopen(g.gl_pathv[i], "re");
		char *line = NULL;
		size_t cap = 0;

		if (!f)
			continue;
		while (level < 0 && getline(&line, &cap, f) > 0) {
			char *p = line, *q;
			long long n = 0;

			while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
			       *p == '\f' || *p == '\v')
				p++;
			if (*p < '0' || *p > '9')
				continue;
			while (*p >= '0' && *p <= '9') {
				n = n * 10 + (*p - '0');
				if (n > LLONG_MAX / 10)
					n = LLONG_MAX / 10;
				p++;
			}
			if (*p++ != ':')
				continue;
			for (q = strstr(p, "din"); q; q = strstr(q + 1, "din")) {
				char *r = q + 3;

				while (*r == ' ' || *r == '\t' || *r == '\f' || *r == '\v')
					r++;
				if (*r == ':' || *r == '=')
					r++;
				while (*r == ' ' || *r == '\t' || *r == '\f' || *r == '\v')
					r++;
				if (*r >= '0' && *r <= '9') {
					if (n == pin)
						level = *r - '0';
					break;
				}
			}
		}
		free(line);
		fclose(f);
	}
	globfree(&g);
	return level;
}

/* ---- RPC --------------------------------------------------------------- */

static void rpc_gpio_adc(const struct arg *req, struct res *r);

static void rpc_op(uint32_t opc, struct arg *args, int argc, struct res *r)
{
	char name[160];
	int pin, md, i;

	switch (opc) {
	case 0x4001:	/* GET_TDD_EINT_NUM: no TDD EINTs on this board */
	case 0x4003:	/* GET_ADC_NUM: no MD ADC channels */
		cstr(ARG(0), 0, SIZE_MAX, name, sizeof(name));
		log_msg("rpc: %s: none", name);
		res_u32(r, FS_FUNC_FAIL);
		res_u32(r, FS_FUNC_FAIL);
		break;
	case 0x4002: {
		int n;

		cstr(ARG(0), 0, SIZE_MAX, name, sizeof(name));
		n = gpio_lookup(name, &pin, &md) ? md : FS_FUNC_FAIL;
		log_msg("rpc: GPIO %s -> %d", name, n);
		res_u32(r, n);
		res_u32(r, n);
		break;
	}
	case 0x4004:	/* GET_EMI_CLK_TYPE: not provided (as stock) */
		for (i = 0; i < 3; i++)
			res_u32(r, FS_FUNC_FAIL);
		break;
	case 0x4005: {
		int t, k, v;

		ARG(2);
		cstr(ARG(0), 0, SIZE_MAX, name, sizeof(name));
		t = i32(ARG(2));
		for (k = 0; eints[k].name && strcmp(eints[k].name, name); k++)
			;
		if (!eints[k].name || t < 0 || t >= 7) {
			v = eints[k].name ? -12 : ERR_QUERY_STRING;
			res_u32(r, v);
			res_u32(r, v);
			break;
		}
		v = eints[k].v[t];
		log_msg("rpc: EINT %s attr %d -> %d", name, t, v);
		if (v < 0) {
			res_u32(r, ERR_QUERY_STRING);
			res_u32(r, ERR_QUERY_STRING);
		} else {
			res_u32(r, 0);
			res_u32(r, v);
		}
		break;
	}
	case 0x4006: {
		int32_t num = i32(ARG(0));
		int v = gpio_level(num);

		log_msg("rpc: GPIO %d level %d", num, v);
		res_u32(r, v);
		res_u32(r, v);
		break;
	}
	case 0x4007:	/* GET_ADC_VAL */
		ARG(0);
		res_u32(r, -1);
		res_u32(r, -1);
		break;
	case 0x4009:
		rpc_gpio_adc(ARG(0), r);
		break;
	case 0x400d:	/* CCCI_LHIF_MAPPING: no ccmni queue remap needed yet */
		res_u32(r, 0);
		res_u32(r, 0);
		break;
	case 0x400e: {
		const struct arg *req = ARG(0);
		unsigned char out[68];

		cstr(req, 2, 66, name, sizeof(name));
		memset(out, 0x0f, sizeof(out));
		if (!req->len)
			bad_args("index out of range");
		if (req->p[0] == 1)
			for (i = 0; dtsi[i].name; i++)
				if (!strcmp(dtsi[i].name, name))
					put_u32(out, dtsi[i].v);
		log_msg("rpc: DTSI %s -> 0x%x", name, get_u32(out));
		res_u32(r, 0);
		res_bytes(r, out, sizeof(out));
		break;
	}
	case 0x400f: {
		const char *v = "";

		cstr(ARG(0), 0, SIZE_MAX, name, sizeof(name));
		for (i = 0; properties[i].key; i++)
			if (!strcmp(properties[i].key, name))
				v = properties[i].v;
		log_msg("rpc: property %s = \"%s\"", name, v);
		res_u32(r, strlen(v));
		res_bytes(r, v, strlen(v) + 1);
		break;
	}
	case 0x4010:	/* SAR_TABLE_IDX_QUERY: no SAR sensor input; MD keeps its default */
		res_u32(r, 0);
		res_u32(r, 0);
		break;
	case 0x4013:	/* QUERY_CARD_TYPE: the stock kernel echoes the request */
		for (i = 0; i < argc; i++)
			res_bytes(r, args[i].p, args[i].len);
		break;
	case 0x4321:	/* IT_OP: loopback test */
		res_u32(r, 1);
		res_u32(r, 0xa5a5);
		break;
	default:
		r->n = -1;	/* unknown */
	}
}

/* Python's repr of a str (the names are ASCII or U+FFFD) */
static void py_repr(struct buf *b, const char *s)
{
	char q = strchr(s, '\'') && !strchr(s, '"') ? '"' : '\'';

	buf_add(b, &q, 1);
	for (; *s; s++) {
		char x[8];

		if (*s == q || *s == '\\') {
			x[0] = '\\';
			x[1] = *s;
			buf_add(b, x, 2);
		} else if (*s == '\t' || *s == '\n' || *s == '\r') {
			x[0] = '\\';
			x[1] = *s == '\t' ? 't' : *s == '\n' ? 'n' : 'r';
			buf_add(b, x, 2);
		} else if ((unsigned char)*s < 0x20 || *s == 0x7f) {
			snprintf(x, sizeof(x), "\\x%02x", (unsigned char)*s);
			buf_add(b, x, 4);
		} else {
			buf_add(b, s, 1);
		}
	}
	buf_add(b, &q, 1);
}

/* pin numbers / levels for up to 3 (v1) or 10 (v2) named GPIOs */
static void rpc_gpio_adc(const struct arg *req, struct res *r)
{
	int v2 = req->len != 2 + 3 * 34 + 12 + 33 + 8, n = v2 ? 10 : 3, i, pin, md;
	size_t off = v2 ? 4 : 2;
	unsigned int mask, valid;
	char names[10][160];
	uint32_t nums[10], out_num[10] = { 0 }, out_val[10] = { 0 };
	struct buf lg = { 0 }, pk = { 0 };

	if (req->len < off)
		bad_args("unpack_from requires a buffer of at least %zu bytes", off);
	if (v2) {
		mask = req->p[0] | req->p[1] << 8;
		valid = req->p[2] | req->p[3] << 8;
	} else {
		mask = req->p[0];
		valid = req->p[1];
	}
	for (i = 0; i < n; i++)
		cstr(req, off + 34 * i, off + 34 * (i + 1), names[i], sizeof(names[i]));
	if (req->len < off + 34 * n + 4 * n)
		bad_args("unpack_from requires a buffer of at least %zu bytes",
			 off + 34 * n + 4 * n);
	for (i = 0; i < n; i++)
		nums[i] = get_u32(req->p + off + 34 * n + 4 * i);
	for (i = 0; i < n; i++) {
		int g;

		if (!(valid & 1u << i))
			continue;
		g = gpio_lookup(names[i], &pin, &md);
		if (mask & 1 && g)
			out_num[i] = md;
		if (mask & 2)
			out_val[i] = (uint32_t)gpio_level(mask & 1 && g ? pin : (long long)nums[i]);
	}
	/* 'rpc: GPIO_ADC %s -> %s %s' with Python list reprs */
	buf_add(&lg, "[", 1);
	for (i = 0, md = 0; i < n; i++) {
		if (!names[i][0])
			continue;
		if (md++)
			buf_add(&lg, ", ", 2);
		py_repr(&lg, names[i]);
	}
	buf_add(&lg, "] -> [", 6);
	for (i = 0; i < n; i++) {
		char x[16];

		snprintf(x, sizeof(x), "%s%u", i ? ", " : "", out_num[i]);
		buf_add(&lg, x, strlen(x));
	}
	buf_add(&lg, "] [", 3);
	for (i = 0; i < n; i++) {
		char x[16];

		snprintf(x, sizeof(x), "%s%u", i ? ", " : "", out_val[i]);
		buf_add(&lg, x, strlen(x));
	}
	buf_add(&lg, "]", 2);
	log_msg("rpc: GPIO_ADC %s", (char *)lg.p);
	free(lg.p);
	for (i = 0; i < n; i++)
		buf_u32(&pk, out_num[i]);
	for (i = 0; i < n; i++)
		buf_u32(&pk, out_val[i]);
	buf_u32(&pk, 0);
	buf_u32(&pk, 0);
	res_u32(r, 0);
	res_bytes(r, pk.p, pk.len);
	free(pk.p);
}

/* one RPC request: the reply packet into out (len 0: none) */
static void rpc_packet(const unsigned char *pkt, size_t len, struct buf *out)
{
	static struct res r;
	uint32_t slot = get_u32(pkt + 12), opc, argc;
	const unsigned char *body = pkt + 16;
	size_t blen = len - 16;
	struct arg *args;
	const char *msg;
	struct buf payload = { 0 };
	jmp_buf jb;
	int n, i;

	out->len = 0;
	if (blen < 8) {
		log_msg("rpc: request too short (%zu bytes)", blen);
		return;
	}
	opc = get_u32(body);
	argc = get_u32(body + 4);
	res_clear(&r);
	args = lv_parse(body, blen, argc, 8, &n, &msg);
	if (!args) {
		log_msg("rpc: bad request 0x%x: %s", opc, msg);
		res_u32(&r, FS_PARAM_ERROR);
		res_u32(&r, FS_PARAM_ERROR);
	} else {
		bad_jmp = &jb;
		if (setjmp(jb)) {
			log_msg("rpc: op 0x%x bad arguments: %s", opc, bad_msg);
			res_clear(&r);
			res_u32(&r, FS_PARAM_ERROR);
			res_u32(&r, FS_PARAM_ERROR);
		} else {
			rpc_op(opc, args, n, &r);
			if (r.n < 0) {
				res_clear(&r);
				log_msg("rpc: unknown op 0x%x (%d args)", opc, n);
				res_u32(&r, FS_NO_OP);
			}
		}
		free(args);
	}
	buf_u32(&payload, opc | 0xffff0000);
	buf_u32(&payload, r.n);
	lv_pack(&payload, &r);
	make_pkt(out, 0, slot, payload.p, payload.len);
	free(payload.p);
	for (i = 0; i < r.n; i++)
		r.items[i].len = 0;
}

/* ---- the file service -------------------------------------------------- */

/*
 * drive letter -> directory: the MD's own data (NVRAM, its copy of
 * Android's partitions, relative to the state root) and read-only modem
 * configuration from the stock vendor image (absolute)
 */
static const char *drive(uint32_t c)
{
	switch (c) {
	case 'Z': return "mnt/vendor/nvdata/md";
	case 'X': return "mnt/vendor/protect_f/md";
	case 'Y': return "mnt/vendor/protect_s/md";
	case 'W': return FW "modem";
	case 'V': return "mnt/vendor/nvdata/md_cmn";
	case 'U': return "data/vendor/mdlpm";
	case 'T': return FW "mdota";
	case 'S': return "mnt/vendor/nvcfg";
	case 'R': return FW "md";
	case 'Q': return "data/vendor_de/md";
	}
	return NULL;
}

/* results per reply (the MD asserts on any other count); default 1 */
static int nres(uint32_t opc)
{
	switch (opc) {
	case 0x1017: return 0;
	case 0x1004: case 0x1009: case 0x100e: case 0x1011: case 0x101e:
	case 0x101f: case 0x1025: return 2;
	case 0x1003: case 0x1012: case 0x1013: case 0x1024: return 3;
	case 0x1022: return 4;
	}
	return 1;
}

static const char *fs_op_name(uint32_t opc)
{
	static const char *const names[] = {
		"fs_open", "fs_seek", "fs_read", "fs_write", "fs_close", "fs_close_all",
		"fs_mkdir", "fs_rmdir", "fs_size", "fs_folder_size", "fs_rename",
		"fs_move", "fs_count", "fs_disk_info", "fs_delete", "fs_attributes",
		"fs_open_hint", "fs_find_first", "fs_find_next", "fs_find_close",
		"fs_ok", "fs_unlock", "fs_shutdown", "fs_xdelete", "fs_ok",
		"fs_get_drive", "fs_cluster_size", "fs_ok", NULL, NULL, NULL, NULL,
		"fs_restore", "fs_cmpt_read", "fs_bin_region", "fs_cmpt_write",
		"fs_file_detail",
	};

	if (opc >= 0x1001 && opc <= 0x1025 && names[opc - 0x1001])
		return names[opc - 0x1001];
	return NULL;
}

static int err_conv(int e)
{
	return e == EMFILE ? E_TOO_MANY : e == EACCES ? E_ACCESS :
	       e == ENOENT ? E_NOT_FOUND : E_GENERIC;
}

/* an open file (fd >= 0) or a search (fd -1) */
struct file {
	int used, fd;
	char *path;
	/* search state */
	char *dir;
	char **names;
	size_t nnames, next;
	uint32_t pat[MAX_PATH];
	size_t npat;
	unsigned char want, mask;
	int32_t maxlen;
};

static struct file files[MAX_FILES + 1];
static char *root;

struct pending {
	uint32_t slot;
	struct buf body;
	struct pending *next;
};

static struct pending *pending;

static size_t utf8_decode_len(const unsigned char *p, size_t n);

/* the host path for a drive path (malloc'd, in *out), or an FS error code */
static int fs_path_cp(const uint32_t *cp, size_t n, char **out)
{
	struct buf b = { 0 };
	const char *base;
	size_t i, len;

	if (n < 2 || cp[1] != ':')
		return E_DRIVE;
	for (i = 0; i + 1 < n; i++)
		if (cp[i] == '.' && cp[i + 1] == '.')
			return E_ACCESS;
	base = drive(cp_upper(cp[0]));
	if (!base)
		return E_DRIVE;
	if (base[0] != '/') {
		buf_add(&b, root, strlen(root));
		if (b.len && b.p[b.len - 1] != '/')
			buf_add(&b, "/", 1);
	}
	buf_add(&b, base, strlen(base));
	len = utf8_decode_len(b.p, b.len) + (n - 2);
	for (i = 2; i < n; i++)
		utf8_put(&b, cp[i] == '\\' ? '/' : cp[i]);
	buf_add(&b, "", 1);
	if (len + 1 > MAX_PATH) {
		free(b.p);
		return E_PATH_LONG;
	}
	*out = (char *)b.p;
	return 0;
}

static int fs_path(const struct arg *w, char **out)
{
	static uint32_t cp[MAX_PATH * 2];

	return fs_path_cp(cp, utf16_decode(w, cp, sizeof(cp) / sizeof(*cp)), out);
}

static struct file *get_file(const struct arg *h)
{
	int32_t i = i32(h);

	return i > 0 && i <= MAX_FILES && files[i].used ? &files[i] : NULL;
}

static void free_search(struct file *f)
{
	size_t i;

	for (i = 0; i < f->nnames; i++)
		free(f->names[i]);
	free(f->names);
	free(f->dir);
	f->names = NULL;
	f->dir = NULL;
	f->nnames = f->next = 0;
}

static void release(int h)
{
	free(files[h].path);
	free_search(&files[h]);
	memset(&files[h], 0, sizeof(files[h]));
}

static int free_handle(void)
{
	int h;

	for (h = 1; h <= MAX_FILES; h++)
		if (!files[h].used)
			return h;
	return 0;
}

static int fs_open_path(const struct arg *wpath, uint32_t flags)
{
	int mode, h, fd, r;
	char *p;

	r = fs_path(wpath, &p);
	if (r)
		return r;
	mode = flags & 0x100 ? O_RDONLY : O_RDWR;
	if (flags & 0x10000)
		mode = O_RDWR | O_CREAT;
	if (flags & 0x20000)
		mode = O_RDWR | O_CREAT | O_TRUNC;
	if (flags & 0x10000000)
		mode |= O_NONBLOCK;
	h = free_handle();
	if (!h) {
		free(p);
		return E_TOO_MANY;
	}
	fd = open(p, mode | O_CLOEXEC, 0660);
	if (fd < 0) {
		free(p);
		return err_conv(errno);
	}
	files[h] = (struct file){ .used = 1, .fd = fd, .path = p };
	return h;
}

static int fs_close_handle(int32_t h)
{
	int fd;

	if (!(h > 0 && h <= MAX_FILES && files[h].used))
		return E_HANDLE;
	fd = files[h].fd;
	release(h);
	if (fd < 0)
		return 0;
	return close(fd) ? err_conv(errno) : 0;
}

static void fs_close_all(void)
{
	int h;

	for (h = 1; h <= MAX_FILES; h++)
		if (files[h].used)
			fs_close_handle(h);
}

/* the fd of an open file; a search handle is a bad argument (TypeError) */
static int file_fd(struct file *f)
{
	if (f->fd < 0)
		bad_args("'NoneType' object cannot be interpreted as an integer");
	return f->fd;
}

/* os.read(fd, n) for n >= 0: one read of up to n bytes */
static ssize_t read_up_to(int fd, int64_t n, unsigned char **data)
{
	struct stat st;
	size_t want = n;
	ssize_t got;

	/* a regular file cannot give more than what is left in it */
	if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
		off_t pos = lseek(fd, 0, SEEK_CUR);
		int64_t left = pos >= 0 && st.st_size > pos ? st.st_size - pos : 0;

		if ((int64_t)want > left)
			want = left;
	} else if (want > (1 << 20)) {
		want = 1 << 20;
	}
	*data = malloc(want ? want : 1);
	got = read(fd, *data, want);
	return got;
}

/* Python slice b[:n] length */
static size_t slice_len(size_t len, int64_t n)
{
	if (n < 0)
		return (int64_t)len + n > 0 ? (size_t)((int64_t)len + n) : 0;
	return (uint64_t)n < len ? (size_t)n : len;
}

/* posixpath.dirname */
static char *py_dirname(const char *p)
{
	const char *s = strrchr(p, '/');
	size_t n = s ? (size_t)(s - p) + 1 : 0, k = n;
	char *d;

	while (k > 0 && p[k - 1] == '/')
		k--;
	if (k == 0)
		k = n;		/* all slashes: kept */
	d = strndup(p, k);
	return d;
}

/* posixpath.join(a, b) */
static char *py_join(const char *a, const char *b)
{
	char *r;

	if (b[0] == '/')
		return strdup(b);
	if (!a[0] || a[strlen(a) - 1] == '/') {
		if (asprintf(&r, "%s%s", a, b) < 0)
			exit(1);
	} else if (asprintf(&r, "%s/%s", a, b) < 0) {
		exit(1);
	}
	return r;
}

static char *rstrip_slash(const char *p)
{
	size_t n = strlen(p);

	while (n && p[n - 1] == '/')
		n--;
	return strndup(p, n);
}

static int is_dir(const char *p)
{
	struct stat st;

	return !stat(p, &st) && S_ISDIR(st.st_mode);
}

/* FAT attributes of a host path: 0x10 directory, 0x01 read-only; -1 */
static int attributes(const char *p)
{
	struct stat st;

	if (stat(p, &st))
		return -1;
	return (access(p, W_OK) ? 1 : 0) | (S_ISDIR(st.st_mode) ? 0x10 : 0);
}

static uint32_t dos_time(time_t t)
{
	struct tm tm;
	int64_t v;

	localtime_r(&t, &tm);
	v = (int64_t)(tm.tm_year + 1900 - 1980) * (1 << 25) | (int64_t)(tm.tm_mon + 1) << 21 |
	    (int64_t)tm.tm_mday << 16 | tm.tm_hour << 11 | tm.tm_min << 5 | (tm.tm_sec & 0x1f);
	return (uint32_t)v;
}

/*
 * FS_DOSDirEntry (0x34 bytes, ccci_fsd FS_EntryLinuxToDos): attributes,
 * name-complete flag, access and modification time, size
 */
static void dos_entry(const char *p, int complete, unsigned char e[0x34])
{
	struct stat st;

	memset(e, 0, 0x34);
	if (stat(p, &st))
		return;
	e[0xb] = attributes(p);
	e[0xc] = complete ? 1 : 0;
	put_u32(e + 0xe, dos_time(st.st_atim.tv_sec));
	put_u32(e + 0x16, dos_time(st.st_mtim.tv_sec));
	put_u32(e + 0x1c, (uint32_t)st.st_size);
}

/* fnmatch.fnmatchcase on code points */
static int fnmatch_cp(const uint32_t *s, size_t ns, const uint32_t *p, size_t np)
{
	size_t si = 0, pi = 0, star_p = (size_t)-1, star_s = 0;

	while (si < ns) {
		if (pi < np && p[pi] == '*') {
			star_p = ++pi;
			star_s = si;
			continue;
		}
		if (pi < np) {
			size_t adv = 1;
			int ok;

			if (p[pi] == '?') {
				ok = 1;
			} else if (p[pi] == '[') {
				/* the class up to its ']' (one right after [ or [! is literal) */
				size_t j = pi + 1;
				int neg = 0, hit = 0;

				if (j < np && p[j] == '!')
					j++;
				if (j < np && p[j] == ']')
					j++;
				while (j < np && p[j] != ']')
					j++;
				if (j >= np) {
					ok = s[si] == '[';	/* no closing ']': a literal [ */
				} else {
					size_t k = pi + 1;

					if (p[k] == '!') {
						neg = 1;
						k++;
					}
					for (; k < j; k++) {
						if (k + 2 < j && p[k + 1] == '-') {
							if (p[k] <= s[si] && s[si] <= p[k + 2])
								hit = 1;
							k += 2;
						} else if (p[k] == s[si]) {
							hit = 1;
						}
					}
					ok = hit != neg;
					adv = j + 1 - pi;
				}
			} else {
				ok = p[pi] == s[si];
			}
			if (ok) {
				pi += adv;
				si++;
				continue;
			}
		}
		if (star_p == (size_t)-1)
			return 0;
		pi = star_p;
		si = ++star_s;
	}
	while (pi < np && p[pi] == '*')
		pi++;
	return pi == np;
}

/* the next matching entry of search handle h: [ret, entry, name], or 0 */
static int find_scan(int h, struct res *r)
{
	struct file *f = &files[h];
	static uint32_t cp[4096], up[4096];

	while (f->next < f->nnames) {
		const char *n = f->names[f->next++];
		size_t nc = fsname_decode(n, cp, 4096), i, k;
		char *p;
		int a;

		if (!strcmp(n, ".") || !strcmp(n, ".."))
			continue;
		for (i = 0; i < nc; i++)
			up[i] = cp_upper(cp[i]);
		if (!fnmatch_cp(up, nc, f->pat, f->npat))
			continue;
		p = py_join(f->dir, n);
		a = attributes(p);
		if (a < 0 || (f->want & ~a) || (a & f->mask)) {
			free(p);
			continue;
		}
		res_u32(r, h);
		{
			unsigned char e[0x34];

			dos_entry(p, (int64_t)nc < f->maxlen, e);
			res_bytes(r, e, sizeof(e));
		}
		if (f->maxlen) {
			struct buf b = { 0 };

			k = slice_len(nc, f->maxlen);
			for (i = 0; i < k; i++) {
				uint32_t c = cp[i];
				unsigned char x[4];

				if (c >= 0x10000) {
					uint32_t hi = 0xd800 + ((c - 0x10000) >> 10),
						 lo = 0xdc00 + ((c - 0x10000) & 0x3ff);

					x[0] = hi;
					x[1] = hi >> 8;
					x[2] = lo;
					x[3] = lo >> 8;
					buf_add(&b, x, 4);
				} else {
					x[0] = c;
					x[1] = c >> 8;
					buf_add(&b, x, 2);
				}
			}
			buf_add(&b, "\0\0", 2);
			res_bytes(r, b.p, b.len);
			free(b.p);
		} else {
			res_bytes(r, NULL, 0);
		}
		free(p);
		return 1;
	}
	return 0;
}

static const unsigned char zero34[0x34];

/* os.walk: count (files + dirs per directory) and/or total lstat size */
static void walk(const char *d, int64_t *count, int64_t *size)
{
	DIR *dir = opendir(d);
	struct dirent *e;
	char **subs = NULL;
	size_t nsubs = 0, i;

	if (!dir)
		return;
	while ((e = readdir(dir))) {
		char *p;
		struct stat st, lst;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		p = py_join(d, e->d_name);
		if (count)
			(*count)++;
		if (!stat(p, &st) && S_ISDIR(st.st_mode)) {
			/* a directory (a symlink to one is listed, not entered) */
			if (!lstat(p, &lst) && !S_ISLNK(lst.st_mode)) {
				subs = realloc(subs, (nsubs + 1) * sizeof(*subs));
				subs[nsubs++] = p;
				continue;
			}
		} else if (size && !lstat(p, &lst)) {
			*size += lst.st_size;
		}
		free(p);
	}
	closedir(dir);
	for (i = 0; i < nsubs; i++) {
		walk(subs[i], count, size);
		free(subs[i]);
	}
	free(subs);
}

/* shutil.rmtree (no symlink following): 0 or errno */
static int rmtree(const char *p)
{
	DIR *dir;
	struct dirent *e;
	struct stat st;

	if (lstat(p, &st))
		return errno;
	if (S_ISLNK(st.st_mode))
		return -1;		/* "Cannot call rmtree on a symbolic link" */
	dir = opendir(p);
	if (!dir)
		return errno;
	while ((e = readdir(dir))) {
		char *q;
		int r;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		q = py_join(p, e->d_name);
		if (!lstat(q, &st) && S_ISDIR(st.st_mode))
			r = rmtree(q);
		else
			r = unlink(q) ? errno : 0;
		free(q);
		if (r) {
			closedir(dir);
			return r;
		}
	}
	closedir(dir);
	return rmdir(p) ? errno : 0;
}

static void fs_op(uint32_t opc, struct arg *args, int argc, struct res *r)
{
	char *p = NULL, *q = NULL;
	struct file *f;
	int e, h;

	switch (opc) {
	case 0x1001:	/* open */
		ARG(1);
		res_u32(r, fs_open_path(ARG(0), i32(ARG(1))));
		break;
	case 0x1002: {	/* seek */
		int32_t w, off;
		off_t pos;

		ARG(2);
		f = get_file(ARG(0));
		w = i32(ARG(2));
		if (!f) {
			res_u32(r, E_HANDLE);
			break;
		}
		if (w < 0 || w >= 3) {
			res_u32(r, E_PARAM);
			break;
		}
		off = i32(ARG(1));
		pos = lseek(file_fd(f), off, w);
		res_u32(r, pos < 0 ? err_conv(errno) : (int64_t)pos);
		break;
	}
	case 0x1003: {	/* read */
		unsigned char *data;
		int32_t n;
		ssize_t got;

		ARG(1);
		f = get_file(ARG(0));
		if (!f) {
			res_u32(r, E_HANDLE);
			res_u32(r, 0);
			res_bytes(r, NULL, 0);
			break;
		}
		n = i32(ARG(1));
		e = file_fd(f);
		/* os.read() with a negative size: OSError EINVAL */
		data = NULL;
		got = n < 0 ? (errno = EINVAL, -1) : read_up_to(e, n, &data);
		if (got < 0) {
			res_u32(r, err_conv(errno));
			res_u32(r, 0);
			res_bytes(r, NULL, 0);
		} else {
			res_u32(r, 0);
			res_u32(r, got);
			res_bytes(r, data, got);
		}
		free(data);
		break;
	}
	case 0x1004: {	/* write */
		const struct arg *d;
		ssize_t n;

		ARG(2);
		f = get_file(ARG(0));
		if (!f) {
			res_u32(r, E_HANDLE);
			res_u32(r, 0);
			break;
		}
		d = ARG(1);
		n = slice_len(d->len, i32(ARG(2)));
		n = write(file_fd(f), d->p, n);
		if (n < 0) {
			res_u32(r, err_conv(errno));
			res_u32(r, 0);
		} else {
			res_u32(r, 0);
			res_u32(r, n);
		}
		break;
	}
	case 0x1005:	/* close */
		res_u32(r, fs_close_handle(i32(ARG(0))));
		break;
	case 0x1006:	/* close all */
		fs_close_all();
		res_u32(r, 0);
		break;
	case 0x1007:	/* mkdir */
	case 0x1008:	/* rmdir */
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		q = rstrip_slash(p);
		e = opc == 0x1007 ? mkdir(q, 0770) : rmdir(q);
		res_u32(r, e ? err_conv(errno) : 0);
		break;
	case 0x1009: {	/* size */
		struct stat st;

		f = get_file(ARG(0));
		if (!f) {
			res_u32(r, E_HANDLE);
			res_u32(r, 0);
			break;
		}
		if (fstat(file_fd(f), &st)) {
			res_u32(r, err_conv(errno));
			res_u32(r, 0);
		} else {
			res_u32(r, 0);
			res_u32(r, st.st_size);
		}
		break;
	}
	case 0x100a: {	/* folder size */
		int64_t size = 0;

		ARG(1);
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		if (!is_dir(p)) {
			res_u32(r, E_NOT_FOUND);
			break;
		}
		walk(p, NULL, &size);
		res_u32(r, size);
		break;
	}
	case 0x100b: {	/* rename (new name relative to the file's directory) */
		static uint32_t cp[MAX_PATH * 2];
		struct buf nb = { 0 };
		size_t n, i;
		int colon = 0;

		ARG(1);
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		n = utf16_decode(ARG(1), cp, sizeof(cp) / sizeof(*cp));
		for (i = 0; i < n; i++)
			colon |= cp[i] == ':';
		if (colon) {
			e = fs_path_cp(cp, n, &q);
			if (e) {
				res_u32(r, e);
				break;
			}
		} else {
			char *d = py_dirname(p);

			cps_to_utf8(&nb, cp, n);
			buf_add(&nb, "", 1);
			q = py_join(d, (char *)nb.p);
			free(d);
			free(nb.p);
		}
		res_u32(r, rename(p, q) ? err_conv(errno) : 0);
		break;
	}
	case 0x100c:	/* move */
		ARG(2);
		e = fs_path(ARG(0), &p);
		if (!e)
			e = fs_path(ARG(1), &q);
		if (e) {
			res_u32(r, e);
			break;
		}
		res_u32(r, rename(p, q) ? err_conv(errno) : 0);
		break;
	case 0x100d: {	/* count */
		DIR *d;
		struct dirent *de;
		int64_t n = 0;

		ARG(1);
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		d = opendir(p);
		if (!d) {
			res_u32(r, err_conv(errno));
			break;
		}
		while ((de = readdir(d)))
			n += strcmp(de->d_name, ".") && strcmp(de->d_name, "..");
		closedir(d);
		res_u32(r, n);
		break;
	}
	case 0x100e: {	/* disk info */
		unsigned char info[0x54] = { 0 };
		struct statvfs st;

		fs_path_cp((uint32_t[]){ 'Z', ':', '\\' }, 3, &p);
		if (statvfs(p, &st)) {
			res_u32(r, err_conv(errno));
			res_bytes(r, info, sizeof(info));
			break;
		}
		put_u32(info + 0x30, 0x200);
		put_u32(info + 0x34, st.f_bsize >> 9);
		put_u32(info + 0x38, st.f_blocks);
		put_u32(info + 0x3c, 0);
		put_u32(info + 0x40, 0x200);
		res_u32(r, 0);
		res_bytes(r, info, sizeof(info));
		break;
	}
	case 0x100f:	/* delete */
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		res_u32(r, unlink(p) ? err_conv(errno) : 0);
		break;
	case 0x1010: {	/* attributes */
		struct stat st;

		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		if (stat(p, &st)) {
			res_u32(r, err_conv(errno));
			break;
		}
		res_u32(r, (access(p, W_OK) ? 1 : 0) | (S_ISDIR(st.st_mode) ? 0x10 : 0));
		break;
	}
	case 0x1011: {	/* open with a hint */
		unsigned char hint[8] = { 0 };

		ARG(1);
		h = fs_open_path(ARG(0), i32(ARG(1)));
		if (argc > 2)
			memcpy(hint, args[2].p, args[2].len < 8 ? args[2].len : 8);
		res_u32(r, h);
		res_bytes(r, hint, 8);
		break;
	}
	case 0x1012: {	/* find first */
		static uint32_t cp[MAX_PATH * 2];
		DIR *d;
		struct dirent *de;
		char *dir, *pat;
		const char *slash;
		struct file *s;
		size_t n, i;

		ARG(2);
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
			break;
		}
		/* os.path.split */
		slash = strrchr(p, '/');
		pat = strdup(slash ? slash + 1 : p);
		dir = py_dirname(p);
		d = opendir(dir);
		if (!d) {
			res_u32(r, err_conv(errno));
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
			free(pat);
			free(dir);
			break;
		}
		h = free_handle();
		if (!h) {
			closedir(d);
			res_u32(r, E_TOO_MANY);
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
			free(pat);
			free(dir);
			break;
		}
		/* attr[0], attr_mask[0], i32(maxlen): after the listing, like Python */
		if (!args[1].len || !args[2].len || (argc > 3 && args[3].len < 4)) {
			closedir(d);
			free(pat);
			free(dir);
			bad_args(!args[1].len || !args[2].len ? "index out of range"
							     : "unpack requires a buffer of 4 bytes");
		}
		s = &files[h];
		memset(s, 0, sizeof(*s));
		s->want = args[1].p[0];
		s->mask = args[2].p[0];
		s->maxlen = argc > 3 ? (int32_t)get_u32(args[3].p) : 0;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			s->names = realloc(s->names, (s->nnames + 1) * sizeof(*s->names));
			s->names[s->nnames++] = strdup(de->d_name);
		}
		closedir(d);
		n = fsname_decode(pat, cp, sizeof(cp) / sizeof(*cp));
		for (i = 0; i < n; i++)
			s->pat[i] = cp_upper(cp[i]);
		s->npat = n;
		free(pat);
		s->used = 1;
		s->fd = -1;
		s->dir = dir;
		s->path = p;
		p = NULL;
		if (!find_scan(h, r)) {
			release(h);
			res_clear(r);
			res_u32(r, E_NO_MORE);
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
		}
		break;
	}
	case 0x1013:	/* find next */
		f = get_file(ARG(0));
		if (!f || f->fd >= 0) {
			res_u32(r, E_HANDLE);
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
			break;
		}
		if (argc > 1)
			f->maxlen = i32(&args[1]);
		if (!find_scan(i32(ARG(0)), r)) {
			res_u32(r, E_NO_MORE);
			res_bytes(r, zero34, sizeof(zero34));
			res_bytes(r, NULL, 0);
			break;
		}
		r->items[0].len = 0;
		buf_u32(&r->items[0], 0);
		break;
	case 0x1014:	/* find close */
		f = get_file(ARG(0));
		if (!f || f->fd >= 0) {
			res_u32(r, E_HANDLE);
			break;
		}
		release(i32(ARG(0)));
		res_u32(r, 0);
		break;
	case 0x1015:	/* LockFAT */
	case 0x1019:	/* ClearDiskFlag */
	case 0x101c:	/* SetDiskFlag */
		res_u32(r, 0);
		break;
	case 0x1016:	/* UnlockAll */
		res_u32(r, 1);
		break;
	case 0x1017:	/* Shutdown */
		fs_close_all();
		break;
	case 0x1018: {	/* xdelete */
		int64_t n = 0;

		ARG(1);
		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			break;
		}
		if (is_dir(p)) {
			walk(p, &n, NULL);
			e = rmtree(p);
			if (e) {
				res_u32(r, e > 0 ? err_conv(e) : E_GENERIC);
				break;
			}
		} else {
			if (unlink(p)) {
				res_u32(r, err_conv(errno));
				break;
			}
			n = 1;
		}
		res_u32(r, n);
		break;
	}
	case 0x101a:	/* get drive */
		res_u32(r, i32(ARG(0)) == 4 ? 'Z' : E_PARAM);
		break;
	case 0x101b:	/* cluster size */
		res_u32(r, 0x200);
		break;
	case 0x1021:	/* Restore (from the binary NVRAM backup): not kept on Arch */
		res_u32(r, E_NOT_FOUND);
		break;
	case 0x1022: {	/* compound read: open, size, seek, read, close */
		uint32_t pr[10] = { 0 }, step;
		int64_t size = 0, count = 0;
		unsigned char *data = NULL, res0[8];
		int fd, err = 0;
		size_t i;

		ARG(1);
		for (i = 0; i < 40 && i < args[1].len; i++)
			((unsigned char *)pr)[i] = args[1].p[i];
		for (i = 0; i < 10; i++)
			pr[i] = get_u32((unsigned char *)&pr[i]);
		if (!(pr[0] & 1)) {
			step = 0;
			err = -7;
			goto cr_out;
		}
		h = fs_open_path(ARG(0), pr[3]);
		if (h < 0) {
			step = 0;
			err = h;
			goto cr_out;
		}
		fd = files[h].fd;
		step = 1;
		if (pr[0] & 2) {
			struct stat st;

			if (fstat(fd, &st)) {
				err = err_conv(errno);
				fs_close_handle(h);
				goto cr_out;
			}
			size = st.st_size;
			step = 3;
		}
		if (pr[0] & 4) {
			if (lseek(fd, (int32_t)pr[5], pr[6] < 3 ? (int)pr[6] : 0) < 0) {
				err = err_conv(errno);
				fs_close_handle(h);
				goto cr_out;
			}
			step |= 4;
		}
		if (pr[0] & 8) {
			ssize_t got = read_up_to(fd, pr[8], &data);

			if (got < 0) {
				err = err_conv(errno);
				fs_close_handle(h);
				goto cr_out;
			}
			count = got;
			step |= 8;
		}
		if (!(pr[0] & 0x10)) {
			fs_close_handle(h);
			err = -6;
			goto cr_out;
		}
		e = fs_close_handle(h);
		if (e < 0)
			err = e;
		else
			step |= 0x10;
cr_out:
		put_u32(res0, step);
		put_u32(res0 + 4, err);
		res_bytes(r, res0, 8);
		res_u32(r, size);
		res_u32(r, count);
		res_bytes(r, data, count);
		free(data);
		break;
	}
	case 0x1023:	/* BinRegionAccess */
		res_u32(r, E_PARAM);
		break;
	case 0x1024: {	/* compound write: open, seek, write, close */
		uint32_t pr[10] = { 0 }, step = 0;
		int64_t written = 0;
		unsigned char res0[8];
		const struct arg *d = argc > 2 ? &args[2] : &(struct arg){ NULL, 0 };
		int fd, err = 0;
		size_t i;

		ARG(1);
		for (i = 0; i < 40 && i < args[1].len; i++)
			((unsigned char *)pr)[i] = args[1].p[i];
		for (i = 0; i < 10; i++)
			pr[i] = get_u32((unsigned char *)&pr[i]);
		h = fs_open_path(ARG(0), pr[3]);
		if (h < 0) {
			err = h;
			goto cw_out;
		}
		fd = files[h].fd;
		step = 1;
		if (pr[0] & 4) {
			if (lseek(fd, (int32_t)pr[4], pr[5] < 3 ? (int)pr[5] : 0) < 0) {
				err = err_conv(errno);
				fs_close_handle(h);
				goto cw_out;
			}
			step |= 4;
		}
		if (pr[0] & 0x20) {
			ssize_t n = write(fd, d->p, slice_len(d->len, pr[7]));

			if (n < 0) {
				err = err_conv(errno);
				fs_close_handle(h);
				goto cw_out;
			}
			written = n;
			step |= 0x20;
		}
		e = fs_close_handle(h);
		step |= 0x10;
		err = e >= 0 ? 0 : e;
cw_out:
		put_u32(res0, step);
		put_u32(res0 + 4, err);
		res_bytes(r, res0, 8);
		res_u32(r, written);
		res_u32(r, 0);
		break;
	}
	case 0x1025: {	/* file detail */
		unsigned char t[24] = { 0 };
		struct stat st;
		int64_t v[3];
		int i;

		e = fs_path(ARG(0), &p);
		if (e) {
			res_u32(r, e);
			res_bytes(r, t, sizeof(t));
			break;
		}
		if (stat(p, &st)) {
			res_u32(r, err_conv(errno));
			res_bytes(r, t, sizeof(t));
			break;
		}
		/* int() of the float times: toward zero */
		v[0] = st.st_mtim.tv_sec + (st.st_mtim.tv_sec < 0 && st.st_mtim.tv_nsec);
		v[1] = st.st_atim.tv_sec + (st.st_atim.tv_sec < 0 && st.st_atim.tv_nsec);
		v[2] = st.st_ctim.tv_sec + (st.st_ctim.tv_sec < 0 && st.st_ctim.tv_nsec);
		for (i = 0; i < 3; i++) {
			put_u32(t + 8 * i, (uint64_t)v[i]);
			put_u32(t + 8 * i + 4, (uint64_t)v[i] >> 32);
		}
		res_u32(r, 0);
		res_bytes(r, t, sizeof(t));
		break;
	}
	default:
		r->n = -1;
	}
	free(p);
	free(q);
}

static void fs_error_res(uint32_t opc, struct res *r)
{
	int n = nres(opc), i;

	res_clear(r);
	if (!n)
		return;
	res_u32(r, E_PARAM);
	for (i = 1; i < n; i++)
		res_bytes(r, NULL, 0);
}

/* the reply packets of one FS request (chunked) into out[], count returned */
static int fs_reply(uint32_t slot, uint32_t opc, const struct res *r, struct buf **out)
{
	struct buf stream = { 0 }, chunk = { 0 };
	int n = 0;
	size_t i;

	buf_u32(&stream, r->n);
	lv_pack(&stream, r);
	for (i = 0; i < stream.len; i += CHUNK) {
		size_t len = stream.len - i < CHUNK ? stream.len - i : CHUNK;
		int last = i + CHUNK >= stream.len;

		chunk.len = 0;
		buf_u32(&chunk, opc | 0xffff0000);
		buf_add(&chunk, stream.p + i, len);
		*out = realloc(*out, (n + 1) * sizeof(**out));
		(*out)[n] = (struct buf){ 0 };
		make_pkt(&(*out)[n], last ? 0 : MORE, slot, chunk.p, chunk.len);
		n++;
	}
	free(stream.p);
	free(chunk.p);
	return n;
}

/* one packet from the MD: the reply packets (count returned) */
static int fs_packet(const unsigned char *pkt, size_t len, struct buf **out)
{
	static struct res r;
	uint32_t d0 = get_u32(pkt), slot = get_u32(pkt + 12), opc, argc;
	struct pending **pp, *pe = NULL;
	struct buf body = { 0 };
	struct arg *args;
	const char *msg;
	jmp_buf jb;
	int n, i;

	for (pp = &pending; *pp; pp = &(*pp)->next)
		if ((*pp)->slot == slot) {
			pe = *pp;
			*pp = pe->next;
			break;
		}
	if (pe) {
		/* the op word is repeated */
		body = pe->body;
		free(pe);
		if (len - 16 > 4)
			buf_add(&body, pkt + 20, len - 20);
	} else {
		buf_add(&body, pkt + 16, len - 16);
	}
	if (d0 & MORE) {
		pe = calloc(1, sizeof(*pe));
		pe->slot = slot;
		pe->body = body;
		pe->next = pending;
		pending = pe;
		return 0;
	}
	if (body.len < 8) {
		log_msg("fs: request too short (%zu bytes)", body.len);
		free(body.p);
		return 0;
	}
	opc = get_u32(body.p);
	argc = get_u32(body.p + 4);
	res_clear(&r);
	args = lv_parse(body.p, body.len, argc, 8, &n, &msg);
	if (!args) {
		log_msg("fs: bad request 0x%x: %s", opc, msg);
		fs_error_res(opc, &r);
	} else {
		bad_jmp = &jb;
		if (setjmp(jb)) {
			log_msg("fs: op 0x%x bad arguments: %s", opc, bad_msg);
			fs_error_res(opc, &r);
		} else {
			fs_op(opc, args, n, &r);
			if (r.n < 0) {
				res_clear(&r);
				log_msg("fs: unknown op 0x%x", opc);
				res_u32(&r, E_PARAM);
			}
		}
		free(args);
	}
	n = fs_reply(slot, opc, &r, out);
	for (i = 0; i < r.n; i++)
		r.items[i].len = 0;
	free(body.p);
	return n;
}

/* ---- AT init ----------------------------------------------------------- */

/*
 * runs at_init on the AT port without blocking the main loop; poll fd
 * while it runs, call at_tick() at least every 200 ms
 */
struct at {
	int active, fd, step;
	double created, step_start, sent;
	struct buf buf;
};

/* the modem's network links (DPMAIF, one per MD interface) */
static int net_links(char ***links)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	int n = 0;

	*links = NULL;
	while (d && (e = readdir(d))) {
		char path[300], *real;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "/sys/class/net/%s", e->d_name);
		real = realpath(path, NULL);
		if (real && strstr(real, "/cezanne-ccci/")) {
			*links = realloc(*links, (n + 1) * sizeof(**links));
			(*links)[n++] = strdup(path);
		}
		free(real);
	}
	if (d)
		closedir(d);
	return n;
}

static void at_finish(struct at *a)
{
	char **links, path[320];
	int n, i, fd;

	if (a->fd >= 0) {
		close(a->fd);
		a->fd = -1;
	}
	fd = open(AT_READY, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", AT_READY, strerror(errno));
		exit(1);
	}
	close(fd);
	/* let ModemManager see the AT port and the data links now */
	n = net_links(&links);
	for (i = -1; i < n; i++) {
		FILE *f;

		snprintf(path, sizeof(path), "%s/uevent",
			 i < 0 ? "/sys/class/wwan/wwan0at0" : links[i]);
		f = fopen(path, "we");
		if (!f) {
			log_msg("udev change: [Errno %d] %s: '%s'", errno, strerror(errno), path);
		} else {
			int bad = fputs("change", f) < 0;

			if (fclose(f) || bad)
				log_msg("udev change: [Errno %d] %s", errno, strerror(errno));
		}
		if (i >= 0)
			free(links[i]);
	}
	free(links);
	log_msg("AT port handed to ModemManager (%.1f s)", now_s() - a->created);
}

static void at_send(struct at *a)
{
	char cmd[32];
	int n;

	a->buf.len = 0;
	a->sent = now_s();
	n = snprintf(cmd, sizeof(cmd), "%s\r", at_init[a->step].cmd);
	if (write(a->fd, cmd, n) < 0) {		/* port removed under us */
		log_msg("AT port: [Errno %d] %s", errno, strerror(errno));
		at_finish(a);
	}
}

static void at_begin(struct at *a)
{
	a->step_start = now_s();
	at_send(a);
}

static void at_next(struct at *a)
{
	if (++a->step < AT_STEPS)
		at_begin(a);
	else
		at_finish(a);
}

static void at_new(struct at *a)
{
	free(a->buf.p);
	*a = (struct at){ .active = 1, .fd = -1, .created = now_s() };
}

/* open the port; 0 while udev has not made the node yet */
static int at_start(struct at *a)
{
	a->fd = open(AT_PORT, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (a->fd < 0) {
		if (now_s() - a->created < 10)
			return 0;
		log_msg("AT port did not appear");
		at_finish(a);
		return 1;
	}
	at_begin(a);
	return 1;
}

static void at_tick(struct at *a)
{
	double now = now_s();
	int retry;

	if (a->fd < 0)
		return;
	retry = at_init[a->step].retry;
	if (now - a->step_start > (retry > AT_TIMEOUT ? retry : AT_TIMEOUT)) {
		log_msg("%s: no success", at_init[a->step].cmd);
		at_next(a);
	} else if (retry && now - a->sent > 1) {
		at_send(a);
	}
}

static int buf_has(const struct buf *b, const char *s)
{
	return b->len && memmem(b->p, b->len, s, strlen(s));
}

static void at_readable(struct at *a)
{
	unsigned char tmp[4096];
	ssize_t n = read(a->fd, tmp, sizeof(tmp));

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		fprintf(stderr, "AT port read: %s\n", strerror(errno));
		exit(1);
	}
	buf_add(&a->buf, tmp, n);
	if (buf_has(&a->buf, "\r\nOK\r\n")) {
		at_next(a);
	} else if (buf_has(&a->buf, "ERROR") && !at_init[a->step].retry) {
		/* the reply, decoded with errors='replace', stripped */
		struct buf t = { 0 };
		size_t i, s = 0, e;

		for (i = 0; i < a->buf.len; i++)
			if (a->buf.p[i] < 0x80)
				buf_add(&t, &a->buf.p[i], 1);
			else
				utf8_put(&t, 0xfffd);
		e = t.len;
		while (s < e && strchr(" \t\r\n\v\f\x1c\x1d\x1e\x1f\x85", t.p[s]))
			s++;
		while (e > s && strchr(" \t\r\n\v\f\x1c\x1d\x1e\x1f", t.p[e - 1]))
			e--;
		log_msg("%s: %.*s", at_init[a->step].cmd, (int)(e - s), (char *)t.p + s);
		free(t.p);
		at_next(a);
	}
}

/* ---- main -------------------------------------------------------------- */

static int read_state(int fd, char *s, size_t size)
{
	ssize_t n = pread(fd, s, size - 1, 0);
	char *p;

	if (n < 0)
		n = 0;
	s[n] = 0;
	while (n && strchr(" \t\r\n", s[n - 1]))
		s[--n] = 0;
	for (p = s; *p && strchr(" \t\r\n", *p); p++)
		;
	memmove(s, p, strlen(p) + 1);
	return n;
}

static void at_ready_clear(void)
{
	unlink(AT_READY);
}

static void wait_driver(int timeout)
{
	double end = now_s() + timeout;

	while (access(SYS "/state", F_OK) || access("/dev/ccci_fs", F_OK)) {
		if (now_s() > end) {
			/* module loaded, device not bound: the probe found no MD
			 * image from lk (charger or recovery boot), which no retry
			 * changes this boot (the unit's RestartPreventExitStatus) */
			if (!access("/sys/module/cezanne_ccci", F_OK) &&
			    !access(SYS, F_OK) && access(SYS "/driver", F_OK)) {
				fputs("cezanne-ccci: no MD image from lk this boot, not retrying\n", stderr);
				exit(3);
			}
			fputs("cezanne-ccci driver not loaded\n", stderr);
			exit(1);
		}
		usleep(200000);
	}
}

static void usage(void)
{
	fputs("usage: cezanne-modemd [-h] [--root ROOT] [--no-boot] [--run RUN] [-v]\n", stderr);
	exit(2);
}

static size_t utf8_decode_len(const unsigned char *p, size_t n)
{
	size_t i, c = 0;

	for (i = 0; i < n; i++)
		c += (p[i] & 0xc0) != 0x80;
	return c;
}

#ifndef CZ_MODEMD_TEST
int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "root", required_argument, NULL, 'r' },
		{ "no-boot", no_argument, NULL, 'n' },
		{ "run", required_argument, NULL, 'R' },
		{ NULL, 0, NULL, 0 },
	};
	static const char *const names[] = { "ccci_fs", "ccci_rpc", "ccci_sys", "ccci_status" };
	int no_boot = 0, c, i, state_fd, nfs = 0, init_started = 0;
	int port[4];
	double run = 0, t0;
	char state[64], s[64], *z;
	struct at init = { .fd = -1 };
	struct pollfd pfd[6];
	unsigned char *pkt = malloc(65536);

	root = "/var/lib/cezanne-modem";
	while ((c = getopt_long(argc, argv, "v", opts, NULL)) != -1) {
		switch (c) {
		case 'r': root = optarg; break;
		case 'n': no_boot = 1; break;
		case 'R': run = strtod(optarg, NULL); break;
		case 'v': verbose = 1; break;
		default: usage();
		}
	}
	if (optind < argc)
		usage();
	z = py_join(root, drive('Z'));
	if (!is_dir(z)) {
		fprintf(stderr, "no NVRAM in %s (run cezanne-modem-nvram-import)\n", root);
		return 1;
	}
	free(z);
	wait_driver(30);

	for (i = 0; i < 4; i++) {
		char dev[32];

		snprintf(dev, sizeof(dev), "/dev/%s", names[i]);
		port[i] = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (port[i] < 0) {
			fprintf(stderr, "%s: %s\n", dev, strerror(errno));
			return 1;
		}
	}
	state_fd = open(SYS "/state", O_RDONLY | O_CLOEXEC);
	if (state_fd < 0) {
		fprintf(stderr, "%s/state: %s\n", SYS, strerror(errno));
		return 1;
	}
	read_state(state_fd, state, sizeof(state));
	log_msg("modem state %s", state);
	/*
	 * the flag outlives a restart of this service: then ModemManager may
	 * already own the port and the radio must not be set back to CFUN=4
	 */
	if (strcmp(state, "ready"))
		at_ready_clear();
	if (!strcmp(state, "ready") && access(AT_READY, F_OK))
		at_new(&init);
	if (!no_boot && (!strcmp(state, "off") || !strcmp(state, "exception"))) {
		FILE *f = fopen(SYS "/boot", "we");

		if (!f || fputs("1\n", f) < 0 || fclose(f)) {
			fprintf(stderr, "%s/boot: %s\n", SYS, strerror(errno));
			return 1;
		}
	}
	t0 = now_s();
	while (!run || now_s() - t0 < run) {
		int np = 0, k;

		if (init.active && !init_started)
			init_started = at_start(&init);
		if (init.active && init_started) {
			at_tick(&init);
			if (init.fd < 0)
				init.active = 0;
		}
		for (i = 0; i < 4; i++)
			pfd[np++] = (struct pollfd){ .fd = port[i], .events = POLLIN };
		pfd[np++] = (struct pollfd){ .fd = state_fd, .events = POLLPRI | POLLERR };
		if (init.active && init.fd >= 0)
			pfd[np++] = (struct pollfd){ .fd = init.fd, .events = POLLIN };
		if (poll(pfd, np, init.active ? 200 : 1000) <= 0)
			continue;
		for (k = 0; k < np; k++) {
			if (!pfd[k].revents)
				continue;
			if (k == 5) {
				if (init.active && init.fd == pfd[k].fd) {
					at_readable(&init);
					if (init.fd < 0)
						init.active = 0;
				}
				continue;
			}
			if (k == 4) {
				read_state(state_fd, s, sizeof(s));
				if (strcmp(s, state)) {
					log_msg("modem state %s (%d FS requests)", s, nfs);
					if (!strcmp(state, "ready"))
						at_ready_clear();
					if (init.active && init.fd >= 0)
						close(init.fd);	/* the port is gone */
					init.fd = -1;
					init.active = 0;
					if (!strcmp(s, "ready")) {
						at_new(&init);
						init_started = 0;
					}
					snprintf(state, sizeof(state), "%s", s);
				}
				continue;
			}
			for (;;) {
				ssize_t len = read(port[k], pkt, 65536);
				struct buf *out = NULL, one = { 0 };
				int nout = 0, j;

				if (len < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					fprintf(stderr, "%s: %s\n", names[k], strerror(errno));
					return 1;
				}
				if (len == 0)
					break;
				if (len < 16)
					continue;
				if (k == 0) {
					nfs++;
					if (verbose) {
						if (len < 20) {
							log_msg("fs request too short (%zd bytes)", len);
						} else {
							uint32_t opc = get_u32(pkt + 16);
							const char *nm = fs_op_name(opc);

							if (nm)
								log_msg("fs %s", nm);
							else
								log_msg("fs 0x%x", opc);
						}
					}
					nout = fs_packet(pkt, len, &out);
				} else if (k == 1) {
					rpc_packet(pkt, len, &one);
					if (one.len) {
						out = malloc(sizeof(*out));
						out[0] = one;
						nout = 1;
					}
				} else {
					char hex[65] = "";
					ssize_t x;

					for (x = 16; x < len && x < 48; x++)
						sprintf(hex + 2 * (x - 16), "%02x", pkt[x]);
					log_msg("%s: ch %u 0x%x 0x%x 0x%x %s", names[k],
						pkt[8] | pkt[9] << 8, get_u32(pkt), get_u32(pkt + 4),
						get_u32(pkt + 12), hex);
				}
				for (j = 0; j < nout; j++) {
					if (write(port[k], out[j].p, out[j].len) < 0) {
						fprintf(stderr, "%s write: %s\n", names[k], strerror(errno));
						return 1;
					}
					free(out[j].p);
				}
				free(out);
			}
		}
	}
	return 0;
}
#endif
