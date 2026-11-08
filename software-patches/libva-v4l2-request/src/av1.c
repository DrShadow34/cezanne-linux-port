// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: AV1 (VAProfileAV1Profile0) -> V4L2 stateless
 * AV1 SEQUENCE / FRAME / TILE_GROUP_ENTRY controls.
 *
 * VA leaves out what V4L2 wants derived from the reference frames (their
 * order hints, the skip mode frames) and gives the tile layout in
 * superblocks; those are computed here, the order hint of each decoded
 * surface is remembered for the frames that reference it. The tile group
 * data buffers are kept whole (tile offsets index into them), so the gap
 * of a tile group header shows the kernel where a tile group ends.
 */
#include <stdlib.h>
#include <string.h>

#include "cvd.h"

static int rel_dist(const VADecPictureParameterBufferAV1 *pp, int a, int b)
{
	int m, diff;

	if (!pp->seq_info_fields.fields.enable_order_hint)
		return 0;
	diff = a - b;
	m = 1 << pp->order_hint_bits_minus_1;
	return (diff & (m - 1)) - (diff & m);
}

static struct cvd_surface *ref_surface(struct cvd_driver *d, const VADecPictureParameterBufferAV1 *pp,
				       unsigned int slot)
{
	VASurfaceID id = pp->ref_frame_map[slot];
	struct cvd_surface *s = id != VA_INVALID_SURFACE ? SURFACE(d, id) : NULL;

	return s && s->decoded ? s : NULL;
}

/* spec 5.9.22 (skip_mode_params) */
static void skip_mode(const VADecPictureParameterBufferAV1 *pp, const uint8_t *hints,
		      struct v4l2_ctrl_av1_frame *fr)
{
	int fwd = -1, bwd = -1, fwd2 = -1, fh = 0, bh = 0, f2h = 0, a, b;
	unsigned int i;

	for (i = 0; i < 7; i++) {
		int h = hints[i + 1];

		if (rel_dist(pp, h, pp->order_hint) < 0) {
			if (fwd < 0 || rel_dist(pp, h, fh) > 0) {
				fwd = i;
				fh = h;
			}
		} else if (rel_dist(pp, h, pp->order_hint) > 0) {
			if (bwd < 0 || rel_dist(pp, h, bh) < 0) {
				bwd = i;
				bh = h;
			}
		}
	}
	if (fwd < 0)
		return;
	if (bwd >= 0) {
		a = fwd;
		b = bwd;
	} else {
		for (i = 0; i < 7; i++) {
			int h = hints[i + 1];

			if (rel_dist(pp, h, fh) < 0 && (fwd2 < 0 || rel_dist(pp, h, f2h) > 0)) {
				fwd2 = i;
				f2h = h;
			}
		}
		if (fwd2 < 0)
			return;
		a = fwd;
		b = fwd2;
	}
	fr->skip_mode_frame[0] = V4L2_AV1_REF_LAST_FRAME + (a < b ? a : b);
	fr->skip_mode_frame[1] = V4L2_AV1_REF_LAST_FRAME + (a < b ? b : a);
}

static void seq_ctrl(const VADecPictureParameterBufferAV1 *pp, struct v4l2_ctrl_av1_sequence *sq)
{
	unsigned int f = pp->seq_info_fields.value;

	memset(sq, 0, sizeof(*sq));
#define SQ(va, v4l) do { if (pp->seq_info_fields.fields.va) sq->flags |= V4L2_AV1_SEQUENCE_FLAG_##v4l; } while (0)
	SQ(still_picture, STILL_PICTURE);
	SQ(use_128x128_superblock, USE_128X128_SUPERBLOCK);
	SQ(enable_filter_intra, ENABLE_FILTER_INTRA);
	SQ(enable_intra_edge_filter, ENABLE_INTRA_EDGE_FILTER);
	SQ(enable_interintra_compound, ENABLE_INTERINTRA_COMPOUND);
	SQ(enable_masked_compound, ENABLE_MASKED_COMPOUND);
	SQ(enable_dual_filter, ENABLE_DUAL_FILTER);
	SQ(enable_order_hint, ENABLE_ORDER_HINT);
	SQ(enable_jnt_comp, ENABLE_JNT_COMP);
	SQ(enable_cdef, ENABLE_CDEF);
	SQ(mono_chrome, MONO_CHROME);
	SQ(color_range, COLOR_RANGE);
	SQ(subsampling_x, SUBSAMPLING_X);
	SQ(subsampling_y, SUBSAMPLING_Y);
	SQ(film_grain_params_present, FILM_GRAIN_PARAMS_PRESENT);
#undef SQ
	(void)f;
	/* not in VA: the frame level says what is used */
	sq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_WARPED_MOTION | V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS |
		     V4L2_AV1_SEQUENCE_FLAG_ENABLE_RESTORATION;
	if (pp->pic_info_fields.bits.use_superres)
		sq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_SUPERRES;
	sq->seq_profile = pp->profile;
	sq->order_hint_bits = pp->order_hint_bits_minus_1 + 1;
	sq->bit_depth = pp->bit_depth_idx == 2 ? 12 : pp->bit_depth_idx == 1 ? 10 : 8;
	sq->max_frame_width_minus_1 = pp->frame_width_minus1;
	sq->max_frame_height_minus_1 = pp->frame_height_minus1;
}

static void seg_ctrl(const VASegmentationStructAV1 *sv, struct v4l2_av1_segmentation *sg)
{
	unsigned int i, j;

	memset(sg, 0, sizeof(*sg));
	if (!sv->segment_info_fields.bits.enabled)
		return;
	sg->flags = V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
	if (sv->segment_info_fields.bits.update_map)
		sg->flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
	if (sv->segment_info_fields.bits.temporal_update)
		sg->flags |= V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
	if (sv->segment_info_fields.bits.update_data)
		sg->flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA;
	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		sg->feature_enabled[i] = sv->feature_mask[i];
		for (j = 0; j < V4L2_AV1_SEG_LVL_MAX; j++) {
			sg->feature_data[i][j] = sv->feature_data[i][j];
			if (!(sv->feature_mask[i] & (1 << j)))
				continue;
			/* spec 5.9.14: SegIdPreSkip, LastActiveSegId */
			sg->last_active_seg_id = i;
			if (j >= V4L2_AV1_SEG_LVL_REF_FRAME)
				sg->flags |= V4L2_AV1_SEGMENTATION_FLAG_SEG_ID_PRE_SKIP;
		}
	}
}

static void tile_ctrl(const VADecPictureParameterBufferAV1 *pp, struct v4l2_av1_tile_info *ti)
{
	unsigned int sb_mi = pp->seq_info_fields.fields.use_128x128_superblock ? 32 : 16;
	unsigned int mi_cols = 2 * ((pp->frame_width_minus1 + 8) >> 3);
	unsigned int mi_rows = 2 * ((pp->frame_height_minus1 + 8) >> 3);
	unsigned int i, pos;

	memset(ti, 0, sizeof(*ti));
	ti->tile_cols = pp->tile_cols;
	ti->tile_rows = pp->tile_rows;
	ti->context_update_tile_id = pp->context_update_tile_id;
	if (pp->pic_info_fields.bits.uniform_tile_spacing_flag)
		ti->flags |= V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING;
	for (i = 0, pos = 0; i < pp->tile_cols && i < V4L2_AV1_MAX_TILE_COLS; i++) {
		ti->mi_col_starts[i] = pos;
		ti->width_in_sbs_minus_1[i] = pp->width_in_sbs_minus_1[i];
		pos += (pp->width_in_sbs_minus_1[i] + 1) * sb_mi;
	}
	ti->mi_col_starts[i] = pos < mi_cols ? pos : mi_cols;
	for (i = 0, pos = 0; i < pp->tile_rows && i < V4L2_AV1_MAX_TILE_ROWS; i++) {
		ti->mi_row_starts[i] = pos;
		ti->height_in_sbs_minus_1[i] = pp->height_in_sbs_minus_1[i];
		pos += (pp->height_in_sbs_minus_1[i] + 1) * sb_mi;
	}
	ti->mi_row_starts[i] = pos < mi_rows ? pos : mi_rows;
}

static void frame_ctrl(struct cvd_driver *d, struct cvd_context *c, struct v4l2_ctrl_av1_frame *fr)
{
	const VADecPictureParameterBufferAV1 *pp = &c->pic.av1;
	unsigned int i, j, up = pp->frame_width_minus1 + 1;
	uint8_t hints[V4L2_AV1_TOTAL_REFS_PER_FRAME] = { 0 };
	bool intra = pp->pic_info_fields.bits.frame_type == V4L2_AV1_KEY_FRAME ||
		     pp->pic_info_fields.bits.frame_type == V4L2_AV1_INTRA_ONLY_FRAME;

	memset(fr, 0, sizeof(*fr));
	tile_ctrl(pp, &fr->tile_info);

	fr->quantization.base_q_idx = pp->base_qindex;
	fr->quantization.delta_q_y_dc = pp->y_dc_delta_q;
	fr->quantization.delta_q_u_dc = pp->u_dc_delta_q;
	fr->quantization.delta_q_u_ac = pp->u_ac_delta_q;
	fr->quantization.delta_q_v_dc = pp->v_dc_delta_q;
	fr->quantization.delta_q_v_ac = pp->v_ac_delta_q;
	fr->quantization.qm_y = pp->qmatrix_fields.bits.qm_y;
	fr->quantization.qm_u = pp->qmatrix_fields.bits.qm_u;
	fr->quantization.qm_v = pp->qmatrix_fields.bits.qm_v;
	fr->quantization.delta_q_res = pp->mode_control_fields.bits.log2_delta_q_res;
	if (pp->qmatrix_fields.bits.using_qmatrix)
		fr->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX;
	if (pp->mode_control_fields.bits.delta_q_present_flag)
		fr->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT;
	if (pp->u_dc_delta_q != pp->v_dc_delta_q || pp->u_ac_delta_q != pp->v_ac_delta_q)
		fr->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA;

	fr->superres_denom = pp->pic_info_fields.bits.use_superres ? pp->superres_scale_denominator : 8;
	seg_ctrl(&pp->seg_info, &fr->segmentation);

	fr->loop_filter.level[0] = pp->filter_level[0];
	fr->loop_filter.level[1] = pp->filter_level[1];
	fr->loop_filter.level[2] = pp->filter_level_u;
	fr->loop_filter.level[3] = pp->filter_level_v;
	fr->loop_filter.sharpness = pp->loop_filter_info_fields.bits.sharpness_level;
	memcpy(fr->loop_filter.ref_deltas, pp->ref_deltas, sizeof(fr->loop_filter.ref_deltas));
	memcpy(fr->loop_filter.mode_deltas, pp->mode_deltas, sizeof(fr->loop_filter.mode_deltas));
	fr->loop_filter.delta_lf_res = pp->mode_control_fields.bits.log2_delta_lf_res;
	if (pp->loop_filter_info_fields.bits.mode_ref_delta_enabled)
		fr->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED;
	if (pp->loop_filter_info_fields.bits.mode_ref_delta_update)
		fr->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE;
	if (pp->mode_control_fields.bits.delta_lf_present_flag)
		fr->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT;
	if (pp->mode_control_fields.bits.delta_lf_multi)
		fr->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI;

	fr->cdef.damping_minus_3 = pp->cdef_damping_minus_3;
	fr->cdef.bits = pp->cdef_bits;
	for (i = 0; i < V4L2_AV1_CDEF_MAX; i++) {
		/* VA: pri << 2 | coded sec (3 means 4) */
		uint8_t ys = pp->cdef_y_strengths[i] & 3, uvs = pp->cdef_uv_strengths[i] & 3;

		fr->cdef.y_pri_strength[i] = pp->cdef_y_strengths[i] >> 2;
		fr->cdef.y_sec_strength[i] = ys == 3 ? 4 : ys;
		fr->cdef.uv_pri_strength[i] = pp->cdef_uv_strengths[i] >> 2;
		fr->cdef.uv_sec_strength[i] = uvs == 3 ? 4 : uvs;
	}

	fr->loop_restoration.frame_restoration_type[0] = pp->loop_restoration_fields.bits.yframe_restoration_type;
	fr->loop_restoration.frame_restoration_type[1] = pp->loop_restoration_fields.bits.cbframe_restoration_type;
	fr->loop_restoration.frame_restoration_type[2] = pp->loop_restoration_fields.bits.crframe_restoration_type;
	fr->loop_restoration.lr_unit_shift = pp->loop_restoration_fields.bits.lr_unit_shift;
	fr->loop_restoration.lr_uv_shift = pp->loop_restoration_fields.bits.lr_uv_shift;
	for (i = 0; i < 3; i++) {
		if (fr->loop_restoration.frame_restoration_type[i])
			fr->loop_restoration.flags |= i ? V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR :
							  V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR;
	}
	fr->loop_restoration.loop_restoration_size[0] = 256 >> (2 - pp->loop_restoration_fields.bits.lr_unit_shift);
	fr->loop_restoration.loop_restoration_size[1] =
		fr->loop_restoration.loop_restoration_size[0] >> pp->loop_restoration_fields.bits.lr_uv_shift;
	fr->loop_restoration.loop_restoration_size[2] = fr->loop_restoration.loop_restoration_size[1];

	/* global motion: identity for INTRA_FRAME, VA's wm[] for LAST..ALTREF */
	fr->global_motion.params[0][2] = fr->global_motion.params[0][5] = 1 << 16;
	for (i = 0; i < 7; i++) {
		const VAWarpedMotionParamsAV1 *w = &pp->wm[i];

		fr->global_motion.type[i + 1] = w->wmtype;
		for (j = 0; j < 6; j++)
			fr->global_motion.params[i + 1][j] = w->wmmat[j];
		if (w->invalid)
			fr->global_motion.invalid |= 1 << (i + 1);
		if (w->wmtype != VAAV1TransformationIdentity)
			fr->global_motion.flags[i + 1] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_GLOBAL;
	}

#define FF(va, v4l) do { if (pp->pic_info_fields.bits.va) fr->flags |= V4L2_AV1_FRAME_FLAG_##v4l; } while (0)
	FF(show_frame, SHOW_FRAME);
	FF(showable_frame, SHOWABLE_FRAME);
	FF(error_resilient_mode, ERROR_RESILIENT_MODE);
	FF(disable_cdf_update, DISABLE_CDF_UPDATE);
	FF(allow_screen_content_tools, ALLOW_SCREEN_CONTENT_TOOLS);
	FF(force_integer_mv, FORCE_INTEGER_MV);
	FF(allow_intrabc, ALLOW_INTRABC);
	FF(use_superres, USE_SUPERRES);
	FF(allow_high_precision_mv, ALLOW_HIGH_PRECISION_MV);
	FF(is_motion_mode_switchable, IS_MOTION_MODE_SWITCHABLE);
	FF(use_ref_frame_mvs, USE_REF_FRAME_MVS);
	FF(disable_frame_end_update_cdf, DISABLE_FRAME_END_UPDATE_CDF);
	FF(allow_warped_motion, ALLOW_WARPED_MOTION);
#undef FF
	if (pp->mode_control_fields.bits.reference_select)
		fr->flags |= V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT;
	if (pp->mode_control_fields.bits.reduced_tx_set_used)
		fr->flags |= V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET;
	if (pp->mode_control_fields.bits.skip_mode_present)
		fr->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT;

	fr->frame_type = pp->pic_info_fields.bits.frame_type;
	fr->order_hint = pp->order_hint;
	fr->upscaled_width = up;
	fr->frame_width_minus_1 = pp->pic_info_fields.bits.use_superres ?
		(up * 8 + pp->superres_scale_denominator / 2) / pp->superres_scale_denominator - 1 :
		pp->frame_width_minus1;
	fr->frame_height_minus_1 = pp->frame_height_minus1;
	fr->render_width_minus_1 = pp->frame_width_minus1;
	fr->render_height_minus_1 = pp->frame_height_minus1;
	fr->interpolation_filter = pp->interp_filter;
	fr->tx_mode = pp->mode_control_fields.bits.tx_mode;
	fr->primary_ref_frame = pp->primary_ref_frame;

	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
		struct cvd_surface *r = intra ? NULL : ref_surface(d, pp, i);

		fr->reference_frame_ts[i] = r ? r->ts : 0;
	}
	hints[0] = pp->order_hint;
	for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
		struct cvd_surface *r;

		fr->ref_frame_idx[i] = pp->ref_frame_idx[i];
		r = intra ? NULL : ref_surface(d, pp, pp->ref_frame_idx[i]);
		hints[i + 1] = r ? r->av1_order_hint : 0;
	}
	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
		fr->order_hints[i] = hints[i];
	if (pp->mode_control_fields.bits.skip_mode_present && !intra)
		skip_mode(pp, hints, fr);
}

VAStatus cvd_av1_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	const VADecPictureParameterBufferAV1 *pp = &c->pic.av1;
	struct v4l2_ctrl_av1_sequence sq;
	struct v4l2_ctrl_av1_frame fr;
	struct v4l2_ext_control ctrls[3];
	VAStatus st;

	if (!c->av1_ntiles || c->av1_ntiles != (unsigned int)pp->tile_cols * pp->tile_rows) {
		cvd_log("AV1 picture with %u of %u tiles\n", c->av1_ntiles, pp->tile_cols * pp->tile_rows);
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
	if (pp->pic_info_fields.bits.large_scale_tile) {
		cvd_log("AV1 large scale tile is not supported\n");
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}
	seq_ctrl(pp, &sq);
	frame_ctrl(d, c, &fr);
	cvd_dbg("av1 %ux%u type %u oh %u q %u tiles %u prim %u\n", pp->frame_width_minus1 + 1,
		pp->frame_height_minus1 + 1, fr.frame_type, fr.order_hint, pp->base_qindex,
		c->av1_ntiles, fr.primary_ref_frame);

	ctrls[0] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_AV1_SEQUENCE,
		.size = sizeof(sq), .ptr = &sq };
	ctrls[1] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_AV1_FRAME,
		.size = sizeof(fr), .ptr = &fr };
	ctrls[2] = (struct v4l2_ext_control){ .id = V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY,
		.size = c->av1_ntiles * sizeof(c->av1_tiles[0]), .ptr = c->av1_tiles };
	st = cvd_submit(d, c, s, ctrls, 3);
	if (!st)
		s->av1_order_hint = pp->order_hint;
	/* the grain / display copy below reads the decoded frame now */
	if (!st && pp->current_display_picture != VA_INVALID_SURFACE &&
	    pp->current_display_picture != (VASurfaceID)s->base.id)
		cvd_surface_wait(d, s);
	/*
	 * With film grain the client shows a second surface: the decoded frame
	 * plus grain, synthesized here (the hardware has no grain stage); the
	 * decoded surface stays the reference. Both are read from the capture
	 * buffer (cached): surface memory can be write-combined (panfrost
	 * buffers in Firefox), far too slow to read sample by sample.
	 */
	if (!st && pp->current_display_picture != VA_INVALID_SURFACE &&
	    pp->current_display_picture != (VASurfaceID)s->base.id) {
		struct cvd_surface *o = SURFACE(d, pp->current_display_picture);

		if (o && pp->film_grain_info.film_grain_info_fields.bits.apply_grain) {
			struct cvd_surface src = *s;

			if (c->grain_src_size < s->size) {
				free(c->grain_src);
				c->grain_src = malloc(s->size);
				c->grain_src_size = c->grain_src ? s->size : 0;
			}
			if (c->grain_src) {
				uint64_t t0 = cvd_us(), t1;

				cvd_detile_to(c, s, c->grain_src, -1);
				t1 = cvd_us();
				src.map = c->grain_src;
				src.fd = -1;
				cvd_av1_film_grain(o, &src, pp);
				o->dirty = false;
				cvd_dbgt("timing: grain detile %llu us, synthesis %llu us\n",
					(unsigned long long)(t1 - t0),
					(unsigned long long)(cvd_us() - t1));
			}
		} else if (o) {
			cvd_detile_to(c, s, o->map, o->fd);
			o->dirty = false;
		}
		if (o)
			o->status = VASurfaceReady;
	}
	c->av1_ntiles = 0;
	return st;
}
