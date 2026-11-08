// SPDX-License-Identifier: GPL-2.0
/*
 * H.264 on the VENC cores: parameter sets (software), per-frame programming,
 * frame-level rate control. The hardware writes every slice NAL itself,
 * start code and slice header included; the SPS / PPS must match what it
 * writes (16-bit frame_num and POC lsb, POC type 0, two default
 * references, num_ref_idx_active_override in P slices, pic_init_qp 26).
 * The vendor's rate-control microcode is not used: QPs are ours.
 */
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <media/v4l2-mem2mem.h>

#include "venc.h"

/* --- bit writer --- */

struct bw {
	u8 *buf;
	unsigned int size, bits;
};

static void bw_u(struct bw *b, unsigned int n, u32 v)
{
	while (n--) {
		unsigned int byte = b->bits >> 3;

		if (byte >= b->size)
			return;
		if (!(b->bits & 7))
			b->buf[byte] = 0;
		if ((v >> n) & 1)
			b->buf[byte] |= 0x80 >> (b->bits & 7);
		b->bits++;
	}
}

static void bw_ue(struct bw *b, u32 v)
{
	unsigned int n = fls(v + 1);

	bw_u(b, n - 1, 0);
	bw_u(b, n, v + 1);
}

static void bw_se(struct bw *b, int v)
{
	bw_ue(b, v > 0 ? 2 * v - 1 : -2 * v);
}

/* start code, NAL header, payload with emulation prevention; length */
static unsigned int nal_write(u8 *out, unsigned int size, u8 hdr, struct bw *b)
{
	unsigned int i, n = 0, zeros = 0, len;

	bw_u(b, 1, 1);				/* rbsp_stop_one_bit */
	while (b->bits & 7)
		bw_u(b, 1, 0);
	len = b->bits >> 3;
	if (size < 5)
		return 0;
	out[n++] = 0;
	out[n++] = 0;
	out[n++] = 0;
	out[n++] = 1;
	out[n++] = hdr;
	for (i = 0; i < len; i++) {
		if (zeros >= 2 && b->buf[i] <= 3) {
			if (n >= size)
				return 0;
			out[n++] = 3;
			zeros = 0;
		}
		if (n >= size)
			return 0;
		out[n++] = b->buf[i];
		zeros = b->buf[i] ? 0 : zeros + 1;
	}
	return n;
}

/* --- stream parameters --- */

static u32 venc_profile_idc(struct venc_ctx *ctx)
{
	switch (ctx->c_profile->val) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		return 77;
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		return 100;
	default:
		return 66;
	}
}

static bool venc_cabac(struct venc_ctx *ctx)
{
	return venc_profile_idc(ctx) == 100 &&
	       ctx->c_entropy->val == V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC;
}

/*
 * +030 bit 21 turns on the 8x8 transform (the BSP's PPS writes
 * transform_8x8_mode_flag from the same field). CABAC only works with it
 * set: CABAC without it came out unparseable. So High always uses 8x8, and
 * Main, which cannot signal 8x8, is CAVLC (the BSP writes Main streams with
 * the High PPS extension instead, which is not Main).
 */
static bool venc_8x8(struct venc_ctx *ctx)
{
	return venc_profile_idc(ctx) == 100;
}

static unsigned int venc_fps_q8(struct venc_ctx *ctx)
{
	if (!ctx->tpf.numerator || !ctx->tpf.denominator)
		return 30 << 8;
	return max(1u, (ctx->tpf.denominator << 8) / ctx->tpf.numerator);
}

/* A.3.1 / table A-1: the lowest level for the frame size, macroblock rate
 * and bitrate, never below the one asked for */
static const struct {
	u8 idc;
	u8 v4l2;
	u32 mbps, fs, br;	/* MaxMBPS, MaxFS, MaxBR (kbit/s, Main) */
} venc_levels[] = {
	{ 10, V4L2_MPEG_VIDEO_H264_LEVEL_1_0, 1485, 99, 64 },
	{ 11, V4L2_MPEG_VIDEO_H264_LEVEL_1_1, 3000, 396, 192 },
	{ 12, V4L2_MPEG_VIDEO_H264_LEVEL_1_2, 6000, 396, 384 },
	{ 13, V4L2_MPEG_VIDEO_H264_LEVEL_1_3, 11880, 396, 768 },
	{ 20, V4L2_MPEG_VIDEO_H264_LEVEL_2_0, 11880, 396, 2000 },
	{ 21, V4L2_MPEG_VIDEO_H264_LEVEL_2_1, 19800, 792, 4000 },
	{ 22, V4L2_MPEG_VIDEO_H264_LEVEL_2_2, 20250, 1620, 4000 },
	{ 30, V4L2_MPEG_VIDEO_H264_LEVEL_3_0, 40500, 1620, 10000 },
	{ 31, V4L2_MPEG_VIDEO_H264_LEVEL_3_1, 108000, 3600, 14000 },
	{ 32, V4L2_MPEG_VIDEO_H264_LEVEL_3_2, 216000, 5120, 20000 },
	{ 40, V4L2_MPEG_VIDEO_H264_LEVEL_4_0, 245760, 8192, 20000 },
	{ 41, V4L2_MPEG_VIDEO_H264_LEVEL_4_1, 245760, 8192, 50000 },
	{ 42, V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 522240, 8704, 50000 },
	{ 50, V4L2_MPEG_VIDEO_H264_LEVEL_5_0, 589824, 22080, 135000 },
	{ 51, V4L2_MPEG_VIDEO_H264_LEVEL_5_1, 983040, 36864, 240000 },
	{ 52, V4L2_MPEG_VIDEO_H264_LEVEL_5_2, 2073600, 36864, 240000 },
};

static u32 venc_level_idc(struct venc_ctx *ctx)
{
	u32 mbs = DIV_ROUND_UP(ctx->crop.width, 16) * DIV_ROUND_UP(ctx->crop.height, 16);
	u32 mbps = (mbs * venc_fps_q8(ctx)) >> 8;
	u32 kbps = ctx->c_rc_en->val ? ctx->c_bitrate->val / 1000 : 0;
	unsigned int i;

	if (venc_profile_idc(ctx) == 100)
		kbps = kbps * 4 / 5;	/* High allows 1.25 x MaxBR */
	for (i = 0; i < ARRAY_SIZE(venc_levels); i++)
		if (venc_levels[i].v4l2 >= ctx->c_level->val && mbs <= venc_levels[i].fs &&
		    mbps <= venc_levels[i].mbps && kbps <= venc_levels[i].br)
			return venc_levels[i].idc;
	return 52;
}

/* VUI colour description from the raw format's colorimetry, if known */
static bool venc_colour(struct venc_ctx *ctx, u8 *prim, u8 *xfer, u8 *mat)
{
	switch (ctx->colorspace) {
	case V4L2_COLORSPACE_REC709:
		*prim = *xfer = *mat = 1;
		return true;
	case V4L2_COLORSPACE_SMPTE170M:
		*prim = *xfer = *mat = 6;
		return true;
	case V4L2_COLORSPACE_BT2020:
		*prim = 9;
		*xfer = 14;
		*mat = 9;
		return true;
	default:
		return false;
	}
}

/* SPS + PPS of the stream, Annex B; their length */
unsigned int venc_h264_headers(struct venc_ctx *ctx, u8 *out, unsigned int size)
{
	u32 profile = venc_profile_idc(ctx), mb_w = DIV_ROUND_UP(ctx->crop.width, 16);
	u32 mb_h = DIV_ROUND_UP(ctx->crop.height, 16);
	u32 crop_r = mb_w * 16 - ctx->crop.width, crop_b = mb_h * 16 - ctx->crop.height;
	u8 rbsp[64], prim, xfer, mat;
	struct bw b = { rbsp, sizeof(rbsp), 0 };
	bool colour = venc_colour(ctx, &prim, &xfer, &mat);
	unsigned int n;

	bw_u(&b, 8, profile);
	bw_u(&b, 8, profile == 66 ? 0xc0 : 0);	/* constrained baseline */
	bw_u(&b, 8, venc_level_idc(ctx));
	bw_ue(&b, 0);				/* seq_parameter_set_id */
	if (profile == 100) {
		bw_ue(&b, 1);			/* chroma_format_idc 4:2:0 */
		bw_ue(&b, 0);
		bw_ue(&b, 0);			/* 8-bit */
		bw_u(&b, 1, 0);
		bw_u(&b, 1, 0);			/* no scaling matrices */
	}
	bw_ue(&b, 12);				/* log2_max_frame_num_minus4: 16 */
	bw_ue(&b, 0);				/* pic_order_cnt_type */
	bw_ue(&b, 12);				/* log2_max_pic_order_cnt_lsb_minus4 */
	bw_ue(&b, 1);				/* max_num_ref_frames */
	bw_u(&b, 1, 0);
	bw_ue(&b, mb_w - 1);
	bw_ue(&b, mb_h - 1);
	bw_u(&b, 1, 1);				/* frame_mbs_only */
	bw_u(&b, 1, 1);				/* direct_8x8_inference */
	bw_u(&b, 1, crop_r || crop_b);
	if (crop_r || crop_b) {
		bw_ue(&b, 0);
		bw_ue(&b, crop_r / 2);
		bw_ue(&b, 0);
		bw_ue(&b, crop_b / 2);
	}
	bw_u(&b, 1, 1);				/* vui_parameters_present */
	bw_u(&b, 1, 0);				/* aspect_ratio_info */
	bw_u(&b, 1, 0);				/* overscan_info */
	bw_u(&b, 1, colour || ctx->quant == V4L2_QUANTIZATION_FULL_RANGE);
	if (colour || ctx->quant == V4L2_QUANTIZATION_FULL_RANGE) {
		bw_u(&b, 3, 5);			/* video_format: unspecified */
		bw_u(&b, 1, ctx->quant == V4L2_QUANTIZATION_FULL_RANGE);
		bw_u(&b, 1, colour);
		if (colour) {
			bw_u(&b, 8, prim);
			bw_u(&b, 8, xfer);
			bw_u(&b, 8, mat);
		}
	}
	bw_u(&b, 1, 0);				/* chroma_loc_info */
	bw_u(&b, 1, 1);				/* timing_info */
	bw_u(&b, 32, ctx->tpf.numerator ?: 1);	/* num_units_in_tick */
	bw_u(&b, 32, 2 * (ctx->tpf.denominator ?: 30));	/* time_scale */
	bw_u(&b, 1, 0);				/* fixed_frame_rate */
	bw_u(&b, 1, 0);				/* nal_hrd */
	bw_u(&b, 1, 0);				/* vcl_hrd */
	bw_u(&b, 1, 0);				/* pic_struct_present */
	bw_u(&b, 1, 1);				/* bitstream_restriction */
	bw_u(&b, 1, 1);				/* motion_vectors_over_pic_boundaries */
	bw_ue(&b, 0);				/* max_bytes_per_pic_denom */
	bw_ue(&b, 0);				/* max_bits_per_mb_denom */
	bw_ue(&b, 16);				/* log2_max_mv_length_horizontal */
	bw_ue(&b, 16);
	bw_ue(&b, 0);				/* max_num_reorder_frames: no B */
	bw_ue(&b, 1);				/* max_dec_frame_buffering */
	n = nal_write(out, size, 0x67, &b);

	b.bits = 0;
	bw_ue(&b, 0);				/* pic_parameter_set_id */
	bw_ue(&b, 0);
	bw_u(&b, 1, venc_cabac(ctx));
	bw_u(&b, 1, 0);				/* bottom_field_pic_order_in_frame */
	bw_ue(&b, 0);				/* num_slice_groups_minus1 */
	bw_ue(&b, 1);
	bw_ue(&b, 1);				/* num_ref_idx_l0 / l1_default_minus1 */
	bw_u(&b, 1, 0);
	bw_u(&b, 2, 0);				/* no weighted prediction */
	bw_se(&b, 0);				/* pic_init_qp_minus26 */
	bw_se(&b, 0);
	bw_se(&b, 0);				/* chroma_qp_index_offset */
	bw_u(&b, 1, 1);				/* deblocking_filter_control_present */
	bw_u(&b, 1, 0);				/* constrained_intra_pred */
	bw_u(&b, 1, 0);				/* redundant_pic_cnt_present */
	if (profile == 100) {
		bw_u(&b, 1, venc_8x8(ctx));
		bw_u(&b, 1, 0);			/* pic_scaling_matrix_present */
		bw_se(&b, 0);
	}
	return n + nal_write(out + n, size - n, 0x68, &b);
}

/* --- rate control --- */

/* log2(x / 2^16) in 8 fractional bits, x > 0 */
static int log2_q8(u64 x)
{
	int e = fls64(x) - 1;
	u64 m = e >= 16 ? x >> (e - 16) : x << (16 - e);	/* 1.16, in [1, 2) */

	/* log2(1 + f) ~ f + 0.343 f (1 - f): within 0.01 */
	m -= 1 << 16;
	m = m + ((m * ((1 << 16) - m) >> 16) * 22 >> 6);
	return ((e - 16) << 8) + (int)(m >> 8);
}

static int venc_rc_qp_limit(struct venc_ctx *ctx, int qp)
{
	return clamp(qp, ctx->c_qpmin->val, max(ctx->c_qpmin->val, ctx->c_qpmax->val));
}

/* bits a frame of this type should take: an I frame counts as 4 P frames */
static u64 venc_rc_target(struct venc_ctx *ctx, bool idr)
{
	u32 gop = ctx->c_gop->val ?: 300;
	u64 per = div_u64((u64)ctx->c_bitrate->val << 8, venc_fps_q8(ctx));
	u64 p = div_u64(per * gop, gop - 1 + 4);

	return max_t(u64, 1, idr ? 4 * p : p);
}

static void venc_rc_init(struct venc_ctx *ctx)
{
	u64 pixels = (u64)ctx->crop.width * ctx->crop.height;
	/* bits per pixel per frame, 16 fractional bits */
	u64 bpp = div64_u64(venc_rc_target(ctx, false) << 16, max_t(u64, 1, pixels));

	memset(&ctx->rc, 0, sizeof(ctx->rc));
	/* 28 at 0.1 bpp, 6 QP per halving */
	ctx->rc.qp_q8 = (28 << 8) - 6 * (log2_q8(bpp ?: 1) - log2_q8(6554));
	ctx->rc.qp_q8 = venc_rc_qp_limit(ctx, ctx->rc.qp_q8 >> 8) << 8;
}

static int venc_rc_qp(struct venc_ctx *ctx, bool idr)
{
	int qp;

	if (!ctx->c_rc_en->val)
		return idr ? ctx->c_qpi->val : ctx->c_qpp->val;
	if (ctx->c_rc_mode->val == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ)
		/* quality 1..100 -> QP 51..0 */
		return venc_rc_qp_limit(ctx, 51 - (ctx->c_quality->val * 51 + 50) / 100);
	qp = (ctx->rc.qp_q8 + 128) >> 8;
	return venc_rc_qp_limit(ctx, idr ? qp - 3 : qp);
}

/*
 * After a frame: move the P QP by the log of how far the frame missed its
 * target (6 QP double the size), half of it per frame, plus a pull from the
 * virtual buffer (one second of bitrate for CBR, two for VBR).
 */
static void venc_rc_update(struct venc_ctx *ctx, bool idr, u32 bits)
{
	u64 target = venc_rc_target(ctx, idr);
	s64 window = (s64)ctx->c_bitrate->val *
		(ctx->c_rc_mode->val == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR ? 1 : 2);
	int step, pull;

	if (!ctx->c_rc_en->val || ctx->c_rc_mode->val == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ)
		return;
	ctx->rc.fullness += (s64)bits - (s64)target;
	ctx->rc.fullness = clamp_t(s64, ctx->rc.fullness, -window, 2 * window);
	step = 6 * (log2_q8(((u64)max(bits, 1u) << 16) / target) / 2);
	pull = (int)div64_s64(ctx->rc.fullness * (6 << 8), max_t(s64, 1, window));
	ctx->rc.qp_q8 += clamp(step, -3 << 8, 4 << 8) + clamp(pull, -2 << 8, 3 << 8);
	ctx->rc.qp_q8 = clamp(ctx->rc.qp_q8, ctx->c_qpmin->val << 8,
			      max(ctx->c_qpmin->val, ctx->c_qpmax->val) << 8);
	ctx->rc.frames++;
}

/* --- streams --- */

/* compressed reconstruction: 8 x (lo + hi) bytes per line of 16-aligned
 * height (BSP H264_AllocateRFSBuffer); lo / hi are also +1c4 */
static u32 venc_rec_lo(u32 mb_w)
{
	return (mb_w * 0x91b + 0x3ff) >> 10;
}

static u32 venc_rec_hi(u32 mb_w)
{
	return (mb_w * 0x309 + 0x3ff) >> 10;
}

size_t venc_h264_rec_size(u32 mb_w, u32 mb_h)
{
	return PAGE_ALIGN(mb_h * 16 * 8 * (venc_rec_lo(mb_w) + venc_rec_hi(mb_w)) + 0x200);
}

static int venc_alloc(struct venc_ctx *ctx, struct venc_buf *b, size_t size)
{
	b->size = PAGE_ALIGN(size);
	b->va = dma_alloc_coherent(ctx->dev->dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->va)
		return -ENOMEM;
	atomic64_add(b->size, &ctx->dev->mem_used);
	return 0;
}

static void venc_free(struct venc_ctx *ctx, struct venc_buf *b)
{
	if (!b->va)
		return;
	dma_free_coherent(ctx->dev->dev, b->size, b->va, b->dma);
	atomic64_sub(b->size, &ctx->dev->mem_used);
	b->va = NULL;
}

void venc_h264_stop(struct venc_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		venc_free(ctx, &ctx->rec[i]);
		venc_free(ctx, &ctx->mv[i]);
	}
	for (i = 0; i < VENC_CORES; i++)
		venc_free(ctx, &ctx->rc_info[i]);
	venc_free(ctx, &ctx->rc_code);
	venc_free(ctx, &ctx->sync);
}

int venc_h264_start(struct venc_ctx *ctx)
{
	u32 mb_w = DIV_ROUND_UP(ctx->crop.width, 16), mb_h = DIV_ROUND_UP(ctx->crop.height, 16);
	unsigned int i;
	int ret = 0;

	for (i = 0; i < 2 && !ret; i++)
		ret = venc_alloc(ctx, &ctx->rec[i], venc_h264_rec_size(mb_w, mb_h)) ?:
		      venc_alloc(ctx, &ctx->mv[i], max(SZ_4K, mb_w * mb_h * 64));
	/* RC info per core, one sync buffer for both (as the BSP; the
	 * hardware has not been seen writing it) */
	for (i = 0; i < ctx->dev->ncores && !ret; i++)
		ret = venc_alloc(ctx, &ctx->rc_info[i], SZ_64K);
	ret = ret ?: venc_alloc(ctx, &ctx->rc_code, SZ_64K) ?: venc_alloc(ctx, &ctx->sync, SZ_4K);
	if (ret) {
		venc_h264_stop(ctx);
		return ret;
	}
	ctx->frame = ctx->last_idr = ctx->idr_count = ctx->since_idr = 0;
	ctx->header_sent = false;
	ctx->force_key = false;
	ctx->hdr_len = venc_h264_headers(ctx, ctx->hdr, sizeof(ctx->hdr));
	venc_rc_init(ctx);
	return ctx->hdr_len ? 0 : -EINVAL;
}

/* VENC_SW_PAUSE / VENC_SW_HRST_N as the BSP's venc_break */
void venc_hw_reset(struct venc_core *c)
{
	unsigned int i;

	venc_w(c, VENC_SW_PAUSE, 1);
	for (i = 0; i < 5000; i++)
		if (venc_r(c, VENC_IRQ_STATUS) & VENC_IRQ_PAUSE)
			break;
	if (i == 5000)
		venc_w(c, VENC_SW_PAUSE, 0);
	venc_w(c, VENC_SW_HRST_N, 0);
	venc_r(c, VENC_SW_HRST_N);
	venc_w(c, VENC_SW_PAUSE, 0);
	venc_w(c, VENC_SW_HRST_N, 1);
	venc_r(c, VENC_SW_HRST_N);
	venc_w(c, VENC_IRQ_ACK, VENC_IRQ_PAUSE);
}

/* addresses as the hardware takes them: >> 4, bits 31:28 = address 35:32 */
static u32 venc_addr(dma_addr_t a)
{
	return lower_32_bits(a) >> 4 | (u32)(upper_32_bits(a) & 0xf) << 28;
}

/*
 * Program and start the job's core. The frame counters move on here, not
 * when the frame is done: with two cores the next frame starts while this
 * one runs (the hardware keeps it VENC_CORE_DIST rows behind).
 */
void venc_h264_run(struct venc_ctx *ctx, struct venc_job *job)
{
	struct venc_dev *d = ctx->dev;
	struct venc_core *core = &d->core[job->core];
	struct vb2_v4l2_buffer *src = job->src, *dst = job->dst;
	const struct venc_fmt *fmt = ctx->src_fmt;
	u32 mb_w = DIV_ROUND_UP(ctx->crop.width, 16), mb_h = DIV_ROUND_UP(ctx->crop.height, 16);
	u32 pitch = ctx->src.plane_fmt[0].bytesperline, cpitch, gop = ctx->c_gop->val;
	u32 profile = venc_profile_idc(ctx), lo = venc_rec_lo(mb_w), qpi, qpp, type;
	struct venc_buf *cur = &ctx->rec[ctx->frame & 1], *ref = &ctx->rec[~ctx->frame & 1];
	/* imported buffers may start their planes at an offset (data_offset) */
	dma_addr_t y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0) +
		       src->vb2_buf.planes[0].data_offset, c, cr = 0;
	dma_addr_t bs = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	size_t bs_size = vb2_plane_size(&dst->vb2_buf, 0);
	bool idr = !ctx->frame || ctx->force_key || (gop && ctx->since_idr >= gop);
	u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);
	unsigned int off = 0;

	/* source planes: separate buffers, or packed after the luma rows */
	if (fmt->rgb) {
		c = y;		/* one plane: +070 unused */
	} else if (fmt->planes > 1) {
		c = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 1) +
		    src->vb2_buf.planes[1].data_offset;
		if (fmt->comps == 3)
			cr = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 2) +
			     src->vb2_buf.planes[2].data_offset;
	} else {
		c = y + (dma_addr_t)pitch * ctx->src.height;
		if (fmt->comps == 3)
			cr = c + (dma_addr_t)pitch / 2 * ctx->src.height / 2;
	}
	/* +070 takes Cb, +094 Cr */
	if (fmt->swap_uv)
		swap(c, cr);
	/* the chroma pitch field holds the luma pitch for three planes too
	 * (the hardware halves it; BSP Count3PlaneUVPitch); for RGB both hold
	 * the pitch in pixels (unused: +1f0 has it in bytes) */
	if (fmt->rgb)
		pitch /= fmt->bpp;
	cpitch = pitch;

	/* parameter sets in front of the slice: every IDR, or once */
	job->hdr_off = 0;
	if (idr && out && (ctx->c_repeat->val || !ctx->idr_count) &&
	    !(ctx->c_header->val == V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE && !ctx->idr_count)) {
		memcpy(out, ctx->hdr, ctx->hdr_len);
		off = ctx->hdr_len;
		/* the hardware starts its output on a 128-byte boundary (lower
		 * address bits are ignored): zero bytes in between are
		 * trailing_zero_8bits of the PPS NAL */
		memset(out + off, 0, ALIGN(off, VENC_BS_ALIGN) - off);
		off = ALIGN(off, VENC_BS_ALIGN);
		job->hdr_off = off;
	}

	if (idr) {
		ctx->last_idr = ctx->frame;
		ctx->since_idr = 0;
		ctx->force_key = false;
	}
	job->idr = idr;
	qpi = venc_rc_qp(ctx, true);
	qpp = venc_rc_qp(ctx, false);
	job->qp = idr ? qpi : qpp;
	if (!gop || gop > 0x3fff)
		gop = 0x3fff;

	/* sequence level, every frame (the reset clears it) */
	venc_w(core, VENC_SW_HRST_N, 0);
	venc_w(core, VENC_SW_HRST_N, 1);
	venc_w(core, VENC_GOP_QPI, qpi << 26 | gop << 14 | gop);
	venc_w(core, VENC_QPP, 0x026e03af | 24 << 26 | qpp << 10);
	venc_w(core, VENC_SEQ_FLAGS, 0x102f8000 | (u32)venc_cabac(ctx) << 31);
	venc_w(core, VENC_MB_SIZE, mb_h << 16 | mb_w);
	/* padding to whole macroblocks: rows [4:0], columns [9:5] */
	venc_w(core, VENC_PITCH_FMT, (mb_h * 16 - ctx->crop.height) |
	       (mb_w * 16 - ctx->crop.width) << 5 |
	       (pitch / 16) << 11 | (cpitch / 16) << 20 | (u32)fmt->hw << 29);
	venc_w(core, VENC_MB_COUNT, mb_w * mb_h);
	/* RGB input: BT.709 limited range out (as the SPS says), matching the
	 * colour of HD video; this also clears the RGB mode for YUV */
	venc_w(core, VENC_RGB_FMT, fmt->rgb);
	venc_w(core, VENC_RGB_PITCH, fmt->rgb ? ctx->src.plane_fmt[0].bytesperline : 0);
	venc_w(core, VENC_RGB_CSC, fmt->rgb ? VENC_CSC_709 | VENC_CSC_LIMITED : 0);
	venc_w(core, VENC_PROFILE, (u32)venc_cabac(ctx) << 31 |
	       (profile == 100 ? 0x40000000 : profile == 77 ? 0x20000000 : 0) |
	       venc_level_idc(ctx) << 22 | (u32)venc_8x8(ctx) << 21);
	venc_w(core, VENC_RC_EN, 0);
	venc_w(core, VENC_RC_FPS, 0);
	venc_w(core, VENC_RC_QP_RANGE, 0x03003300);
	venc_w(core, 0x11c8, 6);
	venc_w(core, 0x054, 0x808);
	venc_w(core, 0x12c, 0x200);
	venc_w(core, 0x11a0, 0x21 | job->core << 4);
	venc_w(core, 0x129c, 0x21 | job->core << 4);
	venc_w(core, 0x1c0, 0x11);
	venc_w(core, 0x1c4, (lo + venc_rec_hi(mb_w)) << 16 | lo);
	/*
	 * Both cores (BSP H264_SeqLvlMMRSetting with two cores): frame k on
	 * core k % 2, each trailing the frame before by 17 macroblock rows
	 * (H264_GetCoreDist at this search range, compressed reference); the
	 * hardware syncs the cores itself. One core: all zero.
	 */
	if (d->ncores > 1) {
		venc_w(core, VENC_CORE_DIST, 17 << 4 | 10);
		venc_w(core, VENC_CORE_CNT, (d->ncores - 1) << 16);
		venc_w(core, VENC_CORE_ID, 0);
	} else {
		venc_w(core, VENC_CORE_DIST, 0);
		venc_w(core, VENC_CORE_CNT, 0);
	}
	venc_w(core, 0x1204, 0);
	venc_w(core, 0x1268, 0);
	venc_w(core, VENC_BS_SIZE, (bs_size - off) >> 7);

	/*
	 * Frame level: +010 6 for an IDR (+8 every other one: idr_pic_id),
	 * 0x400 for the first P after it, 0x500 later; the hardware writes
	 * frame_num = +018 - +01c and the POC lsb as twice that.
	 */
	if (idr)
		type = 0x0332b406 | (ctx->idr_count & 1) << 3;
	else
		type = ctx->since_idr > 1 ? 0x03328500 : 0x03328400;
	venc_w(core, VENC_FRAME_TYPE, type);
	venc_w(core, 0x16c, 0);
	venc_w(core, VENC_FRAME_CNT, ctx->frame);
	venc_w(core, VENC_POC_CNT, ctx->frame);
	venc_w(core, VENC_IDR_CNT, ctx->last_idr);
	venc_w(core, 0x020, 0xff);
	if (d->ncores > 1)
		venc_w(core, VENC_CORE_FRAME, -ctx->frame);
	venc_w(core, 0x11b4, 0x26f);
	venc_w(core, 0x124, 0);
	venc_w(core, 0x128, 0);

	venc_w(core, VENC_RC_CODE, venc_addr(ctx->rc_code.dma));
	venc_w(core, VENC_RC_INFO, venc_addr(ctx->rc_info[job->core].dma));
	if (d->ncores > 1)
		venc_w(core, VENC_CORE_SYNC, venc_addr(ctx->sync.dma));
	venc_w(core, VENC_BS_ADDR, venc_addr(bs + off));
	venc_w(core, VENC_SRC_Y, venc_addr(y));
	venc_w(core, VENC_SRC_C, venc_addr(c));
	venc_w(core, VENC_SRC_CR, cr ? venc_addr(cr) : 0);
	venc_w(core, VENC_REC, venc_addr(cur->dma));
	venc_w(core, VENC_REC_C, venc_addr(cur->dma));
	venc_w(core, VENC_REF, venc_addr(ref->dma));
	venc_w(core, VENC_REF_C, venc_addr(ref->dma));
	venc_w(core, VENC_MV_OUT, venc_addr(ctx->mv[ctx->frame & 1].dma));
	venc_w(core, VENC_MV_IN, venc_addr(ctx->mv[~ctx->frame & 1].dma));
	venc_w(core, VENC_IRQ_EN, 0x3f);
	venc_w(core, VENC_MODE, 0x0200bf60);
	venc_w(core, 0x12b0, 0x1ff);
	venc_w(core, VENC_CLK_EN, 1);
	venc_w(core, VENC_START, 4);

	if (idr)
		ctx->idr_count++;
	ctx->frame++;
	ctx->since_idr++;
}

/* the frame is done (in stream order): its size feeds the rate control */
void venc_h264_done(struct venc_ctx *ctx, struct venc_job *job)
{
	venc_rc_update(ctx, job->idr, (job->bytes + job->hdr_off) * 8);
}

/* --- controls --- */

static int venc_s_ctrl(struct v4l2_ctrl *c)
{
	struct venc_ctx *ctx = container_of(c->handler, struct venc_ctx, ctrl_hdl);

	if (c->id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME)
		ctx->force_key = true;
	/* the rest is read when a frame or the stream starts */
	return 0;
}

static const struct v4l2_ctrl_ops venc_ctrl_ops = {
	.s_ctrl = venc_s_ctrl,
};

int venc_h264_ctrls_init(struct venc_ctx *ctx)
{
	struct v4l2_ctrl_handler *h = &ctx->ctrl_hdl;
	const struct v4l2_ctrl_ops *o = &venc_ctrl_ops;

	v4l2_ctrl_handler_init(h, 20);
	ctx->c_bitrate = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_BITRATE,
					   64000, 240000000, 1000, 8000000);
	ctx->c_rc_mode = v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
						V4L2_MPEG_VIDEO_BITRATE_MODE_CQ, 0,
						V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	ctx->c_rc_en = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0, 1, 1, 1);
	ctx->c_quality = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
					   1, 100, 1, 70);
	ctx->c_gop = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, 16383, 1, 60);
	ctx->c_profile = v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
						V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
						~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
						  BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
						  BIT(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
						  BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)),
						V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
	/* the lowest level the stream may claim; raised to what it needs */
	ctx->c_level = v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
					      V4L2_MPEG_VIDEO_H264_LEVEL_5_2,
					      BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1B),
					      V4L2_MPEG_VIDEO_H264_LEVEL_1_0);
	ctx->c_qpi = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, 0, 51, 1, 26);
	ctx->c_qpp = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, 0, 51, 1, 28);
	ctx->c_qpmin = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 0, 51, 1, 10);
	ctx->c_qpmax = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 0, 51, 1, 51);
	ctx->c_header = v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
					       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, 0,
					       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	ctx->c_repeat = v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 0, 1, 1, 1);
	ctx->c_entropy = v4l2_ctrl_new_std_menu(h, o, V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
						V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, 0,
						V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC);
	v4l2_ctrl_new_std(h, o, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);
	v4l2_ctrl_new_std(h, NULL, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1, 1);
	if (h->error) {
		int ret = h->error;

		v4l2_ctrl_handler_free(h);
		return ret;
	}
	return 0;
}
