// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: HEVC Main (VAProfileHEVCMain) -> V4L2 stateless
 * HEVC SPS / PPS / SLICE_PARAMS / DECODE_PARAMS, frame-based with Annex-B
 * start codes. The hardware parses the slice headers itself; the kernel
 * uses the first slice's parameters for its header words.
 *
 * VA marks which references belong to RefPicSetStCurrBefore / After /
 * LtCurr but not their order: the spec's order is rebuilt from the POCs
 * (before: closest first, after: closest first).
 */
#include <stdlib.h>
#include <string.h>

#include "cvd.h"

static uint64_t ref_ts(struct cvd_driver *d, VASurfaceID id)
{
	struct cvd_surface *s = id != VA_INVALID_SURFACE ? SURFACE(d, id) : NULL;

	return s && s->decoded ? s->ts : 0;
}

static void sort_rps(uint8_t *idx, unsigned int n, const struct v4l2_ctrl_hevc_decode_params *dec,
		     int dir)
{
	unsigned int i, j;

	for (i = 1; i < n; i++)
		for (j = i; j > 0; j--) {
			int a = dec->dpb[idx[j - 1]].pic_order_cnt_val, b = dec->dpb[idx[j]].pic_order_cnt_val;
			uint8_t t;

			if (dir * (a - b) <= 0)
				break;
			t = idx[j - 1];
			idx[j - 1] = idx[j];
			idx[j] = t;
		}
}

/* VA's lists and V4L2's are both raster order (ffmpeg converts from the coded order) */
static void hevc_scaling(const VAIQMatrixBufferHEVC *iq, struct v4l2_ctrl_hevc_scaling_matrix *m)
{
	memcpy(m->scaling_list_4x4, iq->ScalingList4x4, sizeof(m->scaling_list_4x4));
	memcpy(m->scaling_list_8x8, iq->ScalingList8x8, sizeof(m->scaling_list_8x8));
	memcpy(m->scaling_list_16x16, iq->ScalingList16x16, sizeof(m->scaling_list_16x16));
	memcpy(m->scaling_list_32x32, iq->ScalingList32x32, sizeof(m->scaling_list_32x32));
	memcpy(m->scaling_list_dc_coef_16x16, iq->ScalingListDC16x16, sizeof(m->scaling_list_dc_coef_16x16));
	memcpy(m->scaling_list_dc_coef_32x32, iq->ScalingListDC32x32, sizeof(m->scaling_list_dc_coef_32x32));
}

VAStatus cvd_hevc_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	const VAPictureParameterBufferHEVC *pp = &c->pic.hevc;
	const VASliceParameterBufferHEVC *sl = &c->hevc_slice;
	struct v4l2_ctrl_hevc_sps sps = { 0 };
	struct v4l2_ctrl_hevc_pps pps = { 0 };
	struct v4l2_ctrl_hevc_slice_params sp = { 0 };
	struct v4l2_ctrl_hevc_decode_params dec = { 0 };
	struct v4l2_ctrl_hevc_scaling_matrix sm = { 0 };
	struct v4l2_ext_control ctrls[5];
	unsigned int i, n = 0, nal = 0, nctrls = 4;
	uint8_t va2dpb[15];

	if (!c->have_slice)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	/* the first slice NAL's header, after its start code */
	if (c->bs_len > 4 && !c->bs[0] && !c->bs[1] && c->bs[2] == 1)
		nal = (c->bs[3] >> 1) & 0x3f;

	sps.pic_width_in_luma_samples = pp->pic_width_in_luma_samples;
	sps.pic_height_in_luma_samples = pp->pic_height_in_luma_samples;
	sps.bit_depth_luma_minus8 = pp->bit_depth_luma_minus8;
	sps.bit_depth_chroma_minus8 = pp->bit_depth_chroma_minus8;
	sps.log2_max_pic_order_cnt_lsb_minus4 = pp->log2_max_pic_order_cnt_lsb_minus4;
	sps.sps_max_dec_pic_buffering_minus1 = pp->sps_max_dec_pic_buffering_minus1;
	sps.log2_min_luma_coding_block_size_minus3 = pp->log2_min_luma_coding_block_size_minus3;
	sps.log2_diff_max_min_luma_coding_block_size = pp->log2_diff_max_min_luma_coding_block_size;
	sps.log2_min_luma_transform_block_size_minus2 = pp->log2_min_transform_block_size_minus2;
	sps.log2_diff_max_min_luma_transform_block_size = pp->log2_diff_max_min_transform_block_size;
	sps.max_transform_hierarchy_depth_inter = pp->max_transform_hierarchy_depth_inter;
	sps.max_transform_hierarchy_depth_intra = pp->max_transform_hierarchy_depth_intra;
	sps.pcm_sample_bit_depth_luma_minus1 = pp->pcm_sample_bit_depth_luma_minus1;
	sps.pcm_sample_bit_depth_chroma_minus1 = pp->pcm_sample_bit_depth_chroma_minus1;
	sps.log2_min_pcm_luma_coding_block_size_minus3 = pp->log2_min_pcm_luma_coding_block_size_minus3;
	sps.log2_diff_max_min_pcm_luma_coding_block_size = pp->log2_diff_max_min_pcm_luma_coding_block_size;
	sps.num_short_term_ref_pic_sets = pp->num_short_term_ref_pic_sets;
	sps.num_long_term_ref_pics_sps = pp->num_long_term_ref_pic_sps;
	sps.chroma_format_idc = pp->pic_fields.bits.chroma_format_idc;
#define SF(va, v4l) do { if (va) sps.flags |= V4L2_HEVC_SPS_FLAG_##v4l; } while (0)
	SF(pp->pic_fields.bits.separate_colour_plane_flag, SEPARATE_COLOUR_PLANE);
	SF(pp->pic_fields.bits.scaling_list_enabled_flag, SCALING_LIST_ENABLED);
	SF(pp->pic_fields.bits.amp_enabled_flag, AMP_ENABLED);
	SF(pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag, SAMPLE_ADAPTIVE_OFFSET);
	SF(pp->pic_fields.bits.pcm_enabled_flag, PCM_ENABLED);
	SF(pp->pic_fields.bits.pcm_loop_filter_disabled_flag, PCM_LOOP_FILTER_DISABLED);
	SF(pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag, LONG_TERM_REF_PICS_PRESENT);
	SF(pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, SPS_TEMPORAL_MVP_ENABLED);
	SF(pp->pic_fields.bits.strong_intra_smoothing_enabled_flag, STRONG_INTRA_SMOOTHING_ENABLED);
#undef SF

	pps.num_extra_slice_header_bits = pp->num_extra_slice_header_bits;
	pps.num_ref_idx_l0_default_active_minus1 = pp->num_ref_idx_l0_default_active_minus1;
	pps.num_ref_idx_l1_default_active_minus1 = pp->num_ref_idx_l1_default_active_minus1;
	pps.init_qp_minus26 = pp->init_qp_minus26;
	pps.diff_cu_qp_delta_depth = pp->diff_cu_qp_delta_depth;
	pps.pps_cb_qp_offset = pp->pps_cb_qp_offset;
	pps.pps_cr_qp_offset = pp->pps_cr_qp_offset;
	pps.num_tile_columns_minus1 = pp->num_tile_columns_minus1;
	pps.num_tile_rows_minus1 = pp->num_tile_rows_minus1;
	for (i = 0; i < 19; i++)
		pps.column_width_minus1[i] = pp->column_width_minus1[i];
	for (i = 0; i < 21; i++)
		pps.row_height_minus1[i] = pp->row_height_minus1[i];
	pps.pps_beta_offset_div2 = pp->pps_beta_offset_div2;
	pps.pps_tc_offset_div2 = pp->pps_tc_offset_div2;
	pps.log2_parallel_merge_level_minus2 = pp->log2_parallel_merge_level_minus2;
#define PF(va, v4l) do { if (va) pps.flags |= V4L2_HEVC_PPS_FLAG_##v4l; } while (0)
	PF(pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag, DEPENDENT_SLICE_SEGMENT_ENABLED);
	PF(pp->slice_parsing_fields.bits.output_flag_present_flag, OUTPUT_FLAG_PRESENT);
	PF(pp->pic_fields.bits.sign_data_hiding_enabled_flag, SIGN_DATA_HIDING_ENABLED);
	PF(pp->slice_parsing_fields.bits.cabac_init_present_flag, CABAC_INIT_PRESENT);
	PF(pp->pic_fields.bits.constrained_intra_pred_flag, CONSTRAINED_INTRA_PRED);
	PF(pp->pic_fields.bits.transform_skip_enabled_flag, TRANSFORM_SKIP_ENABLED);
	PF(pp->pic_fields.bits.cu_qp_delta_enabled_flag, CU_QP_DELTA_ENABLED);
	PF(pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag,
	   PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT);
	PF(pp->pic_fields.bits.weighted_pred_flag, WEIGHTED_PRED);
	PF(pp->pic_fields.bits.weighted_bipred_flag, WEIGHTED_BIPRED);
	PF(pp->pic_fields.bits.transquant_bypass_enabled_flag, TRANSQUANT_BYPASS_ENABLED);
	PF(pp->pic_fields.bits.tiles_enabled_flag, TILES_ENABLED);
	PF(pp->pic_fields.bits.entropy_coding_sync_enabled_flag, ENTROPY_CODING_SYNC_ENABLED);
	PF(pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag, LOOP_FILTER_ACROSS_TILES_ENABLED);
	PF(pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag,
	   PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED);
	PF(pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag,
	   DEBLOCKING_FILTER_OVERRIDE_ENABLED);
	PF(pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag, PPS_DISABLE_DEBLOCKING_FILTER);
	PF(pp->slice_parsing_fields.bits.lists_modification_present_flag, LISTS_MODIFICATION_PRESENT);
	PF(pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag,
	   SLICE_SEGMENT_HEADER_EXTENSION_PRESENT);
#undef PF
	/* not in VA: implied by what it controls */
	if (pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
	    pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
	    pp->pps_beta_offset_div2 || pp->pps_tc_offset_div2)
		pps.flags |= V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	sp.bit_size = sl->slice_data_size * 8;
	sp.data_byte_offset = sl->slice_data_byte_offset;
	sp.num_entry_point_offsets = sl->num_entry_point_offsets;
	sp.nal_unit_type = nal;
	sp.nuh_temporal_id_plus1 = 1;
	sp.slice_type = sl->LongSliceFlags.fields.slice_type;
	sp.colour_plane_id = sl->LongSliceFlags.fields.color_plane_id;
	sp.slice_pic_order_cnt = pp->CurrPic.pic_order_cnt;
	sp.num_ref_idx_l0_active_minus1 = sl->num_ref_idx_l0_active_minus1;
	sp.num_ref_idx_l1_active_minus1 = sl->num_ref_idx_l1_active_minus1;
	sp.collocated_ref_idx = sl->collocated_ref_idx;
	sp.five_minus_max_num_merge_cand = sl->five_minus_max_num_merge_cand;
	sp.slice_qp_delta = sl->slice_qp_delta;
	sp.slice_cb_qp_offset = sl->slice_cb_qp_offset;
	sp.slice_cr_qp_offset = sl->slice_cr_qp_offset;
	sp.slice_beta_offset_div2 = sl->slice_beta_offset_div2;
	sp.slice_tc_offset_div2 = sl->slice_tc_offset_div2;
	sp.slice_segment_addr = sl->slice_segment_address;
	sp.short_term_ref_pic_set_size = pp->st_rps_bits;
#define LF(va, v4l) do { if (sl->LongSliceFlags.fields.va) sp.flags |= V4L2_HEVC_SLICE_PARAMS_FLAG_##v4l; } while (0)
	LF(slice_sao_luma_flag, SLICE_SAO_LUMA);
	LF(slice_sao_chroma_flag, SLICE_SAO_CHROMA);
	LF(slice_temporal_mvp_enabled_flag, SLICE_TEMPORAL_MVP_ENABLED);
	LF(mvd_l1_zero_flag, MVD_L1_ZERO);
	LF(cabac_init_flag, CABAC_INIT);
	LF(collocated_from_l0_flag, COLLOCATED_FROM_L0);
	LF(slice_deblocking_filter_disabled_flag, SLICE_DEBLOCKING_FILTER_DISABLED);
	LF(slice_loop_filter_across_slices_enabled_flag, SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED);
	LF(dependent_slice_segment_flag, DEPENDENT_SLICE_SEGMENT);
#undef LF

	dec.pic_order_cnt_val = pp->CurrPic.pic_order_cnt;
	memset(va2dpb, 0xff, sizeof(va2dpb));
	for (i = 0; i < 15; i++) {
		const VAPictureHEVC *r = &pp->ReferenceFrames[i];
		struct v4l2_hevc_dpb_entry *e;
		unsigned int k = n;

		if (r->picture_id == VA_INVALID_SURFACE || (r->flags & VA_PICTURE_HEVC_INVALID))
			continue;
		va2dpb[i] = k;
		e = &dec.dpb[n++];
		e->timestamp = ref_ts(d, r->picture_id);
		e->pic_order_cnt_val = r->pic_order_cnt;
		if (r->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE)
			e->flags |= V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE;
		if (r->flags & VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE)
			dec.poc_st_curr_before[dec.num_poc_st_curr_before++] = k;
		else if (r->flags & VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)
			dec.poc_st_curr_after[dec.num_poc_st_curr_after++] = k;
		else if (r->flags & VA_PICTURE_HEVC_RPS_LT_CURR)
			dec.poc_lt_curr[dec.num_poc_lt_curr++] = k;
	}
	dec.num_active_dpb_entries = n;
	/* the final lists (VA: ReferenceFrames indices; V4L2: DPB indices) */
	for (i = 0; i < 15; i++) {
		sp.ref_idx_l0[i] = sl->RefPicList[0][i] < 15 ? va2dpb[sl->RefPicList[0][i]] : 0xff;
		sp.ref_idx_l1[i] = sl->RefPicList[1][i] < 15 ? va2dpb[sl->RefPicList[1][i]] : 0xff;
	}
	sort_rps(dec.poc_st_curr_before, dec.num_poc_st_curr_before, &dec, -1);
	sort_rps(dec.poc_st_curr_after, dec.num_poc_st_curr_after, &dec, 1);
	if (pp->slice_parsing_fields.bits.RapPicFlag)
		dec.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IRAP_PIC;
	if (pp->slice_parsing_fields.bits.IdrPicFlag)
		dec.flags |= V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC;
	cvd_dbg("hevc depth %u/%u ltiles %u merge %u col %u l0 %u\n", pp->max_transform_hierarchy_depth_inter,
		pp->max_transform_hierarchy_depth_intra, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag,
		sl->five_minus_max_num_merge_cand, sl->collocated_ref_idx, sl->num_ref_idx_l0_active_minus1);
	cvd_dbg("hevc %ux%u poc %d nal %u type %u refs %u/%u/%u\n", sps.pic_width_in_luma_samples,
		sps.pic_height_in_luma_samples, dec.pic_order_cnt_val, nal, sp.slice_type,
		dec.num_poc_st_curr_before, dec.num_poc_st_curr_after, dec.num_poc_lt_curr);

	ctrls[0] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_HEVC_SPS, .size = sizeof(sps), .ptr = &sps };
	ctrls[1] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_HEVC_PPS, .size = sizeof(pps), .ptr = &pps };
	ctrls[2] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.size = sizeof(sp), .ptr = &sp };
	ctrls[3] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
		.size = sizeof(dec), .ptr = &dec };
	if (pp->pic_fields.bits.scaling_list_enabled_flag && c->have_iq) {
		hevc_scaling(&c->hevc_iq, &sm);
		ctrls[nctrls++] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
			.size = sizeof(sm), .ptr = &sm };
	}
	return cvd_submit(d, c, s, ctrls, nctrls);
}
