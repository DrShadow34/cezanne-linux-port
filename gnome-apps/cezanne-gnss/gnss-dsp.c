// SPDX-License-Identifier: MIT
/*
 * The CONNSYS GNSS DSP link (/dev/gpsdl0 = L1, /dev/gpsdl1 = L5): framing,
 * descrambling and the MVCD boot of the DSP RAM code (see NOTES "GNSS").
 *
 * Link framing (both directions):
 *   AA F0 | word u16 | id u16 | payload | sum u16 | AA 0F
 * word: bits 0..11 = 2 + payload + 2, bits 12..15 = a per-type counter on
 * the DSP's reports; sum = 16-bit sum of word, id and payload; between the
 * markers 0xDE escapes: DE DF = AA, DE E0 = DE. The DSP's reports (all but
 * the boot handshake FE31 / FE32) are scrambled: plain = ror8(raw, 3) ^ 0x59
 * per payload byte (a zero arrives as 0xCA).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gnss.h"

#define FRAG		1400	/* code bytes per 0xFE0C fragment */
#define MAX_FRAME	1425	/* longest stuffed fragment frame sent whole */

/*
 * 0xFE08 boot header (as recorded from the stock software 2026-10-29); words
 * 7 (code size in 16-bit words), 8 / 9 (load / exec address) and 11
 * (fragment count) are set per image, word 10 is the cipher key (0x7B), the
 * rest is DSP configuration still to be understood
 */
static const char header_hex[] =
	"1a0000000000c301a502000000003f88007404747b003200000000000500c800"
	"6f3c2e86e7050200d200b7002800594a1e00c3372800594a8f65070001002800"
	"ff008000470064005000d8005000bb0003003c006f00890088000200fb000100"
	"8568000a000003004e4f";

FILE *dsp_log;		/* open frame log: "utc link dir id hex" per frame */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

double mono_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void sleep_s(double s)
{
	struct timespec ts;

	if (s <= 0)
		return;
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - (time_t)s) * 1e9);
	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static unsigned char descramble(unsigned char c)
{
	return (((c >> 3) | (c << 5)) & 0xff) ^ 0x59;
}

static void log_frame(const char *name, const char *dir, int mid, const unsigned char *p, size_t n)
{
	size_t i;

	if (!dsp_log)
		return;
	pthread_mutex_lock(&log_lock);
	fprintf(dsp_log, "%.3f %s %s %04x ", real_s(), name, dir, mid);
	for (i = 0; i < n; i++)
		fprintf(dsp_log, "%02x", p[i]);
	fputc('\n', dsp_log);
	fflush(dsp_log);
	pthread_mutex_unlock(&log_lock);
}

/* the frame of mid + payload into out (stuffed); its length */
static size_t frame(int mid, const unsigned char *p, size_t n, unsigned char *out)
{
	unsigned char body[4 + 2048 + 2];
	size_t blen = 0, o = 0, i;
	unsigned sum = 0;

	body[blen++] = (n + 4) & 0xff;
	body[blen++] = (n + 4) >> 8;
	body[blen++] = mid & 0xff;
	body[blen++] = mid >> 8;
	memcpy(body + blen, p, n);
	blen += n;
	for (i = 0; i < blen; i++)
		sum += body[i];
	body[blen++] = sum & 0xff;
	body[blen++] = (sum >> 8) & 0xff;
	out[o++] = 0xaa;
	out[o++] = 0xf0;
	for (i = 0; i < blen; i++) {
		if (body[i] == 0xde) {
			out[o++] = 0xde;
			out[o++] = 0xe0;
		} else if (body[i] == 0xaa) {
			out[o++] = 0xde;
			out[o++] = 0xdf;
		} else {
			out[o++] = body[i];
		}
	}
	out[o++] = 0xaa;
	out[o++] = 0x0f;
	return o;
}

static size_t unstuff(const unsigned char *b, size_t n, unsigned char *out)
{
	size_t i = 0, o = 0;

	while (i < n) {
		if (b[i] == 0xde && i + 1 < n) {
			out[o++] = b[i + 1] == 0xdf ? 0xaa : 0xde;
			i += 2;
		} else {
			out[o++] = b[i++];
		}
	}
	return o;
}

static unsigned char *find2(unsigned char *b, size_t n, unsigned char x, unsigned char y)
{
	size_t i;

	for (i = 0; i + 1 < n; i++)
		if (b[i] == x && b[i + 1] == y)
			return b + i;
	return NULL;
}

static void parse(struct link *l, const unsigned char *data, size_t n)
{
	pthread_mutex_lock(&l->lock);
	if (l->blen + n > l->bcap) {
		l->bcap = (l->blen + n) * 2;
		l->buf = realloc(l->buf, l->bcap);
	}
	memcpy(l->buf + l->blen, data, n);
	l->blen += n;
	for (;;) {
		unsigned char *j = find2(l->buf, l->blen, 0xaa, 0xf0), *k, *body;
		size_t blen, rest, len;
		unsigned word, mid, sum = 0, i;

		k = j ? find2(j + 2, l->blen - (j + 2 - l->buf), 0xaa, 0x0f) : NULL;
		if (!k)
			break;
		body = malloc(k - (j + 2) + 1);
		blen = unstuff(j + 2, k - (j + 2), body);
		rest = l->blen - (k + 2 - l->buf);
		memmove(l->buf, k + 2, rest);
		l->blen = rest;
		if (blen < 6) {
			free(body);
			continue;
		}
		word = body[0] | body[1] << 8;
		mid = body[2] | body[3] << 8;
		len = word & 0xfff;
		for (i = 0; i < len && i < blen; i++)
			sum += body[i];
		if (blen != len + 2 || (sum & 0xffff) != (unsigned)(body[len] | body[len + 1] << 8)) {
			fprintf(stderr, "%s bad frame ", l->name);
			for (i = 0; i < 16 && i < blen; i++)
				fprintf(stderr, "%02x", body[i]);
			fputc('\n', stderr);
			free(body);
			continue;
		}
		{
			struct frame f = { mono_s() - l->t0, (int)mid, (int)(word >> 12), NULL, len - 4 };

			f.p = malloc(f.len ? f.len : 1);
			memcpy(f.p, body + 4, f.len);
			if (mid != 0xfe31 && mid != 0xfe32)
				for (i = 0; i < f.len; i++)
					f.p[i] = descramble(f.p[i]);
			if (l->nframes == l->cap) {
				l->cap = l->cap ? 2 * l->cap : 64;
				l->frames = realloc(l->frames, l->cap * sizeof(*l->frames));
			}
			l->frames[l->nframes++] = f;
			log_frame(l->name, "rx", mid, f.p, f.len);
			pthread_cond_broadcast(&l->cond);
		}
		free(body);
	}
	pthread_mutex_unlock(&l->lock);
}

static void *reader(void *arg)
{
	struct link *l = arg;
	unsigned char data[8192];

	while (l->alive) {
		ssize_t n = read(l->fd, data, sizeof(data));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (l->alive)
				fprintf(stderr, "%s read: [Errno %d] %s\n", l->name, errno, strerror(errno));
			break;
		}
		if (n && l->alive)
			parse(l, data, n);
	}
	pthread_mutex_lock(&l->lock);
	l->reader_done = 1;
	pthread_cond_broadcast(&l->cond);
	pthread_mutex_unlock(&l->lock);
	return NULL;
}

/* one DSP link; frames in arrival order, consumed by the caller */
int link_open(struct link *l, const char *name)
{
	const char *dev = !strcmp(name, "L1") ? "/dev/gpsdl0" : "/dev/gpsdl1";

	memset(l, 0, sizeof(*l));
	l->name = name;
	/* the driver has no poll, ignores O_NONBLOCK */
	l->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (l->fd < 0)
		return -1;
	pthread_mutex_init(&l->lock, NULL);
	pthread_cond_init(&l->cond, NULL);
	l->t0 = mono_s();
	l->alive = 1;
	pthread_create(&l->reader, NULL, reader, l);
	return 0;
}

/*
 * stop the reader (its blocking read returns with the DSP's next 1 Hz
 * report) and close the device (the kernel powers the link down with the
 * last close)
 */
void link_close(struct link *l)
{
	struct timespec end;
	size_t i;

	l->alive = 0;
	clock_gettime(CLOCK_REALTIME, &end);
	end.tv_sec += 3;
	pthread_mutex_lock(&l->lock);
	while (!l->reader_done)
		if (pthread_cond_timedwait(&l->cond, &l->lock, &end) == ETIMEDOUT)
			break;
	pthread_mutex_unlock(&l->lock);
	if (l->reader_done)
		pthread_join(l->reader, NULL);
	else
		pthread_detach(l->reader);
	close(l->fd);
	pthread_mutex_lock(&l->lock);
	for (i = 0; i < l->nframes; i++)
		free(l->frames[i].p);
	l->nframes = 0;
	pthread_mutex_unlock(&l->lock);
}

int link_send(struct link *l, int mid, const unsigned char *p, size_t n)
{
	unsigned char *data = malloc(2 * (n + 8) + 4);
	size_t len = frame(mid, p, n, data), off = 0;

	while (off < len) {
		ssize_t w = write(l->fd, data + off, len - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			free(data);
			return -1;
		}
		off += w;
	}
	free(data);
	if (mid != 0xfe0c)	/* not the image download */
		log_frame(l->name, "tx", mid, p, n);
	return 0;
}

/* all frames received so far (the caller frees them) */
size_t link_take(struct link *l, struct frame **out)
{
	size_t n;

	pthread_mutex_lock(&l->lock);
	*out = l->frames;
	n = l->nframes;
	l->frames = NULL;
	l->nframes = l->cap = 0;
	pthread_mutex_unlock(&l->lock);
	return n;
}

void frame_free(struct frame *f)
{
	free(f->p);
	f->p = NULL;
}

/* the first frame mid (with payload match if given) within timeout: 1 */
int link_wait(struct link *l, int mid, double timeout, const unsigned char *match, size_t mlen,
	      struct frame *out)
{
	double end = mono_s() + timeout;

	pthread_mutex_lock(&l->lock);
	for (;;) {
		size_t i;
		double left;
		struct timespec ts;

		for (i = 0; i < l->nframes; i++) {
			struct frame *f = &l->frames[i];

			if (f->mid == mid && (!match || (f->len == mlen && !memcmp(f->p, match, mlen)))) {
				*out = *f;
				memmove(l->frames + i, l->frames + i + 1, (l->nframes - i - 1) * sizeof(*f));
				l->nframes--;
				pthread_mutex_unlock(&l->lock);
				return 1;
			}
		}
		left = end - mono_s();
		if (left <= 0) {
			pthread_mutex_unlock(&l->lock);
			return 0;
		}
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += (time_t)left;
		ts.tv_nsec += (long)((left - (time_t)left) * 1e9);
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&l->cond, &l->lock, &ts);
	}
}

/*
 * the image in FRAG-byte chunks; a chunk whose stuffed frame would be longer
 * than MAX_FRAME goes as two halves (the L5 image has one such chunk).
 * Fragment numbers count the pieces from 1. Returns the count; offsets and
 * lengths in off[] / len[].
 */
static size_t pieces(const unsigned char *image, size_t n, size_t *off, size_t *len)
{
	unsigned char buf[2 * (FRAG + 16)], tmp[FRAG + 2];
	size_t i, cnt = 0;

	for (i = 0; i < n; i += FRAG) {
		size_t c = n - i < FRAG ? n - i : FRAG;

		tmp[0] = tmp[1] = 0;
		memcpy(tmp + 2, image + i, c);
		if (frame(0xfe0c, tmp, c + 2, buf) > MAX_FRAME) {
			size_t half = c / 2;

			off[cnt] = i;
			len[cnt++] = half;
			off[cnt] = i + half;
			len[cnt++] = c - half;
		} else {
			off[cnt] = i;
			len[cnt++] = c;
		}
	}
	return cnt;
}

/* MVCD: header, hello, fragments each acked; 1 when all are in */
int dsp_boot(struct link *l, const unsigned char *image, size_t n)
{
	unsigned char header[sizeof(header_hex) / 2];
	size_t maxp = 2 * (n / FRAG + 2), *off = malloc(maxp * sizeof(size_t)),
	       *len = malloc(maxp * sizeof(size_t)), np, i;
	int load = !strcmp(l->name, "L1") ? 0x7400 : 0x6000;
	struct frame hello;

	for (i = 0; i < sizeof(header); i++) {
		unsigned v;

		sscanf(header_hex + 2 * i, "%2x", &v);
		header[i] = v;
	}
	np = pieces(image, n, off, len);
	header[14] = (n / 2) & 0xff;		/* code size, 16-bit words */
	header[15] = (n / 2) >> 8;
	header[16] = load & 0xff;		/* load / exec address */
	header[17] = load >> 8;
	header[18] = (load + 4) & 0xff;
	header[19] = (load + 4) >> 8;
	header[22] = np & 0xff;
	header[23] = np >> 8;
	link_send(l, 0xfe08, header, sizeof(header));
	if (!link_wait(l, 0xfe31, 1.0, NULL, 0, &hello)) {
		free(off);
		free(len);
		return 0;
	}
	frame_free(&hello);
	for (i = 0; i < np; i++) {
		unsigned char *buf = malloc(len[i] + 2), num[2] = { (i + 1) & 0xff, (i + 1) >> 8 };
		struct frame ack;

		memcpy(buf, num, 2);
		memcpy(buf + 2, image + off[i], len[i]);
		link_send(l, 0xfe0c, buf, len[i] + 2);
		free(buf);
		if (!link_wait(l, 0xfe32, 1.0, num, 2, &ack)) {
			fprintf(stderr, "%s: no ack for fragment %zu\n", l->name, i + 1);
			free(off);
			free(len);
			return 0;
		}
		frame_free(&ack);
	}
	free(off);
	free(len);
	return 1;
}
