/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CEZANNE_VDEC_HEVC_H
#define CEZANNE_VDEC_HEVC_H

#include <linux/types.h>
#include <media/v4l2-ctrls.h>

struct vdec_work_buf;

/* an entry of RefPicListTemp0 / 1 */
struct vdec_hevc_ref {
	u8 id;				/* the picture's capture buffer */
	s8 poc_diff;			/* PicOrderCntVal - the reference's */
	bool lt;
	dma_addr_t y, c, mv;
};

struct vdec_hevc_run {
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_slice_params *sh;	/* the first slice */
	const struct v4l2_ctrl_hevc_decode_params *dec;
	const struct v4l2_ctrl_hevc_scaling_matrix *sm;
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t bs_dma;
	u32 bs_len, bs_size, first_nal;	/* first slice NAL start code */
	dma_addr_t y_dma, c_dma, mv_dma;
	u8 cur_id;
	u32 log2_ctb, ctb_w, ctb_h, addr_bits;
	u32 col_bits[16], row_bits[16];
	unsigned int n_curr;		/* NumPicTotalCurr */
	/* hevc_vld+0c8: the slice header's RPS as the hardware re-parses it */
	u8 rps_pred, rps_st, lt_pics, lt_sps;
	/* PocLtCurr in slice header order (lt_order: their DPB indices) */
	s32 lt_poc[16];
	bool lt_full[16], lt_known;
	unsigned int lt_n;
	u8 lt_order[16];
	u8 lt_ids[16];			/* avc_mv+328: the DPB's long-term pictures (id + 1) */
	unsigned int n_lt_ids;
	struct vdec_hevc_ref temp[2][16];
	dma_addr_t ec_y, ec_c, ec_mv;	/* where missing references point */
	struct vdec_work_buf *ube, *err;
};

#endif
