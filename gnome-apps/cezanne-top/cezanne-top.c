// SPDX-License-Identifier: MIT
/*
 * cezanne-top: nvtop-style monitor for SoC GPUs and media engines - GPU
 * render load per engine, clock, temperature, GPU memory, hardware
 * decoder / encoder load, and the processes using them (libcezanne-top).
 *
 * cezanne-top [-d seconds] [-n samples] [-j]
 *   -d  refresh interval (default 1)
 *   -n  stop after n samples (default: run until interrupted)
 *   -j  one JSON object per sample instead of the screen
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ctop.h"

static void bar(float pct, int width)
{
	int n = pct / 100 * width + 0.5, i;

	putchar('[');
	for (i = 0; i < width; i++)
		putchar(i < n ? '|' : ' ');
	putchar(']');
}

static const char *size_str(uint64_t b, char *buf, size_t len)
{
	if (b >= 1ULL << 30)
		snprintf(buf, len, "%.1fG", b / (double)(1ULL << 30));
	else
		snprintf(buf, len, "%.1fM", b / (double)(1ULL << 20));
	return buf;
}

static void pct_col(uint32_t valid, uint32_t bit, float v)
{
	if (valid & bit)
		printf(" %5.1f", v);
	else
		printf("     -");
}

static int by_load(const void *a, const void *b)
{
	const struct ctop_proc *x = a, *y = b;
	float lx = x->util + x->decode + x->encode, ly = y->util + y->decode + y->encode;

	if (lx != ly)
		return lx < ly ? 1 : -1;
	return x->memory < y->memory ? 1 : x->memory > y->memory ? -1 : 0;
}

static void show(const struct ctop_gpu *g)
{
	struct ctop_proc *p = malloc(g->nproc * sizeof(*p) + 1);
	char a[16], b[16];
	uint32_t i;

	printf("%s  (%s, %s)\n", g->name, g->id, g->driver);
	printf("GPU  ");
	bar(g->valid & CTOP_UTIL ? g->util : 0, 30);
	printf(" %5.1f%%  ", g->util);
	for (i = 0; i < g->engines; i++)
		printf(" %s %.1f%%", g->engine_name[i], g->engine_util[i]);
	printf("\nMEM  ");
	bar(g->memory_total ? 100.0 * g->memory_used / g->memory_total : 0, 30);
	printf(" %s / %s\n", size_str(g->memory_used, a, sizeof(a)),
	       size_str(g->memory_total, b, sizeof(b)));
	printf("DEC  ");
	bar(g->decode, 30);
	if (g->valid & CTOP_DECODE)
		printf(" %5.1f%%\n", g->decode);
	else
		printf("   n/a\n");
	printf("ENC  ");
	bar(g->encode, 30);
	if (g->valid & CTOP_ENCODE)
		printf(" %5.1f%%\n", g->encode);
	else
		printf("   n/a\n");
	if (g->valid & CTOP_CLOCK)
		printf("CLK  %u / %u MHz", g->clock_mhz, g->clock_max_mhz);
	if (g->valid & CTOP_TEMP)
		printf("   TEMP %.1f C", g->temp_mc / 1000.0);
	printf("\n\n%7s  %-16s %6s %8s %6s %6s\n", "PID", "COMMAND", "GPU%", "GPU MEM",
	       "DEC%", "ENC%");
	if (!p)
		return;
	memcpy(p, g->proc, g->nproc * sizeof(*p));
	qsort(p, g->nproc, sizeof(*p), by_load);
	for (i = 0; i < g->nproc; i++) {
		printf("%7d  %-16s", p[i].pid, p[i].comm);
		pct_col(p[i].valid, CTOP_UTIL, p[i].util);
		if (p[i].valid & CTOP_MEMORY)
			printf(" %8s", size_str(p[i].memory, a, sizeof(a)));
		else
			printf(" %8s", "-");
		pct_col(p[i].valid, CTOP_DECODE, p[i].decode);
		pct_col(p[i].valid, CTOP_ENCODE, p[i].encode);
		putchar('\n');
	}
	free(p);
}

static void json_str(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			printf("\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			printf("\\u%04x", *s);
		else
			putchar(*s);
	}
	putchar('"');
}

static void json_num(const char *key, uint32_t valid, uint32_t bit, double v, int comma)
{
	printf("%s\"%s\":", comma ? "," : "", key);
	if (valid & bit)
		printf("%.2f", v);
	else
		printf("null");
}

static void json(const struct ctop *c)
{
	uint32_t i, k;

	printf("{\"gpus\":[");
	for (i = 0; i < ctop_count(c); i++) {
		const struct ctop_gpu *g = ctop_gpu(c, i);

		printf("%s{\"id\":", i ? "," : "");
		json_str(g->id);
		printf(",\"name\":");
		json_str(g->name);
		printf(",\"driver\":");
		json_str(g->driver);
		json_num("util", g->valid, CTOP_UTIL, g->util, 1);
		printf(",\"engines\":{");
		for (k = 0; k < g->engines; k++) {
			printf("%s", k ? "," : "");
			json_str(g->engine_name[k]);
			printf(":%.2f", g->engine_util[k]);
		}
		printf("}");
		json_num("clock_mhz", g->valid, CTOP_CLOCK, g->clock_mhz, 1);
		json_num("clock_max_mhz", g->valid, CTOP_CLOCK_MAX, g->clock_max_mhz, 1);
		json_num("temp_c", g->valid, CTOP_TEMP, g->temp_mc / 1000.0, 1);
		printf(",\"memory_used\":%llu,\"memory_total\":%llu",
		       (unsigned long long)g->memory_used, (unsigned long long)g->memory_total);
		json_num("decode", g->valid, CTOP_DECODE, g->decode, 1);
		json_num("encode", g->valid, CTOP_ENCODE, g->encode, 1);
		printf(",\"processes\":[");
		for (k = 0; k < g->nproc; k++) {
			const struct ctop_proc *p = &g->proc[k];

			printf("%s{\"pid\":%d,\"comm\":", k ? "," : "", p->pid);
			json_str(p->comm);
			json_num("util", p->valid, CTOP_UTIL, p->util, 1);
			printf(",\"memory\":%llu", (unsigned long long)p->memory);
			json_num("decode", p->valid, CTOP_DECODE, p->decode, 1);
			json_num("encode", p->valid, CTOP_ENCODE, p->encode, 1);
			printf("}");
		}
		printf("]}");
	}
	printf("]}\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	double delay = 1;
	long count = -1, n = 0;
	int as_json = 0, opt;
	struct timespec ts;
	struct ctop *c;
	uint32_t i;

	while ((opt = getopt(argc, argv, "d:n:jh")) != -1) {
		switch (opt) {
		case 'd':
			delay = atof(optarg);
			break;
		case 'n':
			count = atol(optarg);
			break;
		case 'j':
			as_json = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-d seconds] [-n samples] [-j]\n", argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}
	if (delay < 0.1)
		delay = 0.1;
	c = ctop_open();
	if (!c || !ctop_count(c)) {
		fprintf(stderr, "cezanne-top: no DRM render device found\n");
		return 1;
	}
	ts.tv_sec = delay;
	ts.tv_nsec = (delay - ts.tv_sec) * 1e9;
	ctop_refresh(c);		/* first sample: loads need two */
	while (count < 0 || n++ < count) {
		nanosleep(&ts, NULL);
		ctop_refresh(c);
		if (as_json) {
			json(c);
			continue;
		}
		if (isatty(STDOUT_FILENO))
			printf("\033[H\033[2J");
		for (i = 0; i < ctop_count(c); i++)
			show(ctop_gpu(c, i));
		fflush(stdout);
	}
	ctop_close(c);
	return 0;
}
