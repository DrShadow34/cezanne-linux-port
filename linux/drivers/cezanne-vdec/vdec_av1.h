/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cezanne-vdec AV1: per-frame parameters in the form the MT6885 vdec
 * programming wants. The structures follow mainline's MT8195 driver
 * (vdec_av1_req_lat_if.c): MediaTek's own decoder keeps the same types,
 * and the register packing in vdec_av1_hw.c reads them (see NOTES.md
 * "AV1 register map").
 */
#ifndef CEZANNE_VDEC_AV1_H
#define CEZANNE_VDEC_AV1_H

#include <linux/types.h>
#include <media/v4l2-ctrls.h>

#define VDEC_AV1_CDF_SIZE	0x3fa0		/* one context set */
#define VDEC_AV1_CDF_BUF	SZ_16K
#define VDEC_AV1_QM_SIZE	0x8d0
#define VDEC_AV1_IQ_LUT		0x400		/* per bit depth */
#define VDEC_AV1_IQ_SIZE	(2 * VDEC_AV1_IQ_LUT + 32 * VDEC_AV1_QM_SIZE)
#define VDEC_AV1_TILE_ENTRY	64
#define VDEC_AV1_TILE_BUF	(VDEC_AV1_TILE_ENTRY * V4L2_AV1_MAX_TILE_COUNT)
#define VDEC_AV1_COLS_BUF	0x190000	/* the vendor's, for any size */
#define VDEC_AV1_MAX_FB		32		/* capture buffers tracked */
#define VDEC_AV1_NO_IDX		-1

/* tools/gen_av1_tables.py -> vdec_av1_tables.c */
extern const u16 vdec_av1_cdf_default[4][VDEC_AV1_CDF_SIZE / 2];
extern const u16 vdec_av1_iq_lut[2][VDEC_AV1_IQ_LUT / 2];
extern const u8 vdec_av1_qm[32][VDEC_AV1_QM_SIZE];

struct vdec_av1_scale {
	bool is_scaled;
	int x_scale, y_scale, x_step, y_step;
};

struct vdec_av1_gm {
	int wmtype;
	int wmmat[8];
	s16 alpha, beta, gamma, delta;
	bool invalid;
};

struct vdec_av1_seg {
	bool enabled, update_map, temporal_update, update_data;
	int feature_data[V4L2_AV1_MAX_SEGMENTS][V4L2_AV1_SEG_LVL_MAX];
	u16 feature_mask[V4L2_AV1_MAX_SEGMENTS];
	bool preskip;
	int last_active;
};

struct vdec_av1_mfmv {
	u32 valid[3];
	u32 dir[3];			/* 2: from a past frame, 0: a future one */
	int ref_to_cur[3];
	int fb[3];			/* frame buffer index */
	int count;
};

/* what a decoded frame leaves for the frames that reference it */
struct vdec_av1_fbinfo {
	bool valid;
	u8 frame_type;
	bool intra;
	u32 order_hint;
	u32 order_hints[V4L2_AV1_TOTAL_REFS_PER_FRAME];	/* its refs', [k + 1] */
	u32 w, h, upscaled_w, mi_cols, mi_rows;
};

struct vdec_av1_fbbuf {
	void *mv_va, *cdf_va, *seg_va;
	dma_addr_t mv, cdf, seg;
	size_t mv_size, seg_size;
};

struct vdec_av1_state {
	struct vdec_av1_fbinfo info[VDEC_AV1_MAX_FB];
	struct vdec_av1_fbbuf buf[VDEC_AV1_MAX_FB];
	struct vdec_work_buf cdf_default, iq, tile_lat, tile_core, mv_tmp;
	struct vdec_work_buf cols;	/* LAT side buffer for more than 8 tile columns */
};

struct vdec_av1_run {
	const struct v4l2_ctrl_av1_sequence *seq;
	const struct v4l2_ctrl_av1_frame *fh;
	const struct v4l2_ctrl_av1_tile_group_entry *tge;
	unsigned int ntiles;
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t bs_dma;
	u32 bs_len, bs_size;
	int fb;					/* this frame's buffer index */
	dma_addr_t y_dma, c_dma;

	/* sequence */
	u8 bitdepth, sb_log2;		/* 6 or 7 */
	/* frame */
	u32 w, h, upscaled_w, mi_cols, mi_rows, aligned_w, aligned_h;
	bool intra, superres;
	u8 reference_mode;		/* 0 single, 2 select */
	u8 q_ctx;			/* default CDF set */
	int qindex[V4L2_AV1_MAX_SEGMENTS];
	bool lossless[V4L2_AV1_MAX_SEGMENTS], coded_lossless;
	struct vdec_av1_seg seg;
	bool seg_prev;			/* the primary ref's segment map is read */
	struct vdec_av1_gm gm[V4L2_AV1_TOTAL_REFS_PER_FRAME];
	u8 sign_bias[V4L2_AV1_TOTAL_REFS_PER_FRAME];
	int skip_frame[2];		/* skip_mode_frame - LAST, or -1 */
	u8 cdef_y[8], cdef_uv[8];
	u32 lr_size[3];
	int col_sb[V4L2_AV1_MAX_TILE_COLS + 1], row_sb[V4L2_AV1_MAX_TILE_ROWS + 1];
	u8 tile_last[V4L2_AV1_MAX_TILE_COUNT];	/* last tile of its tile group */
	struct vdec_av1_mfmv mfmv;

	/* references LAST..ALTREF, [0..6] */
	int ref_fb[V4L2_AV1_REFS_PER_FRAME];
	struct vdec_av1_scale scale[V4L2_AV1_REFS_PER_FRAME];
	dma_addr_t ref_y[V4L2_AV1_REFS_PER_FRAME], ref_c[V4L2_AV1_REFS_PER_FRAME];
	u32 ref_w[V4L2_AV1_REFS_PER_FRAME], ref_h[V4L2_AV1_REFS_PER_FRAME];
	int prev_fb;			/* primary_ref_frame's buffer, or -1 */

	/* buffers */
	dma_addr_t cdf_in, cdf_out, seg_prev_dma, seg_cur, mv_cur, mv_mfmv[3];
	dma_addr_t cols_dma;
	struct vdec_work_buf *ube, *err;
};

#endif
