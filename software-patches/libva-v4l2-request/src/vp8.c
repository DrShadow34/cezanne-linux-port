// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: VP8 (VAProfileVP8Version0_3) -> V4L2 stateless
 * VP8 frame control.
 *
 * The kernel driver parses the frame header on the hardware from the
 * frame's first byte, but VA clients hand over the data from the first
 * partition on (FFmpeg cuts the 3 / 10 uncompressed header bytes off,
 * GStreamer passes them and points slice_data_offset past them), so the
 * header is rebuilt from the picture parameters.
 */
#include <string.h>

#include "cvd.h"

static int clip7(int q)
{
	return q < 0 ? 0 : q > 127 ? 127 : q;
}

/*
 * VA gives the quantizer indices per segment and component (base, then
 * base + each delta, clipped to 0..127); V4L2 wants the base per segment
 * and the five deltas. The deltas come from the segment furthest from the
 * clipping edges, where none of them can have been clipped.
 */
static void vp8_quant(const VAIQMatrixBufferVP8 *iq, bool seg, struct v4l2_ctrl_vp8_frame *fr)
{
	unsigned int i, r = 0;
	int best = -1;

	for (i = 0; i < (seg ? 4u : 1u); i++) {
		int b = iq->quantization_index[i][0];
		int room = b < 127 - b ? b : 127 - b;

		if (room > best) {
			best = room;
			r = i;
		}
	}
	fr->quant.y_ac_qi = clip7(iq->quantization_index[0][0]);
	fr->quant.y_dc_delta = iq->quantization_index[r][1] - iq->quantization_index[r][0];
	fr->quant.y2_dc_delta = iq->quantization_index[r][2] - iq->quantization_index[r][0];
	fr->quant.y2_ac_delta = iq->quantization_index[r][3] - iq->quantization_index[r][0];
	fr->quant.uv_dc_delta = iq->quantization_index[r][4] - iq->quantization_index[r][0];
	fr->quant.uv_ac_delta = iq->quantization_index[r][5] - iq->quantization_index[r][0];
	if (seg)
		for (i = 0; i < 4; i++)
			fr->segment.quant_update[i] = clip7(iq->quantization_index[i][0]);
}

static uint64_t ref_ts(struct cvd_driver *d, VASurfaceID id)
{
	struct cvd_surface *rs = id != VA_INVALID_SURFACE ? SURFACE(d, id) : NULL;

	return rs && rs->decoded ? rs->ts : 0;
}

VAStatus cvd_vp8_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	const VAPictureParameterBufferVP8 *pp = &c->pic.vp8;
	const VASliceParameterBufferVP8 *sp = &c->vp8_slice;
	struct v4l2_ctrl_vp8_frame fr;
	struct v4l2_ext_control ctrl;
	bool key = !pp->pic_fields.bits.key_frame;	/* VA: 0 = keyframe, as the tag */
	bool seg = pp->pic_fields.bits.segmentation_enabled;
	unsigned int hdr = key ? 10 : 3, first, i;
	uint32_t tag;
	uint8_t h[10];

	if (!c->have_slice || !c->have_probs || !c->have_iq || !sp->num_of_partitions ||
	    sp->num_of_partitions > 9) {
		cvd_log("VP8 picture without slice / probability / quantizer data\n");
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
	first = sp->partition_size[0] + ((sp->macroblock_offset + 7) >> 3);

	/* frame tag, start code and size (scaling bits 0: not decoded) */
	tag = !key | (pp->pic_fields.bits.version & 7) << 1 | 1 << 4 | first << 5;
	h[0] = tag;
	h[1] = tag >> 8;
	h[2] = tag >> 16;
	h[3] = 0x9d;
	h[4] = 0x01;
	h[5] = 0x2a;
	h[6] = pp->frame_width;
	h[7] = (pp->frame_width >> 8) & 0x3f;
	h[8] = pp->frame_height;
	h[9] = (pp->frame_height >> 8) & 0x3f;
	if (c->bs_len + hdr > c->bs_cap)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	memmove(c->bs + hdr, c->bs, c->bs_len);
	memcpy(c->bs, h, hdr);
	c->bs_len += hdr;

	memset(&fr, 0, sizeof(fr));
	if (seg) {
		fr.segment.flags = V4L2_VP8_SEGMENT_FLAG_ENABLED;	/* absolute values */
		if (pp->pic_fields.bits.update_mb_segmentation_map)
			fr.segment.flags |= V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP;
		if (pp->pic_fields.bits.update_segment_feature_data)
			fr.segment.flags |= V4L2_VP8_SEGMENT_FLAG_UPDATE_FEATURE_DATA;
		for (i = 0; i < 4; i++)
			fr.segment.lf_update[i] = pp->loop_filter_level[i];
		memcpy(fr.segment.segment_probs, pp->mb_segment_tree_probs, 3);
	}
	vp8_quant(&c->vp8_iq, seg, &fr);
	/* the frame level only decides whether the filter runs at all; the
	 * levels per segment are above */
	fr.lf.level = pp->pic_fields.bits.loop_filter_disable ? 0 :
		      pp->loop_filter_level[0] ? pp->loop_filter_level[0] : 1;
	if (!seg)
		fr.lf.level = pp->pic_fields.bits.loop_filter_disable ? 0 : pp->loop_filter_level[0];
	fr.lf.sharpness_level = pp->pic_fields.bits.sharpness_level;
	memcpy(fr.lf.ref_frm_delta, pp->loop_filter_deltas_ref_frame, 4);
	memcpy(fr.lf.mb_mode_delta, pp->loop_filter_deltas_mode, 4);
	if (pp->pic_fields.bits.loop_filter_adj_enable)
		fr.lf.flags |= V4L2_VP8_LF_ADJ_ENABLE;
	if (pp->pic_fields.bits.mode_ref_lf_delta_update)
		fr.lf.flags |= V4L2_VP8_LF_DELTA_UPDATE;
	if (pp->pic_fields.bits.filter_type)
		fr.lf.flags |= V4L2_VP8_LF_FILTER_TYPE_SIMPLE;

	memcpy(fr.entropy.coeff_probs, c->vp8_probs.dct_coeff_probs, sizeof(fr.entropy.coeff_probs));
	memcpy(fr.entropy.y_mode_probs, pp->y_mode_probs, 4);
	memcpy(fr.entropy.uv_mode_probs, pp->uv_mode_probs, 3);
	memcpy(fr.entropy.mv_probs, pp->mv_probs, sizeof(fr.entropy.mv_probs));
	fr.coder_state.range = pp->bool_coder_ctx.range;
	fr.coder_state.value = pp->bool_coder_ctx.value;
	fr.coder_state.bit_count = pp->bool_coder_ctx.count;

	fr.width = pp->frame_width;
	fr.height = pp->frame_height;
	fr.version = pp->pic_fields.bits.version;
	fr.prob_skip_false = pp->prob_skip_false;
	fr.prob_intra = pp->prob_intra;
	fr.prob_last = pp->prob_last;
	fr.prob_gf = pp->prob_gf;
	fr.num_dct_parts = sp->num_of_partitions - 1;
	fr.first_part_size = first;
	fr.first_part_header_bits = sp->macroblock_offset;
	for (i = 0; i < fr.num_dct_parts; i++)
		fr.dct_part_sizes[i] = sp->partition_size[i + 1];
	if (!key) {
		fr.last_frame_ts = ref_ts(d, pp->last_ref_frame);
		fr.golden_frame_ts = ref_ts(d, pp->golden_ref_frame);
		fr.alt_frame_ts = ref_ts(d, pp->alt_ref_frame);
	}
	fr.flags = V4L2_VP8_FRAME_FLAG_SHOW_FRAME;
	if (key)
		fr.flags |= V4L2_VP8_FRAME_FLAG_KEY_FRAME;
	if (pp->pic_fields.bits.mb_no_coeff_skip)
		fr.flags |= V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF;
	if (pp->pic_fields.bits.sign_bias_golden)
		fr.flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN;
	if (pp->pic_fields.bits.sign_bias_alternate)
		fr.flags |= V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT;
	cvd_dbg("vp8 %ux%u key %d q %u lf %u parts %u first %u mb %u\n", fr.width, fr.height,
		key, fr.quant.y_ac_qi, fr.lf.level, fr.num_dct_parts, first, sp->macroblock_offset);
	if (cvd_debug) {
		uint32_t sum = 0;

		for (i = 0; i < c->bs_len; i++)
			sum = sum * 31 + c->bs[i];
		cvd_log("vp8 frame %zu bytes sum %08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			c->bs_len, sum, c->bs[0], c->bs[1], c->bs[2], c->bs[3], c->bs[4], c->bs[5],
			c->bs[6], c->bs[7], c->bs[8], c->bs[9], c->bs[10], c->bs[11]);
	}

	ctrl = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_VP8_FRAME,
		.size = sizeof(fr), .ptr = &fr };
	return cvd_submit(d, c, s, &ctrl, 1);
}
