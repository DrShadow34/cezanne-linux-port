// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: AV1 stateless decoding (V4L2_PIX_FMT_AV1_FRAME, the AV1
 * SEQUENCE / FRAME / TILE_GROUP_ENTRY controls).
 *
 * The derived frame parameters (global motion shear, quantizer indices,
 * scale factors, sign bias, tile starts) follow mainline's MT8195 driver
 * (vdec_av1_req_lat_if.c); motion field projection follows MediaTek's
 * vpud (vdec_av1_motion_field_estimation), which the hardware expects.
 * Per capture buffer the driver keeps the frame's CDF context, motion
 * vectors and segment map for the frames that reference it.
 */
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec.h"
#include "vdec_av1.h"

static int av1_debug;
module_param(av1_debug, int, 0644);
MODULE_PARM_DESC(av1_debug, "log the next N AV1 frames' parameters");

#define AV1_DIV_ROUND_UP_POW2(value, n)			\
({							\
	typeof(n) _n  = n;				\
	typeof(value) _value = value;			\
	(_value + (BIT(_n) >> 1)) >> _n;		\
})

#define AV1_DIV_ROUND_UP_POW2_SIGNED(value, n)				\
({									\
	typeof(n) _n_  = n;						\
	typeof(value) _value_ = value;					\
	(((_value_) < 0) ? -AV1_DIV_ROUND_UP_POW2(-(_value_), (_n_))	\
		: AV1_DIV_ROUND_UP_POW2((_value_), (_n_)));		\
})

#define DIV_LUT_PREC_BITS	14
#define DIV_LUT_BITS		8
#define DIV_LUT_NUM		BIT(DIV_LUT_BITS)
#define WARP_PARAM_REDUCE_BITS	6
#define WARPEDMODEL_PREC_BITS	16
#define AV1_REF_SCALE_SHIFT	14
#define AV1_SCALE_SUBPEL_BITS	10
#define AV1_PRIMARY_REF_NONE	7

/* DIV_LUT (from mainline vdec_av1_req_lat_if.c) */
static const short div_lut[DIV_LUT_NUM + 1] = {
	16384, 16320, 16257, 16194, 16132, 16070, 16009, 15948, 15888, 15828, 15768,
	15709, 15650, 15592, 15534, 15477, 15420, 15364, 15308, 15252, 15197, 15142,
	15087, 15033, 14980, 14926, 14873, 14821, 14769, 14717, 14665, 14614, 14564,
	14513, 14463, 14413, 14364, 14315, 14266, 14218, 14170, 14122, 14075, 14028,
	13981, 13935, 13888, 13843, 13797, 13752, 13707, 13662, 13618, 13574, 13530,
	13487, 13443, 13400, 13358, 13315, 13273, 13231, 13190, 13148, 13107, 13066,
	13026, 12985, 12945, 12906, 12866, 12827, 12788, 12749, 12710, 12672, 12633,
	12596, 12558, 12520, 12483, 12446, 12409, 12373, 12336, 12300, 12264, 12228,
	12193, 12157, 12122, 12087, 12053, 12018, 11984, 11950, 11916, 11882, 11848,
	11815, 11782, 11749, 11716, 11683, 11651, 11619, 11586, 11555, 11523, 11491,
	11460, 11429, 11398, 11367, 11336, 11305, 11275, 11245, 11215, 11185, 11155,
	11125, 11096, 11067, 11038, 11009, 10980, 10951, 10923, 10894, 10866, 10838,
	10810, 10782, 10755, 10727, 10700, 10673, 10645, 10618, 10592, 10565, 10538,
	10512, 10486, 10460, 10434, 10408, 10382, 10356, 10331, 10305, 10280, 10255,
	10230, 10205, 10180, 10156, 10131, 10107, 10082, 10058, 10034, 10010, 9986,
	9963,  9939,  9916,  9892,  9869,  9846,  9823,  9800,  9777,  9754,  9732,
	9709,  9687,  9664,  9642,  9620,  9598,  9576,  9554,  9533,  9511,  9489,
	9468,  9447,  9425,  9404,  9383,  9362,  9341,  9321,  9300,  9279,  9259,
	9239,  9218,  9198,  9178,  9158,  9138,  9118,  9098,  9079,  9059,  9039,
	9020,  9001,  8981,  8962,  8943,  8924,  8905,  8886,  8867,  8849,  8830,
	8812,  8793,  8775,  8756,  8738,  8720,  8702,  8684,  8666,  8648,  8630,
	8613,  8595,  8577,  8560,  8542,  8525,  8508,  8490,  8473,  8456,  8439,
	8422,  8405,  8389,  8372,  8355,  8339,  8322,  8306,  8289,  8273,  8257,
	8240,  8224,  8208,  8192,
};

static const struct v4l2_ctrl_config vdec_av1_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_AV1_SEQUENCE },
	{
		.id = V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY,
		.dims = { V4L2_AV1_MAX_TILE_COUNT },
	},
	{ .id = V4L2_CID_STATELESS_AV1_FRAME },
	{ .id = V4L2_CID_STATELESS_AV1_FILM_GRAIN },
	{
		.id = V4L2_CID_MPEG_VIDEO_AV1_PROFILE,
		.min = V4L2_MPEG_VIDEO_AV1_PROFILE_MAIN,
		.max = V4L2_MPEG_VIDEO_AV1_PROFILE_MAIN,
		.def = V4L2_MPEG_VIDEO_AV1_PROFILE_MAIN,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_AV1_LEVEL,
		.min = V4L2_MPEG_VIDEO_AV1_LEVEL_2_0,
		.max = V4L2_MPEG_VIDEO_AV1_LEVEL_5_1,
		.def = V4L2_MPEG_VIDEO_AV1_LEVEL_4_0,
	},
};

int vdec_av1_ctrls_init(struct v4l2_ctrl_handler *hdl)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdec_av1_ctrls); i++) {
		struct v4l2_ctrl_config c = vdec_av1_ctrls[i];

		if (c.id == V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY)
			c.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY;
		v4l2_ctrl_new_custom(hdl, &c, NULL);
	}
	return hdl->error;
}

static int av1_alloc(struct vdec_ctx *ctx, struct vdec_work_buf *b, size_t size)
{
	b->size = size;
	b->va = dma_alloc_coherent(ctx->dev->dev, size, &b->dma, GFP_KERNEL);
	return b->va ? 0 : -ENOMEM;
}

static void av1_free(struct vdec_ctx *ctx, struct vdec_work_buf *b)
{
	if (b->va)
		dma_free_coherent(ctx->dev->dev, b->size, b->va, b->dma);
	b->va = NULL;
}

static void av1_free_fb(struct vdec_ctx *ctx, struct vdec_av1_fbbuf *b)
{
	struct device *dev = ctx->dev->dev;

	if (b->mv_va)
		dma_free_coherent(dev, b->mv_size, b->mv_va, b->mv);
	if (b->cdf_va)
		dma_free_coherent(dev, VDEC_AV1_CDF_BUF, b->cdf_va, b->cdf);
	if (b->seg_va)
		dma_free_coherent(dev, b->seg_size, b->seg_va, b->seg);
	memset(b, 0, sizeof(*b));
}

/* motion vectors and segment ids of a frame: per 128x128 superblock */
static size_t av1_mv_size(u32 w, u32 h)
{
	return PAGE_ALIGN(DIV_ROUND_UP(w, 128) * DIV_ROUND_UP(h, 128) * SZ_1K);
}

static size_t av1_seg_size(u32 w, u32 h)
{
	return PAGE_ALIGN(DIV_ROUND_UP(w, 128) * DIV_ROUND_UP(h, 128) * 512);
}

static int av1_get_fb(struct vdec_ctx *ctx, unsigned int i, u32 w, u32 h)
{
	struct vdec_av1_fbbuf *b = &ctx->av1->buf[i];
	struct device *dev = ctx->dev->dev;
	size_t mv = av1_mv_size(w, h), seg = av1_seg_size(w, h);

	if (b->mv_va && b->mv_size >= mv && b->seg_size >= seg)
		return 0;
	av1_free_fb(ctx, b);
	b->mv_size = mv;
	b->seg_size = seg;
	b->mv_va = dma_alloc_coherent(dev, mv, &b->mv, GFP_KERNEL);
	b->cdf_va = dma_alloc_coherent(dev, VDEC_AV1_CDF_BUF, &b->cdf, GFP_KERNEL);
	b->seg_va = dma_alloc_coherent(dev, seg, &b->seg, GFP_KERNEL);
	if (!b->mv_va || !b->cdf_va || !b->seg_va) {
		av1_free_fb(ctx, b);
		return -ENOMEM;
	}
	return 0;
}

int vdec_av1_start(struct vdec_ctx *ctx)
{
	struct vdec_av1_state *st;
	unsigned int i;
	u8 *iq;
	int ret;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	ctx->av1 = st;
	ret = av1_alloc(ctx, &st->cdf_default, 4 * VDEC_AV1_CDF_BUF) ?:
	      av1_alloc(ctx, &st->iq, PAGE_ALIGN(VDEC_AV1_IQ_SIZE)) ?:
	      av1_alloc(ctx, &st->tile_lat, VDEC_AV1_TILE_BUF) ?:
	      av1_alloc(ctx, &st->tile_core, VDEC_AV1_TILE_BUF) ?:
	      /* LAT -> core side information (avc_mv+3c0, racing +0ac / +0b0);
	       * size not known, MIUI's lies inside a larger working area */
	      av1_alloc(ctx, &st->mv_tmp, SZ_2M) ?:
	      av1_alloc(ctx, &st->cols, VDEC_AV1_COLS_BUF);
	if (ret) {
		vdec_av1_stop(ctx);
		return ret;
	}
	for (i = 0; i < 4; i++)
		memcpy(st->cdf_default.va + i * VDEC_AV1_CDF_BUF, vdec_av1_cdf_default[i],
		       VDEC_AV1_CDF_SIZE);
	iq = st->iq.va;
	memcpy(iq, vdec_av1_iq_lut, sizeof(vdec_av1_iq_lut));
	memcpy(iq + 2 * VDEC_AV1_IQ_LUT, vdec_av1_qm, sizeof(vdec_av1_qm));
	return 0;
}

void vdec_av1_stop(struct vdec_ctx *ctx)
{
	struct vdec_av1_state *st = ctx->av1;
	unsigned int i;

	if (!st)
		return;
	for (i = 0; i < VDEC_AV1_MAX_FB; i++)
		av1_free_fb(ctx, &st->buf[i]);
	av1_free(ctx, &st->cdf_default);
	av1_free(ctx, &st->iq);
	av1_free(ctx, &st->tile_lat);
	av1_free(ctx, &st->tile_core);
	av1_free(ctx, &st->mv_tmp);
	av1_free(ctx, &st->cols);
	kfree(st);
	ctx->av1 = NULL;
}

static const void *vdec_ctrl(struct vdec_ctx *ctx, u32 id, unsigned int *elems)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	if (c && elems)
		*elems = c->elems;
	return c ? c->p_cur.p : NULL;
}

static int av1_msb(u32 n)
{
	return n ? 31 - __builtin_clz(n) : -1;
}

static s16 av1_resolve_divisor(u32 d, s16 *shift)
{
	int f, e;

	*shift = av1_msb(d);
	e = d - ((u32)1 << *shift);
	if (*shift > DIV_LUT_BITS)
		f = AV1_DIV_ROUND_UP_POW2(e, *shift - DIV_LUT_BITS);
	else
		f = e << (DIV_LUT_BITS - *shift);
	if (f > DIV_LUT_NUM)
		return -1;
	*shift += DIV_LUT_PREC_BITS;
	return div_lut[f];
}

static void av1_shear(struct vdec_av1_gm *gm)
{
	const int *mat = gm->wmmat;
	s16 shift, y;
	s64 gv, dv;

	if (mat[2] <= 0)
		return;
	gm->alpha = clamp_val(mat[2] - (1 << WARPEDMODEL_PREC_BITS), S16_MIN, S16_MAX);
	gm->beta = clamp_val(mat[3], S16_MIN, S16_MAX);
	y = av1_resolve_divisor(abs(mat[2]), &shift) * (mat[2] < 0 ? -1 : 1);
	gv = ((s64)mat[4] * (1 << WARPEDMODEL_PREC_BITS)) * y;
	gm->gamma = clamp_val((int)AV1_DIV_ROUND_UP_POW2_SIGNED(gv, shift), S16_MIN, S16_MAX);
	dv = ((s64)mat[3] * mat[4]) * y;
	gm->delta = clamp_val(mat[5] - (int)AV1_DIV_ROUND_UP_POW2_SIGNED(dv, shift) -
			      (1 << WARPEDMODEL_PREC_BITS), S16_MIN, S16_MAX);
	gm->alpha = AV1_DIV_ROUND_UP_POW2_SIGNED(gm->alpha, WARP_PARAM_REDUCE_BITS) *
		    (1 << WARP_PARAM_REDUCE_BITS);
	gm->beta = AV1_DIV_ROUND_UP_POW2_SIGNED(gm->beta, WARP_PARAM_REDUCE_BITS) *
		   (1 << WARP_PARAM_REDUCE_BITS);
	gm->gamma = AV1_DIV_ROUND_UP_POW2_SIGNED(gm->gamma, WARP_PARAM_REDUCE_BITS) *
		    (1 << WARP_PARAM_REDUCE_BITS);
	gm->delta = AV1_DIV_ROUND_UP_POW2_SIGNED(gm->delta, WARP_PARAM_REDUCE_BITS) *
		    (1 << WARP_PARAM_REDUCE_BITS);
}

static int av1_rel_dist(const struct v4l2_ctrl_av1_sequence *seq, int a, int b)
{
	int m, diff;

	if (!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT))
		return 0;
	diff = a - b;
	m = 1 << (seq->order_hint_bits - 1);
	return (diff & (m - 1)) - (diff & m);
}

/*
 * Motion field projection sources (vdec_av1_motion_field_estimation):
 * LAST if its own ALTREF is not this frame's GOLDEN, then BWDREF,
 * ALTREF2, ALTREF when they come later, then LAST2; three at most.
 * dir 2 marks a projection from the past (ref_to_cur negated).
 */
static bool av1_mfmv_add(struct vdec_ctx *ctx, struct vdec_av1_run *run, int ref, u32 dir)
{
	struct vdec_av1_mfmv *m = &run->mfmv;
	const struct vdec_av1_fbinfo *f;
	int fb = run->ref_fb[ref - 1], dist;

	m->fb[m->count] = VDEC_AV1_NO_IDX;
	if (fb < 0)
		return false;
	f = &ctx->av1->info[fb];
	if (!f->valid || f->intra || f->mi_rows != run->mi_rows || f->mi_cols != run->mi_cols)
		return false;
	dist = av1_rel_dist(run->seq, f->order_hint, run->fh->order_hint);
	m->valid[m->count] = 1;
	m->dir[m->count] = dir;
	m->ref_to_cur[m->count] = dir == 2 ? -dist : dist;
	m->fb[m->count] = fb;
	m->count++;
	return true;
}

static void av1_mfmv(struct vdec_ctx *ctx, struct vdec_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	int stamp = 2, last = run->ref_fb[0];
	unsigned int i;

	memset(&run->mfmv, 0, sizeof(run->mfmv));
	for (i = 0; i < 3; i++)
		run->mfmv.fb[i] = VDEC_AV1_NO_IDX;
	if (!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT) || run->intra ||
	    !(fh->flags & V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS))
		return;
	/* as libaom av1_setup_motion_field */
	if (last >= 0) {
		const struct vdec_av1_fbinfo *f = &ctx->av1->info[last];

		if (f->order_hints[V4L2_AV1_REF_ALTREF_FRAME] != fh->order_hints[V4L2_AV1_REF_GOLDEN_FRAME])
			av1_mfmv_add(ctx, run, V4L2_AV1_REF_LAST_FRAME, 2);
		stamp--;
	}
	if (av1_rel_dist(seq, fh->order_hints[V4L2_AV1_REF_BWDREF_FRAME], fh->order_hint) > 0 &&
	    av1_mfmv_add(ctx, run, V4L2_AV1_REF_BWDREF_FRAME, 0))
		stamp--;
	if (av1_rel_dist(seq, fh->order_hints[V4L2_AV1_REF_ALTREF2_FRAME], fh->order_hint) > 0 &&
	    av1_mfmv_add(ctx, run, V4L2_AV1_REF_ALTREF2_FRAME, 0))
		stamp--;
	if (stamp >= 0 && run->mfmv.count < 3 &&
	    av1_rel_dist(seq, fh->order_hints[V4L2_AV1_REF_ALTREF_FRAME], fh->order_hint) > 0 &&
	    av1_mfmv_add(ctx, run, V4L2_AV1_REF_ALTREF_FRAME, 0))
		stamp--;
	if (stamp >= 0 && run->mfmv.count < 3)
		av1_mfmv_add(ctx, run, V4L2_AV1_REF_LAST2_FRAME, 2);
}

static void av1_scale(struct vdec_av1_scale *s, u32 ref_w, u32 ref_h, u32 w, u32 h)
{
	memset(s, 0, sizeof(*s));
	if (2 * w < ref_w || 2 * h < ref_h || w > 16 * ref_w || h > 16 * ref_h ||
	    (ref_w == w && ref_h == h)) {
		s->x_scale = s->y_scale = -1;
		return;
	}
	s->x_scale = ((ref_w << AV1_REF_SCALE_SHIFT) + (w >> 1)) / w;
	s->y_scale = ((ref_h << AV1_REF_SCALE_SHIFT) + (h >> 1)) / h;
	s->is_scaled = s->x_scale != BIT(AV1_REF_SCALE_SHIFT) || s->y_scale != BIT(AV1_REF_SCALE_SHIFT);
	s->x_step = AV1_DIV_ROUND_UP_POW2(s->x_scale, AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
	s->y_step = AV1_DIV_ROUND_UP_POW2(s->y_scale, AV1_REF_SCALE_SHIFT - AV1_SCALE_SUBPEL_BITS);
}

static int av1_setup(struct vdec_ctx *ctx, struct vdec_av1_run *run)
{
	struct vdec_av1_state *st = ctx->av1;
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	const struct v4l2_av1_segmentation *sg = &fh->segmentation;
	const struct v4l2_av1_quantization *q = &fh->quantization;
	const struct v4l2_av1_tile_info *ti = &fh->tile_info;
	u32 mib_log2;
	unsigned int i, j;

	run->bitdepth = seq->bit_depth;
	run->sb_log2 = seq->flags & V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK ? 7 : 6;
	mib_log2 = run->sb_log2 - 2;
	run->w = fh->frame_width_minus_1 + 1;
	run->h = fh->frame_height_minus_1 + 1;
	run->upscaled_w = fh->upscaled_width ?: run->w;
	run->mi_cols = ((run->w + 7) >> 3) << 1;
	run->mi_rows = ((run->h + 7) >> 3) << 1;
	/* the capture buffers' coded size */
	run->aligned_w = ctx->dst_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	run->aligned_h = ctx->dst_fmt.fmt.pix_mp.plane_fmt[0].sizeimage / run->aligned_w;
	if (ctx->cap_10bit)
		run->aligned_h = run->aligned_h * 4 / 5;
	run->intra = fh->frame_type == V4L2_AV1_KEY_FRAME || fh->frame_type == V4L2_AV1_INTRA_ONLY_FRAME;
	run->superres = run->upscaled_w != run->w;
	run->reference_mode = !run->intra && (fh->flags & V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT) ? 2 : 0;
	run->q_ctx = q->base_q_idx <= 20 ? 0 : q->base_q_idx <= 60 ? 1 : q->base_q_idx <= 120 ? 2 : 3;

	/* segmentation; the feature data only counts while enabled */
	memset(&run->seg, 0, sizeof(run->seg));
	if (sg->flags & V4L2_AV1_SEGMENTATION_FLAG_ENABLED) {
		run->seg.enabled = true;
		run->seg.update_map = sg->flags & V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
		run->seg.temporal_update = sg->flags & V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
		run->seg.update_data = sg->flags & V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA;
		run->seg.preskip = sg->flags & V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP;
		run->seg.last_active = sg->last_active_seg_id;
		for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
			run->seg.feature_mask[i] = sg->feature_enabled[i];
			for (j = 0; j < V4L2_AV1_SEG_LVL_MAX; j++)
				run->seg.feature_data[i][j] = sg->feature_data[i][j];
		}
	}
	run->coded_lossless = true;
	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		int qi = q->base_q_idx;

		if (run->seg.enabled && (run->seg.feature_mask[i] & BIT(V4L2_AV1_SEG_LVL_ALT_Q)))
			qi = clamp_val(qi + run->seg.feature_data[i][V4L2_AV1_SEG_LVL_ALT_Q], 0, 255);
		run->qindex[i] = qi;
		run->lossless[i] = !qi && !q->delta_q_y_dc && !q->delta_q_u_ac && !q->delta_q_u_dc &&
				   !q->delta_q_v_ac && !q->delta_q_v_dc;
		if (!run->lossless[i])
			run->coded_lossless = false;
	}

	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
		struct vdec_av1_gm *g = &run->gm[i];

		memset(g, 0, sizeof(*g));
		g->wmtype = fh->global_motion.type[i];
		for (j = 0; j < 6; j++)
			g->wmmat[j] = fh->global_motion.params[i][j];
		g->invalid = !!(fh->global_motion.invalid & BIT(i));
		if (g->wmtype <= V4L2_AV1_WARP_MODEL_AFFINE)
			av1_shear(g);
	}

	for (i = 0; i < V4L2_AV1_CDEF_MAX; i++) {
		u8 ys = fh->cdef.y_sec_strength[i], uvs = fh->cdef.uv_sec_strength[i];

		run->cdef_y[i] = fh->cdef.y_pri_strength[i] << 2 | (ys == 4 ? 3 : ys);
		run->cdef_uv[i] = fh->cdef.uv_pri_strength[i] << 2 | (uvs == 4 ? 3 : uvs);
	}
	/* the unit sizes count only with restoration on (vendor: 0 else) */
	for (i = 0; i < 3; i++)
		run->lr_size[i] = fh->loop_restoration.frame_restoration_type[0] ||
				  fh->loop_restoration.frame_restoration_type[1] ||
				  fh->loop_restoration.frame_restoration_type[2] ?
				  fh->loop_restoration.loop_restoration_size[i] : 0;
	for (i = 0; i <= ti->tile_cols && i <= V4L2_AV1_MAX_TILE_COLS; i++)
		run->col_sb[i] = ALIGN(ti->mi_col_starts[i], BIT(mib_log2)) >> mib_log2;
	for (i = 0; i <= ti->tile_rows && i <= V4L2_AV1_MAX_TILE_ROWS; i++)
		run->row_sb[i] = ALIGN(ti->mi_row_starts[i], BIT(mib_log2)) >> mib_log2;
	/* a tile group ends where the next tile's data does not follow on */
	for (i = 0; i < run->ntiles; i++)
		run->tile_last[i] = i + 1 == run->ntiles ||
			run->tge[i + 1].tile_offset != run->tge[i].tile_offset + run->tge[i].tile_size;
	for (i = 0; i < 2; i++)
		run->skip_frame[i] = fh->flags & V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT ?
				     fh->skip_mode_frame[i] - V4L2_AV1_REF_LAST_FRAME : -1;

	/* references */
	memset(run->sign_bias, 0, sizeof(run->sign_bias));
	run->prev_fb = VDEC_AV1_NO_IDX;
	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		struct vb2_buffer *vb = NULL;
		const struct vdec_av1_fbinfo *f;

		run->ref_fb[i] = VDEC_AV1_NO_IDX;
		run->ref_y[i] = run->y_dma;
		run->ref_c[i] = run->c_dma;
		run->ref_w[i] = run->upscaled_w;
		run->ref_h[i] = run->h;
		memset(&run->scale[i], 0, sizeof(run->scale[i]));
		if (run->intra)
			continue;
		vb = vb2_find_buffer(cap, fh->reference_frame_ts[fh->ref_frame_idx[i]]);
		if (!vb || vb->index >= VDEC_AV1_MAX_FB || !st->info[vb->index].valid) {
			dev_info_ratelimited(ctx->dev->dev, "av1: reference %u missing\n", i);
			continue;
		}
		f = &st->info[vb->index];
		run->ref_fb[i] = vb->index;
		run->ref_y[i] = vb2_dma_contig_plane_dma_addr(vb, 0);
		run->ref_c[i] = vb2_dma_contig_plane_dma_addr(vb, 1);
		run->ref_w[i] = f->upscaled_w;
		run->ref_h[i] = f->h;
		av1_scale(&run->scale[i], f->upscaled_w, f->h, run->w, run->h);
		run->sign_bias[i + 1] = av1_rel_dist(seq, fh->order_hints[i + 1], fh->order_hint) > 0;
	}
	if (fh->primary_ref_frame != AV1_PRIMARY_REF_NONE)
		run->prev_fb = run->ref_fb[fh->primary_ref_frame];
	av1_mfmv(ctx, run);

	/* buffers */
	run->cols_dma = st->cols.dma;
	run->cdf_in = st->cdf_default.dma + run->q_ctx * VDEC_AV1_CDF_BUF;
	if (run->prev_fb >= 0)
		run->cdf_in = st->buf[run->prev_fb].cdf;
	run->cdf_out = st->buf[run->fb].cdf;
	run->mv_cur = st->buf[run->fb].mv;
	run->seg_cur = st->buf[run->fb].seg;
	for (i = 0; i < 3; i++)
		run->mv_mfmv[i] = run->mfmv.fb[i] >= 0 ? st->buf[run->mfmv.fb[i]].mv : 0;
	run->seg_prev = run->seg.enabled && run->prev_fb >= 0 &&
			st->info[run->prev_fb].mi_rows == run->mi_rows &&
			st->info[run->prev_fb].mi_cols == run->mi_cols;
	run->seg_prev_dma = run->seg_prev ? st->buf[run->prev_fb].seg : 0;
	if (fh->primary_ref_frame == AV1_PRIMARY_REF_NONE || !run->seg.enabled)
		memset(st->buf[run->fb].seg_va, 0, st->buf[run->fb].seg_size);
	return 0;
}

static void av1_log(struct vdec_ctx *ctx, const struct vdec_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *fh = run->fh;

	dev_info(ctx->dev->dev,
		 "av1 fb %d type %u oh %u [%u %u %u %u %u %u %u] %ux%u (up %u) q %u ctx %u tiles %ux%u (%u) prim %u prev %d refs %d %d %d %d %d %d %d mfmv %d [%d %d %d] seg %u\n",
		 run->fb, fh->frame_type, fh->order_hint, fh->order_hints[1], fh->order_hints[2],
		 fh->order_hints[3], fh->order_hints[4], fh->order_hints[5], fh->order_hints[6],
		 fh->order_hints[7], run->w, run->h, run->upscaled_w,
		 fh->quantization.base_q_idx, run->q_ctx, fh->tile_info.tile_cols,
		 fh->tile_info.tile_rows, run->ntiles, fh->primary_ref_frame, run->prev_fb,
		 run->ref_fb[0], run->ref_fb[1], run->ref_fb[2], run->ref_fb[3], run->ref_fb[4],
		 run->ref_fb[5], run->ref_fb[6], run->mfmv.count, run->mfmv.fb[0], run->mfmv.fb[1],
		 run->mfmv.fb[2], run->seg.enabled);
}

int vdec_av1_run(struct vdec_ctx *ctx)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_av1_state *st = ctx->av1;
	struct vdec_av1_run *run;
	struct media_request *req;
	unsigned long flags;
	int ret = 0;

	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	run->src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	req = run->src->vb2_buf.req_obj.req;
	if (req)
		ret = v4l2_ctrl_request_setup(req, &ctx->ctrl_hdl);
	v4l2_m2m_buf_copy_metadata(run->src, run->dst);

	run->seq = vdec_ctrl(ctx, V4L2_CID_STATELESS_AV1_SEQUENCE, NULL);
	run->fh = vdec_ctrl(ctx, V4L2_CID_STATELESS_AV1_FRAME, NULL);
	run->tge = vdec_ctrl(ctx, V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY, &run->ntiles);
	if (ret || !run->seq || !run->fh || !run->tge) {
		ret = ret ?: -EINVAL;
		goto out;
	}
	if (run->seq->seq_profile || (run->seq->bit_depth != 8 && run->seq->bit_depth != 10) ||
	    (run->seq->flags & V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME)) {
		dev_info_ratelimited(d->dev, "av1: only main profile 4:2:0, 8 / 10 bit\n");
		ret = -EINVAL;
		goto out;
	}
	if ((run->seq->bit_depth == 10) != ctx->cap_10bit) {
		dev_info_ratelimited(d->dev, "av1: %u-bit stream, capture format %s\n",
				     run->seq->bit_depth, ctx->cap_10bit ? "MT2110T" : "MM21");
		ret = -EINVAL;
		goto out;
	}
	if (!run->ntiles || run->ntiles != run->fh->tile_info.tile_cols * run->fh->tile_info.tile_rows) {
		dev_info_ratelimited(d->dev, "av1: %u tile entries for %ux%u tiles\n", run->ntiles,
				     run->fh->tile_info.tile_cols, run->fh->tile_info.tile_rows);
		ret = -EINVAL;
		goto out;
	}

	run->fb = run->dst->vb2_buf.index;
	if (run->fb >= VDEC_AV1_MAX_FB) {
		ret = -EINVAL;
		goto out;
	}
	ret = av1_get_fb(ctx, run->fb, run->fh->upscaled_width ?: run->fh->frame_width_minus_1 + 1,
			 run->fh->frame_height_minus_1 + 1);
	if (ret)
		goto out;
	run->bs_dma = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->bs_len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->bs_size = vb2_plane_size(&run->src->vb2_buf, 0);
	run->y_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->c_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	ret = av1_setup(ctx, run);
	if (ret)
		goto out;
	run->ube = &ctx->ube[ctx->slot];
	run->err = &ctx->err[ctx->slot];
	ctx->slot = (ctx->slot + 1) % VDEC_WORK_SLOTS;
	if (av1_debug > 0) {
		av1_log(ctx, run);
		av1_debug--;
	}

	/* what the frames referencing this one will need (refresh after done
	 * is the client's business: references come by timestamp) */
	st->info[run->fb] = (struct vdec_av1_fbinfo) {
		.valid = true,
		.frame_type = run->fh->frame_type,
		.intra = run->intra,
		.order_hint = run->fh->order_hint,
		.w = run->w, .h = run->h, .upscaled_w = run->upscaled_w,
		.mi_cols = run->mi_cols, .mi_rows = run->mi_rows,
	};
	memcpy(st->info[run->fb].order_hints, run->fh->order_hints,
	       sizeof(st->info[run->fb].order_hints));

	vdec_hw_av1_lat(ctx, run);
	vdec_hw_av1_core(ctx, run);
	spin_lock_irqsave(&d->irq_lock, flags);
	d->pending = BIT(VDEC_CORE);
	d->irq_status[VDEC_CORE] = d->irq_status[VDEC_LAT] = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	vdec_hw_av1_start(d);
out:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->ctrl_hdl);
	kfree(run);
	return ret;
}

void vdec_av1_done(struct vdec_ctx *ctx, bool ok)
{
	struct vdec_av1_state *st = ctx->av1;
	struct vb2_v4l2_buffer *dst;

	if (!st || ok)
		return;
	/* a failed picture is no reference */
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (dst && dst->vb2_buf.index < VDEC_AV1_MAX_FB)
		st->info[dst->vb2_buf.index].valid = false;
}
