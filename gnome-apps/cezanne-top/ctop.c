// SPDX-License-Identifier: MIT
/*
 * libcezanne-top: see ctop.h. One sample = a walk over /proc/<pid>/fd for
 * DRM render / card nodes (fdinfo per client, deduplicated by
 * drm-client-id), the devfreq and thermal files of each GPU, and the
 * vcodec_* attributes of every V4L2 codec device. Processes of other users
 * are invisible without root (fdinfo needs ptrace access).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "ctop.h"

#define MAX_GPUS	4
#define MAX_MEDIA	4
#define MAX_ZONES	8

/* panfrost uapi (include/uapi/drm/panfrost_drm.h), no libdrm needed */
struct panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};
#define PANFROST_GET_PARAM	_IOWR('d', 0x40 + 0x04, struct panfrost_get_param)
#define PANFROST_PROD_ID	0
#define PANFROST_REVISION	1
#define PANFROST_SHADER_PRESENT	2

struct client {			/* one DRM client in one sample */
	uint64_t id;
	int pid;
	uint64_t ns[CTOP_MAX_ENGINES];
	uint64_t mem;
};

struct clients {
	struct client *v;
	size_t n, cap;
};

struct gpu {
	struct ctop_gpu pub;
	char card_node[32];
	char devfreq[160];		/* devfreq directory or "" */
	char zone[MAX_ZONES][64];	/* thermal zone temp files */
	int nzones;
	struct clients cur, prev;
	struct ctop_proc *proc;
	size_t proc_cap;
};

struct mclient {		/* one open file of a codec device */
	int pid;
	char comm[16];
	uint64_t ns;
};

struct media {
	char dir[128];		/* sysfs device directory */
	int encode;
	uint64_t busy, prev_busy;
	struct mclient *cur, *prev;
	size_t ncur, nprev, cap, prev_cap;
};

struct ctop {
	struct gpu gpu[MAX_GPUS];
	uint32_t ngpu;
	struct media media[MAX_MEDIA];
	uint32_t nmedia;
	struct timespec t, prev_t;
	int samples;
};

static int read_str(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return 0;
}

static int read_u64(const char *path, uint64_t *v)
{
	char buf[32];

	if (read_str(path, buf, sizeof(buf)))
		return -1;
	*v = strtoull(buf, NULL, 10);
	return 0;
}

/* make room for need elements; NULL (array unchanged) if out of memory */
static void *grow(void *p, size_t *cap, size_t need, size_t size)
{
	size_t c = *cap ? *cap : 16;

	if (need <= *cap)
		return p;
	while (c < need)
		c *= 2;
	p = realloc(p, c * size);
	if (p)
		*cap = c;
	return p;
}

static const char *mali_name(uint32_t prod)
{
	/* GPU_ID[31:16] = arch major / minor / rev / product major; the
	 * model ignores the arch revision (G77 here reads 0x9080). Valhall
	 * ids as in mainline/mesa-g77 */
	switch (prod & 0xf00f) {
	case 0x9000: return "Mali-G77";
	case 0x9001:
	case 0x9003: return "Mali-G57";
	}
	return NULL;
}

static void gpu_name(struct gpu *g)
{
	struct panfrost_get_param p = { .param = PANFROST_PROD_ID };
	const char *name;
	int fd, cores = 0;

	snprintf(g->pub.name, sizeof(g->pub.name), "%s GPU", g->pub.driver);
	if (strcmp(g->pub.driver, "panfrost"))
		return;
	fd = open(g->pub.render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	if (!ioctl(fd, PANFROST_GET_PARAM, &p)) {
		/* the GPU_ID register, as Vulkan's deviceID (panvk) */
		g->pub.vendor_id = 0x13b5;	/* Arm */
		g->pub.device_id = p.value << 16;
		p = (struct panfrost_get_param){ .param = PANFROST_REVISION };
		if (!ioctl(fd, PANFROST_GET_PARAM, &p))
			g->pub.device_id |= p.value & 0xffff;
		p = (struct panfrost_get_param){ .param = PANFROST_SHADER_PRESENT };
		if (!ioctl(fd, PANFROST_GET_PARAM, &p))
			cores = __builtin_popcountll(p.value);
		name = mali_name(g->pub.device_id >> 16);
		if (name && cores)
			snprintf(g->pub.name, sizeof(g->pub.name), "%s MC%d", name, cores);
		else if (name)
			snprintf(g->pub.name, sizeof(g->pub.name), "%s", name);
		else
			snprintf(g->pub.name, sizeof(g->pub.name), "Mali (id %#x)",
				 g->pub.device_id);
	}
	close(fd);
}

/* thermal zones whose type starts with "gpu" (gpu0-thermal, gpu1-thermal) */
static void gpu_zones(struct gpu *g)
{
	DIR *d = opendir("/sys/class/thermal");
	struct dirent *e;
	char path[300], type[64];

	if (!d)
		return;
	while ((e = readdir(d)) && g->nzones < MAX_ZONES) {
		if (strncmp(e->d_name, "thermal_zone", 12))
			continue;
		snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", e->d_name);
		if (read_str(path, type, sizeof(type)) || strncasecmp(type, "gpu", 3))
			continue;
		snprintf(g->zone[g->nzones++], sizeof(g->zone[0]),
			 "/sys/class/thermal/%s/temp", e->d_name);
	}
	closedir(d);
}

static void find_gpus(struct ctop *c)
{
	DIR *d = opendir("/sys/class/drm");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) && c->ngpu < MAX_GPUS) {
		char path[300], link[256], buf[256], *s;
		struct gpu *g = &c->gpu[c->ngpu];
		DIR *dd;
		struct dirent *f;
		ssize_t n;

		if (strncmp(e->d_name, "renderD", 7))
			continue;
		snprintf(path, sizeof(path), "/sys/class/drm/%s/device", e->d_name);
		n = readlink(path, link, sizeof(link) - 1);
		if (n < 0)
			continue;
		link[n] = 0;
		s = strrchr(link, '/');
		memset(g, 0, sizeof(*g));
		snprintf(g->pub.id, sizeof(g->pub.id), "%s", s ? s + 1 : link);
		snprintf(g->pub.render_node, sizeof(g->pub.render_node), "/dev/dri/%s", e->d_name);
		snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", e->d_name);
		n = readlink(path, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			s = strrchr(buf, '/');
			snprintf(g->pub.driver, sizeof(g->pub.driver), "%s", s ? s + 1 : buf);
		}
		/* the card node of the same device */
		snprintf(path, sizeof(path), "/sys/class/drm/%s/device/drm", e->d_name);
		dd = opendir(path);
		while (dd && (f = readdir(dd)))
			if (!strncmp(f->d_name, "card", 4))
				snprintf(g->card_node, sizeof(g->card_node), "/dev/dri/%s", f->d_name);
		if (dd)
			closedir(dd);
		snprintf(path, sizeof(path), "/sys/class/drm/%s/device/devfreq", e->d_name);
		dd = opendir(path);
		while (dd && (f = readdir(dd)))
			if (f->d_name[0] != '.')
				snprintf(g->devfreq, sizeof(g->devfreq), "%s/%s", path, f->d_name);
		if (dd)
			closedir(dd);
		gpu_name(g);
		gpu_zones(g);
		c->ngpu++;
	}
	closedir(d);
}

static void find_media(struct ctop *c)
{
	DIR *d = opendir("/sys/bus/platform/devices");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) && c->nmedia < MAX_MEDIA) {
		struct media *m = &c->media[c->nmedia];
		char path[300], engine[16];

		snprintf(path, sizeof(path), "/sys/bus/platform/devices/%s/vcodec_engine", e->d_name);
		if (read_str(path, engine, sizeof(engine)))
			continue;
		memset(m, 0, sizeof(*m));
		snprintf(m->dir, sizeof(m->dir), "/sys/bus/platform/devices/%s", e->d_name);
		m->encode = !strcmp(engine, "encode");
		c->nmedia++;
	}
	closedir(d);
}

struct ctop *ctop_open(void)
{
	struct ctop *c = calloc(1, sizeof(*c));

	if (!c)
		return NULL;
	find_gpus(c);
	find_media(c);
	return c;
}

static int engine_index(struct ctop_gpu *g, const char *name, size_t len)
{
	uint32_t i;

	if (len >= sizeof(g->engine_name[0]))
		len = sizeof(g->engine_name[0]) - 1;
	for (i = 0; i < g->engines; i++)
		if (!strncmp(g->engine_name[i], name, len) && !g->engine_name[i][len])
			return i;
	if (g->engines == CTOP_MAX_ENGINES)
		return -1;
	memcpy(g->engine_name[i], name, len);
	g->engine_name[i][len] = 0;
	g->engines++;
	return i;
}

/* one fdinfo file of a DRM fd: add its client unless already seen */
static void read_fdinfo(struct gpu *g, int pid, const char *path)
{
	char buf[2048], *line, *save;
	struct client cl = { .pid = pid }, *v;
	int have_id = 0;
	size_t i;

	if (read_str(path, buf, sizeof(buf)))
		return;
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *v = strchr(line, ':');

		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(line, "drm-client-id")) {
			cl.id = strtoull(v, NULL, 10);
			have_id = 1;
		} else if (!strncmp(line, "drm-engine-", 11)) {
			int k = engine_index(&g->pub, line + 11, strlen(line + 11));

			if (k >= 0)
				cl.ns[k] = strtoull(v, NULL, 10);
		} else if (!strcmp(line, "drm-resident-memory")) {
			char *unit;
			uint64_t x = strtoull(v, &unit, 10);

			while (*unit == ' ' || *unit == '\t')
				unit++;
			cl.mem = x << (!strncmp(unit, "KiB", 3) ? 10 : !strncmp(unit, "MiB", 3) ? 20 : 0);
		}
	}
	if (!have_id)
		return;
	for (i = 0; i < g->cur.n; i++)
		if (g->cur.v[i].id == cl.id)
			return;		/* same client through another fd / process */
	v = grow(g->cur.v, &g->cur.cap, g->cur.n + 1, sizeof(cl));
	if (!v)
		return;
	g->cur.v = v;
	g->cur.v[g->cur.n++] = cl;
}

static void scan_proc(struct ctop *c)
{
	DIR *proc = opendir("/proc");
	struct dirent *p;
	uint32_t i;

	for (i = 0; i < c->ngpu; i++)
		c->gpu[i].cur.n = 0;
	if (!proc)
		return;
	while ((p = readdir(proc))) {
		char path[300], link[64];
		DIR *fds;
		struct dirent *f;
		int pid;

		if (!isdigit((unsigned char)p->d_name[0]))
			continue;
		pid = atoi(p->d_name);
		snprintf(path, sizeof(path), "/proc/%d/fd", pid);
		fds = opendir(path);
		if (!fds)
			continue;
		while ((f = readdir(fds))) {
			ssize_t n;

			if (f->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "/proc/%d/fd/%s", pid, f->d_name);
			n = readlink(path, link, sizeof(link) - 1);
			if (n < 9)
				continue;
			link[n] = 0;
			if (strncmp(link, "/dev/dri/", 9))
				continue;
			for (i = 0; i < c->ngpu; i++) {
				struct gpu *g = &c->gpu[i];

				if (strcmp(link, g->pub.render_node) && strcmp(link, g->card_node))
					continue;
				snprintf(path, sizeof(path), "/proc/%d/fdinfo/%s", pid, f->d_name);
				read_fdinfo(g, pid, path);
				break;
			}
		}
		closedir(fds);
	}
	closedir(proc);
}

static void read_media(struct media *m)
{
	char path[160], buf[4096], *line, *save;
	struct mclient *t;
	size_t tc;

	/* previous sample <- current */
	t = m->prev, m->prev = m->cur, m->cur = t;
	tc = m->prev_cap, m->prev_cap = m->cap, m->cap = tc;
	m->nprev = m->ncur;
	m->ncur = 0;
	m->prev_busy = m->busy;
	snprintf(path, sizeof(path), "%s/vcodec_busy_ns", m->dir);
	read_u64(path, &m->busy);
	snprintf(path, sizeof(path), "%s/vcodec_clients", m->dir);
	if (read_str(path, buf, sizeof(buf)))
		return;
	/* one entry per process: the sum over its open files */
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		struct mclient mc = { 0 }, *v;
		unsigned long long ns, jobs;
		int used = 0;
		size_t i;

		if (sscanf(line, "%d %llu %llu %n", &mc.pid, &ns, &jobs, &used) < 3)
			continue;
		for (i = 0; i < m->ncur && m->cur[i].pid != mc.pid; i++)
			;
		if (i < m->ncur) {
			m->cur[i].ns += ns;
			continue;
		}
		mc.ns = ns;
		snprintf(mc.comm, sizeof(mc.comm), "%s", line + used);
		v = grow(m->cur, &m->cap, m->ncur + 1, sizeof(mc));
		if (!v)
			break;
		m->cur = v;
		m->cur[m->ncur++] = mc;
	}
}

static struct ctop_proc *proc_get(struct gpu *g, int pid)
{
	struct ctop_proc *p;
	char path[64], comm[16];
	uint32_t i;

	for (i = 0; i < g->pub.nproc; i++)
		if (g->proc[i].pid == pid)
			return &g->proc[i];
	p = grow(g->proc, &g->proc_cap, g->pub.nproc + 1, sizeof(*p));
	if (!p)
		return NULL;
	g->proc = p;
	p = &g->proc[g->pub.nproc++];
	memset(p, 0, sizeof(*p));
	p->pid = pid;
	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	if (!read_str(path, comm, sizeof(comm)))
		memcpy(p->comm, comm, sizeof(comm));
	return p;
}

static float pct(uint64_t ns, double dt)
{
	double v = dt > 0 ? ns / dt * 100.0 : 0;

	return v > 100 ? 100 : v;
}

static void gpu_update(struct ctop *c, struct gpu *g, double dt)
{
	struct ctop_gpu *pub = &g->pub;
	uint64_t eng[CTOP_MAX_ENGINES] = { 0 }, v;
	size_t i, j;
	uint32_t k;
	char buf[64];

	pub->valid = 0;
	pub->nproc = 0;
	pub->memory_used = 0;
	for (i = 0; i < g->cur.n; i++) {
		struct client *cl = &g->cur.v[i];
		struct client *old = NULL;
		struct ctop_proc *p = proc_get(g, cl->pid);
		uint64_t busiest = 0;

		pub->memory_used += cl->mem;
		for (j = 0; j < g->prev.n && !old; j++)
			if (g->prev.v[j].id == cl->id)
				old = &g->prev.v[j];
		if (!p)
			continue;
		p->memory += cl->mem;
		p->valid |= CTOP_MEMORY;
		if (!old || !c->samples)
			continue;
		for (k = 0; k < pub->engines; k++) {
			uint64_t d = cl->ns[k] >= old->ns[k] ? cl->ns[k] - old->ns[k] : 0;

			eng[k] += d;
			busiest = d > busiest ? d : busiest;
		}
		p->util += pct(busiest, dt);
		p->valid |= CTOP_UTIL;
	}
	if (c->samples && dt > 0) {
		pub->util = 0;
		for (k = 0; k < pub->engines; k++) {
			pub->engine_util[k] = pct(eng[k], dt);
			if (pub->engine_util[k] > pub->util)
				pub->util = pub->engine_util[k];
		}
		pub->valid |= CTOP_UTIL;
	}
	pub->valid |= CTOP_MEMORY;
	if (!read_str("/proc/meminfo", buf, sizeof(buf)) && !strncmp(buf, "MemTotal:", 9))
		pub->memory_total = strtoull(buf + 9, NULL, 10) << 10;

	if (g->devfreq[0]) {
		char path[200];

		snprintf(path, sizeof(path), "%s/cur_freq", g->devfreq);
		if (!read_u64(path, &v)) {
			pub->clock_mhz = v / 1000000;
			pub->valid |= CTOP_CLOCK;
		}
		snprintf(path, sizeof(path), "%s/max_freq", g->devfreq);
		if (!read_u64(path, &v)) {
			pub->clock_max_mhz = v / 1000000;
			pub->valid |= CTOP_CLOCK_MAX;
		}
	}
	for (k = 0; k < (uint32_t)g->nzones; k++) {
		if (read_str(g->zone[k], buf, sizeof(buf)))
			continue;
		if (!(pub->valid & CTOP_TEMP) || atoi(buf) > pub->temp_mc)
			pub->temp_mc = atoi(buf);
		pub->valid |= CTOP_TEMP;
	}

	/* the SoC's codec engines show up on every GPU (there is one) */
	pub->decode = pub->encode = 0;
	for (k = 0; k < c->nmedia; k++) {
		struct media *m = &c->media[k];
		float *dev_load = m->encode ? &pub->encode : &pub->decode;

		pub->valid |= m->encode ? CTOP_ENCODE : CTOP_DECODE;
		if (c->samples)
			*dev_load += pct(m->busy - m->prev_busy, dt);
		for (i = 0; i < m->ncur; i++) {
			struct mclient *mc = &m->cur[i];
			struct ctop_proc *p = proc_get(g, mc->pid);
			uint64_t before = mc->ns;

			if (!p)
				continue;
			for (j = 0; j < m->nprev; j++)
				if (m->prev[j].pid == mc->pid)
					before = m->prev[j].ns;
			/* a file closed in between can make the sum shrink */
			if (before > mc->ns)
				before = mc->ns;
			if (m->encode)
				p->encode += pct(mc->ns - before, dt);
			else
				p->decode += pct(mc->ns - before, dt);
			p->valid |= m->encode ? CTOP_ENCODE : CTOP_DECODE;
			if (!p->comm[0])
				snprintf(p->comm, sizeof(p->comm), "%s", mc->comm);
		}
	}
	if (pub->decode > 100)
		pub->decode = 100;
	if (pub->encode > 100)
		pub->encode = 100;
	pub->proc = g->proc;
}

int ctop_refresh(struct ctop *c)
{
	double dt;
	uint32_t i;

	for (i = 0; i < c->ngpu; i++) {
		struct clients t = c->gpu[i].prev;

		c->gpu[i].prev = c->gpu[i].cur;
		c->gpu[i].cur = t;
	}
	c->prev_t = c->t;
	clock_gettime(CLOCK_MONOTONIC, &c->t);
	dt = (c->t.tv_sec - c->prev_t.tv_sec) * 1e9 + (c->t.tv_nsec - c->prev_t.tv_nsec);
	scan_proc(c);
	for (i = 0; i < c->nmedia; i++)
		read_media(&c->media[i]);
	for (i = 0; i < c->ngpu; i++)
		gpu_update(c, &c->gpu[i], dt);
	c->samples++;
	return 0;
}

uint32_t ctop_count(const struct ctop *c)
{
	return c->ngpu;
}

const struct ctop_gpu *ctop_gpu(const struct ctop *c, uint32_t i)
{
	return i < c->ngpu ? &c->gpu[i].pub : NULL;
}

void ctop_close(struct ctop *c)
{
	uint32_t i;

	if (!c)
		return;
	for (i = 0; i < c->ngpu; i++) {
		free(c->gpu[i].cur.v);
		free(c->gpu[i].prev.v);
		free(c->gpu[i].proc);
	}
	for (i = 0; i < c->nmedia; i++) {
		free(c->media[i].cur);
		free(c->media[i].prev);
	}
	free(c);
}
