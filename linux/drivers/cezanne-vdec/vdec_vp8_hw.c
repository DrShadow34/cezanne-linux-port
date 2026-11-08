// SPDX-License-Identifier: GPL-2.0
/*
 * MT6885 VP8 decode on the core engine (no LAT), as MIUI's vpud drives it
 * (captures in mainline/vcodec/trace/caps2/cap-v8-*, NOTES "VP8").
 *
 * The VP8 VLD block (core +0x27800) has two bit readers: the first
 * partition on core vld, the token partitions on core bs2. Reading
 * +0x00 peeks 32 bits of the first reader, reading +4 * n consumes n bits;
 * +0x200 / +0x200 + 4 * n are the same on the second reader. Reading
 * +0x110 / +0x114 starts the bool decoder of a reader at its position and
 * +0x80 + 4 * n returns an n-bit literal from the first one.
 *
 * vpud parses the frame header itself through those reads; the hardware
 * then decodes the macroblocks from wherever the bool decoder stopped.
 * So the header is parsed here the same way, in syntax order, with two
 * hardware commands in the middle that parse the coefficient (+0x148)
 * and motion vector (+0x14c) probability updates against the tables
 * written before them. Those updates replace probabilities, so starting
 * from the client's (already updated) tables gives the same result.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "vdec.h"

static int vp8_poll(struct vdec_dev *d, u32 reg, u32 mask, u32 want, const char *what)
{
	u32 v;
	int ret = readl_poll_timeout_atomic(d->regs + reg, v, (v & mask) == want, 1, 20000);

	if (ret)
		dev_err(d->dev, "vp8: %s timed out (%#x = %#x)\n", what, reg, v);
	return ret;
}

/* reset one of the two bit readers (VP8_ResetVLD) */
static void vp8_reset(struct vdec_dev *d, bool second)
{
	u32 rd = second ? CORE_BS2 : CORE_VLD;

	vdec_w(d, SOC_GCON + 0x0, 1);
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);
	vdec_w(d, CORE_GCON + 0x0, 1);
	vdec_w(d, CORE_MISC + 0xf4, vdec_r(d, CORE_MISC + 0xf4) & ~1u);
	vdec_w(d, CORE_GCON + 0x0, 1);
	/* larb4 sleep protection around the reader reset */
	vdec_w(d, CORE_LARB + 0xc, vdec_r(d, CORE_LARB + 0xc) | 1);
	vp8_poll(d, CORE_LARB + 0xc, BIT(16), BIT(16), "larb idle");
	vdec_w(d, rd + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0x84, 1);
	vdec_w(d, CORE_MISC + 0xc8, 0xff7f);
	vdec_w(d, CORE_MISC + 0xcc, 0x71201102);
	vdec_w(d, CORE_MISC + 0x178, ~4u);
	vdec_w(d, CORE_MC + 0x920, vdec_r(d, CORE_MC + 0x920) | 0x01000000);
	vdec_w(d, rd + 0x108, 0);
	vdec_w(d, CORE_LARB + 0xc, vdec_r(d, CORE_LARB + 0xc) & ~1u);
	vdec_w(d, CORE_GCON + 0x18, 1);
	vdec_w(d, CORE_MISC + 0xec, 1);
	vdec_w(d, CORE_VP8 + 0xa4, 1);
	vdec_w(d, CORE_MISC + 0x160, 0x0065ff10);
}

/* n bits from a reader's barrel shifter (VP8_VLDGetBits) */
static u32 vp8_bits(struct vdec_dev *d, bool second, unsigned int n)
{
	u32 off = second ? 0x200 : 0, peek, i;

	for (i = 0; i < n / 32; i++)
		vdec_r(d, CORE_VP8 + (off | 0x80));
	peek = vdec_r(d, CORE_VP8 + off);
	vdec_r(d, CORE_VP8 + (off | (n & 31) << 2));
	return n >= 32 ? peek : peek >> (32 - n);
}

/* an n-bit literal from the first partition's bool decoder */
static u32 vp8_lit(struct vdec_dev *d, unsigned int n)
{
	return vdec_r(d, CORE_VP8 + 0x80 + 4 * n);
}

static s32 vp8_slit(struct vdec_dev *d, unsigned int n)
{
	u32 v = vp8_lit(d, n);

	return vp8_lit(d, 1) ? -(s32)v : (s32)v;
}

/* bitstream window end: just past the data, as MIUI sizes it (the whole
 * buffer as the window made larger frames come out empty) */
static dma_addr_t vp8_bs_end(const struct vdec_vp8_run *run)
{
	return run->bs_dma + min_t(u32, ALIGN(run->bs_len + SZ_2K, 64), run->bs_size);
}

/*
 * Point a reader at addr (VP8_InitBarrelShifter): the bitstream buffer
 * bounds >> 6, the start address and the data end, a fetch, then skip
 * the bytes before addr inside its 16-byte line.
 */
static int vp8_bs_init(struct vdec_dev *d, const struct vdec_vp8_run *run, bool second,
		       dma_addr_t addr)
{
	u32 rd = second ? CORE_BS2 : CORE_VLD;
	unsigned int i;
	int ret;

	vdec_w(d, rd + 0xb4, run->bs_dma >> 6);
	vdec_w(d, rd + 0xb8, vp8_bs_end(run) >> 6);
	vdec_w(d, rd + 0xb0, addr);
	/* data end: MIUI leaves 1 KiB of slack after the frame (the readers
	 * prefetch; ending at the last byte stalls the final MB row) */
	vdec_w(d, rd + 0x110, run->bs_dma + min(run->bs_len + SZ_1K, run->bs_size));
	vdec_w(d, rd + 0x108, 0x100);
	vdec_w(d, rd + 0x108, 0);
	/* reader DMA configuration as MIUI's VP8 jobs find it (the reset
	 * value is 0x04000014) */
	vdec_w(d, rd + 0x140, 0x01000014);
	vdec_w(d, rd + 0x8c, 0x100000);
	ret = vp8_poll(d, rd + 0xe8, 1, 1, "bitstream fetch");
	if (!ret && (vdec_r(d, CORE_VLD + 0xf4) & BIT(15)))
		ret = vp8_poll(d, CORE_VLD + 0xf4, 1, 1, "vld ready");
	vdec_w(d, rd + 0x8c, 0x800000);
	for (i = 0; i < (addr & 15); i++)
		vp8_bits(d, second, 8);
	return ret;
}

/* segment-ID SRAM through the mc+93c / +940 port (1 << 16 = write) */
static void vp8_seg_sram(struct vdec_dev *d, u32 *map, bool write)
{
	unsigned int i, j;

	for (i = 0; i < VDEC_VP8_SEG_WORDS / 4; i++) {
		for (j = 4; j-- > 0;) {
			vdec_w(d, CORE_MC + 0x93c, (write ? BIT(16) : 0) | (0x400 + i) << 2 | j);
			if (write)
				vdec_w(d, CORE_MC + 0x940, map[i * 4 + j]);
			else
				map[i * 4 + j] = vdec_r(d, CORE_MC + 0x940);
		}
	}
}

void vdec_hw_vp8_save_seg(struct vdec_dev *d, u32 *map)
{
	vp8_seg_sram(d, map, false);
}

static u32 le_word(const u8 *p, unsigned int n)
{
	u32 v = 0;
	unsigned int i;

	for (i = 0; i < n && i < 4; i++)
		v |= (u32)p[i] << (8 * i);
	return v;
}

/* the small tables of +0x108 (index) / +0x10c (data words) */
static void vp8_table(struct vdec_dev *d, u32 idx, const u8 *p, unsigned int n)
{
	unsigned int i;

	vdec_w(d, CORE_VP8 + 0x108, idx);
	for (i = 0; i < n; i += 4)
		vdec_w(d, CORE_VP8 + 0x10c, le_word(p + i, n - i));
}

/* motion vector probabilities: is_short / sign in +0x120, the trees in
 * the table (row short 0x0c, row long 0x10, column short 0x14, long 0x18) */
static void vp8_mv_probs(struct vdec_dev *d, const struct v4l2_vp8_entropy *e)
{
	vdec_w(d, CORE_VP8 + 0x120, e->mv_probs[1][0] | e->mv_probs[0][0] << 8 |
	       e->mv_probs[1][1] << 16 | (u32)e->mv_probs[0][1] << 24);
	vp8_table(d, 0x0c, &e->mv_probs[0][2], 7);
	vp8_table(d, 0x10, &e->mv_probs[0][9], 10);
	vp8_table(d, 0x14, &e->mv_probs[1][2], 7);
	vp8_table(d, 0x18, &e->mv_probs[1][9], 10);
}

/* coefficient probabilities: +0x100 = 4 * [type][band][ctx], +0x104 x3 */
static void vp8_coef_probs(struct vdec_dev *d, const struct v4l2_vp8_entropy *e)
{
	unsigned int t, b, c, i = 0;

	for (t = 0; t < 4; t++)
		for (b = 0; b < 8; b++)
			for (c = 0; c < 3; c++, i++) {
				const u8 *p = e->coeff_probs[t][b][c];

				vdec_w(d, CORE_VP8 + 0x100, 4 * i);
				vdec_w(d, CORE_VP8 + 0x104, le_word(p, 4));
				vdec_w(d, CORE_VP8 + 0x104, le_word(p + 4, 4));
				vdec_w(d, CORE_VP8 + 0x104, le_word(p + 8, 3));
			}
}

/* token partitions, after the second reader was pointed at the first
 * partition's end: the size table, the partition addresses / sizes
 * (+0x0c0 + 4i / +0x0e0 + 4i), then the token bool decoder starts */
static void vp8_partitions(struct vdec_dev *d, const struct vdec_vp8_run *run)
{
	unsigned int i;

	for (i = 0; i < run->nparts; i++) {
		vdec_w(d, CORE_VP8 + 0xc0 + 4 * i, run->bs_dma + run->part_off[i]);
		if (i + 1 < run->nparts)
			vp8_bits(d, true, 24);
		vdec_w(d, CORE_VP8 + 0xe0 + 4 * i, run->part_size[i]);
	}
	vdec_w(d, CORE_VP8 + 0x144, 0x100);
	vdec_w(d, CORE_VP8 + 0x144, 0);
	vdec_r(d, CORE_VP8 + 0x114);
}

int vdec_hw_vp8(struct vdec_ctx *ctx, const struct vdec_vp8_run *run)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_vp8_state *st = ctx->vp8;
	const struct v4l2_ctrl_vp8_frame *f = run->frame;
	const struct v4l2_vp8_entropy *e = &f->entropy;
	u32 mb_w = DIV_ROUND_UP(run->w, 16), mb_h = DIV_ROUND_UP(run->h, 16);
	bool seg = false, seg_map = false, skip;
	unsigned int i, log2_parts;
	u32 tag, v, probs;
	int ret;

	/* reader resets, the segment map back into the SRAM, again (MIUI) */
	vp8_reset(d, false);
	ret = vp8_bs_init(d, run, false, run->bs_dma);
	vdec_w(d, CORE_VP8 + 0x144, 1);
	vdec_w(d, CORE_VP8 + 0x144, 0);
	vp8_reset(d, true);
	vdec_w(d, CORE_BS2 + 0xb4, run->bs_dma >> 6);
	vdec_w(d, CORE_BS2 + 0xb8, vp8_bs_end(run) >> 6);
	vdec_w(d, CORE_MISC + 0x4, 1);
	vp8_seg_sram(d, st->seg_map, true);
	vp8_reset(d, false);
	ret = ret ?: vp8_bs_init(d, run, false, run->bs_dma);
	vdec_w(d, CORE_VP8 + 0x144, 1);
	vdec_w(d, CORE_VP8 + 0x144, 0);
	vp8_reset(d, true);
	vdec_w(d, CORE_BS2 + 0xb4, run->bs_dma >> 6);
	vdec_w(d, CORE_BS2 + 0xb8, vp8_bs_end(run) >> 6);
	vdec_w(d, CORE_MISC + 0x4, 1);
	if (ret)
		return ret;

	/* uncompressed chunk: frame tag, start code and size on keyframes */
	tag = vp8_bits(d, false, 24);
	if (run->key) {
		vp8_bits(d, false, 24);
		vp8_bits(d, false, 16);
		vp8_bits(d, false, 16);
	}
	vdec_w(d, CORE_VLD_TOP + 0x68, (mb_h - 1) << 16 | (mb_w - 1));
	vdec_w(d, CORE_VP8 + 0xa8, run->part_end << 8 | (f->version & 7) << 1 | !run->key);
	if (run->key)
		vp8_mv_probs(d, e);
	vdec_r(d, CORE_VP8 + 0x110);

	/* frame header (RFC 6386 19.2) */
	if (run->key) {
		vp8_lit(d, 1);				/* color_space */
		st->clamping_off = vp8_lit(d, 1);
	}
	if (vp8_lit(d, 1)) {				/* segmentation_enabled */
		bool data;

		seg = true;
		seg_map = vp8_lit(d, 1);
		data = vp8_lit(d, 1);
		if (data) {
			vp8_lit(d, 1);			/* segment_feature_mode */
			for (i = 0; i < 4; i++)
				if (vp8_lit(d, 1))
					vp8_slit(d, 7);
			for (i = 0; i < 4; i++)
				if (vp8_lit(d, 1))
					vp8_slit(d, 6);
		}
		if (seg_map) {
			for (i = 0; i < 3; i++)
				if (vp8_lit(d, 1))
					vp8_lit(d, 8);
			vp8_table(d, 0, f->segment.segment_probs, 3);
		}
	}
	vp8_lit(d, 1);					/* filter_type */
	vp8_lit(d, 6);					/* loop_filter_level */
	vp8_lit(d, 3);					/* sharpness_level */
	if (vp8_lit(d, 1) && vp8_lit(d, 1))		/* lf adj enable, delta update */
		for (i = 0; i < 8; i++)
			if (vp8_lit(d, 1))
				vp8_slit(d, 6);
	ret = vp8_bs_init(d, run, true, run->bs_dma + run->part_end);
	log2_parts = vp8_lit(d, 2);
	if (BIT(log2_parts) != run->nparts) {
		dev_info_ratelimited(d->dev, "vp8: %lu partitions in the header, %u in the control\n",
				     BIT(log2_parts), run->nparts);
		return -EINVAL;
	}
	vp8_partitions(d, run);
	vp8_lit(d, 7);					/* y_ac_qi */
	for (i = 0; i < 5; i++)
		if (vp8_lit(d, 1))
			vp8_slit(d, 4);
	for (i = 0; i < 4; i++)
		for (v = 0; v < 3; v++) {
			vdec_w(d, CORE_VLD + 0x260, 0x10 * i + 4 * v);
			vdec_w(d, CORE_VLD + 0x264, run->dq[i][v]);
		}
	if (!run->key) {
		bool gold = vp8_lit(d, 1), alt = vp8_lit(d, 1);

		if (!gold)
			vp8_lit(d, 2);			/* copy_buffer_to_golden */
		if (!alt)
			vp8_lit(d, 2);			/* copy_buffer_to_alternate */
		v = vp8_lit(d, 1);			/* sign_bias_golden */
		v |= vp8_lit(d, 1) << 1;		/* sign_bias_alternate */
		vdec_w(d, CORE_AVC_MV + 0x2d4, v);
	}
	vp8_lit(d, 1);					/* refresh_entropy_probs */
	if (!run->key)
		vp8_lit(d, 1);				/* refresh_last */
	/* coefficient probability updates: parsed by the hardware */
	vp8_coef_probs(d, e);
	vdec_w(d, CORE_VP8 + 0x148, 1);
	ret = ret ?: vp8_poll(d, CORE_VP8 + 0x148, ~0u, 0, "coefficient update");
	skip = vp8_lit(d, 1);				/* mb_no_coeff_skip */
	vdec_w(d, CORE_VP8 + 0xac, st->clamping_off | seg << 4 | (seg && seg_map) << 8 |
	       log2_parts << 12 | skip << 16);
	probs = skip ? vp8_lit(d, 8) : 0;		/* prob_skip_false */
	if (!run->key) {
		probs |= vp8_lit(d, 8) << 8;		/* prob_intra */
		probs |= vp8_lit(d, 8) << 16;		/* prob_last */
		probs |= vp8_lit(d, 8) << 24;		/* prob_gf */
	}
	vdec_w(d, CORE_VP8 + 0xb0, probs);
	if (!run->key) {
		if (vp8_lit(d, 1))
			for (i = 0; i < 4; i++)
				vp8_lit(d, 8);
		if (vp8_lit(d, 1))
			for (i = 0; i < 3; i++)
				vp8_lit(d, 8);
		vp8_table(d, 4, e->y_mode_probs, 4);
		vp8_table(d, 8, e->uv_mode_probs, 3);
		vp8_mv_probs(d, e);
		/* motion vector probability updates: hardware */
		vdec_w(d, CORE_VP8 + 0x14c, 1);
		ret = ret ?: vp8_poll(d, CORE_VP8 + 0x14c, ~0u, 0, "mv update");
	}
	if (vdec_trace > 0)
		dev_info(d->dev, "vp8 tag %06x key %d seg %d/%d parts %lu probs %08x\n",
			 tag, run->key, seg, seg_map, BIT(log2_parts), probs);

	/* reconstruction */
	vdec_w(d, CORE_MC + 0x208, run->w);
	vdec_w(d, CORE_MC + 0x20c, ALIGN(run->h, 16));
	vdec_w(d, CORE_MC + 0x980, mb_w);
	/* motion compensation filter (RFC 6386 9.1): bit 4 six-tap (version
	 * 0; bilinear otherwise), bit 8 full-pixel (version 3) */
	vdec_w(d, CORE_MC + 0x97c, (vdec_r(d, CORE_MC + 0x97c) & 1) | (f->version == 3) << 8 |
	       (f->version == 0) << 4);
	vdec_w(d, CORE_MC + 0x934, run->c_off);
	for (i = 0; i < 3; i++)
		vdec_w(d, CORE_MC + 0x3e0 + 4 * i, run->ref_y[i]);
	vdec_w(d, CORE_MC + 0x220, 1);
	vdec_w(d, CORE_MC + 0x250, 1);
	vdec_w(d, CORE_MC + 0x224, run->y_dma >> 9);
	vdec_w(d, CORE_MC + 0x228, run->c_dma >> 8);
	vdec_w(d, CORE_MC + 0x22c, mb_w);
	vdec_w(d, CORE_MC + 0x260, mb_w - 1);
	vdec_w(d, CORE_MC + 0x264, mb_h - 1);
	vdec_w(d, CORE_MC + 0x57c, 0);
	vdec_w(d, CORE_MC + 0xa20, st->wb[0].dma);
	vdec_w(d, CORE_MC + 0xa24, st->wb[1].dma);
	vdec_w(d, CORE_MISC + 0xf0, vdec_r(d, CORE_MISC + 0xf0) | 1);
	/* loop filter kind for the core: 3 normal, 2 simple (MIUI writes it
	 * with the pp+020 group whenever the level is not 0; left at 0 the
	 * filter is skipped) */
	vdec_w(d, CORE_MC + 0x238, f->lf.flags & V4L2_VP8_LF_FILTER_TYPE_SIMPLE ? 2 : 3);
	/* loop filter: enable, filter type, delta enable, inter, sharpness;
	 * the level per segment, reference and mode deltas as bytes */
	if (f->lf.level) {
		vdec_w(d, CORE_PP + 0x20, 1 |
		       !!(f->lf.flags & V4L2_VP8_LF_FILTER_TYPE_SIMPLE) << 4 |
		       !!(f->lf.flags & V4L2_VP8_LF_ADJ_ENABLE) << 8 | !run->key << 12 |
		       (f->lf.sharpness_level & 7) << 16);
		vdec_w(d, CORE_PP + 0x24, le_word(run->lf_level, 4));
		vdec_w(d, CORE_PP + 0x28, le_word((const u8 *)f->lf.ref_frm_delta, 4));
		vdec_w(d, CORE_PP + 0x2c, le_word((const u8 *)f->lf.mb_mode_delta, 4));
	} else {
		vdec_w(d, CORE_PP + 0x20, 0);
		vdec_w(d, CORE_PP + 0x24, 0);
	}
	vdec_w(d, CORE_MC + 0x850, run->aux_dma >> 4);
	vdec_w(d, CORE_MC + 0x854, run->y_dma >> 4);
	vdec_w(d, CORE_MC + 0x858, 1);
	vdec_w(d, CORE_VLD_TOP + 0x5c, vdec_r(d, CORE_VLD_TOP + 0x5c) | BIT(29));
	vdec_w(d, CORE_MISC + 0x4, 3);
	return ret;
}

/* bring-up: nonzero registers of the side-effect-free blocks before start */
static int vp8_snap;
module_param(vp8_snap, int, 0644);
MODULE_PARM_DESC(vp8_snap, "dump core registers before the next N VP8 starts");

static void vp8_snapshot(struct vdec_dev *d)
{
	static const struct { u32 base, len; const char *n; } blk[] = {
		{ CORE_MC, 0x1000, "mc" }, { CORE_PP, 0x1000, "pp" }, { CORE_MISC, 0x400, "misc" },
		{ CORE_VLD_TOP, 0x800, "top" }, { SOC_UFO_ENC, 0x200, "ufo" },
		{ CORE_AVC_MV, 0x300, "mv" }, { CORE_GCON, 0x100, "gcon" },
		{ CORE_AVC_VLD, 0x300, "avc" }, { SOC_GCON, 0x100, "sgcon" },
		{ SOC_MCORE_TOP, 0x100, "soc0" }, { SOC_2000, 0x100, "soc2" },
		{ SOC_RACING, 0x200, "soc4" }, { CORE_LARB, 0x400, "larb4" },
		{ CORE_VLD + 0x100, 0x100, "vld1" }, { CORE_BS2 + 0x100, 0x100, "bs2_1" },
		{ CORE_VP8 + 0xa0, 0x70, "vp8a" }, { CORE_VP8 + 0x118, 0xe8, "vp8b" },
	};
	unsigned int i, o;

	for (i = 0; i < ARRAY_SIZE(blk); i++)
		for (o = 0; o < blk[i].len; o += 4) {
			u32 v = readl(d->regs + blk[i].base + o);

			if (v)
				pr_info("vp8snap %s+%03x %08x\n", blk[i].n, o, v);
		}
}

void vdec_hw_vp8_start(struct vdec_dev *d)
{
	if (vp8_snap > 0) {
		vp8_snap--;
		vp8_snapshot(d);
	}
	vdec_w(d, CORE_VP8 + 0xb8, 1);
	vdec_w(d, CORE_VP8 + 0xb8, 0);
	/* the done IRQ latch, as for H.264 / VP9 */
	vdec_w(d, CORE_VLD + 0xa4, 0x11);
}
