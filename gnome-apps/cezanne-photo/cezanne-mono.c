// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cezanne-mono: streams the Redmi K30 Ultra's monochrome sensor (OV02B1B,
 * V4L2 node "cezanne-mono") with an exposure loop of its own, and saves
 * exposure brackets on request for the HDR, portrait and low light steps
 * (cezanne-photo).
 *
 * stdin commands, one per line:
 *   capture <prefix>   save <prefix>-0.pgm, -1.pgm, -2.pgm: EV -2, 0, +1.5
 *                      relative to the metered exposure; prints "done <n>"
 *   quit
 * Frames are 10-bit (Y10: 16 bits per pixel from the CAMSV, black level
 * 64), 1600x1200 in sensor orientation, saved as 16-bit PGMs (maxval 1023);
 * each PGM's comment holds the exposure in lines and the gain code (16 =
 * 1x) it was taken with.
 *
 * Exposure: lines (4 .. frame length - 7) first, then analogue gain
 * (16 .. 248).  A new value shows in the frames captured DELAY frames
 * after the change (sensor delay plus the frames already queued), counted
 * in buffer sequence numbers; the loop meters every DELAY frames.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NBUF		4
#define EXP_MIN		4
#define GAIN_MIN	16
#define GAIN_MAX	248
#define DELAY		4	/* frames until a control change shows */

static int vfd, sfd;
static unsigned char *buf[NBUF];	/* 16-bit pixels */
static unsigned width, height, stride;
static int exp_max = 1214;

/* the node / subdev whose name starts with @prefix */
static int open_by_name(const char *cls, const char *prefix)
{
	char path[300], name[64];
	struct dirent *e;
	DIR *d = opendir("/sys/class/video4linux");
	int fd = -1;

	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, cls, strlen(cls)))
			continue;
		snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && !strncmp(name, prefix, strlen(prefix))) {
			snprintf(path, sizeof(path), "/dev/%s", e->d_name);
			fd = open(path, O_RDWR);
		}
		fclose(f);
		if (fd >= 0)
			break;
	}
	if (d)
		closedir(d);
	return fd;
}

static int set_ctrl(unsigned id, int val)
{
	struct v4l2_control c = { .id = id, .value = val };

	return ioctl(sfd, VIDIOC_S_CTRL, &c);
}

/* exposure * gain in "line x 1x" units -> lines and gain code */
static void apply(double ev, int *lines, int *gain)
{
	double l = ev, g = GAIN_MIN;

	if (l > exp_max) {
		g = GAIN_MIN * l / exp_max;
		l = exp_max;
	}
	if (l < EXP_MIN)
		l = EXP_MIN;
	if (g > GAIN_MAX)
		g = GAIN_MAX;
	*lines = (int)l;
	*gain = (int)g;
	set_ctrl(V4L2_CID_EXPOSURE, *lines);
	set_ctrl(V4L2_CID_ANALOGUE_GAIN, *gain);
}

static unsigned px(const unsigned char *p, unsigned x, unsigned y)
{
	return ((const uint16_t *)(p + y * stride))[x];
}

/* mean and the share of near-white pixels, subsampled (8-bit units) */
static void meter(const unsigned char *p, double *mean, double *clip)
{
	unsigned long sum = 0, n = 0, hi = 0;

	for (unsigned y = height / 8; y < height * 7 / 8; y += 8)
		for (unsigned x = width / 8; x < width * 7 / 8; x += 8) {
			unsigned v = px(p, x, y) >> 2;
			sum += v;
			hi += v >= 250;
			n++;
		}
	*mean = (double)sum / n;
	*clip = (double)hi / n;
}

static int save(const char *path, const unsigned char *p, int lines, int gain)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return -1;
	fprintf(f, "P5\n# exposure %d gain %d\n%u %u\n1023\n", lines, gain, width, height);
	/* 16-bit PGM samples are big-endian */
	unsigned char *row = malloc(2 * width);
	for (unsigned y = 0; row && y < height; y++) {
		for (unsigned x = 0; x < width; x++) {
			unsigned v = px(p, x, y);
			row[2 * x] = v >> 8;
			row[2 * x + 1] = v & 0xff;
		}
		fwrite(row, 2, width, f);
	}
	free(row);
	return fclose(f);
}

int main(void)
{
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = f.type,
					  .memory = V4L2_MEMORY_MMAP };
	struct v4l2_queryctrl qc = { .id = V4L2_CID_EXPOSURE };
	double ev = 600;	/* lines at 1x */
	int lines, gain, i;
	char line[400];

	setvbuf(stdout, NULL, _IOLBF, 0);
	vfd = open_by_name("video", "cezanne-mono");
	sfd = open_by_name("v4l-subdev", "ov02b1b");
	if (vfd < 0 || sfd < 0) {
		fprintf(stderr, "cezanne-mono: no mono camera\n");
		return 1;
	}
	if (!ioctl(sfd, VIDIOC_QUERYCTRL, &qc))
		exp_max = qc.maximum;
	/* 10 bits in 16 */
	ioctl(vfd, VIDIOC_G_FMT, &f);
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_Y10;
	if (ioctl(vfd, VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_Y10) {
		fprintf(stderr, "cezanne-mono: no 10-bit format\n");
		return 1;
	}
	width = f.fmt.pix.width;
	height = f.fmt.pix.height;
	stride = f.fmt.pix.bytesperline;
	if (ioctl(vfd, VIDIOC_REQBUFS, &rb) || rb.count < 2) {
		perror("cezanne-mono: buffers");
		return 1;
	}
	for (i = 0; i < (int)rb.count; i++) {
		struct v4l2_buffer b = { .index = i, .type = f.type, .memory = rb.memory };

		ioctl(vfd, VIDIOC_QUERYBUF, &b);
		buf[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, vfd, b.m.offset);
		ioctl(vfd, VIDIOC_QBUF, &b);
	}
	apply(ev, &lines, &gain);
	int type = f.type;
	if (ioctl(vfd, VIDIOC_STREAMON, &type)) {
		perror("cezanne-mono: stream");
		return 1;
	}
	printf("ready %ux%u\n", width, height);

	/* bracket state: -1 idle, else the step being waited for */
	static const double steps[] = { 0.25, 1.0, 2.83 };	/* EV -2, 0, +1.5 */
	char prefix[400] = "";
	int step = -1;
	unsigned last_seq = 0, valid_seq = 0;	/* frames from valid_seq on */
	double metered = ev;

	for (;;) {
		struct pollfd p[2] = { { vfd, POLLIN, 0 }, { 0, POLLIN, 0 } };

		if (poll(p, 2, 1000) < 0 && errno != EINTR)
			break;
		if (p[1].revents & (POLLIN | POLLHUP)) {
			if (!fgets(line, sizeof(line), stdin) || !strncmp(line, "quit", 4))
				break;
			if (!strncmp(line, "capture ", 8) && step < 0) {
				snprintf(prefix, sizeof(prefix), "%s", line + 8);
				prefix[strcspn(prefix, "\n")] = 0;
				metered = ev;
				step = 0;
				apply(metered * steps[0], &lines, &gain);
				valid_seq = last_seq + DELAY;
			}
		}
		if (!(p[0].revents & POLLIN))
			continue;

		struct v4l2_buffer b = { .type = f.type, .memory = rb.memory };
		if (ioctl(vfd, VIDIOC_DQBUF, &b))
			continue;
		const unsigned char *px = buf[b.index];
		last_seq = b.sequence;

		if (step >= 0) {
			if ((int)(b.sequence - valid_seq) >= 0) {
				char path[512];

				snprintf(path, sizeof(path), "%s-%d.pgm", prefix, step);
				save(path, px, lines, gain);
				if (++step < (int)(sizeof(steps) / sizeof(steps[0]))) {
					apply(metered * steps[step], &lines, &gain);
					valid_seq = last_seq + DELAY;
				} else {
					step = -1;
					ev = metered;
					apply(ev, &lines, &gain);
					printf("done %s\n", prefix);
				}
			}
		} else if ((int)(b.sequence - valid_seq) >= 0) {
			/* aim for a mean of 90 (dark-heavy midtones), with < 1 % clipped */
			double mean, clip, k;

			meter(px, &mean, &clip);
			k = clip > 0.01 ? 0.8 : 90.0 / (mean > 1 ? mean : 1);
			if (k > 1.5)
				k = 1.5;
			if (k < 0.5)
				k = 0.5;
			if (fabs(k - 1) > 0.04) {
				ev *= pow(k, 0.7);
				if (ev < EXP_MIN)
					ev = EXP_MIN;
				if (ev > (double)exp_max * GAIN_MAX / GAIN_MIN)
					ev = (double)exp_max * GAIN_MAX / GAIN_MIN;
				apply(ev, &lines, &gain);
			}
			valid_seq = last_seq + DELAY;
		}
		ioctl(vfd, VIDIOC_QBUF, &b);
	}
	ioctl(vfd, VIDIOC_STREAMOFF, &type);
	return 0;
}
