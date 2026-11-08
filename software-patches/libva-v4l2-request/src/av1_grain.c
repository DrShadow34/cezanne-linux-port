// SPDX-License-Identifier: MIT
/*
 * AV1 film grain synthesis (spec 7.18.3) on the CPU: the hardware decodes
 * the frame without grain (the reference keeps it that way) and the client
 * shows current_display_picture, which gets the decoded frame plus grain.
 *
 * 4:2:0 only (Profile 0). Surfaces are NV12 (8 bit) or P010 (10 bit, the
 * value in the top bits of each 16-bit sample).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <linux/dma-buf.h>

#include "cvd.h"
#include "av1_gaussian.h"

#define GRAIN_W		82
#define GRAIN_H		73
#define GRAIN_CW	44	/* 4:2:0 chroma grain template */
#define GRAIN_CH	38
#define BLK		32

struct grain {
	int bd, gss, grain_min, grain_max, scaling_shift;
	int ar_lag, ar_shift, overlap, cfl;
	int num_y, num_uv[2];
	int min_y, max_y, min_uv, max_uv;
	int mult[2], luma_mult[2], offset[2];	/* already re-centred */
	unsigned int seed;
	int16_t lut_y[GRAIN_H][GRAIN_W];
	int16_t lut_uv[2][GRAIN_CH][GRAIN_CW];
	uint8_t scale[3][1 << 12];
};

static int rnd(int bits, unsigned int *state)
{
	unsigned int r = *state;
	unsigned int bit = (r ^ (r >> 1) ^ (r >> 3) ^ (r >> 12)) & 1;

	*state = (r >> 1) | (bit << 15);
	return (*state >> (16 - bits)) & ((1 << bits) - 1);
}

static int round2(int x, int n)
{
	return n ? (x + (1 << (n - 1))) >> n : x;
}

static int clip(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* 7.18.3.3: white noise from the Gaussian sequence, then the AR filter */
static void gen_luma(struct grain *g, const int8_t *coef)
{
	unsigned int seed = g->seed;
	int shift = 12 - g->bd + g->gss, x, y, dx, dy;

	for (y = 0; y < GRAIN_H; y++)
		for (x = 0; x < GRAIN_W; x++)
			g->lut_y[y][x] = round2(gaussian_sequence[rnd(11, &seed)], shift);
	for (y = 3; y < GRAIN_H; y++)
		for (x = 3; x < GRAIN_W - 3; x++) {
			const int8_t *c = coef;
			int sum = 0;

			for (dy = -g->ar_lag; dy <= 0; dy++)
				for (dx = -g->ar_lag; dx <= g->ar_lag; dx++) {
					if (!dx && !dy)
						goto done;
					sum += *c++ * g->lut_y[y + dy][x + dx];
				}
done:
			g->lut_y[y][x] = clip(g->lut_y[y][x] + round2(sum, g->ar_shift), g->grain_min,
					      g->grain_max);
		}
}

static void gen_chroma(struct grain *g, int pl, const int8_t *coef)
{
	unsigned int seed = g->seed ^ (pl ? 0x49d8 : 0xb524);
	int shift = 12 - g->bd + g->gss, x, y, dx, dy;

	for (y = 0; y < GRAIN_CH; y++)
		for (x = 0; x < GRAIN_CW; x++)
			g->lut_uv[pl][y][x] = round2(gaussian_sequence[rnd(11, &seed)], shift);
	for (y = 3; y < GRAIN_CH; y++)
		for (x = 3; x < GRAIN_CW - 3; x++) {
			const int8_t *c = coef;
			int sum = 0;

			for (dy = -g->ar_lag; dy <= 0; dy++)
				for (dx = -g->ar_lag; dx <= g->ar_lag; dx++) {
					if (!dx && !dy) {
						/* the last coefficient weighs the co-located luma grain */
						if (g->num_y) {
							int lx = ((x - 3) << 1) + 3, ly = ((y - 3) << 1) + 3;
							int l = g->lut_y[ly][lx] + g->lut_y[ly][lx + 1] +
								g->lut_y[ly + 1][lx] + g->lut_y[ly + 1][lx + 1];

							sum += round2(l, 2) * *c;
						}
						goto done;
					}
					sum += *c++ * g->lut_uv[pl][y + dy][x + dx];
				}
done:
			g->lut_uv[pl][y][x] = clip(g->lut_uv[pl][y][x] + round2(sum, g->ar_shift),
						   g->grain_min, g->grain_max);
		}
}

/* 7.18.3.4: piecewise linear scaling function, one entry per sample value */
static void gen_scaling(uint8_t *lut, int bd, const uint8_t *val, const uint8_t *scl, int n)
{
	int size = 1 << bd, sh = bd - 8, i, x, end;

	if (!n) {
		memset(lut, 0, size);
		return;
	}
	memset(lut, scl[0], val[0] << sh);
	for (i = 0; i < n - 1; i++) {
		int bx = val[i], by = scl[i], dx = val[i + 1] - bx, dy = scl[i + 1] - by;
		int delta = dx > 0 ? dy * ((0x10000 + (dx >> 1)) / dx) : 0, d = 0x8000;

		for (x = 0; x < dx; x++, d += delta)
			lut[(bx + x) << sh] = by + (d >> 16);
	}
	end = val[n - 1] << sh;
	memset(lut + end, scl[n - 1], size - end);
	if (!sh)
		return;
	/* fill the values between the 8-bit points */
	for (i = 0; i < n - 1; i++) {
		int bx = val[i] << sh, dx = (val[i + 1] << sh) - bx, pad = 1 << sh;

		for (x = 0; x < dx; x += pad) {
			int range = lut[bx + x + pad] - lut[bx + x], r = pad >> 1, k;

			for (k = 1; k < pad; k++) {
				r += range;
				lut[bx + x + k] = lut[bx + x] + (r >> sh);
			}
		}
	}
}

/* a grain sample of block (bx, by) back from the current one, offsets as drawn */
static int lut_at(const struct grain *g, int pl, const int off[2][2], int bx, int by, int x, int y)
{
	int r = off[bx][by];

	if (pl < 0)
		return g->lut_y[9 + 2 * (r & 15) + y + BLK * by][9 + 2 * (r >> 4) + x + BLK * bx];
	return g->lut_uv[pl][6 + (r & 15) + y + (BLK / 2) * by][6 + (r >> 4) + x + (BLK / 2) * bx];
}

/* the grain of one sample with the left / top overlap blends of 7.18.3.5 */
static int grain_at(const struct grain *g, int pl, const int off[2][2], int x, int y, int xo, int yo)
{
	static const int wl[2][2] = { { 27, 17 }, { 17, 27 } };
	static const int wc[2] = { 23, 22 };
	const int *wx = pl < 0 ? wl[x < 2 ? x : 0] : wc, *wy = pl < 0 ? wl[y < 2 ? y : 0] : wc;
	int v = lut_at(g, pl, off, 0, 0, x, y), top;

	if (x < xo)
		v = clip(round2(lut_at(g, pl, off, 1, 0, x, y) * wx[0] + v * wx[1], 5), g->grain_min,
			 g->grain_max);
	if (y < yo) {
		top = lut_at(g, pl, off, 0, 1, x, y);
		if (x < xo)
			top = clip(round2(lut_at(g, pl, off, 1, 1, x, y) * wx[0] + top * wx[1], 5),
				   g->grain_min, g->grain_max);
		v = clip(round2(top * wy[0] + v * wy[1], 5), g->grain_min, g->grain_max);
	}
	return v;
}

struct plane {
	uint8_t *base;
	unsigned int pitch, step, off;	/* step / off: bytes between / to the samples */
};

/* sample access: 8-bit NV12 or 10-bit P010 (value << 6) */
static int px_get(const struct plane *p, int hi, int x, int y)
{
	const uint8_t *a = p->base + (size_t)y * p->pitch + (size_t)x * p->step + p->off;

	return hi ? *(const uint16_t *)a >> (16 - hi) : *a;
}

static void px_put(const struct plane *p, int hi, int x, int y, int v)
{
	uint8_t *a = p->base + (size_t)y * p->pitch + (size_t)x * p->step + p->off;

	if (hi)
		*(uint16_t *)a = v << (16 - hi);
	else
		*a = v;
}

/*
 * Adds a block's grain to its samples: T = uint8_t (NV12) or uint16_t (P010,
 * value << sh). Chroma (pl >= 0) samples are every other T (interleaved U /
 * V) and weigh the luma row 2y; lw is the luma width, x0 the block's column.
 */
#define DEFINE_ADD_BLOCK(T)									\
static void add_block_##T(const struct grain *g, int pl, const int16_t (*blk)[BLK],		\
			  uint8_t *dp, const uint8_t *sp, const uint8_t *lp, size_t pitch,	\
			  int sh, int lw, int x0, int bw, int bh)				\
{												\
	int sub = pl >= 0, step = 1 + sub, maxv = (1 << g->bd) - 1, x, y;			\
	int lo = sub ? g->min_uv : g->min_y, top = sub ? g->max_uv : g->max_y;			\
	const uint8_t *scale = !sub || g->cfl ? g->scale[0] : g->scale[1 + pl];			\
												\
	for (y = 0; y < bh; y++) {								\
		T *d = (T *)(dp + y * pitch);							\
		const T *s = (const T *)(sp + y * pitch);					\
		const T *l = sub ? (const T *)(lp + 2 * y * pitch) : NULL;			\
												\
		for (x = 0; x < bw; x++) {							\
			int v = s[x * step] >> sh, u = v, n;					\
												\
			if (sub) {								\
				/* an odd width repeats the last luma column */			\
				int lx = 2 * (x0 + x), lx1 = lx + 1 < lw ? lx + 1 : lx;		\
				int avg = ((l[lx - 2 * x0] >> sh) + (l[lx1 - 2 * x0] >> sh) + 1) >> 1;	\
												\
				u = avg;							\
				if (!g->cfl)							\
					u = clip(((avg * g->luma_mult[pl] + v * g->mult[pl]) >> 6) +	\
						 g->offset[pl] * (1 << (g->bd - 8)), 0, maxv);	\
			}									\
			n = round2(scale[u] * blk[y][x], g->scaling_shift);			\
			d[x * step] = clip(v + n, lo, top) << sh;				\
		}									\
	}										\
}
DEFINE_ADD_BLOCK(uint8_t)
DEFINE_ADD_BLOCK(uint16_t)

/* one 32-line stripe of a plane (pl -1 = luma; lw = the luma width) */
static void apply_row(const struct grain *g, int pl, const struct plane *dst, const struct plane *src,
		      const struct plane *luma, int lw, int hi, int w, int h, int row)
{
	int sub = pl >= 0, bs = BLK >> sub, rows = 1 + (g->overlap && row > 0);
	int bh = h - row * bs < bs ? h - row * bs : bs, y0 = row * bs;
	int sz = hi ? 2 : 1, sh = hi ? 16 - hi : 0;
	unsigned int seed[2];
	int off[2][2] = { { 0 } }, bx, x, y, i;
	int16_t blk[BLK][BLK];

	for (i = 0; i < rows; i++) {
		seed[i] = g->seed;
		seed[i] ^= (((row - i) * 37 + 178) & 0xff) << 8;
		seed[i] ^= ((row - i) * 173 + 105) & 0xff;
	}
	for (bx = 0; bx < w; bx += bs) {
		int bw = w - bx < bs ? w - bx : bs;
		int xo = g->overlap && bx ? (2 >> sub < bw ? 2 >> sub : bw) : 0;
		int yo = g->overlap && row ? (2 >> sub < bh ? 2 >> sub : bh) : 0;
		size_t at = (size_t)y0 * src->pitch + (size_t)bx * src->step + src->off;
		uint8_t *dp = dst->base + (size_t)y0 * dst->pitch + (size_t)bx * dst->step + dst->off;
		const uint8_t *lp = sub ? luma->base + (size_t)2 * y0 * luma->pitch +
				    (size_t)2 * bx * luma->step : NULL;

		if (g->overlap && bx)
			for (i = 0; i < rows; i++)
				off[1][i] = off[0][i];
		for (i = 0; i < rows; i++)
			off[0][i] = rnd(8, &seed[i]);
		/* the block's grain: its template window, blended where it overlaps */
		for (y = 0; y < bh; y++) {
			const int16_t *t = sub ? &g->lut_uv[pl][6 + (off[0][0] & 15) + y][6 + (off[0][0] >> 4)] :
					   &g->lut_y[9 + 2 * (off[0][0] & 15) + y][9 + 2 * (off[0][0] >> 4)];

			if (y < yo) {
				for (x = 0; x < bw; x++)
					blk[y][x] = grain_at(g, pl, off, x, y, xo, yo);
				continue;
			}
			for (x = 0; x < xo; x++)
				blk[y][x] = grain_at(g, pl, off, x, y, xo, yo);
			memcpy(&blk[y][xo], t + xo, (bw - xo) * sizeof(blk[0][0]));
		}
		if (sz == 1)
			add_block_uint8_t(g, pl, (const int16_t (*)[BLK])blk, dp, src->base + at, lp,
					  src->pitch, sh, lw, bx, bw, bh);
		else
			add_block_uint16_t(g, pl, (const int16_t (*)[BLK])blk, dp, src->base + at, lp,
					   src->pitch, sh, lw, bx, bw, bh);
	}
}

static void copy_plane(const struct plane *dst, const struct plane *src, int hi, int w, int h)
{
	int x, y;

	for (y = 0; y < h; y++) {
		if (src->step == (hi ? 2u : 1u)) {
			memcpy(dst->base + (size_t)y * dst->pitch, src->base + (size_t)y * src->pitch,
			       (size_t)w * src->step);
			continue;
		}
		for (x = 0; x < w; x++)
			px_put(dst, hi, x, y, px_get(src, hi, x, y));
	}
}

#define FG_THREADS	4

/* the highest-capacity CPUs (big cores): helper threads run there */
static cpu_set_t *fg_big_cpus(void)
{
	static cpu_set_t set;
	static int done;
	int cap[CPU_SETSIZE] = { 0 }, best = 0, c;
	char path[64];
	FILE *f;

	if (done)
		return CPU_COUNT(&set) ? &set : NULL;
	done = 1;
	CPU_ZERO(&set);
	/* in a seccomp sandbox (Firefox's RDD) setting affinity kills the process */
	f = fopen("/proc/self/status", "r");
	if (!f)
		return NULL;
	while (fgets(path, sizeof(path), f))
		if (!strncmp(path, "Seccomp:", 8) && atoi(path + 8))
			best = -1;
	fclose(f);
	if (best < 0)
		return NULL;
	for (c = 0; c < 64; c++) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", c);
		f = fopen(path, "r");
		if (!f)
			break;
		if (fscanf(f, "%d", &cap[c]) != 1)
			cap[c] = 0;
		fclose(f);
		if (cap[c] > best)
			best = cap[c];
	}
	for (c = 0; c < 64 && best; c++)
		if (cap[c] == best)
			CPU_SET(c, &set);
	return CPU_COUNT(&set) ? &set : NULL;
}

struct fg_job {
	const struct grain *g;
	const struct plane *dy, *sy, *duv, *suv;
	int hi, w, h, cw, ch, n, k;	/* stripes k, k + n, ... */
};

static void *fg_stripes(void *arg)
{
	const struct fg_job *j = arg;
	const struct grain *g = j->g;
	int row, pl;

	for (row = j->k; row * BLK < j->h; row += j->n) {
		if (g->num_y)
			apply_row(g, -1, j->dy, j->sy, NULL, j->w, j->hi, j->w, j->h, row);
		for (pl = 0; pl < 2; pl++)
			if ((g->num_uv[pl] || g->cfl) && row * (BLK / 2) < j->ch)
				apply_row(g, pl, &j->duv[pl], &j->suv[pl], j->sy, j->w, j->hi, j->cw, j->ch,
					  row);
	}
	return NULL;
}

void cvd_av1_film_grain(struct cvd_surface *dst, const struct cvd_surface *src,
			const VADecPictureParameterBufferAV1 *pp)
{
	const VAFilmGrainStructAV1 *fg = &pp->film_grain_info;
	struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
	static struct grain g;	/* 30 KiB of templates and tables, decodes are serial */
	int bd = pp->bit_depth_idx == 1 ? 10 : 8, hi = bd > 8 ? bd : 0;
	int w = pp->frame_width_minus1 + 1, h = pp->frame_height_minus1 + 1;
	int cw, ch, pl, i, n;
	struct plane sy, dy, suv[2], duv[2];
	struct fg_job job[FG_THREADS];
	pthread_t tid[FG_THREADS];
	pthread_attr_t attr;
	cpu_set_t *big;

	if (w > (int)src->width)
		w = src->width;
	if (w > (int)dst->width)
		w = dst->width;
	if (h > (int)src->height)
		h = src->height;
	if (h > (int)dst->height)
		h = dst->height;
	cw = (w + 1) / 2;
	ch = (h + 1) / 2;

	memset(&g, 0, offsetof(struct grain, lut_y));
	g.bd = bd;
	g.grain_min = -(128 << (bd - 8));
	g.grain_max = (128 << (bd - 8)) - 1;
	g.scaling_shift = fg->film_grain_info_fields.bits.grain_scaling_minus_8 + 8;
	g.ar_lag = fg->film_grain_info_fields.bits.ar_coeff_lag;
	g.ar_shift = fg->film_grain_info_fields.bits.ar_coeff_shift_minus_6 + 6;
	g.overlap = fg->film_grain_info_fields.bits.overlap_flag;
	g.cfl = fg->film_grain_info_fields.bits.chroma_scaling_from_luma;
	g.num_y = fg->num_y_points;
	g.num_uv[0] = fg->num_cb_points;
	g.num_uv[1] = fg->num_cr_points;
	g.mult[0] = fg->cb_mult - 128;
	g.mult[1] = fg->cr_mult - 128;
	g.luma_mult[0] = fg->cb_luma_mult - 128;
	g.luma_mult[1] = fg->cr_luma_mult - 128;
	g.offset[0] = fg->cb_offset - 256;
	g.offset[1] = fg->cr_offset - 256;
	g.seed = fg->grain_seed;
	if (fg->film_grain_info_fields.bits.clip_to_restricted_range) {
		g.min_y = g.min_uv = 16 << (bd - 8);
		g.max_y = 235 << (bd - 8);
		/* MC_IDENTITY (RGB) keeps the luma range on all planes */
		g.max_uv = (pp->matrix_coefficients == 0 ? 235 : 240) << (bd - 8);
	} else {
		g.min_y = g.min_uv = 0;
		g.max_y = g.max_uv = (1 << bd) - 1;
	}

	g.gss = fg->film_grain_info_fields.bits.grain_scale_shift;
	gen_luma(&g, fg->ar_coeffs_y);
	if (g.num_uv[0] || g.cfl)
		gen_chroma(&g, 0, fg->ar_coeffs_cb);
	if (g.num_uv[1] || g.cfl)
		gen_chroma(&g, 1, fg->ar_coeffs_cr);
	if (g.num_y || g.cfl)
		gen_scaling(g.scale[0], bd, fg->point_y_value, fg->point_y_scaling, g.num_y);
	if (g.num_uv[0])
		gen_scaling(g.scale[1], bd, fg->point_cb_value, fg->point_cb_scaling, g.num_uv[0]);
	if (g.num_uv[1])
		gen_scaling(g.scale[2], bd, fg->point_cr_value, fg->point_cr_scaling, g.num_uv[1]);

	sy = (struct plane){ src->map, src->pitch, hi ? 2 : 1, 0 };
	dy = (struct plane){ dst->map, dst->pitch, hi ? 2 : 1, 0 };
	for (pl = 0; pl < 2; pl++) {
		suv[pl] = (struct plane){ src->map + src->luma_size, src->pitch, hi ? 4 : 2, (hi ? 2 : 1) * pl };
		duv[pl] = (struct plane){ dst->map + dst->luma_size, dst->pitch, hi ? 4 : 2, (hi ? 2 : 1) * pl };
	}

	cvd_ioctl(dst->fd, DMA_BUF_IOCTL_SYNC, &sync);
	/* planes without grain first, then the stripes (independent) on up to 4 threads */
	if (!g.num_y)
		copy_plane(&dy, &sy, hi, w, h);
	for (pl = 0; pl < 2; pl++)
		if (!g.num_uv[pl] && !g.cfl)
			copy_plane(&duv[pl], &suv[pl], hi, cw, ch);
	big = fg_big_cpus();
	if (big) {
		pthread_attr_init(&attr);
		pthread_attr_setaffinity_np(&attr, sizeof(*big), big);
	}
	/* helpers left to the scheduler start on the little cores: slower than one thread */
	n = big ? (h + BLK - 1) / BLK / 8 : 1;
	n = n < 1 ? 1 : n > FG_THREADS ? FG_THREADS : n;
	for (i = 0; i < n; i++) {
		job[i] = (struct fg_job){ &g, &dy, &sy, duv, suv, hi, w, h, cw, ch, n, i };
		if (i && pthread_create(&tid[i], big ? &attr : NULL, fg_stripes, &job[i]))
			job[i].n = 0;		/* not started: done below */
	}
	fg_stripes(&job[0]);
	for (i = 1; i < n; i++) {
		if (job[i].n)
			pthread_join(tid[i], NULL);
		else
			fg_stripes(&(struct fg_job){ &g, &dy, &sy, duv, suv, hi, w, h, cw, ch, n, i });
	}
	if (big)
		pthread_attr_destroy(&attr);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
	cvd_ioctl(dst->fd, DMA_BUF_IOCTL_SYNC, &sync);
}
