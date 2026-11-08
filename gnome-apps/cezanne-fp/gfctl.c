// SPDX-License-Identifier: GPL-2.0
/*
 * gfctl: talk to the Goodix fingerprint TA (Microtrust TEE) directly.
 * Bring-up / protocol test tool; written from the command protocol seen in
 * MIUI's libgf_hal.so (interoperability), no vendor code.
 *
 * /dev/teei_fp ioctl CMD_FP_CMD (_IO('T', 2)) takes
 *   u32 status | u32 | u32 | u32 len | payload[len]
 * and returns the TA's reply in place. Every payload starts with
 *   u8 reset_flag (TA sets it when the sensor needs a reset), u8 pad[3],
 *   u32 cmd (module: 1000 core, 1001 algo, 1002 sensor, 1003 test,
 *   1012 tap, 1013 dsp), u32 sub, i32 result, ...
 *
 *   gfctl            sensor + algo init (MIUI's order)
 *   gfctl auth [S]   init, then one authentication: waits up to S seconds
 *                    for a touch on the spot (the spot must be lit: panel
 *                    hbm = 2 + tools/fod-light.py), captures and matches
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CMD_FP_CMD	_IO('T', 2)
#define GF_BUF_LEN	0x96000

#define GF_IOC_INIT		_IOR('g', 0, uint8_t)
#define GF_IOC_RESET		_IO('g', 2)
#define GF_IOC_ENABLE_IRQ	_IO('g', 3)
#define GF_IOC_ENABLE_SPI_CLK	_IOW('g', 5, uint32_t)
#define GF_IOC_DISABLE_SPI_CLK	_IO('g', 6)
#define GF_IOC_ENABLE_POWER	_IO('g', 7)

struct gf_hdr {
	uint8_t reset_flag;
	uint8_t pad[3];
	uint32_t cmd;
	uint32_t sub;
	int32_t result;
} __attribute__((packed));

static int tee_fd = -1, dev_fd = -1;
static uint8_t *iobuf;
static uint32_t spi_speed = 1;

static int gf_cmd(uint32_t cmd, uint32_t sub, void *payload, uint32_t len)
{
	struct gf_hdr *h = payload;
	int ret;

	h->reset_flag = 0;
	h->cmd = cmd;
	h->sub = sub;
	h->result = 0;
	memset(iobuf, 0, 16 + len);
	memcpy(iobuf + 12, &len, 4);
	memcpy(iobuf + 16, payload, len);
	/* like the vendor HAL: SPI clock on only around each TA command */
	ioctl(dev_fd, GF_IOC_ENABLE_SPI_CLK, &spi_speed);
	ret = ioctl(tee_fd, CMD_FP_CMD, iobuf);
	ioctl(dev_fd, GF_IOC_DISABLE_SPI_CLK);
	if (ret) {
		fprintf(stderr, "cmd %u/%u: ioctl %s\n", cmd, sub, strerror(errno));
		return -errno;
	}
	memcpy(payload, iobuf + 16, len);
	ret = *(int32_t *)iobuf;
	return ret ? ret : h->result;
}

static void dump(const char *what, const void *p, unsigned int n)
{
	const uint8_t *b = p;
	unsigned int i;

	printf("%s:", what);
	for (i = 0; i < n; i++)
		printf("%s%02x", i % 16 ? " " : "\n  ", b[i]);
	printf("\n");
}

static int quiet;

static int stepp(const char *name, uint32_t cmd, uint32_t sub, uint8_t *p, uint32_t len)
{
	struct timespec t;
	int ret = gf_cmd(cmd, sub, p, len);

	clock_gettime(CLOCK_BOOTTIME, &t);	/* the kernel log's clock */
	printf("%5ld.%03ld %-14s %u/%-3u -> %d (%#x)%s\n", (long)t.tv_sec, t.tv_nsec / 1000000,
	       name, cmd, sub, ret, ret, p[0] ? " [reset requested]" : "");
	if (!quiet)
		dump("  reply", p, len);
	fflush(stdout);
	return ret;
}

static void put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static int step(const char *name, uint32_t cmd, uint32_t sub, uint32_t len)
{
	uint8_t p[0x400] = { 0 };

	return stepp(name, cmd, sub, p, len);
}

static void init_ta(void)
{
	uint8_t route;

	/* MIUI's mfp-daemon at start (iotrace capture, mainline/fingerprint/ref/
	 * miui-20261030 fp-hal.log): startTap, wakeup, detect init, detect, DSP
	 * init (1013/0, answers status 1001), sensor init, algo init, sleep,
	 * init finish; the framework's setActiveGroup / enumerate come later */
	ioctl(dev_fd, GF_IOC_ENABLE_POWER);
	ioctl(dev_fd, GF_IOC_RESET);

	step("startTap", 1012, 0, 0x1c);
	step("wakeup", 1002, 9, 0x20);
	step("detect init", 1002, 0, 0x1c);
	step("detectSensor", 1002, 1, 0x2c);
	ioctl(dev_fd, GF_IOC_INIT, &route);
	ioctl(dev_fd, GF_IOC_ENABLE_IRQ);
	step("dsp init", 1013, 0, 0x24);
	{
		/* Xiaomi's gf_delmar_sensor_init_t: 0x1c0, product id ("cezanne" = 4)
		 * at 0x1b8, vendor.panel.display (unset on MIUI: 8) at 0x1bc */
		uint8_t p[0x1c0] = { 0 };
		uint32_t product = 4, panel = 8;

		memcpy(p + 0x1b8, &product, 4);
		memcpy(p + 0x1bc, &panel, 4);
		stepp("sensor init", 1002, 2, p, sizeof(p));
	}
	{
		/* gf_delmar_algo_init_t: 0x230; +0x28 = 1 as MIUI sends it, +0x2c
		 * ro.boot.oled_wp (empty on cezanne), +0x22c its length */
		uint8_t p[0x230] = { 0 };
		uint32_t one = 1;

		memcpy(p + 0x28, &one, 4);
		stepp("algo init", 1001, 0, p, sizeof(p));
	}
	step("sleep", 1002, 8, 0x1c);
	step("init finish", 1000, 0xd, 0x1c);
	{
		uint8_t p[0x120] = { 0 };
		uint32_t gid = 0;

		memcpy(p + 0x1c, &gid, 4);
		strcpy((char *)p + 0x20, "/data/vendor/goodix/fpdata");
		stepp("setActiveGroup", 1000, 0, p, sizeof(p));
	}
	step("enumerate", 1000, 7, 0x70);
}

/* the sensor spot in panel pixels (= touchscreen units), as MIUI's HAL
 * passes it to the TA in 1002/101 */
#define FOD_X	540
#define FOD_Y	1833
#define FOD_R	101
/* panel high brightness: 2 = the BSP's FOD mode (instant), only while the
 * finger is down, as MIUI does */
#define PANEL_HBM	"/sys/bus/mipi-dsi/devices/1400e000.dsi.0/hbm"

static int hbm_set(const char *v)
{
	char old[8] = "0";
	int fd = open(PANEL_HBM, O_RDWR);

	if (fd < 0)
		return '0';
	if (read(fd, old, sizeof(old) - 1) <= 0)
		old[0] = '0';
	lseek(fd, 0, SEEK_SET);
	if (write(fd, v, strlen(v)) < 0)
		perror("hbm");
	close(fd);
	return old[0];
}

static int touch_open(void)
{
	char path[64], name[64];
	struct dirent *e;
	DIR *d = opendir("/dev/input");
	int fd = -1;

	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && strstr(name, "Goodix"))
			break;
		close(fd);
		fd = -1;
	}
	if (d)
		closedir(d);
	return fd;
}

/* first touch report inside the spot (1.5 radii, a fingertip is wider) */
static int wait_touch(int ms, int *x, int *y)
{
	struct pollfd pf = { .events = POLLIN };
	struct input_event ev;
	int down = 0, cx = -1, cy = -1;
	struct timespec t0, t;

	pf.fd = touch_open();
	if (pf.fd < 0) {
		fprintf(stderr, "no touchscreen\n");
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		int left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);

		if (left <= 0 || poll(&pf, 1, left) <= 0)
			break;
		if (read(pf.fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
		if (ev.type == EV_KEY && ev.code == BTN_TOUCH)
			down = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X)
			cx = ev.value;
		else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y)
			cy = ev.value;
		else if (ev.type == EV_SYN && down && cx >= 0 && cy >= 0 &&
			 (cx - FOD_X) * (cx - FOD_X) + (cy - FOD_Y) * (cy - FOD_Y) <=
			 9 * FOD_R * FOD_R / 4) {
			*x = cx;
			*y = cy;
			close(pf.fd);
			return 0;
		}
	}
	close(pf.fd);
	return -1;
}

/* MIUI's mfp-daemon at an unlock (ref/miui-20261030 fpa-hal.log) */
static int auth(int seconds)
{
	uint8_t p[0x400];
	uint32_t fid = 0;
	int x, y, i, ret = -1;

	quiet = 1;
	step("authenticate", 1000, 1, 0x28);
	memset(p, 0, sizeof(p));
	put32(p + 0x20, 1);
	stepp("screen on", 1000, 11, p, 0x24);
	printf("touch the fingerprint spot (%d s)\n", seconds);
	fflush(stdout);
	if (wait_touch(seconds * 1000, &x, &y)) {
		printf("no touch on the spot\n");
		return 2;
	}
	printf("finger down at %d,%d\n", x, y);
	char hbm_old[2] = { hbm_set("2"), 0 };

	/* MIUI captures about 200 ms after the touch: the HAL waits for the
	 * spot to be drawn at high brightness ("UI ready") */
	usleep(200000);
	step("wakeup", 1002, 9, 0x20);
	step("algo 20", 1001, 20, 0x30);
	for (i = 0; i < 3; i++) {
		if (i)
			step("algo 4", 1001, 4, 0x378);
		memset(p, 0, sizeof(p));
		put32(p + 0x20, 2);
		put32(p + 0x28, 1);
		put32(p + 0x3c, 1);
		stepp("capture", 1002, 3, p, 0x48);
		memset(p, 0, sizeof(p));
		put32(p + 0x20, 1);
		put32(p + 0x2c, x);
		put32(p + 0x30, y);
		put32(p + 0x34, FOD_X);
		put32(p + 0x38, FOD_Y);
		put32(p + 0x3c, 0x43844000);	/* 264.5f, MIUI's value */
		put32(p + 0x40, FOD_R);
		stepp("touch info", 1002, 101, p, 0x50);
		memset(p, 0, sizeof(p));
		put32(p + 0x24, 1);
		put32(p + 0x58, 2);
		put32(p + 0x88, 1);
		put32(p + 0x90, 0x00010001);
		put32(p + 0x94, 0x14);
		ret = stepp("match", 1001, 2, p, 0xe8);
		if (!ret) {
			memcpy(&fid, p + 0x30, 4);
			printf("MATCH: finger id %#x\n", fid);
			break;
		}
		printf("%s\n", ret == 1008 ? "no match" : "match failed");
	}
	hbm_set(hbm_old);
	step("algo 4", 1001, 4, 0x378);
	memset(p, 0, sizeof(p));
	put32(p + 0x20, 1);
	put32(p + 0x28, 1);
	stepp("algo 20", 1001, 20, p, 0x30);
	step("sleep", 1002, 8, 0x1c);
	if (!ret) {
		memset(p, 0, sizeof(p));
		put32(p + 0x1c, fid);
		stepp("algo 21", 1001, 21, p, 0x20);
	}
	step("algo 24", 1001, 24, 0x1c);
	memset(p, 0, sizeof(p));
	put32(p + 0x1c, 1);
	stepp("screen", 1000, 11, p, 0x24);
	return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
	iobuf = calloc(1, GF_BUF_LEN);
	dev_fd = open("/dev/goodix_fp", O_RDWR);
	tee_fd = open("/dev/teei_fp", O_RDWR);
	if (dev_fd < 0 || tee_fd < 0 || !iobuf) {
		perror("open /dev/goodix_fp or /dev/teei_fp");
		return 1;
	}
	if (argc > 1 && !strcmp(argv[1], "auth")) {
		quiet = 1;
		init_ta();
		return auth(argc > 2 ? atoi(argv[2]) : 30);
	}
	init_ta();
	return 0;
}
