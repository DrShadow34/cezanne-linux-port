// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: VP9 controls and per-picture job setup (frame-based: the
 * whole frame, uncompressed header included, in the source buffer).
 *
 * The LAT parses the uncompressed and compressed headers itself and
 * applies the compressed header's probability deltas to the context it
 * was given, so the driver writes the pre-delta context of the frame
 * (one of four saved ones, v4l2-vp9 helpers) through the probability
 * ports and tells the LAT which probability groups the compressed header
 * carries. The software copy gets the same deltas (v4l2_vp9_fw_update_probs)
 * and is saved back when refresh_frame_context is set. Backward adaptation
 * (frame_parallel_decoding_mode = 0) still needs the symbol counts the LAT
 * dumps after each picture (NOTES "VP9"): not done yet, such streams
 * drift after the first inter frame.
 */
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "vdec.h"
#include "vdec_vp9_quant.h"

/* LAT symbol counts dump, same layout as the MT8192 LAT (mainline
 * vdec_vp9_req_lat_if.c, struct vdec_vp9_slice_frame_counts) */
struct vdec_vp9_counts {
	union {
		struct {
			u32 band_0[3];
			u32 padding0[1];
			u32 band_1_5[5][6];
			u32 padding1[2];
		} eob_branch[4][2][2];
		u32 eob_branch_space[256 * 4];
	};
	struct {
		u32 band_0[3][4];
		u32 band_1_5[5][6][4];
	} coef_probs[4][2][2];
	u32 intra_inter[4][2];
	u32 comp_inter[5][2];
	u32 comp_inter_padding[2];
	u32 comp_ref[5][2];
	u32 comp_ref_padding[2];
	u32 single_ref[5][2][2];
	u32 inter_mode[7][4];
	u32 y_mode[4][12];
	u32 uv_mode[10][10];
	u32 partition[16][4];
	u32 switchable_interp[4][4];
	u32 tx_p8x8[2][2];
	u32 tx_p16x16[2][4];
	u32 tx_p32x32[2][4];
	u32 skip[3][4];
	u32 joint[4];
	struct {
		u32 sign[2];
		u32 class0[2];
		u32 classes[12];
		u32 bits[10][2];
		u32 padding[4];
		u32 class0_fp[2][4];
		u32 fp[4];
		u32 class0_hp[2];
		u32 hp[2];
	} mvcomp[2];
	u32 reserved[126][4];
};

struct vdec_vp9_counts_map {
	u32 skip[3][2];
	u32 y_mode[4][10];
	u32 filter[4][3];
	u32 sign[2][2];
	u32 classes[2][11];
	u32 class0[2][2];
	u32 bits[2][10][2];
	u32 class0_fp[2][2][4];
	u32 fp[2][4];
	u32 class0_hp[2][2];
	u32 hp[2][2];
};

static void vdec_vp9_map_counts(struct vdec_vp9_counts *c, struct vdec_vp9_counts_map *m,
				struct v4l2_vp9_frame_symbol_counts *h)
{
	unsigned int i, j, k, l, n;

	h->partition = &c->partition;
	h->intra_inter = &c->intra_inter;
	h->tx32p = &c->tx_p32x32;
	h->tx16p = &c->tx_p16x16;
	h->tx8p = &c->tx_p8x8;
	h->uv_mode = &c->uv_mode;
	h->comp = &c->comp_inter;
	h->comp_ref = &c->comp_ref;
	h->single_ref = &c->single_ref;
	h->mv_mode = &c->inter_mode;
	h->mv_joint = &c->joint;
	for (i = 0; i < 3; i++)
		memcpy(m->skip[i], c->skip[i], sizeof(m->skip[0]));
	h->skip = &m->skip;
	for (i = 0; i < 4; i++)
		memcpy(m->y_mode[i], c->y_mode[i], sizeof(m->y_mode[0]));
	h->y_mode = &m->y_mode;
	for (i = 0; i < 4; i++)
		memcpy(m->filter[i], c->switchable_interp[i], sizeof(m->filter[0]));
	h->filter = &m->filter;
	for (i = 0; i < 2; i++) {
		memcpy(m->sign[i], c->mvcomp[i].sign, sizeof(m->sign[0]));
		memcpy(m->classes[i], c->mvcomp[i].classes, sizeof(m->classes[0]));
		memcpy(m->class0[i], c->mvcomp[i].class0, sizeof(m->class0[0]));
		for (j = 0; j < 10; j++)
			memcpy(m->bits[i][j], c->mvcomp[i].bits[j], sizeof(m->bits[0][0]));
		for (j = 0; j < 2; j++)
			memcpy(m->class0_fp[i][j], c->mvcomp[i].class0_fp[j],
			       sizeof(m->class0_fp[0][0]));
		memcpy(m->fp[i], c->mvcomp[i].fp, sizeof(m->fp[0]));
		memcpy(m->class0_hp[i], c->mvcomp[i].class0_hp, sizeof(m->class0_hp[0]));
		memcpy(m->hp[i], c->mvcomp[i].hp, sizeof(m->hp[0]));
	}
	h->sign = &m->sign;
	h->classes = &m->classes;
	h->class0 = &m->class0;
	h->bits = &m->bits;
	h->class0_fp = &m->class0_fp;
	h->fp = &m->fp;
	h->class0_hp = &m->class0_hp;
	h->hp = &m->hp;
	/* coefficient counts: hardware c3 = "more coefs" (helper eob[1]) */
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			for (k = 0; k < 2; k++) {
				for (n = 0; n < 3; n++) {
					h->coeff[i][j][k][0][n] = (u32 (*)[3])
						&c->coef_probs[i][j][k].band_0[n];
					h->eob[i][j][k][0][n][0] = &c->eob_branch[i][j][k].band_0[n];
					h->eob[i][j][k][0][n][1] = &c->coef_probs[i][j][k].band_0[n][3];
				}
				for (l = 1; l < 6; l++)
					for (n = 0; n < 6; n++) {
						h->coeff[i][j][k][l][n] = (u32 (*)[3])
							&c->coef_probs[i][j][k].band_1_5[l - 1][n];
						h->eob[i][j][k][l][n][0] =
							&c->eob_branch[i][j][k].band_1_5[l - 1][n];
						h->eob[i][j][k][l][n][1] =
							&c->coef_probs[i][j][k].band_1_5[l - 1][n][3];
					}
			}
}

static int vp9_counts_buf = 1;	/* 0: save_a (+1dc 2), 1: save_b (+1dc 1) */
module_param(vp9_counts_buf, int, 0644);

static int vp9_dump;
module_param(vp9_dump, int, 0644);
MODULE_PARM_DESC(vp9_dump, "hex-dump the LAT probability dump and the software context of N pictures");

static int vp9_check_post;
module_param(vp9_check_post, int, 0644);
MODULE_PARM_DESC(vp9_check_post, "compare the read-back context with the software one (needs COMPRESSED_HDR)");

static int vp9_debug;
module_param(vp9_debug, int, 0644);
MODULE_PARM_DESC(vp9_debug, "log the next N VP9 pictures");

static const struct v4l2_ctrl_config vdec_vp9_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_VP9_FRAME },
	{ .id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR },
	{
		.id = V4L2_CID_MPEG_VIDEO_VP9_PROFILE,
		.min = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
		.max = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
		.def = V4L2_MPEG_VIDEO_VP9_PROFILE_0,
	},
};

int vdec_vp9_ctrls_init(struct v4l2_ctrl_handler *hdl)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdec_vp9_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &vdec_vp9_ctrls[i], NULL);
	return hdl->error;
}

#define VP9_SAVE_SIZE	SZ_16K
#define VP9_MV_SIZE(mb)	PAGE_ALIGN((size_t)(mb) * 128)

static int vdec_vp9_alloc(struct vdec_ctx *ctx, struct vdec_work_buf *b, size_t size)
{
	b->size = size;
	b->va = dma_alloc_coherent(ctx->dev->dev, size, &b->dma, GFP_KERNEL);
	return b->va ? 0 : -ENOMEM;
}

static void vdec_vp9_free(struct vdec_ctx *ctx, struct vdec_work_buf *b)
{
	if (b->va)
		dma_free_coherent(ctx->dev->dev, b->size, b->va, b->dma);
	b->va = NULL;
}

int vdec_vp9_start(struct vdec_ctx *ctx)
{
	struct vdec_vp9_state *st;
	unsigned int i, mb = ctx->mb_w * ctx->mb_h;
	int ret;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	ctx->vp9 = st;
	for (i = 0; i < 4; i++)
		memcpy(&st->fctx[i], &v4l2_vp9_default_probs, sizeof(st->fctx[i]));
	ret = vdec_vp9_alloc(ctx, &st->lat_mv, VP9_MV_SIZE(mb)) ?:
	      vdec_vp9_alloc(ctx, &st->core_mv, VP9_MV_SIZE(mb)) ?:
	      vdec_vp9_alloc(ctx, &st->save_a, VP9_SAVE_SIZE) ?:
	      vdec_vp9_alloc(ctx, &st->save_b, VP9_SAVE_SIZE) ?:
	      /* segment maps (MIUI: 128 KiB apart at 640x360) */
	      vdec_vp9_alloc(ctx, &st->lat_seg, max_t(size_t, SZ_128K, PAGE_ALIGN(mb * 32))) ?:
	      vdec_vp9_alloc(ctx, &st->core_seg, max_t(size_t, SZ_128K, PAGE_ALIGN(mb * 32)));
	if (ret) {
		vdec_vp9_stop(ctx);
		return ret;
	}
	return 0;
}

void vdec_vp9_stop(struct vdec_ctx *ctx)
{
	struct vdec_vp9_state *st = ctx->vp9;

	if (!st)
		return;
	vdec_vp9_free(ctx, &st->lat_mv);
	vdec_vp9_free(ctx, &st->core_mv);
	vdec_vp9_free(ctx, &st->save_a);
	vdec_vp9_free(ctx, &st->save_b);
	vdec_vp9_free(ctx, &st->lat_seg);
	vdec_vp9_free(ctx, &st->core_seg);
	kfree(st);
	ctx->vp9 = NULL;
}

static const void *vdec_ctrl(struct vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	return c ? c->p_cur.p : NULL;
}

/* capture buffer of a reference by timestamp; the current picture if gone */
static void vdec_vp9_ref(struct vdec_ctx *ctx, struct vdec_vp9_run *run,
			 unsigned int i, u64 ts)
{
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct vb2_buffer *vb = run->inter ? vb2_find_buffer(cap, ts) : NULL;

	if (!vb) {
		if (run->inter)
			dev_warn_ratelimited(ctx->dev->dev, "vp9: reference %u (ts %llu) missing, frame ts %llu (tgid %d)\n",
					     i, ts, run->src->vb2_buf.timestamp, ctx->tgid);
		run->ref_y[i] = run->y_dma;
		run->ref_c[i] = run->c_dma;
		return;
	}
	run->ref_y[i] = vb2_dma_contig_plane_dma_addr(vb, 0);
	run->ref_c[i] = vb2_dma_contig_plane_dma_addr(vb, 1);
}

/*
 * Probability groups the compressed header carries (the LAT is told
 * which to parse): the syntax of 6.3 in order, one bit each.
 */
static u32 vdec_vp9_groups(const struct v4l2_ctrl_vp9_frame *f)
{
	u32 g = BIT(0) | BIT(1) | BIT(2);	/* tx, coef, skip */

	if (f->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY))
		return g;
	g |= BIT(3) | BIT(5);			/* inter_mode, is_inter */
	if (f->interpolation_filter == V4L2_VP9_INTERP_FILTER_SWITCHABLE)
		g |= BIT(4);
	if (f->reference_mode == V4L2_VP9_REFERENCE_MODE_SELECT)
		g |= BIT(6);			/* comp_inter */
	if (f->reference_mode != V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE)
		g |= BIT(7);			/* single_ref */
	if (f->reference_mode != V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE)
		g |= BIT(8);			/* comp_ref */
	g |= BIT(9) | BIT(10) | BIT(11) | BIT(12);	/* y_mode, partition, mv, mv fr */
	if (f->flags & V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV)
		g |= BIT(13);			/* mv hp */
	return g;
}

static u32 vdec_vp9_q(int idx, bool ten)
{
	return (ten ? vp9_dc_qlookup_10 : vp9_dc_qlookup)[clamp(idx, 0, 255)];
}

static void vdec_vp9_quant(struct vdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	unsigned int seg, nseg = (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) ? 8 : 1;
	const u16 *ac = f->bit_depth == 10 ? vp9_ac_qlookup_10 : vp9_ac_qlookup;
	bool ten = f->bit_depth == 10;

	for (seg = 0; seg < nseg; seg++) {
		int q = f->quant.base_q_idx;

		if (v4l2_vp9_seg_feat_enabled(f->seg.feature_enabled, V4L2_VP9_SEG_LVL_ALT_Q, seg)) {
			if (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE)
				q = f->seg.feature_data[seg][V4L2_VP9_SEG_LVL_ALT_Q];
			else
				q += f->seg.feature_data[seg][V4L2_VP9_SEG_LVL_ALT_Q];
			q = clamp(q, 0, 255);
		}
		run->quant[2 * seg] = ac[q] << 16 | vdec_vp9_q(q + f->quant.delta_q_y_dc, ten);
		run->quant[2 * seg + 1] = ac[clamp(q + f->quant.delta_q_uv_ac, 0, 255)] << 16 |
					  vdec_vp9_q(q + f->quant.delta_q_uv_dc, ten);
	}
}

static void vdec_vp9_log(struct vdec_dev *d, const struct vdec_vp9_run *run, u8 idx)
{
	const struct v4l2_ctrl_vp9_frame *f = run->frame;

	dev_info(d->dev, "vp9 %ux%u %s%s flags %#x q=%u lf=%u ctx=%u/%u reset=%u groups=%#x uhdr=%u chdr=%u bs=%u prev_mvs=%u quant0=%#x\n",
		 run->w, run->h, run->inter ? "inter" : "intra",
		 (f->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) ? "" : " hidden",
		 f->flags, f->quant.base_q_idx, f->lf.level, idx,
		 f->frame_context_idx, f->reset_frame_context, run->groups,
		 f->uncompressed_header_size, f->compressed_header_size,
		 run->bs_len, run->use_prev_mvs, run->quant[0]);
}

int vdec_vp9_run(struct vdec_ctx *ctx)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_vp9_state *st = ctx->vp9;
	struct vdec_vp9_run *run;
	struct media_request *req;
	const struct v4l2_ctrl_vp9_frame *f;
	unsigned long flags;
	u8 idx;
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

	run->frame = f = vdec_ctrl(ctx, V4L2_CID_STATELESS_VP9_FRAME);
	run->hdr = vdec_ctrl(ctx, V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
	if (ret || !f || !run->hdr) {
		ret = ret ?: -EINVAL;
		goto out;
	}
	run->w = f->frame_width_minus_1 + 1;
	run->h = f->frame_height_minus_1 + 1;
	if (ALIGN(run->w, 16) / 16 != ctx->mb_w || ALIGN(run->h, 16) / 16 != ctx->mb_h) {
		dev_info_ratelimited(d->dev, "frame %ux%u does not match the format (%ux%u MBs)\n",
				     run->w, run->h, ctx->mb_w, ctx->mb_h);
		ret = -EINVAL;
		goto out;
	}
	if (!((f->profile == 0 && f->bit_depth == 8) || (f->profile == 2 && f->bit_depth == 10)) ||
	    !(f->flags & V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING) ||
	    !(f->flags & V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING)) {
		dev_info_ratelimited(d->dev, "only VP9 profile 0 8-bit / profile 2 10-bit 4:2:0\n");
		ret = -EINVAL;
		goto out;
	}
	if ((f->bit_depth == 10) != ctx->cap_10bit) {
		dev_info_ratelimited(d->dev, "vp9: %u-bit stream, capture format %s\n", f->bit_depth,
				     ctx->cap_10bit ? "MT2110T" : "MM21");
		ret = -EINVAL;
		goto out;
	}
	if (!f->quant.base_q_idx && !f->quant.delta_q_y_dc && !f->quant.delta_q_uv_dc &&
	    !f->quant.delta_q_uv_ac) {
		/* lossless (WHT) hangs the core with the current programming */
		dev_info_ratelimited(d->dev, "vp9: lossless frames are not supported\n");
		ret = -EINVAL;
		goto out;
	}
	run->inter = !(f->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME | V4L2_VP9_FRAME_FLAG_INTRA_ONLY));
	run->bs_dma = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->bs_len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->bs_size = vb2_plane_size(&run->src->vb2_buf, 0);
	run->y_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->c_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	vdec_vp9_ref(ctx, run, 0, f->last_frame_ts);
	vdec_vp9_ref(ctx, run, 1, f->golden_frame_ts);
	vdec_vp9_ref(ctx, run, 2, f->alt_frame_ts);
	run->use_prev_mvs = run->inter && st->have_last && st->last_show &&
			    !st->last_intra && st->last_w == run->w && st->last_h == run->h &&
			    !(f->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT);
	run->groups = vdec_vp9_groups(f);
	vdec_vp9_quant(run);
	run->ube = &ctx->ube[ctx->slot];
	run->err = &ctx->err[ctx->slot];
	ctx->slot = (ctx->slot + 1) % VDEC_WORK_SLOTS;

	/* frame context: reset as the header says, then the pre-delta table */
	idx = v4l2_vp9_reset_frame_ctx(f, st->fctx);
	memcpy(&run->probs, &st->fctx[idx], sizeof(run->probs));
	/* software copy with the client's deltas, only for vp9_check_post */
	memcpy(&st->post, &run->probs, sizeof(st->post));
	v4l2_vp9_fw_update_probs(&st->post, run->hdr, f);
	/* the context to store is read back from the LAT after the picture
	 * (vdec_vp9_done): the compressed header's deltas are applied by the
	 * hardware, the client's COMPRESSED_HDR control is not needed */
	memcpy(&st->pre, &run->probs, sizeof(st->pre));
	st->refresh = f->flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
	st->adapt = st->refresh && !(f->flags & (V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT |
						V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE));
	st->adapt_idx = idx;
	st->adapt_intra = !run->inter;
	st->adapt_use_128 = run->inter && st->last_key;
	st->adapt_interp = f->interpolation_filter;
	st->adapt_flags = f->flags;
	st->last_key = f->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	st->have_last = true;
	st->last_w = run->w;
	st->last_h = run->h;
	st->last_show = f->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
	/* only an intra-only previous frame disables its MVs (a keyframe
	 * does not: MIUI sets avc_mv+3c4 on the first inter frame) */
	st->last_intra = !!(f->flags & V4L2_VP9_FRAME_FLAG_INTRA_ONLY);

	if (vp9_debug > 0) {
		vdec_vp9_log(d, run, idx);
		print_hex_dump(KERN_INFO, "vp9F ", DUMP_PREFIX_OFFSET, 16, 1, f, sizeof(*f), false);
		vp9_debug--;
	}
	vdec_hw_vp9_lat(ctx, run);
	vdec_hw_vp9_core(ctx, run);
	spin_lock_irqsave(&d->irq_lock, flags);
	d->pending = BIT(VDEC_CORE);
	d->irq_status[VDEC_CORE] = d->irq_status[VDEC_LAT] = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	vdec_hw_vp9_start(d);
out:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->ctrl_hdl);
	kfree(run);
	return ret;
}

static u32 vp9_sum(const u32 *p, unsigned int n)
{
	u32 s = 0;

	while (n--)
		s += *p++;
	return s;
}

/*
 * Backward adaptation (libvpx vp9_adapt_*_probs): merge the counts into
 * the context the picture started from. Intra pictures adapt only the
 * coefficients and keep the other probabilities with their deltas.
 */
static void vdec_vp9_adapt(struct vdec_ctx *ctx)
{
	struct vdec_vp9_state *st = ctx->vp9;
	struct vdec_vp9_counts *c = (st->counts_buf ? st->save_b : st->save_a).va;
	struct v4l2_vp9_frame_symbol_counts *h;
	struct vdec_vp9_counts_map *m;
	struct v4l2_vp9_frame_context *res = &st->fctx[st->adapt_idx];

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!h || !m)
		goto out;
	vdec_vp9_map_counts(c, m, h);
	if (st->adapt_intra) {
		memcpy(res, &st->post, sizeof(*res));
		memcpy(res->coef, st->pre.coef, sizeof(res->coef));
	} else {
		memcpy(res, &st->pre, sizeof(*res));
	}
	v4l2_vp9_adapt_coef_probs(res, h, st->adapt_use_128, st->adapt_intra);
	if (!st->adapt_intra)
		v4l2_vp9_adapt_noncoef_probs(res, h, st->adapt_refmode, st->adapt_interp,
					     st->adapt_tx_mode, st->adapt_flags);
	if (vp9_debug > 0) {
		const u32 *a = st->save_a.va, *b = st->save_b.va;
		unsigned int i, na = 0, nb = 0, fa = ~0u, fb = ~0u;

		for (i = 0; i < VP9_SAVE_SIZE / 4; i++) {
			if (a[i]) { na++; if (fa == ~0u) fa = i; }
			if (b[i]) { nb++; if (fb == ~0u) fb = i; }
		}
		dev_info(ctx->dev->dev, "vp9 save buffers: a %u nonzero words (first %d), b %u (first %d)\n",
			 na, fa, nb, fb);
	}
	if (vp9_debug > 0)
		dev_info(ctx->dev->dev, "vp9 counts: skip %u partition %u intra_inter %u y_mode %u mv_joint %u coef %u | other buf skip %u\n",
			 vp9_sum(&c->skip[0][0], 12), vp9_sum(&c->partition[0][0], 64),
			 vp9_sum(&c->intra_inter[0][0], 8), vp9_sum(&c->y_mode[0][0], 48),
			 vp9_sum(c->joint, 4), vp9_sum(&c->coef_probs[0][0][0].band_0[0][0], 12),
			 vp9_sum(&((struct vdec_vp9_counts *)(st->counts_buf ? st->save_a : st->save_b).va)->skip[0][0], 12));
out:
	kfree(h);
	kfree(m);
}

/* after the core is done: let the LAT dump its context and counts */
void vdec_vp9_done(struct vdec_ctx *ctx, bool ok)
{
	struct vdec_vp9_state *st = ctx->vp9;

	if (!st)
		return;
	memset(st->save_a.va, 0, VP9_SAVE_SIZE);
	memset(st->save_b.va, 0, VP9_SAVE_SIZE);
	if (ok && vdec_hw_vp9_save(ctx->dev, st->save_a.dma, st->save_b.dma)) {
		dev_warn_ratelimited(ctx->dev->dev, "vp9: context save timed out\n");
		ok = false;
	}
	st->counts_buf = vp9_counts_buf;
	if (ok && vp9_dump > 0) {
		vp9_dump--;
		print_hex_dump(KERN_INFO, "vp9A ", DUMP_PREFIX_OFFSET, 32, 4,
			       st->save_a.va, 2304, false);
		print_hex_dump(KERN_INFO, "vp9P ", DUMP_PREFIX_OFFSET, 32, 1,
			       &st->post, sizeof(st->post), false);
	}
	st->adapt_tx_mode = st->hw_tx_mode;
	st->adapt_refmode = st->hw_refmode;
	if (ok && st->refresh) {
		struct v4l2_vp9_frame_context *hw = kmalloc(sizeof(*hw), GFP_KERNEL);

		if (hw) {
			vdec_hw_vp9_read_probs(ctx->dev, hw);
			if (vp9_check_post && memcmp(hw, &st->post, sizeof(*hw))) {
				const u8 *x = (const u8 *)hw, *y = (const u8 *)&st->post;
				unsigned int i, n = 0, first = ~0u;

				for (i = 0; i < sizeof(*hw); i++)
					if (x[i] != y[i]) {
						n++;
						if (first == ~0u)
							first = i;
					}
				dev_info(ctx->dev->dev, "vp9: hw context differs from software in %u bytes (first at %u: %02x vs %02x)\n",
					 n, first, x[first], y[first]);
			}
			memcpy(&st->post, hw, sizeof(*hw));
			kfree(hw);
		}
		memcpy(&st->fctx[st->adapt_idx], &st->post, sizeof(st->post));
	}
	if (ok && st->adapt) {
		/* counts through the register ports into save_b */
		vdec_hw_vp9_read_counts(ctx->dev, st->save_b.va);
		st->counts_buf = 1;
		vdec_vp9_adapt(ctx);
	}
	st->adapt = false;
}
