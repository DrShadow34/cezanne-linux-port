// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: H.264 (frame-based, Annex-B start codes). The
 * kernel driver parses the slice header itself, so SLICE_PARAMS is not
 * sent; SPS / PPS / scaling matrix / decode parameters come from the VA
 * picture parameters.
 */
#include <string.h>
#include <linux/v4l2-controls.h>

#include "cvd.h"

/* Exp-Golomb ue(v) from p (n bytes), bit position *pos */
static unsigned int read_ue(const uint8_t *p, size_t n, unsigned int *pos)
{
	unsigned int zeros = 0, v = 0;

	while (*pos < n * 8 && !((p[*pos >> 3] >> (7 - (*pos & 7))) & 1) && zeros < 31) {
		zeros++;
		(*pos)++;
	}
	(*pos)++;
	while (zeros-- && *pos < n * 8) {
		v = v << 1 | ((p[*pos >> 3] >> (7 - (*pos & 7))) & 1);
		(*pos)++;
	}
	return v + ((1u << 0) - 1) + 0;
}

/* pic_parameter_set_id of the first slice (after first_mb, slice_type);
 * the first bytes of a slice header never contain emulation prevention */
static unsigned int slice_pps_id(const uint8_t *nal, size_t n)
{
	unsigned int pos = 8, k, v = 0;

	for (k = 0; k < 3; k++) {
		unsigned int start = pos, zeros;

		v = read_ue(nal, n, &pos);
		zeros = (pos - start - 1) / 2;
		v += (1u << zeros) - 1;
	}
	return v;
}

static int profile_idc(VAProfile p)
{
	switch (p) {
	case VAProfileH264ConstrainedBaseline:
		return 66;
	case VAProfileH264Main:
		return 77;
	default:
		return 100;
	}
}

VAStatus cvd_h264_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	const VAPictureParameterBufferH264 *pp = &c->pic.h264;
	const VASliceParameterBufferH264 *sl = &c->h264_slice;
	struct v4l2_ctrl_h264_sps sps = { 0 };
	struct v4l2_ctrl_h264_pps pps = { 0 };
	struct v4l2_ctrl_h264_scaling_matrix sm;
	struct v4l2_ctrl_h264_decode_params dec = { 0 };
	struct v4l2_ext_control ctrls[4];
	unsigned int i, n = 0;

	if (!c->have_slice)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	sps.profile_idc = profile_idc(c->profile);
	sps.level_idc = 51;
	sps.chroma_format_idc = pp->seq_fields.bits.chroma_format_idc;
	sps.bit_depth_luma_minus8 = pp->bit_depth_luma_minus8;
	sps.bit_depth_chroma_minus8 = pp->bit_depth_chroma_minus8;
	sps.log2_max_frame_num_minus4 = pp->seq_fields.bits.log2_max_frame_num_minus4;
	sps.pic_order_cnt_type = pp->seq_fields.bits.pic_order_cnt_type;
	sps.log2_max_pic_order_cnt_lsb_minus4 = pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4;
	sps.max_num_ref_frames = pp->num_ref_frames;
	sps.pic_width_in_mbs_minus1 = pp->picture_width_in_mbs_minus1;
	sps.pic_height_in_map_units_minus1 = pp->seq_fields.bits.frame_mbs_only_flag ?
		pp->picture_height_in_mbs_minus1 : (pp->picture_height_in_mbs_minus1 + 1) / 2 - 1;
	if (pp->seq_fields.bits.frame_mbs_only_flag)
		sps.flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	if (pp->seq_fields.bits.mb_adaptive_frame_field_flag)
		sps.flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	if (pp->seq_fields.bits.direct_8x8_inference_flag)
		sps.flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
	if (pp->seq_fields.bits.delta_pic_order_always_zero_flag)
		sps.flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO;
	if (pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag)
		sps.flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;

	pps.pic_parameter_set_id = c->bs_len > 4 ? slice_pps_id(c->bs + 3, c->bs_len - 3) : 0;
	/* VA has no PPS defaults: the first slice's active counts are them
	 * unless that slice overrides them, and the kernel re-reads the
	 * override from the slice header */
	pps.num_ref_idx_l0_default_active_minus1 = sl->num_ref_idx_l0_active_minus1;
	pps.num_ref_idx_l1_default_active_minus1 = sl->num_ref_idx_l1_active_minus1;
	pps.weighted_bipred_idc = pp->pic_fields.bits.weighted_bipred_idc;
	pps.pic_init_qp_minus26 = pp->pic_init_qp_minus26;
	pps.pic_init_qs_minus26 = pp->pic_init_qs_minus26;
	pps.chroma_qp_index_offset = pp->chroma_qp_index_offset;
	pps.second_chroma_qp_index_offset = pp->second_chroma_qp_index_offset;
	if (pp->pic_fields.bits.entropy_coding_mode_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (pp->pic_fields.bits.pic_order_present_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
	if (pp->pic_fields.bits.weighted_pred_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
	if (pp->pic_fields.bits.deblocking_filter_control_present_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	if (pp->pic_fields.bits.constrained_intra_pred_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
	if (pp->pic_fields.bits.redundant_pic_cnt_present_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
	if (pp->pic_fields.bits.transform_8x8_mode_flag)
		pps.flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;

	memset(&sm, 16, sizeof(sm));
	if (c->have_iq) {
		pps.flags |= V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT;
		memcpy(sm.scaling_list_4x4, c->iq.ScalingList4x4, sizeof(sm.scaling_list_4x4));
		memcpy(sm.scaling_list_8x8[0], c->iq.ScalingList8x8[0], 64);
		memcpy(sm.scaling_list_8x8[1], c->iq.ScalingList8x8[1], 64);
	}

	dec.frame_num = pp->frame_num;
	dec.nal_ref_idc = pp->pic_fields.bits.reference_pic_flag ? 1 : 0;
	dec.top_field_order_cnt = pp->CurrPic.TopFieldOrderCnt;
	dec.bottom_field_order_cnt = pp->CurrPic.BottomFieldOrderCnt;
	if (pp->pic_fields.bits.field_pic_flag)
		dec.flags |= V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	if (c->bs_len > 3 && (c->bs[3] & 0x1f) == 5)
		dec.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	for (i = 0; i < 16; i++) {
		const VAPictureH264 *r = &pp->ReferenceFrames[i];
		struct v4l2_h264_dpb_entry *e = &dec.dpb[i];
		struct cvd_surface *rs;

		if (r->flags & VA_PICTURE_H264_INVALID || r->picture_id == VA_INVALID_SURFACE)
			continue;
		rs = SURFACE(d, r->picture_id);
		if (!rs || !rs->decoded)
			continue;
		e->reference_ts = rs->ts;
		e->frame_num = r->frame_idx;
		e->pic_num = r->frame_idx;
		e->fields = V4L2_H264_FRAME_REF;
		e->top_field_order_cnt = r->TopFieldOrderCnt;
		e->bottom_field_order_cnt = r->BottomFieldOrderCnt;
		e->flags = V4L2_H264_DPB_ENTRY_FLAG_VALID;
		if (r->flags & (VA_PICTURE_H264_SHORT_TERM_REFERENCE |
				VA_PICTURE_H264_LONG_TERM_REFERENCE))
			e->flags |= V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
		if (r->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE)
			e->flags |= V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM;
	}

	ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_SPS,
		.size = sizeof(sps), .ptr = &sps };
	ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_PPS,
		.size = sizeof(pps), .ptr = &pps };
	ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
		.size = sizeof(sm), .ptr = &sm };
	ctrls[n++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
		.size = sizeof(dec), .ptr = &dec };
	return cvd_submit(d, c, s, ctrls, n);
}
