// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: HEVC stateless decoding (V4L2_PIX_FMT_HEVC_SLICE,
 * frame-based, Annex-B start codes). The SLICE_PARAMS control is a
 * dynamic array; only the first slice is used: the LAT parses the slice
 * headers itself.
 */
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec.h"
#include "vdec_hevc.h"

static int hevc_debug;
module_param(hevc_debug, int, 0644);
MODULE_PARM_DESC(hevc_debug, "log the next N HEVC frames' parameters");

static const struct v4l2_ctrl_config vdec_hevc_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_HEVC_SPS },
	{ .id = V4L2_CID_STATELESS_HEVC_PPS },
	{
		.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
		.dims = { 600 },
	},
	{ .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX },
	{ .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS },
	{
		.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.min = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.max = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.def = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_START_CODE,
		.min = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
		.min = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.max = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE,
		.def = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN,
		.menu_skip_mask = BIT(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10),
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
		.min = V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
		.max = V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1,
		.def = V4L2_MPEG_VIDEO_HEVC_LEVEL_4,
	},
};

int vdec_hevc_ctrls_init(struct v4l2_ctrl_handler *hdl)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdec_hevc_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &vdec_hevc_ctrls[i], NULL);
	return hdl->error;
}

static const void *vdec_ctrl(struct vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	return c ? c->p_cur.p : NULL;
}

/* tile boundaries: bit k of the column / row bitmap = a tile ends at CTB k */
static void hevc_tile_bits(struct vdec_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	unsigned int cols = 1, rows = 1, i, pos;

	memset(run->col_bits, 0, sizeof(run->col_bits));
	memset(run->row_bits, 0, sizeof(run->row_bits));
	if (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) {
		cols = pps->num_tile_columns_minus1 + 1;
		rows = pps->num_tile_rows_minus1 + 1;
	}
	for (i = 0, pos = 0; i < cols; i++) {
		if (i + 1 == cols)
			pos = run->ctb_w;
		else if (pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)
			pos = (i + 1) * run->ctb_w / cols;
		else
			pos += pps->column_width_minus1[i] + 1;
		if (pos - 1 < 512)
			run->col_bits[(pos - 1) / 32] |= BIT((pos - 1) % 32);
	}
	for (i = 0, pos = 0; i < rows; i++) {
		if (i + 1 == rows)
			pos = run->ctb_h;
		else if (pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)
			pos = (i + 1) * run->ctb_h / rows;
		else
			pos += pps->row_height_minus1[i] + 1;
		if (pos - 1 < 512)
			run->row_bits[(pos - 1) / 32] |= BIT((pos - 1) % 32);
	}
}

/* an RBSP bit reader over a copy of the slice header (emulation prevention removed) */
struct hevc_br {
	u8 buf[192];
	unsigned int len, pos;
	bool err;
};

static u32 hevc_bits(struct hevc_br *b, unsigned int n)
{
	u32 v = 0;

	while (n--) {
		if (b->pos >= b->len * 8) {
			b->err = true;
			return 0;
		}
		v = v << 1 | (b->buf[b->pos / 8] >> (7 - b->pos % 8) & 1);
		b->pos++;
	}
	return v;
}

static u32 hevc_ue(struct hevc_br *b)
{
	unsigned int z = 0;

	while (!hevc_bits(b, 1) && !b->err && z < 31)
		z++;
	return ((1u << z) - 1) + hevc_bits(b, z);
}

/*
 * hevc_vld+0c8 wants counts the hardware needs to re-parse the slice header:
 * the size of the RPS an inter-predicted slice RPS refers to (the number of
 * its used_by_curr_pic_flags minus one), the short-term RPS size (pictures
 * that are gone included) and the long-term counts. Without the SPS's RPS
 * sets (short_term_ref_pic_set_sps_flag) the DPB's short-term count stands
 * in for the size.
 */
static void hevc_slice_rps(struct vdec_hevc_run *run, const u8 *va)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	unsigned int i, zeros = 0, nal, st, end, n_st = 0, sets = sps->num_short_term_ref_pic_sets;
	struct hevc_br *b;

	for (i = 0; i < dec->num_active_dpb_entries && i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++)
		if (!(dec->dpb[i].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE))
			n_st++;
	run->rps_st = n_st;
	run->rps_pred = run->lt_pics = run->lt_sps = 0;
	run->lt_n = 0;
	run->lt_known = false;
	if (!va || run->first_nal + 5 > run->bs_len)
		return;
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return;
	for (i = run->first_nal + 3; i < run->bs_len && b->len < sizeof(b->buf); i++) {
		if (zeros >= 2 && va[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = va[i] ? 0 : zeros + 1;
		b->buf[b->len++] = va[i];
	}
	nal = hevc_bits(b, 16) >> 9 & 0x3f;
	hevc_bits(b, 1);				/* first_slice_segment_in_pic_flag */
	if (nal >= 16 && nal <= 23)
		hevc_bits(b, 1);			/* no_output_of_prior_pics_flag */
	hevc_ue(b);					/* slice_pic_parameter_set_id */
	hevc_bits(b, pps->num_extra_slice_header_bits);
	hevc_ue(b);					/* slice_type */
	if (pps->flags & V4L2_HEVC_PPS_FLAG_OUTPUT_FLAG_PRESENT)
		hevc_bits(b, 1);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		hevc_bits(b, 2);
	if (nal == 19 || nal == 20) {			/* IDR: no RPS */
		run->rps_st = 0;
		goto out;
	}
	hevc_bits(b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
	if (!hevc_bits(b, 1)) {
		/* st_ref_pic_set(num_short_term_ref_pic_sets) in the slice header */
		st = b->pos;
		end = st + run->sh->short_term_ref_pic_set_size;
		if (sets && hevc_bits(b, 1)) {
			unsigned int j = 0, n = 0;

			hevc_ue(b);			/* delta_idx_minus1 */
			hevc_bits(b, 1);		/* delta_rps_sign */
			hevc_ue(b);			/* abs_delta_rps_minus1 */
			/* used_by_curr_pic_flag [use_delta_flag] up to the set's end */
			while (b->pos < end && !b->err) {
				bool use = hevc_bits(b, 1) || hevc_bits(b, 1);

				n += use;
				j++;
			}
			if (b->pos == end && j) {
				run->rps_pred = j - 1;
				run->rps_st = n;
			}
		} else if (!b->err) {
			u32 neg = hevc_ue(b), pos = hevc_ue(b);

			if (!b->err)
				run->rps_st = neg + pos;
		}
		b->pos = end;
	} else if (sets > 1) {
		hevc_bits(b, fls(sets - 1));		/* short_term_ref_pic_set_idx */
	}
	if (sps->flags & V4L2_HEVC_SPS_FLAG_LONG_TERM_REF_PICS_PRESENT) {
		unsigned int max_lsb = 1u << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		s32 poc = run->dec->pic_order_cnt_val;
		u32 cycle = 0;

		if (sps->num_long_term_ref_pics_sps)
			run->lt_sps = hevc_ue(b);
		run->lt_pics = hevc_ue(b);
		if (b->err) {
			run->lt_sps = run->lt_pics = 0;
			goto out;
		}
		/* the long-term pictures in their order (8.3.2); an SPS
		 * candidate's POC is not known here: order unknown then */
		run->lt_known = !run->lt_sps;
		for (i = 0; i < run->lt_sps + run->lt_pics && i < 16; i++) {
			u32 lsb = 0, delta = 0;
			bool used = false, msb;

			if (i < run->lt_sps) {
				if (sps->num_long_term_ref_pics_sps > 1)
					hevc_bits(b, fls(sps->num_long_term_ref_pics_sps - 1));
			} else {
				lsb = hevc_bits(b, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
				used = hevc_bits(b, 1);
			}
			msb = hevc_bits(b, 1);
			if (msb)
				delta = hevc_ue(b);
			cycle = i == 0 || i == run->lt_sps ? delta : delta + cycle;
			if (!used || run->lt_n >= ARRAY_SIZE(run->lt_poc))
				continue;
			run->lt_full[run->lt_n] = msb;
			run->lt_poc[run->lt_n++] = msb ? poc - (s32)(cycle * max_lsb) -
						   (poc & (max_lsb - 1)) + lsb : lsb;
		}
		if (b->err)
			run->lt_known = false;
	}
out:
	kfree(b);
}

static bool hevc_ref(struct vdec_ctx *ctx, struct vdec_hevc_run *run, unsigned int idx, bool lt,
		     struct vdec_hevc_ref *r)
{
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	const struct v4l2_hevc_dpb_entry *e;
	struct vb2_buffer *vb;
	struct vdec_dst_buf *db;
	int diff;

	if (idx >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return false;
	e = &run->dec->dpb[idx];
	vb = vb2_find_buffer(cap, e->timestamp);
	if (!vb)
		return false;
	db = container_of(to_vb2_v4l2_buffer(vb), struct vdec_dst_buf, base.vb);
	diff = run->dec->pic_order_cnt_val - e->pic_order_cnt_val;
	r->id = vb->index;
	r->poc_diff = clamp(diff, -128, 127);
	r->lt = lt;
	r->y = vb2_dma_contig_plane_dma_addr(vb, 0);
	r->c = vb2_dma_contig_plane_dma_addr(vb, 1);
	r->mv = db->mv_dma;
	return true;
}

/* PocLtCurr's DPB indices in the slice header's order, if that is known */
static bool hevc_lt_order(struct vdec_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	u32 lsb_mask = BIT(run->sps->log2_max_pic_order_cnt_lsb_minus4 + 4) - 1;
	u16 taken = 0;
	unsigned int k, j;

	if (!run->lt_known || run->lt_n != dec->num_poc_lt_curr)
		return false;
	for (k = 0; k < run->lt_n; k++) {
		for (j = 0; j < dec->num_poc_lt_curr; j++) {
			s32 poc = dec->dpb[dec->poc_lt_curr[j] % V4L2_HEVC_DPB_ENTRIES_NUM_MAX].pic_order_cnt_val;

			if (!(taken & BIT(j)) && (run->lt_full[k] ? poc == run->lt_poc[k] :
						  (poc & lsb_mask) == (u32)run->lt_poc[k]))
				break;
		}
		if (j == dec->num_poc_lt_curr)
			return false;
		taken |= BIT(j);
		run->lt_order[k] = dec->poc_lt_curr[j];
	}
	return true;
}

/*
 * Without the slice header's order (long-term pictures from the SPS's
 * candidates): read it off the final lists, which follow RefPicListTemp
 * while the PPS allows no list modification. Pictures in neither list keep
 * the DPB's order.
 */
static bool hevc_lt_from_lists(struct vdec_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	const struct v4l2_ctrl_hevc_slice_params *sh = run->sh;
	unsigned int nst[2] = { dec->num_poc_st_curr_before + dec->num_poc_st_curr_after,
				dec->num_poc_st_curr_before + dec->num_poc_st_curr_after };
	unsigned int act[2] = { sh->num_ref_idx_l0_active_minus1 + 1,
				sh->slice_type == V4L2_HEVC_SLICE_TYPE_B ?
				sh->num_ref_idx_l1_active_minus1 + 1 : 0 };
	const u8 *fin[2] = { sh->ref_idx_l0, sh->ref_idx_l1 };
	unsigned int l, p, j, n = 0;
	u16 taken = 0;

	if (dec->num_poc_lt_curr < 2 || run->pps->flags & V4L2_HEVC_PPS_FLAG_LISTS_MODIFICATION_PRESENT ||
	    sh->slice_type == V4L2_HEVC_SLICE_TYPE_I)
		return false;
	for (l = 0; l < 2; l++)
		for (p = nst[l]; p < act[l] && p < nst[l] + dec->num_poc_lt_curr; p++)
			for (j = 0; j < dec->num_poc_lt_curr; j++)
				if (!(taken & BIT(j)) && dec->poc_lt_curr[j] == fin[l][p]) {
					taken |= BIT(j);
					run->lt_order[n++] = dec->poc_lt_curr[j];
					break;
				}
	for (j = 0; j < dec->num_poc_lt_curr; j++)
		if (!(taken & BIT(j)))
			run->lt_order[n++] = dec->poc_lt_curr[j];
	return true;
}

static int hevc_setup(struct vdec_ctx *ctx, struct vdec_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	struct vdec_hevc_ref cur = { 0 };
	const u8 *lists[2][3] = {
		{ dec->poc_st_curr_before, dec->poc_st_curr_after, dec->poc_lt_curr },
		{ dec->poc_st_curr_after, dec->poc_st_curr_before, dec->poc_lt_curr },
	};
	unsigned int counts[2][3] = {
		{ dec->num_poc_st_curr_before, dec->num_poc_st_curr_after, dec->num_poc_lt_curr },
		{ dec->num_poc_st_curr_after, dec->num_poc_st_curr_before, dec->num_poc_lt_curr },
	};
	unsigned int l, g, i, n, ctbs;
	u8 *va;

	run->log2_ctb = sps->log2_min_luma_coding_block_size_minus3 + 3 +
			sps->log2_diff_max_min_luma_coding_block_size;
	run->ctb_w = DIV_ROUND_UP(sps->pic_width_in_luma_samples, BIT(run->log2_ctb));
	run->ctb_h = DIV_ROUND_UP(sps->pic_height_in_luma_samples, BIT(run->log2_ctb));
	ctbs = run->ctb_w * run->ctb_h;
	run->addr_bits = ctbs > 1 ? fls(ctbs - 1) : 0;
	hevc_tile_bits(run);

	/* the first slice NAL: the buffer's first start code */
	run->first_nal = 0;
	va = vb2_plane_vaddr(&run->src->vb2_buf, 0);
	if (va) {
		for (i = 0; i + 3 <= run->bs_len; i++)
			if (!va[i] && !va[i + 1] && va[i + 2] == 1)
				break;
		run->first_nal = i + 3 <= run->bs_len ? i : 0;
	}
	hevc_slice_rps(run, va);
	if (hevc_lt_order(run) || hevc_lt_from_lists(run))
		lists[0][2] = lists[1][2] = run->lt_order;
	/* every long-term picture of the DPB, by id (TMVP must not scale to them) */
	run->n_lt_ids = 0;
	for (i = 0; i < dec->num_active_dpb_entries && i < V4L2_HEVC_DPB_ENTRIES_NUM_MAX; i++) {
		struct vb2_buffer *vb;

		if (!(dec->dpb[i].flags & V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE))
			continue;
		vb = vb2_find_buffer(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx), dec->dpb[i].timestamp);
		if (vb && run->n_lt_ids < ARRAY_SIZE(run->lt_ids))
			run->lt_ids[run->n_lt_ids++] = vb->index;
	}

	cur.id = run->cur_id;
	cur.y = run->y_dma;
	cur.c = run->c_dma;
	cur.mv = run->mv_dma;
	run->n_curr = dec->num_poc_st_curr_before + dec->num_poc_st_curr_after + dec->num_poc_lt_curr;
	if (run->n_curr > 16)
		return -EINVAL;
	for (l = 0; l < 2; l++) {
		n = 0;
		for (g = 0; g < 3; g++)
			for (i = 0; i < counts[l][g]; i++) {
				struct vdec_hevc_ref *r = &run->temp[l][n++];

				if (!hevc_ref(ctx, run, lists[l][g][i], g == 2, r)) {
					dev_info_ratelimited(ctx->dev->dev, "hevc: reference %u missing\n",
							     lists[l][g][i]);
					*r = cur;
					r->lt = g == 2;
				}
			}
	}
	if (run->n_curr) {
		run->ec_y = run->temp[0][0].y;
		run->ec_c = run->temp[0][0].c;
		run->ec_mv = run->temp[0][0].mv;
	} else {
		run->ec_y = run->y_dma;
		run->ec_c = run->c_dma;
		run->ec_mv = run->mv_dma;
	}
	return 0;
}

int vdec_hevc_run(struct vdec_ctx *ctx)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_hevc_run *run;
	struct media_request *req;
	struct vdec_dst_buf *db;
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

	run->sps = vdec_ctrl(ctx, V4L2_CID_STATELESS_HEVC_SPS);
	run->pps = vdec_ctrl(ctx, V4L2_CID_STATELESS_HEVC_PPS);
	run->sh = vdec_ctrl(ctx, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	run->dec = vdec_ctrl(ctx, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	run->sm = vdec_ctrl(ctx, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	if (ret || !run->sps || !run->pps || !run->sh || !run->dec) {
		ret = ret ?: -EINVAL;
		goto out;
	}
	if (run->sps->bit_depth_luma_minus8 != run->sps->bit_depth_chroma_minus8 ||
	    (run->sps->bit_depth_luma_minus8 && run->sps->bit_depth_luma_minus8 != 2) ||
	    run->sps->chroma_format_idc != 1) {
		dev_info_ratelimited(d->dev, "hevc: only 4:2:0, 8 or 10 bit (Main, Main10)\n");
		ret = -EINVAL;
		goto out;
	}
	if (!!run->sps->bit_depth_luma_minus8 != ctx->cap_10bit) {
		dev_info_ratelimited(d->dev, "hevc: %u-bit stream, capture format %s\n",
				     run->sps->bit_depth_luma_minus8 + 8, ctx->cap_10bit ? "MT2110T" : "MM21");
		ret = -EINVAL;
		goto out;
	}
	run->bs_dma = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->bs_len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->bs_size = vb2_plane_size(&run->src->vb2_buf, 0);
	run->y_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->c_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	db = container_of(run->dst, struct vdec_dst_buf, base.vb);
	run->mv_dma = db->mv_dma;
	run->cur_id = run->dst->vb2_buf.index;
	ret = hevc_setup(ctx, run);
	if (ret)
		goto out;
	run->ube = &ctx->ube[ctx->slot];
	run->err = &ctx->err[ctx->slot];
	ctx->slot = (ctx->slot + 1) % VDEC_WORK_SLOTS;
	if (hevc_debug > 0) {
		dev_info(d->dev, "hevc id %u poc %d type %u nal %u qp %d refs %u (%u/%u/%u) first %u ctb %u %ux%u sps %llx pps %llx sh %llx src %u len %u/%u\n",
			 run->cur_id, run->dec->pic_order_cnt_val, run->sh->slice_type,
			 run->sh->nal_unit_type, run->sh->slice_qp_delta, run->n_curr,
			 run->dec->num_poc_st_curr_before, run->dec->num_poc_st_curr_after,
			 run->dec->num_poc_lt_curr, run->first_nal, run->log2_ctb, run->ctb_w, run->ctb_h,
			 (u64)run->sps->flags, (u64)run->pps->flags, (u64)run->sh->flags,
			 run->src->vb2_buf.index, run->bs_len, run->bs_size);
		hevc_debug--;
	}
	vdec_hw_hevc_lat(ctx, run);
	vdec_hw_hevc_core(ctx, run);
	spin_lock_irqsave(&d->irq_lock, flags);
	d->pending = BIT(VDEC_CORE);
	d->irq_status[VDEC_CORE] = d->irq_status[VDEC_LAT] = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	vdec_hw_hevc_start(d);
out:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->ctrl_hdl);
	kfree(run);
	return ret;
}
