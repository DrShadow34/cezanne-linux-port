// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: H.264 controls and per-picture job setup (frame-based,
 * Annex-B). Parses the slice header, builds the reference lists and the
 * header words of the run description; vdec_h264_hw.c turns it into
 * register writes.
 */
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "vdec.h"

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "log the slice header and lists of the next N pictures");

#ifdef VDEC_BRINGUP
/* test/vdec-bitsearch: replace a computed header word (avc_vld+088 / +090) */
static uint force_w088, force_w090;
module_param(force_w088, uint, 0644);
module_param(force_w090, uint, 0644);
#endif

static const struct v4l2_ctrl_config vdec_h264_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_H264_SPS },
	{ .id = V4L2_CID_STATELESS_H264_PPS },
	{ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX },
	{ .id = V4L2_CID_STATELESS_H264_SLICE_PARAMS },
	{ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS },
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		/* the LAT finds slice NALs by their start codes */
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
		.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.menu_skip_mask = BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
		.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.max = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
		.def = V4L2_MPEG_VIDEO_H264_LEVEL_4_1,
	},
};

/* per file handle: requests bind to the handler of the file they come in on */
int vdec_h264_ctrls_init(struct v4l2_ctrl_handler *hdl)
{
	unsigned int i;
	int ret;

	v4l2_ctrl_handler_init(hdl, ARRAY_SIZE(vdec_h264_ctrls) + 3);
	for (i = 0; i < ARRAY_SIZE(vdec_h264_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &vdec_h264_ctrls[i], NULL);
	ret = hdl->error;
	if (ret)
		v4l2_ctrl_handler_free(hdl);
	return ret;
}

/*
 * Per-picture co-located MV buffer. MIUI's sat 512 KiB apart at 1080p
 * (8160 MBs, 64 bytes per MB); 128 bytes per MB leaves room.
 */
size_t vdec_h264_mv_size(u32 mb_w, u32 mb_h)
{
	return PAGE_ALIGN((size_t)mb_w * mb_h * 128);
}

static void vdec_free_work(struct vdec_ctx *ctx)
{
	struct device *dev = ctx->dev->dev;
	unsigned int i;

	for (i = 0; i < VDEC_WORK_SLOTS; i++) {
		struct vdec_work_buf *b[] = { &ctx->ube[i], &ctx->err[i] };
		unsigned int k;

		for (k = 0; k < 2; k++) {
			if (b[k]->va)
				dma_free_coherent(dev, b[k]->size, b[k]->va, b[k]->dma);
			b[k]->va = NULL;
		}
	}
}

int vdec_h264_start(struct vdec_ctx *ctx)
{
	struct device *dev = ctx->dev->dev;
	unsigned int i;

	for (i = 0; i < VDEC_WORK_SLOTS; i++) {
		ctx->ube[i].size = vdec_hw_ube_size(ctx->mb_w, ctx->mb_h);
		ctx->ube[i].va = dma_alloc_coherent(dev, ctx->ube[i].size,
						    &ctx->ube[i].dma, GFP_KERNEL);
		ctx->err[i].size = vdec_hw_err_size(ctx->mb_w, ctx->mb_h);
		ctx->err[i].va = dma_alloc_coherent(dev, ctx->err[i].size,
						    &ctx->err[i].dma, GFP_KERNEL);
		if (!ctx->ube[i].va || !ctx->err[i].va) {
			vdec_free_work(ctx);
			return -ENOMEM;
		}
	}
	ctx->slot = 0;
	return 0;
}

void vdec_h264_stop(struct vdec_ctx *ctx)
{
	vdec_free_work(ctx);
}

static const void *vdec_ctrl(struct vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	return c ? c->p_cur.p : NULL;
}

/* --- slice header --- */

struct brd {
	const u8 *b;
	u32 len;		/* bits */
	u32 pos;
	bool err;
};

static u32 br_get(struct brd *r, unsigned int n)
{
	u32 v = 0;

	while (n--) {
		if (r->pos >= r->len) {
			r->err = true;
			return 0;
		}
		v = v << 1 | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
		r->pos++;
	}
	return v;
}

static u32 br_ue(struct brd *r)
{
	unsigned int zeros = 0;
	u32 v;

	while (!r->err && !br_get(r, 1)) {
		if (++zeros > 31) {
			r->err = true;
			return 0;
		}
	}
	v = br_get(r, zeros);
	return r->err ? 0 : (1u << zeros) - 1 + v;
}

static s32 br_se(struct brd *r)
{
	u32 k = br_ue(r);

	if (r->err)
		return 0;
	return (k & 1) ? (s32)((k + 1) / 2) : -(s32)(k / 2);
}

/* first slice NAL (types 1..5) of the AU: its payload after the NAL header */
static const u8 *vdec_h264_find_slice(const u8 *cpu, u32 len, u32 *n,
				      u8 *nal_type, u8 *nal_ref)
{
	u32 i;

	for (i = 0; i + 3 < len; i++) {
		if (cpu[i] || cpu[i + 1] || cpu[i + 2] != 1)
			continue;
		*nal_type = cpu[i + 3] & 0x1f;
		*nal_ref = (cpu[i + 3] >> 5) & 3;
		if (*nal_type >= 1 && *nal_type <= 5) {
			*n = len - i - 4;
			return cpu + i + 4;
		}
		i += 2;		/* no start code begins inside this one */
	}
	return NULL;
}

/* strip emulation prevention bytes; returns the number of bytes written */
static u32 vdec_h264_rbsp(const u8 *bs, u32 n, u8 *out, u32 max)
{
	u32 j, k = 0;

	for (j = 0; j < n && k < max; j++) {
		if (j + 2 < n && !bs[j] && !bs[j + 1] && bs[j + 2] == 3) {
			out[k++] = 0;
			if (k < max)
				out[k++] = 0;
			j += 2;		/* past 00 00 03 */
		} else {
			out[k++] = bs[j];
		}
	}
	return k;
}

static void vdec_h264_parse_mod(struct brd *r, struct vdec_h264_shdr *sh,
				unsigned int list)
{
	unsigned int n = 0;

	sh->mod_flag[list] = br_get(r, 1);
	if (!sh->mod_flag[list])
		return;
	for (;;) {
		u32 idc = br_ue(r);

		if (r->err || idc == 3)
			break;
		if (idc > 3 || n >= VDEC_H264_LIST_LEN) {	/* MVC idc 4 / 5 */
			r->err = true;
			break;
		}
		sh->mod[list][n].idc = idc;
		sh->mod[list][n].val = br_ue(r);
		n++;
	}
	sh->nmod[list] = n;
}

/*
 * Slice header (7.3.3) of the first slice NAL of the access unit, up to
 * slice_qp_delta. Frame-based clients are not required to send
 * SLICE_PARAMS, and the hardware words and reference lists need the real
 * slice type, reference counts, modification commands and QP delta.
 */
static bool vdec_h264_parse_shdr(struct vdec_dev *d, struct vdec_h264_run *run,
				 const u8 *cpu, u8 *scratch)
{
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	struct vdec_h264_shdr *sh = &run->sh;
	struct brd r = {};
	const u8 *bs;
	u8 nal_type = 0, nal_ref = 0;
	u32 n = 0, first_mb, pps_id;
	bool p_like, b_slice, i_like, field = false;
	bool cabac = pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	bool chroma = sps->chroma_format_idc &&
		      !(sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE);

	memset(sh, 0, sizeof(*sh));
	bs = vdec_h264_find_slice(cpu, run->bs_len, &n, &nal_type, &nal_ref);
	if (!bs || !n) {
		dev_info_ratelimited(d->dev, "no slice NAL in the %u-byte AU\n",
				     run->bs_len);
		return false;
	}
	/* a slice header fits in the first KB */
	r.b = scratch;
	r.len = vdec_h264_rbsp(bs, n, scratch, SZ_1K) * 8;

	first_mb = br_ue(&r);
	sh->type = br_ue(&r) % 5;
	pps_id = br_ue(&r);
	if (r.err)
		return false;
	if (first_mb)
		dev_info_ratelimited(d->dev, "first slice starts at MB %u\n", first_mb);
	if (pps_id != pps->pic_parameter_set_id)
		dev_info_ratelimited(d->dev, "slice uses PPS %u, control PPS %u\n",
				     pps_id, pps->pic_parameter_set_id);
	sh->idr = nal_type == 5;
	sh->nal_ref_idc = nal_ref;
	p_like = sh->type == V4L2_H264_SLICE_TYPE_P ||
		 sh->type == V4L2_H264_SLICE_TYPE_SP;
	b_slice = sh->type == V4L2_H264_SLICE_TYPE_B;
	i_like = !p_like && !b_slice;

	if (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE)
		br_get(&r, 2);				/* colour_plane_id */
	sh->frame_num = br_get(&r, sps->log2_max_frame_num_minus4 + 4);
	if (!(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY)) {
		field = br_get(&r, 1);			/* field_pic_flag */
		if (field)
			br_get(&r, 1);			/* bottom_field_flag */
	}
	if (sh->idr)
		br_ue(&r);				/* idr_pic_id */
	if (sps->pic_order_cnt_type == 0) {
		br_get(&r, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if ((pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) &&
		    !field)
			br_se(&r);			/* delta_pic_order_cnt_bottom */
	} else if (sps->pic_order_cnt_type == 1 &&
		   !(sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO)) {
		br_se(&r);				/* delta_pic_order_cnt[0] */
		if ((pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) &&
		    !field)
			br_se(&r);			/* delta_pic_order_cnt[1] */
	}
	if (pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT)
		br_ue(&r);				/* redundant_pic_cnt */
	if (b_slice)
		sh->direct_spatial = br_get(&r, 1);
	sh->l0 = pps->num_ref_idx_l0_default_active_minus1;
	sh->l1 = pps->num_ref_idx_l1_default_active_minus1;
	if (!i_like && br_get(&r, 1)) {		/* num_ref_idx_active_override_flag */
		sh->l0 = br_ue(&r);
		if (b_slice)
			sh->l1 = br_ue(&r);
	}
	if (r.err || sh->l0 >= VDEC_H264_LIST_LEN || sh->l1 >= VDEC_H264_LIST_LEN)
		return false;
	/* ref_pic_list_modification() (NAL types 1..5: not the MVC form) */
	if (!i_like)
		vdec_h264_parse_mod(&r, sh, 0);
	if (b_slice)
		vdec_h264_parse_mod(&r, sh, 1);
	/* pred_weight_table() */
	if (((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) && p_like) ||
	    (pps->weighted_bipred_idc == 1 && b_slice)) {
		unsigned int list, i;

		br_ue(&r);				/* luma_log2_weight_denom */
		if (chroma)
			br_ue(&r);			/* chroma_log2_weight_denom */
		for (list = 0; list < (b_slice ? 2u : 1u); list++) {
			unsigned int cnt = (list ? sh->l1 : sh->l0) + 1;

			for (i = 0; i < cnt && !r.err; i++) {
				if (br_get(&r, 1)) {		/* luma_weight_lX_flag */
					br_se(&r);
					br_se(&r);
				}
				if (chroma && br_get(&r, 1)) {	/* chroma_weight_lX_flag */
					br_se(&r);
					br_se(&r);
					br_se(&r);
					br_se(&r);
				}
			}
		}
	}
	/* dec_ref_pic_marking() */
	if (nal_ref) {
		if (sh->idr) {
			br_get(&r, 2);	/* no_output_of_prior_pics, long_term_reference */
		} else if (br_get(&r, 1)) {	/* adaptive_ref_pic_marking_mode */
			unsigned int k;

			for (k = 0; k < 64 && !r.err; k++) {
				u32 mmco = br_ue(&r);

				if (!mmco)
					break;
				if (mmco == 1 || mmco == 3)
					br_ue(&r);	/* difference_of_pic_nums_minus1 */
				if (mmco == 2)
					br_ue(&r);	/* long_term_pic_num */
				if (mmco == 3 || mmco == 6)
					br_ue(&r);	/* long_term_frame_idx */
				if (mmco == 4)
					br_ue(&r);	/* max_long_term_frame_idx_plus1 */
				if (mmco == 5)
					sh->mmco5 = true;
			}
		}
	}
	if (cabac && !i_like)
		br_ue(&r);				/* cabac_init_idc */
	sh->qp_delta = br_se(&r);
	/* slice_qs_delta and the deblocking fields follow: not needed */
	sh->parsed = !r.err;
	if (!sh->parsed)
		dev_info_ratelimited(d->dev, "slice header parse ran out of bits\n");
	return sh->parsed;
}

/* --- DPB and reference lists --- */

/*
 * DPB entries -> capture buffers (by timestamp), FrameNumWrap, and the
 * table order: short-term references by PicNum (FrameNumWrap) descending,
 * then long-term ones by LongTermPicNum ascending (the P initial list order;
 * a 377-frame stream with 4-bit frame_num is bit-exact across its wraps).
 */
static void vdec_h264_map_dpb(struct vdec_ctx *ctx, struct vdec_h264_run *run,
			      const struct v4l2_h264_reflist_builder *b)
{
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	unsigned int i, j;

	run->n_dpb = 0;
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *e = &run->dec->dpb[i];
		struct vb2_buffer *vb;
		struct vdec_dst_buf *db;

		run->ref_y[i] = run->ref_c[i] = run->ref_mv[i] = 0;
		run->ref_fnw[i] = b->refs[i].frame_num;
		run->ref_lt[i] = b->refs[i].longterm;
		if (!(e->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;
		vb = vb2_find_buffer(cap, e->reference_ts);
		if (!vb) {
			dev_dbg(ctx->dev->dev, "DPB %u: no buffer for ts %llu\n",
				i, e->reference_ts);
			continue;
		}
		db = container_of(to_vb2_v4l2_buffer(vb), struct vdec_dst_buf, base.vb);
		run->ref_y[i] = vb2_dma_contig_plane_dma_addr(vb, 0);
		run->ref_c[i] = vb2_dma_contig_plane_dma_addr(vb, 1);
		run->ref_mv[i] = db->mv_dma;
		run->ref_buf[i] = vb->index;

		for (j = run->n_dpb; j > 0; j--) {
			unsigned int p = run->dpb_order[j - 1];
			bool after;		/* does i come after p? */

			if (run->ref_lt[p] != run->ref_lt[i])
				after = run->ref_lt[i];
			else if (run->ref_lt[i])
				after = run->ref_fnw[i] >= run->ref_fnw[p];
			else	/* PicNum (FrameNumWrap) descending */
				after = run->ref_fnw[i] <= run->ref_fnw[p];
			if (after)
				break;
			run->dpb_order[j] = p;
		}
		run->dpb_order[j] = i;
		run->n_dpb++;
	}
}

/* 8.2.4.3: apply the slice's modification commands to a list of n_active
 * entries (DPB indices); a command naming a missing picture is skipped */
static void vdec_h264_modify(const struct vdec_h264_run *run, u8 *list,
			     unsigned int n_active, unsigned int which)
{
	const struct vdec_h264_shdr *sh = &run->sh;
	s32 max_pic_num = 1 << (run->sps->log2_max_frame_num_minus4 + 4);
	s32 cur_pic_num = run->dec->frame_num;	/* frames: CurrPicNum = frame_num */
	s32 pred = cur_pic_num;
	unsigned int ref_idx = 0, k;
	u8 tmp[VDEC_H264_LIST_LEN + 1];

	memcpy(tmp, list, n_active);
	tmp[n_active] = VDEC_H264_NO_REF;
	for (k = 0; k < sh->nmod[which] && ref_idx < n_active; k++) {
		u8 idc = sh->mod[which][k].idc;
		u32 val = sh->mod[which][k].val;
		bool lt = idc == 2;
		unsigned int c, nidx, i;
		int found = -1;
		s32 num;

		if (!lt) {
			s32 nowrap;

			if (idc == 0) {
				nowrap = pred - (s32)(val + 1);
				if (nowrap < 0)
					nowrap += max_pic_num;
			} else {
				nowrap = pred + (s32)(val + 1);
				if (nowrap >= max_pic_num)
					nowrap -= max_pic_num;
			}
			pred = nowrap;
			num = nowrap > cur_pic_num ? nowrap - max_pic_num : nowrap;
		} else {
			num = val;	/* LongTermPicNum = LongTermFrameIdx (frames) */
		}
		for (i = 0; i < run->n_dpb; i++) {
			unsigned int e = run->dpb_order[i];

			if (run->ref_lt[e] == lt && run->ref_fnw[e] == num) {
				found = e;
				break;
			}
		}
		if (found < 0)
			continue;
		for (c = n_active; c > ref_idx; c--)
			tmp[c] = tmp[c - 1];
		tmp[ref_idx++] = found;
		/* drop the later copy of the same picture */
		nidx = ref_idx;
		for (c = ref_idx; c <= n_active; c++)
			if (tmp[c] != found)
				tmp[nidx++] = tmp[c];
	}
	memcpy(list, tmp, n_active);
}

static void vdec_h264_build_list(const struct vdec_h264_run *run, u8 *list,
				 u8 *n, const struct v4l2_h264_reference *init,
				 unsigned int n_init, unsigned int n_active,
				 unsigned int which)
{
	unsigned int i;

	if (!run->sh.mod_flag[which]) {
		/* unmodified: the whole initial list, as MIUI writes it */
		for (i = 0; i < n_init; i++)
			list[i] = init[i].index;
		*n = n_init;
		return;
	}
	for (i = 0; i < n_active; i++)
		list[i] = i < n_init ? init[i].index : VDEC_H264_NO_REF;
	vdec_h264_modify(run, list, n_active, which);
	*n = n_active;
}

static void vdec_h264_lists(struct vdec_h264_run *run,
			    const struct v4l2_h264_reflist_builder *b)
{
	struct v4l2_h264_reference init0[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference init1[V4L2_H264_REF_LIST_LEN];
	const struct vdec_h264_shdr *sh = &run->sh;
	bool b_slice = sh->type == V4L2_H264_SLICE_TYPE_B;
	bool i_like = sh->type == V4L2_H264_SLICE_TYPE_I ||
		      sh->type == V4L2_H264_SLICE_TYPE_SI;
	unsigned int n_init = b->num_valid;

	run->n_l0 = run->n_l1 = 0;
	if (i_like || !n_init)
		return;
	if (b_slice)
		v4l2_h264_build_b_ref_lists(b, init0, init1);
	else
		v4l2_h264_build_p_ref_list(b, init0);

	vdec_h264_build_list(run, run->l0, &run->n_l0, init0, n_init, sh->l0 + 1, 0);
	if (b_slice) {
		vdec_h264_build_list(run, run->l1, &run->n_l1, init1, n_init,
				     sh->l1 + 1, 1);
	} else {
		/* P: the second table (which carries the MV buffers) gets
		 * list 0 as well; repeated pictures from the modification
		 * commands (x264's weighted duplicates) are kept */
		memcpy(run->l1, run->l0, run->n_l0);
		run->n_l1 = run->n_l0;
	}
}

/* --- header words --- */

static void vdec_h264_words(struct vdec_h264_run *run)
{
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct vdec_h264_shdr *sh = &run->sh;
	bool cabac = pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	bool b_slice = sh->type == V4L2_H264_SLICE_TYPE_B;
	bool i_like = sh->type == V4L2_H264_SLICE_TYPE_I ||
		      sh->type == V4L2_H264_SLICE_TYPE_SI;
	u32 kind;

	/*
	 * +088 (SPS), as the vendor's H264_HAL_SetSPSAVLD (avc_vld word 0x22):
	 * [1:0] chroma_format_idc, [5:2] log2_max_frame_num_minus4,
	 * [7:6] pic_order_cnt_type, [11:8] log2_max_pic_order_cnt_lsb_minus4,
	 * bit 12 delta_pic_order_always_zero, [17:13] max_num_ref_frames,
	 * bit 18 frame_mbs_only, bit 19 mb_adaptive_frame_field, bit 20
	 * direct_8x8_inference, bit 21 always set. The LAT parses the slice
	 * headers itself with these: any field it misses shifts the rest.
	 */
	run->words.sps_word = BIT(21) |
		(sps->chroma_format_idc & 3) |
		((sps->log2_max_frame_num_minus4 & 0xf) << 2) |
		((sps->pic_order_cnt_type & 3) << 6) |
		((sps->log2_max_pic_order_cnt_lsb_minus4 & 0xf) << 8) |
		((sps->flags & V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO) ? BIT(12) : 0) |
		((sps->max_num_ref_frames & 0x1f) << 13) |
		((sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ? BIT(18) : 0) |
		((sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) ? BIT(19) : 0) |
		((sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) ? BIT(20) : 0);
	/*
	 * +090 (PPS), as H264_HAL_SetPPSAVLD (word 0x24): bit 0
	 * entropy_coding_mode, bit 1 bottom_field_pic_order_in_frame_present
	 * (adds delta_pic_order_cnt_bottom to every slice header: AWS
	 * Elemental streams set it, x264 never; without it the LAT decoded
	 * a few macroblock rows of garbage), bit 2 weighted_pred_flag,
	 * [4:3] weighted_bipred_idc, [10:5] pic_init_qp_minus26 (two's
	 * complement), [15:11] chroma_qp_index_offset, bit 16
	 * deblocking_filter_control_present, bit 17 constrained_intra_pred,
	 * bit 18 transform_8x8_mode, [23:19] second_chroma_qp_index_offset.
	 */
	run->words.pps_word =
		((pps->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT) ?
		 BIT(1) : 0) |
		((pps->flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) ?
		 BIT(16) : 0) |
		(cabac ? BIT(0) : 0) |
		((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) ? BIT(2) : 0) |
		((pps->weighted_bipred_idc & 3) << 3) |
		(((u32)pps->pic_init_qp_minus26 & 0x3f) << 5) |
		(((u32)pps->chroma_qp_index_offset & 0x1f) << 11) |
		((pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) ? BIT(17) : 0) |
		((pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) ? BIT(18) : 0) |
		(((u32)pps->second_chroma_qp_index_offset & 0x1f) << 19);
	/* +094: [4:0] num_ref_idx_l0_default_active_minus1 (the engines parse
	 * P slices without an override with it); scaling matrices: bit 18
	 * on, [15:10] use the uploaded 4x4 list i, [17:16] the 8x8 ones
	 * (MIUI sets all eight for custom matrices; the upload always holds
	 * the effective lists) */
	run->words.ref_word = (run->pps->num_ref_idx_l0_default_active_minus1 & 0x1f) |
			      (run->sm ? BIT(18) | GENMASK(17, 10) : 0);
	/* +098: [15:13] 7 for I / IDR, 6 for B, 5 for P; bit 19 on B;
	 * [23:20] num_ref_idx_l0_active_minus1 in effect (the PPS default
	 * on I slices; two bits were not enough for the 8-reference clip) */
	kind = b_slice ? 6 : i_like ? 7 : 5;
	run->words.slice_word = (kind << 13) | (b_slice ? BIT(19) : 0) |
				((sh->l0 & 0xf) << 20);
	/* +09c: [19:17] 7 on IDR, 2 on other reference pictures, 0 on
	 * non-reference ones; [6:0] slice_qp_delta, 7-bit two's complement
	 * (MIUI writes 0x75 for -11) */
	run->words.type_word = ((u32)sh->qp_delta & 0x7f) |
			       ((sh->idr ? 7 : sh->nal_ref_idc ? 2 : 0) << 17);
#ifdef VDEC_BRINGUP
	if (force_w088)
		run->words.sps_word = force_w088;
	if (force_w090)
		run->words.pps_word = force_w090;
#endif
}

static void vdec_h264_log(struct vdec_dev *d, const struct vdec_h264_run *run)
{
	const struct vdec_h264_shdr *sh = &run->sh;
	char dpb[64], l0[100], l1[100];
	unsigned int i, n;

	for (i = 0, n = 0; i < run->n_dpb && n < sizeof(dpb) - 8; i++)
		n += scnprintf(dpb + n, sizeof(dpb) - n, "%u%s ",
			       run->dec->dpb[run->dpb_order[i]].frame_num,
			       run->ref_lt[run->dpb_order[i]] ? "L" : "");
	for (i = 0, n = 0; i < run->n_l0 && n < sizeof(l0) - 8; i++)
		n += scnprintf(l0 + n, sizeof(l0) - n, "%u ", run->l0[i] == VDEC_H264_NO_REF ?
			       999 : run->dec->dpb[run->l0[i]].frame_num);
	for (i = 0, n = 0; i < run->n_l1 && n < sizeof(l1) - 8; i++)
		n += scnprintf(l1 + n, sizeof(l1) - n, "%u ", run->l1[i] == VDEC_H264_NO_REF ?
			       999 : run->dec->dpb[run->l1[i]].frame_num);
	dev_info(d->dev, "fn=%u poc=%d/%d type=%u%s ref=%u l0=%u l1=%u qpd=%d mod=%u/%u dpb=[%s] t1=[%s] t2=[%s] words %08x %08x %08x %08x\n",
		 sh->frame_num, run->poc_top, run->poc_bottom, sh->type,
		 sh->idr ? " IDR" : "", sh->nal_ref_idc, sh->l0, sh->l1,
		 sh->qp_delta, sh->nmod[0], sh->nmod[1], dpb, l0, l1,
		 run->words.sps_word, run->words.pps_word,
		 run->words.slice_word, run->words.type_word);
}

int vdec_h264_run(struct vdec_ctx *ctx)
{
	struct vdec_dev *d = ctx->dev;
	struct v4l2_h264_reflist_builder builder;
	struct vdec_h264_run *run;
	struct media_request *req;
	struct vdec_dst_buf *db;
	unsigned long flags;
	u8 *bsv, *scratch;
	int ret = 0;

	run = kzalloc(sizeof(*run) + SZ_1K, GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	scratch = (u8 *)(run + 1);
	run->src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	req = run->src->vb2_buf.req_obj.req;
	if (req)
		ret = v4l2_ctrl_request_setup(req, &ctx->ctrl_hdl);
	v4l2_m2m_buf_copy_metadata(run->src, run->dst);

	run->sps = vdec_ctrl(ctx, V4L2_CID_STATELESS_H264_SPS);
	run->pps = vdec_ctrl(ctx, V4L2_CID_STATELESS_H264_PPS);
	run->dec = vdec_ctrl(ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	run->sm = run->pps && run->pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT ?
		  vdec_ctrl(ctx, V4L2_CID_STATELESS_H264_SCALING_MATRIX) : NULL;
	if (ret || !run->sps || !run->pps || !run->dec) {
		ret = ret ?: -EINVAL;
		goto out;
	}
	/* frames only (no PAFF / MBAFF) */
	if (!(run->sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ||
	    run->dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) {
		dev_info_ratelimited(d->dev, "interlaced streams are not supported\n");
		ret = -EINVAL;
		goto out;
	}
	if (run->sps->pic_width_in_mbs_minus1 + 1 != ctx->mb_w ||
	    run->sps->pic_height_in_map_units_minus1 + 1 != ctx->mb_h) {
		dev_info_ratelimited(d->dev, "SPS size %ux%u MBs != format %ux%u\n",
				     run->sps->pic_width_in_mbs_minus1 + 1,
				     run->sps->pic_height_in_map_units_minus1 + 1,
				     ctx->mb_w, ctx->mb_h);
		ret = -EINVAL;
		goto out;
	}

	run->bs_dma = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->bs_len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->bs_size = vb2_plane_size(&run->src->vb2_buf, 0);
	bsv = vb2_plane_vaddr(&run->src->vb2_buf, 0);
	if (!bsv) {
		dev_err_once(d->dev, "bitstream buffer has no kernel mapping\n");
		ret = -EINVAL;
		goto out;
	}
	if (!vdec_h264_parse_shdr(d, run, bsv, scratch)) {
		ret = -EINVAL;
		goto out;
	}
	/*
	 * The CABAC LAT only stops parsing a slice at the next start code;
	 * an Annex-B AU without a trailing NAL makes it overrun into the
	 * buffer until the UBE fifo fills. Append an access unit delimiter.
	 */
	if (run->bs_len + 5 <= run->bs_size) {
		static const u8 aud[] = { 0, 0, 0, 1, 0x09 };
		unsigned long lo = run->bs_len & ~63UL;
		unsigned long hi = ALIGN(run->bs_len + sizeof(aud), 64UL);

		memcpy(bsv + run->bs_len, aud, sizeof(aud));
		dma_sync_single_for_device(d->dev, run->bs_dma + lo, hi - lo,
					   DMA_TO_DEVICE);
		run->bs_len += sizeof(aud);
	} else {
		dev_info_ratelimited(d->dev, "no room for the AU delimiter\n");
	}
	run->poc_top = run->dec->top_field_order_cnt;
	run->poc_bottom = run->dec->bottom_field_order_cnt;
	run->y_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->c_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 1);
	db = container_of(run->dst, struct vdec_dst_buf, base.vb);
	run->mv_dma = db->mv_dma;
	run->ube = &ctx->ube[ctx->slot];
	run->err = &ctx->err[ctx->slot];
	ctx->slot = (ctx->slot + 1) % VDEC_WORK_SLOTS;

	v4l2_h264_init_reflist_builder(&builder, run->dec, run->sps, run->dec->dpb);
	vdec_h264_map_dpb(ctx, run, &builder);
	vdec_h264_lists(run, &builder);
	vdec_h264_words(run);
	vdec_hw_h264_lat(ctx, run);
	vdec_hw_h264_core(ctx, run);
	if (debug > 0) {
		vdec_h264_log(d, run);
		vdec_hw_dump(d, "programmed");
		debug--;
	}

	spin_lock_irqsave(&d->irq_lock, flags);
	d->pending = BIT(VDEC_CORE);
	d->irq_status[VDEC_CORE] = d->irq_status[VDEC_LAT] = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	vdec_hw_start(d);
out:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->ctrl_hdl);
	kfree(run);
	return ret;
}
