// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: VP9 profile 0 and 2 (10 bit). VA-API's VP9 parameters lack the
 * loop filter deltas, the quantizer index and the segment feature data, so
 * the uncompressed header (VP9 spec 6.2) is parsed here from the frame data;
 * references come from the VA picture parameters. The compressed header is
 * left to the hardware (the kernel driver reads the resulting probabilities
 * back), so V4L2_CID_STATELESS_VP9_COMPRESSED_HDR is not sent.
 */
#include <string.h>
#include <linux/v4l2-controls.h>

#include "cvd.h"

struct br {
	const uint8_t *p;
	size_t n;
	unsigned int pos;
	bool err;
};

static unsigned int f(struct br *b, unsigned int bits)
{
	unsigned int v = 0;

	while (bits--) {
		if (b->pos >= b->n * 8) {
			b->err = true;
			return 0;
		}
		v = v << 1 | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
		b->pos++;
	}
	return v;
}

static int su(struct br *b, unsigned int bits)
{
	int v = f(b, bits);

	return f(b, 1) ? -v : v;
}

void cvd_vp9_reset(struct cvd_vp9_state *st)
{
	static const int8_t ref[4] = { 1, 0, -1, -1 };

	memcpy(st->ref_deltas, ref, sizeof(ref));
	memset(st->mode_deltas, 0, sizeof(st->mode_deltas));
	memset(st->seg_enabled_mask, 0, sizeof(st->seg_enabled_mask));
	memset(st->seg_data, 0, sizeof(st->seg_data));
	memset(st->seg_tree, 255, sizeof(st->seg_tree));
	memset(st->seg_pred, 255, sizeof(st->seg_pred));
	st->seg_abs = false;
}

static bool color_config(struct br *b, unsigned int profile, struct v4l2_ctrl_vp9_frame *fr)
{
	unsigned int cs;

	fr->bit_depth = 8;
	if (profile >= 2)
		fr->bit_depth = f(b, 1) ? 12 : 10;
	cs = f(b, 3);
	if (cs != 7) {		/* not RGB */
		if (f(b, 1))
			fr->flags |= V4L2_VP9_FRAME_FLAG_COLOR_RANGE_FULL_SWING;
		if (profile == 1 || profile == 3) {
			if (f(b, 1))
				fr->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING;
			if (f(b, 1))
				fr->flags |= V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
			f(b, 1);
		} else {
			fr->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
				     V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
		}
	} else {
		fr->flags |= V4L2_VP9_FRAME_FLAG_COLOR_RANGE_FULL_SWING;
		if (profile == 1 || profile == 3)
			f(b, 1);
	}
	return !b->err;
}

static void frame_size(struct br *b, struct v4l2_ctrl_vp9_frame *fr)
{
	fr->frame_width_minus_1 = f(b, 16);
	fr->frame_height_minus_1 = f(b, 16);
}

static void render_size(struct br *b, struct v4l2_ctrl_vp9_frame *fr)
{
	if (f(b, 1)) {
		fr->render_width_minus_1 = f(b, 16);
		fr->render_height_minus_1 = f(b, 16);
	} else {
		fr->render_width_minus_1 = fr->frame_width_minus_1;
		fr->render_height_minus_1 = fr->frame_height_minus_1;
	}
}

static uint8_t read_prob(struct br *b)
{
	return f(b, 1) ? f(b, 8) : 255;
}

/* returns false on a malformed header or a frame the driver cannot take */
static bool parse_uncompressed(struct cvd_vp9_state *st, const uint8_t *p, size_t n,
			       struct v4l2_ctrl_vp9_frame *fr, uint8_t ref_idx[3])
{
	static const uint8_t literal_to_type[4] = { 1, 0, 2, 3 };
	static const unsigned int feat_bits[4] = { 8, 6, 2, 0 };
	static const bool feat_signed[4] = { true, true, false, false };
	struct br b = { .p = p, .n = n };
	unsigned int profile, i, j, sb_cols, min_log2, max_log2;
	bool key, intra_only = false, error_res;

	if (f(&b, 2) != 2)
		return false;
	profile = f(&b, 1);
	profile |= f(&b, 1) << 1;
	if (profile == 3)
		f(&b, 1);
	fr->profile = profile;
	if (f(&b, 1))		/* show_existing_frame: nothing to decode */
		return false;
	key = !f(&b, 1);
	if (f(&b, 1))
		fr->flags |= V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
	error_res = f(&b, 1);
	if (key)
		fr->flags |= V4L2_VP9_FRAME_FLAG_KEY_FRAME;
	if (error_res)
		fr->flags |= V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT;
	if (key) {
		if (f(&b, 24) != 0x498342 || !color_config(&b, profile, fr))
			return false;
		frame_size(&b, fr);
		render_size(&b, fr);
	} else {
		if (!(fr->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME))
			intra_only = f(&b, 1);
		if (!error_res) {
			unsigned int r = f(&b, 2);

			fr->reset_frame_context = r == 2 ? V4L2_VP9_RESET_FRAME_CTX_SPEC :
						  r == 3 ? V4L2_VP9_RESET_FRAME_CTX_ALL :
							   V4L2_VP9_RESET_FRAME_CTX_NONE;
		}
		if (intra_only) {
			fr->flags |= V4L2_VP9_FRAME_FLAG_INTRA_ONLY;
			if (f(&b, 24) != 0x498342)
				return false;
			if (profile > 0) {
				if (!color_config(&b, profile, fr))
					return false;
			} else {
				fr->bit_depth = 8;
				fr->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
					     V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
			}
			f(&b, 8);	/* refresh_frame_flags */
			frame_size(&b, fr);
			render_size(&b, fr);
		} else {
			bool found = false;

			fr->bit_depth = 8;
			fr->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
				     V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
			f(&b, 8);	/* refresh_frame_flags */
			for (i = 0; i < 3; i++) {
				ref_idx[i] = f(&b, 3);
				if (f(&b, 1))
					fr->ref_frame_sign_bias |= 1 << i;
			}
			for (i = 0; i < 3 && !found; i++)
				found = f(&b, 1);	/* found_ref: size of that reference */
			if (!found)
				frame_size(&b, fr);	/* else: preset from the VA params */
			render_size(&b, fr);
			if (f(&b, 1))
				fr->flags |= V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV;
			fr->interpolation_filter = f(&b, 1) ? V4L2_VP9_INTERP_FILTER_SWITCHABLE :
						   literal_to_type[f(&b, 2)];
		}
	}
	if (!error_res) {
		if (f(&b, 1))
			fr->flags |= V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
		if (f(&b, 1))
			fr->flags |= V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
	} else {
		fr->flags |= V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
	}
	fr->frame_context_idx = f(&b, 2);
	if (key || intra_only || error_res)
		cvd_vp9_reset(st);	/* setup_past_independence() */

	/* loop_filter_params() */
	fr->lf.level = f(&b, 6);
	fr->lf.sharpness = f(&b, 3);
	if (f(&b, 1)) {
		fr->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;
		if (f(&b, 1)) {
			fr->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE;
			for (i = 0; i < 4; i++)
				if (f(&b, 1))
					st->ref_deltas[i] = su(&b, 6);
			for (i = 0; i < 2; i++)
				if (f(&b, 1))
					st->mode_deltas[i] = su(&b, 6);
		}
	}
	memcpy(fr->lf.ref_deltas, st->ref_deltas, 4);
	memcpy(fr->lf.mode_deltas, st->mode_deltas, 2);

	/* quantization_params() */
	fr->quant.base_q_idx = f(&b, 8);
	fr->quant.delta_q_y_dc = f(&b, 1) ? su(&b, 4) : 0;
	fr->quant.delta_q_uv_dc = f(&b, 1) ? su(&b, 4) : 0;
	fr->quant.delta_q_uv_ac = f(&b, 1) ? su(&b, 4) : 0;

	/* segmentation_params() */
	if (f(&b, 1)) {
		fr->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ENABLED;
		if (f(&b, 1)) {
			fr->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP;
			for (i = 0; i < 7; i++)
				st->seg_tree[i] = read_prob(&b);
			if (f(&b, 1)) {
				fr->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
				for (i = 0; i < 3; i++)
					st->seg_pred[i] = read_prob(&b);
			} else {
				memset(st->seg_pred, 255, 3);
			}
		}
		if (f(&b, 1)) {
			fr->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA;
			st->seg_abs = f(&b, 1);
			for (i = 0; i < 8; i++) {
				st->seg_enabled_mask[i] = 0;
				for (j = 0; j < 4; j++) {
					int v = 0;

					if (f(&b, 1)) {
						st->seg_enabled_mask[i] |= 1 << j;
						v = f(&b, feat_bits[j]);
						if (feat_signed[j] && f(&b, 1))
							v = -v;
					}
					st->seg_data[i][j] = v;
				}
			}
		}
	}
	if (st->seg_abs)
		fr->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE;
	memcpy(fr->seg.tree_probs, st->seg_tree, 7);
	memcpy(fr->seg.pred_probs, st->seg_pred, 3);
	memcpy(fr->seg.feature_enabled, st->seg_enabled_mask, 8);
	memcpy(fr->seg.feature_data, st->seg_data, sizeof(st->seg_data));

	/* tile_info() */
	sb_cols = (fr->frame_width_minus_1 + 64) / 64;
	for (min_log2 = 0; (64u << min_log2) < sb_cols; min_log2++)
		;
	for (max_log2 = 1; (sb_cols >> max_log2) >= 4; max_log2++)
		;
	max_log2--;
	fr->tile_cols_log2 = min_log2;
	while (fr->tile_cols_log2 < max_log2 && f(&b, 1))
		fr->tile_cols_log2++;
	fr->tile_rows_log2 = f(&b, 1);
	if (fr->tile_rows_log2)
		fr->tile_rows_log2 += f(&b, 1);
	fr->compressed_header_size = f(&b, 16);
	fr->uncompressed_header_size = (b.pos + 7) / 8;
	return !b.err && fr->compressed_header_size;
}

VAStatus cvd_vp9_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	const VADecPictureParameterBufferVP9 *pp = &c->pic.vp9;
	struct v4l2_ctrl_vp9_frame fr = { 0 };
	struct v4l2_ext_control ctrl;
	uint8_t ref_idx[3] = { 0 };
	__u64 *ts[3] = { &fr.last_frame_ts, &fr.golden_frame_ts, &fr.alt_frame_ts };
	unsigned int i;

	/* the frame size (also when the header takes it from a reference,
	 * which the tile layout parse needs) is in the VA parameters */
	fr.frame_width_minus_1 = pp->frame_width - 1;
	fr.frame_height_minus_1 = pp->frame_height - 1;
	if (!parse_uncompressed(&c->vp9, c->bs, c->bs_len, &fr, ref_idx)) {
		cvd_log("VP9 frame header not decodable\n");
		return VA_STATUS_ERROR_DECODING_ERROR;
	}
	/* inter frames carry no color config: the stream's depth (profile 2) */
	if (fr.profile >= 2 && pp->bit_depth)
		fr.bit_depth = pp->bit_depth;
	fr.frame_width_minus_1 = pp->frame_width - 1;
	fr.frame_height_minus_1 = pp->frame_height - 1;
	for (i = 0; i < 3; i++) {
		VASurfaceID rid = pp->reference_frames[ref_idx[i]];
		struct cvd_surface *rs = rid != VA_INVALID_SURFACE ? SURFACE(d, rid) : NULL;

		*ts[i] = rs && rs->decoded ? rs->ts : 0;
	}
	cvd_dbg("vp9 %ux%u flags %#x q %u lf %u tiles %u/%u hdr %u+%u\n",
		pp->frame_width, pp->frame_height, fr.flags, fr.quant.base_q_idx, fr.lf.level,
		fr.tile_cols_log2, fr.tile_rows_log2, fr.uncompressed_header_size,
		fr.compressed_header_size);
	ctrl = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_VP9_FRAME,
		.size = sizeof(fr), .ptr = &fr };
	return cvd_submit(d, c, s, &ctrl, 1);
}
