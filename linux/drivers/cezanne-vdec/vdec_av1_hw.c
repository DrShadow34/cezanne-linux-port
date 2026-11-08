// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: AV1 LAT and core register programming.
 *
 * The packing follows MediaTek's vpud HAL for this decoder (decompiled
 * from MIUI's libvcodecdrv.so, functions named below; checked against
 * MIUI captures through an emulator of that library, see NOTES.md
 * "Video codec: AV1"). Register names are the HAL's: block + index * 4.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "vdec.h"
#include "vdec_av1.h"

#define LAT_AV1			0x15800
#define CORE_AV1		0x28800
#define SOC_LATTOP		0x01000

static int av1_poll(struct vdec_dev *d, u32 reg, u32 mask, u32 want, const char *what)
{
	u32 v;
	int ret = readl_poll_timeout_atomic(d->regs + reg, v, (v & mask) == want, 1, 10000);

	if (ret)
		dev_warn(d->dev, "av1: %s timeout (%05x = %08x)\n", what, reg, v);
	return ret;
}

/* vdec_hal_av1_set_mv_dec_params */
static void av1_mv(struct vdec_dev *d, const struct vdec_av1_run *run, bool lat, dma_addr_t mv_tmp)
{
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	u32 mv = lat ? LAT_AVC_MV : CORE_AVC_MV, v;
	unsigned int i;

	vdec_w(d, mv + 0x500, !!(fh->flags & V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS));
	for (i = 0; i < 7; i++) {
		const struct vdec_av1_gm *g = &run->gm[i + 1];

		vdec_w(d, mv + 0x504 + 16 * i, ((g->wmtype & 3) | (g->invalid & 1) << 3) +
		       ((g->wmmat[0] & 0xffffff) << 4));
		vdec_w(d, mv + 0x508 + 16 * i, g->wmmat[1] & 0xffffff);
		vdec_w(d, mv + 0x50c + 16 * i, (g->wmmat[2] & 0x1ffff) | (u32)g->wmmat[3] << 17);
		vdec_w(d, mv + 0x510 + 16 * i, (g->wmmat[4] & 0x7fff) | (u32)g->wmmat[5] << 15);
	}
	vdec_w(d, mv + 0x574, (fh->order_hint & 0xff) | (fh->order_hints[1] & 0xff) << 8 |
	       (fh->order_hints[2] & 0xff) << 16 | (u32)fh->order_hints[3] << 24);
	vdec_w(d, mv + 0x578, (fh->order_hints[4] & 0xff) | (fh->order_hints[5] & 0xff) << 8 |
	       (fh->order_hints[6] & 0xff) << 16 | (u32)fh->order_hints[7] << 24);
	/* bits 0..6: the reference is there (frame->ref_frame_valid) */
	v = 0;
	for (i = 0; i < 7; i++)
		v |= (u32)(run->ref_fb[i] >= 0) << i;
	v |= (run->mfmv.ref_to_cur[0] & 0xff) << 7 | (run->mfmv.ref_to_cur[1] & 0xff) << 15 |
	     (u32)(run->mfmv.ref_to_cur[2] & 0xff) << 23;
	vdec_w(d, mv + 0x57c, v);
	if (lat) {
		const struct vdec_av1_mfmv *m = &run->mfmv;

		vdec_w(d, mv + 0x580, (m->valid[0] & 1) | (m->valid[1] & 1) << 1 |
		       (m->valid[2] & 1) << 2 | (m->dir[0] & 3) << 3 | (m->dir[1] & 3) << 5 |
		       (m->dir[2] & 3) << 7);
		for (i = 0; i < 3; i++)
			vdec_w(d, mv + 0x584 + 4 * i, run->mv_mfmv[i] >> 4);
		vdec_w(d, mv + 0x20c, run->mv_cur >> 4);
	}
	vdec_w(d, mv + 0x3c0, mv_tmp >> 4);
}

static u32 av1_seg_bits(const struct vdec_av1_seg *s, unsigned int feature)
{
	u32 v = 0;
	unsigned int i;

	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++)
		v |= (s->feature_mask[i] >> feature & 1) << i;
	return v;
}

static u32 av1_lr_unit(u32 size)
{
	return size >= 256 ? 3 : size >= 128 ? 2 : size >= 64 ? 1 : 0;
}

/* vdec_util_av1_set_vld_dec_params + vdec_hal_av1_set_vld_dec_params */
static void av1_vld(struct vdec_dev *d, const struct vdec_av1_run *run, bool lat, dma_addr_t tile)
{
	const struct v4l2_ctrl_av1_sequence *seq = run->seq;
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	const struct vdec_av1_seg *s = &run->seg;
	u32 top = lat ? LAT_VLD_TOP : CORE_VLD_TOP, av1 = lat ? LAT_AV1 : CORE_AV1;
	u32 sb = BIT(run->sb_log2) - 1, v;
	unsigned int i;

#define SF(f) (!!(seq->flags & V4L2_AV1_SEQUENCE_FLAG_##f))
#define FF(f) (!!(fh->flags & V4L2_AV1_FRAME_FLAG_##f))
	if (run->bitdepth == 10 || run->bitdepth == 12) {
		vdec_w(d, top + 0x100, run->bitdepth == 10 ? 0x22 : 0x44);
		if (lat)
			vdec_w(d, LAT_PP + 0xb90, 1);	/* as the HEVC LAT */
	}
	vdec_w(d, top + 0x70, run->h << 16 | run->w);
	vdec_w(d, top + 0x68, (((run->h + sb) >> run->sb_log2) - 1) << 16 |
	       (((run->w + sb) >> run->sb_log2) - 1));
	vdec_w(d, top + 0x184, run->h << 16 | run->upscaled_w);
	vdec_w(d, top + 0x180, run->aligned_h << 16 | run->aligned_w);
	vdec_w(d, av1 + 0xa8, run->sb_log2 | SF(ENABLE_FILTER_INTRA) << 4 |
	       SF(ENABLE_INTRA_EDGE_FILTER) << 5 | SF(ENABLE_INTERINTRA_COMPOUND) << 6 |
	       SF(ENABLE_MASKED_COMPOUND) << 7 | SF(ENABLE_DUAL_FILTER) << 8 |
	       SF(ENABLE_JNT_COMP) << 9 | SF(MONO_CHROME) << 10 | SF(ENABLE_ORDER_HINT) << 11 |
	       ((seq->order_hint_bits - 1) & 0xf) << 12);
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (u32)(run->sign_bias[i] & 1) << (24 + i);
	vdec_w(d, av1 + 0xac, v | FF(SKIP_MODE_PRESENT) << 23 | FF(REDUCED_TX_SET) << 22 |
	       (fh->tx_mode & 3) << 20 |
	       !!(fh->tile_info.flags & V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING) << 19 |
	       (fh->interpolation_filter & 7) << 16 | FF(ALLOW_WARPED_MOTION) << 15 |
	       FF(IS_MOTION_MODE_SWITCHABLE) << 14 | (run->reference_mode & 3) << 12 |
	       run->superres << 7 | FF(ALLOW_HIGH_PRECISION_MV) << 6 | FF(ALLOW_INTRABC) << 5 |
	       FF(FORCE_INTEGER_MV) << 4 | FF(ALLOW_SCREEN_CONTENT_TOOLS) << 3 |
	       FF(ERROR_RESILIENT_MODE) << 2 | (fh->frame_type & 3));
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (u32)run->lossless[i] << (8 + i);
	vdec_w(d, av1 + 0xb0, v | av1_seg_bits(s, V4L2_AV1_SEG_LVL_REF_SKIP) << 16 |
	       (u32)run->seg_prev << 24 | run->coded_lossless << 7 | (s->last_active & 7) << 4 |
	       s->preskip << 3 | s->temporal_update << 2 | s->update_map << 1 | s->enabled);
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (s->feature_data[i][V4L2_AV1_SEG_LVL_REF_FRAME] & 7) << (3 * i);
	vdec_w(d, av1 + 0xb4, v + (av1_seg_bits(s, V4L2_AV1_SEG_LVL_REF_FRAME) << 24));
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (u32)(run->qindex[i] > 0) << i;
	vdec_w(d, av1 + 0xb8, v << 24 | av1_seg_bits(s, V4L2_AV1_SEG_LVL_REF_GLOBALMV) << 16 |
	       run->q_ctx << 12 | (fh->cdef.bits & 0xf) << 8 |
	       (fh->loop_filter.delta_lf_res & 3) << 6 | (fh->quantization.delta_q_res & 3) << 4 |
	       !!(fh->loop_filter.flags & V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI) << 3 |
	       !!(fh->loop_filter.flags & V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT) << 2 |
	       !!(fh->quantization.flags & V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT));
	v = 0;
	for (i = 0; i < 8; i++)
		v |= (u32)(s->feature_data[i][V4L2_AV1_SEG_LVL_ALT_Q] < 0) << (24 + i);
	vdec_w(d, av1 + 0xbc, v | av1_seg_bits(s, V4L2_AV1_SEG_LVL_ALT_Q) << 16 |
	       (u8)fh->quantization.delta_q_y_dc << 8 | fh->quantization.base_q_idx);
	for (i = 0; i < 2; i++) {
		const int *q = &s->feature_data[4 * i][V4L2_AV1_SEG_LVL_ALT_Q];

		vdec_w(d, av1 + 0xc0 + 4 * i, abs(q[0]) | abs(q[V4L2_AV1_SEG_LVL_MAX]) << 8 |
		       abs(q[2 * V4L2_AV1_SEG_LVL_MAX]) << 16 |
		       (u32)abs(q[3 * V4L2_AV1_SEG_LVL_MAX]) << 24);
	}
	vdec_w(d, av1 + 0xc8, (u8)fh->quantization.delta_q_u_dc |
	       (u8)fh->quantization.delta_q_u_ac << 8 | (u8)fh->quantization.delta_q_v_dc << 16 |
	       (u32)(u8)fh->quantization.delta_q_v_ac << 24);
	v = 0;
	for (i = 0; i < 7; i++)
		v |= (u32)run->scale[i].is_scaled << i;
	vdec_w(d, av1 + 0xcc, av1_lr_unit(run->lr_size[0]) << 20 | av1_lr_unit(run->lr_size[2]) << 22 |
	       (fh->loop_restoration.frame_restoration_type[0] & 3) << 24 |
	       (fh->loop_restoration.frame_restoration_type[1] & 3) << 26 |
	       (fh->loop_restoration.frame_restoration_type[2] & 3) << 28 |
	       (run->skip_frame[1] & 0xf) << 16 | (run->skip_frame[0] & 0xf) << 12 |
	       (SF(ENABLE_SUPERRES) ? fh->superres_denom : 0) << 7 | v);
	vdec_w(d, av1 + 0xdc, (fh->tile_info.tile_rows - 1) << 16 | (fh->tile_info.tile_cols - 1));
	vdec_w(d, av1 + 0xe4, !FF(DISABLE_CDF_UPDATE) | FF(DISABLE_FRAME_END_UPDATE_CDF) << 4);
	vdec_w(d, top + 0xb8, tile);
	vdec_w(d, (lat ? LAT_VLD : CORE_VLD) + 0x140, 0x100014);
	vdec_w(d, (lat ? LAT_BS2 : CORE_BS2) + 0x140, 0x100014);
	if (lat) {
		if (s->enabled) {
			if (run->seg_prev)
				vdec_w(d, top + 0x118, run->seg_prev_dma);
			vdec_w(d, top + 0x11c, run->seg_cur);
		}
		vdec_w(d, av1 + 0x1b0, run->cdf_in);
		vdec_w(d, av1 + 0x1b4, run->cdf_out);
		/* nine and more tile columns use a side buffer (av1+1b8) */
		if (fh->tile_info.tile_cols > 8) {
			vdec_w(d, av1 + 0x1bc, (vdec_r(d, av1 + 0x1bc) & ~7u) | 4);
			vdec_w(d, av1 + 0x1dc, 1);
			vdec_w(d, av1 + 0x1b8, run->cols_dma);
		} else {
			vdec_w(d, av1 + 0x1dc, 0);
			vdec_w(d, SOC_LATTOP + 0x20, 0);
		}
	}
#undef SF
#undef FF
}

/*
 * Tile info buffer (vdec_util_av1_set_vld_dec_params): 64 bytes a tile.
 * The LAT reads the tile data from the bitstream, the core from the
 * LAT's copy at the same offsets (base 0).
 */
static void av1_tiles(const struct vdec_av1_run *run, u32 *buf, dma_addr_t base)
{
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	bool update = !(fh->flags & V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE);
	unsigned int i;

	memset(buf, 0, run->ntiles * VDEC_AV1_TILE_ENTRY);
	for (i = 0; i < run->ntiles; i++) {
		const struct v4l2_ctrl_av1_tile_group_entry *t = &run->tge[i];
		u32 *e = buf + i * VDEC_AV1_TILE_ENTRY / 4;
		u32 a = base + t->tile_offset;

		e[0] = t->tile_size << 3;
		e[1] = a & ~15u;
		e[2] = (a & 15) << 3;
		e[3] = ((run->col_sb[t->tile_col + 1] - run->col_sb[t->tile_col] - 1) & 0x3f) |
		       ((run->row_sb[t->tile_row + 1] - run->row_sb[t->tile_row] - 1) & 0x1ff) << 7;
		e[4] = update << 18 | run->tile_last[i] << 16;
		if (i == fh->tile_info.context_update_tile_id &&
		    !(fh->flags & V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF))
			e[4] |= BIT(17);
	}
}

/*
 * Spec 7.16 upscaling: a plane's horizontal step (init false) or initial
 * subpel position (init true), 14 fractional bits; without superres the
 * step is 1.0 and the position the 0x80 rounding offset.
 */
static u32 av1_superres(const struct vdec_av1_run *run, bool chroma, bool init)
{
	int down = (run->w + chroma) >> chroma, up = (run->upscaled_w + chroma) >> chroma;
	int step = ((down << 14) + up / 2) / up, err = up * step - (down << 14);

	if (!init)
		return step & 0x7fff;
	return ((-((up - down) << 13) + up / 2) / up + 128 - err / 2) & 0x3fff;
}

/* vdec_util_av1_set_mc_dec_params + vdec_hal_av1_set_mc_dec_params */
static void av1_mc(struct vdec_dev *d, const struct vdec_av1_run *run)
{
	static const u16 dims[7] = { 0xd4c, 0xd50, 0xd54, 0xdd8, 0xddc, 0xde0, 0xde4 };
	static const u16 pitch[7] = { 0xd70, 0xd74, 0xd78, 0xe08, 0xe0c, 0xe10, 0xe14 };
	static const u16 step[7] = { 0xd40, 0xd44, 0xd48, 0xdc8, 0xdcc, 0xdd0, 0xdd4 };
	static const u16 ry[7] = { 0xd28, 0xd2c, 0xd30, 0x000, 0x004, 0x008, 0x00c };
	static const u16 rc[7] = { 0xd34, 0xd38, 0xd3c, 0x09c, 0x0a0, 0x0a4, 0x0a8 };
	unsigned int i;
	u32 scaled = 0, f[3] = { 0 }, v;

	if (!run->intra) {
		for (i = 0; i < 7; i++)
			vdec_w(d, CORE_MC + dims[i], (run->ref_h[i] & 0x3fff) |
			       (run->ref_w[i] & 0x3fff) << 16);
	}
	for (i = 0; i < 7; i++) {
		const struct vdec_av1_scale *sc = &run->scale[i];
		/* per reference: y in bits 8 / 4 / 0, x in 24 / 20 / 16 of
		 * mc+d7c, +d90, +d94; 4 = the reference is not larger (the
		 * vendor's scale * 10000 < 0x9c44000: up to 16385) */
		u32 sh = (2 - i % 3) * 4;

		if (!sc->is_scaled)
			continue;
		scaled |= BIT(i);
		f[i / 3] |= (sc->y_scale <= 16385 ? 4 : 3) << sh |
			    (sc->x_scale <= 16385 ? 4 : 3) << (sh + 16);
	}
	vdec_w(d, CORE_MC + 0xd80, (vdec_r(d, CORE_MC + 0xd80) & ~0x7fu) | scaled);
	if (scaled) {
		for (i = 0; i < 7; i++) {
			const struct vdec_av1_scale *sc = &run->scale[i];
			u32 base = i < 3 ? 0xd58 + 8 * i : 0xde8 + 8 * (i - 3);

			if (sc->is_scaled) {
				vdec_w(d, CORE_MC + base, sc->x_scale);
				vdec_w(d, CORE_MC + base + 4, sc->y_scale);
			}
		}
		for (i = 0; i < 7; i++)
			vdec_w(d, CORE_MC + step[i], (run->scale[i].y_step & 0x1fff) |
			       (run->scale[i].x_step & 0x1fff) << 16);
		vdec_w(d, CORE_MC + 0xd7c, f[0]);
		vdec_w(d, CORE_MC + 0xd90, f[1]);
		v = vdec_r(d, CORE_MC + 0xd94) & ~0x07000700u;
		vdec_w(d, CORE_MC + 0xd94, v | (f[2] & 0x7000700));
	}
	if (!run->intra) {
		for (i = 0; i < 7; i++)
			/* the buffer pitch (the vendor's buf width / 16), not the ref's width */
			vdec_w(d, CORE_MC + pitch[i], run->aligned_w >> 4);
		for (i = 0; i < 7; i++) {
			const struct vdec_av1_gm *g = &run->gm[i + 1];

			vdec_w(d, CORE_MC + 0x480 + 16 * i, g->alpha);
			vdec_w(d, CORE_MC + 0x484 + 16 * i, g->beta);
			vdec_w(d, CORE_MC + 0x488 + 16 * i, g->gamma);
			vdec_w(d, CORE_MC + 0x48c + 16 * i, g->delta);
		}
		for (i = 0; i < 7; i++) {
			vdec_w(d, CORE_MC + ry[i], run->ref_y[i]);
			vdec_w(d, CORE_MC + rc[i], run->ref_c[i]);
		}
	}
	if (run->fh->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC) {
		vdec_w(d, CORE_MC + 0xd4c, (ALIGN(run->w, 16) << 16 & 0x3ff00000) |
		       (ALIGN(run->h, 16) & 0x3ff0));
		vdec_w(d, CORE_MC + 0xd70, ALIGN(run->w, 32) >> 4);
		vdec_w(d, CORE_MC + 0xd28, run->y_dma);
		vdec_w(d, CORE_MC + 0xd34, run->c_dma);
	}
	vdec_w(d, CORE_MC + 0xd88, run->mi_rows | run->mi_cols << 16);
	vdec_w(d, CORE_MC + 0xb5c, vdec_r(d, CORE_MC + 0xb5c) | 1);
}

/* vdec_util_av1_set_iq_dec_params: the table by DMA */
static void av1_iq(struct vdec_dev *d, struct vdec_av1_state *st, const struct vdec_av1_run *run)
{
	const struct v4l2_av1_quantization *q = &run->fh->quantization;
	dma_addr_t qm = st->iq.dma + 2 * VDEC_AV1_IQ_LUT;

	vdec_w(d, CORE_PP + 0x628, st->iq.dma + (run->bitdepth > 8 ? VDEC_AV1_IQ_LUT : 0));
	if (q->flags & V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX) {
		vdec_w(d, CORE_PP + 0x628, qm + q->qm_y * VDEC_AV1_QM_SIZE);
		vdec_w(d, CORE_PP + 0x628, qm + (16 + q->qm_u) * VDEC_AV1_QM_SIZE);
		vdec_w(d, CORE_PP + 0x628, qm + (16 + q->qm_v) * VDEC_AV1_QM_SIZE);
		vdec_w(d, CORE_PP + 0x65c, 0x111);
	} else {
		vdec_w(d, CORE_PP + 0x65c, 0x110);
	}
}

/* vdec_hal_av1_set_lr_dec_params: output buffer and size */
static void av1_out(struct vdec_dev *d, const struct vdec_av1_run *run)
{
	bool ibc = run->fh->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC;

	vdec_w(d, CORE_PP + 0xb88, 0);
	vdec_w(d, CORE_PP + 0xf0, (ibc ? ALIGN(run->h, 8) : run->h) << 16 |
	       (ibc ? ALIGN(run->upscaled_w, 8) : run->upscaled_w));
	vdec_w(d, CORE_MC + 0x224, run->y_dma >> 9);
	vdec_w(d, CORE_MC + 0x228, run->c_dma >> 8);
	if (run->bitdepth == 10) {
		/* 10-bit output: MT2110T blocks in the AV1 core's 128-byte
		 * arrangement (pp+b8c, HEVC's jump mode, changes nothing here) */
		vdec_w(d, CORE_PP + 0xb90, 1);
		vdec_w(d, CORE_MC + 0xcec, 1);
	}
}

/* vdec_hal_av1_set_pp_dec_params: loop filter */
static void av1_lf(struct vdec_dev *d, const struct vdec_av1_run *run)
{
	static const u16 segreg[4][2] = {
		{ 0x0e8, 0x0ec }, { 0xe04, 0xe08 }, { 0xe0c, 0xe10 }, { 0xe14, 0xe18 },
	};
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	const struct v4l2_av1_loop_filter *lf = &fh->loop_filter;
	const struct vdec_av1_seg *s = &run->seg;
	bool on = !(fh->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC);
	u32 sb = BIT(run->sb_log2) - 1, v = 0;
	unsigned int i, k;

	if (on) {
		const s8 *rd = lf->ref_deltas, *md = lf->mode_deltas;

		vdec_w(d, CORE_PP + 0xd8, (lf->level[1] & 0x3f) << 16 | (lf->level[0] & 0x3f) << 22 |
		       (lf->sharpness & 7) << 8 |
		       !!(lf->flags & V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED));
		vdec_w(d, CORE_PP + 0xdc, (rd[0] & 0x7f) | (rd[1] & 0x7f) << 8 | (rd[2] & 0x7f) << 16 |
		       (rd[3] & 0x7f) << 24);
		vdec_w(d, CORE_PP + 0x5c, (rd[4] & 0x7f) | (rd[5] & 0x7f) << 8 | (rd[6] & 0x7f) << 16 |
		       (rd[7] & 0x7f) << 24);
		vdec_w(d, CORE_PP + 0x60, (lf->level[2] & 0x3f) << 6 | (lf->level[3] & 0x3f) |
		       !!(lf->flags & V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT) << 13 |
		       !!(lf->flags & V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI) << 12);
		vdec_w(d, CORE_PP + 0xe0, (md[0] & 0x7f) | (md[1] & 0x7f) << 8);
		if (s->enabled)
			v = av1_seg_bits(s, V4L2_AV1_SEG_LVL_ALT_LF_Y_V);
		vdec_w(d, CORE_PP + 0xe4, v);
		v = 0;
		if (s->enabled) {
			/* per segment: LF_Y_H bit i, LF_U bit 8 + i, LF_V bit
			 * 16 + i... as vdec_hal_av1_set_pp_dec_params packs */
			for (i = 0; i < 8; i++) {
				u16 m = s->feature_mask[i];

				v |= (m >> 2 & 1) << i | (m >> 3 & 1) << (8 + i) | (m >> 4 & 1) << (16 + i);
			}
		}
		vdec_w(d, CORE_PP + 0xe00, v);
		for (k = 0; k < 4; k++)
			for (i = 0; i < 2; i++) {
				const int *f = &s->feature_data[4 * i][k + 1];

				vdec_w(d, CORE_PP + segreg[k][i], (f[0] & 0x7f) |
				       (f[V4L2_AV1_SEG_LVL_MAX] & 0x7f) << 8 |
				       (f[2 * V4L2_AV1_SEG_LVL_MAX] & 0x7f) << 16 |
				       (u32)(f[3 * V4L2_AV1_SEG_LVL_MAX] & 0x7f) << 24);
			}
	}
	vdec_w(d, CORE_MC + 0x220, on);
	vdec_w(d, CORE_MC + 0x238, 3);
	vdec_w(d, CORE_MC + 0x250, 1);
	vdec_w(d, CORE_MC + 0x260, (((run->mi_cols * 4 + sb) & ~sb) >> 4) - 1);
	vdec_w(d, CORE_MC + 0x264, (((run->mi_rows * 4 + sb) & ~sb) >> 4) - 1);
}

/* vdec_hal_av1_set_cdef_dec_params */
static void av1_cdef(struct vdec_dev *d, const struct vdec_av1_run *run)
{
	const struct v4l2_ctrl_av1_frame *fh = run->fh;
	bool skip = run->coded_lossless || (fh->flags & V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC) ||
		    (!fh->cdef.bits && !run->cdef_y[0] && !run->cdef_uv[0]);
	const u8 *y = run->cdef_y, *uv = run->cdef_uv;
	unsigned int i;

	if (!skip) {
		for (i = 0; i < 2; i++) {
			vdec_w(d, CORE_PP + 0xf00 + 4 * i, (y[4 * i] & 0x3f) | (y[4 * i + 1] & 0x3f) << 8 |
			       (y[4 * i + 2] & 0x3f) << 16 | (y[4 * i + 3] & 0x3f) << 24);
		}
		for (i = 0; i < 2; i++) {
			vdec_w(d, CORE_PP + 0xf08 + 4 * i, (uv[4 * i] & 0x3f) |
			       (uv[4 * i + 1] & 0x3f) << 8 | (uv[4 * i + 2] & 0x3f) << 16 |
			       (uv[4 * i + 3] & 0x3f) << 24);
		}
	}
	vdec_w(d, CORE_PP + 0xf10, ((fh->cdef.damping_minus_3 + 3) & 7) | skip << 4);
}

static void av1_bs(struct vdec_dev *d, u32 vld, u32 bs2, dma_addr_t base, u32 lo, u32 hi)
{
	vdec_w(d, bs2 + 0xb4, lo);
	vdec_w(d, bs2 + 0xb8, hi);
	vdec_w(d, bs2 + 0xec, 0x10000000);
	vdec_w(d, vld + 0xec, 0x10000000);
	vdec_w(d, vld + 0xb4, lo);
	vdec_w(d, vld + 0xb8, hi);
	vdec_w(d, vld + 0xb0, base);
	vdec_w(d, vld + 0x108, 0x100);
	vdec_w(d, vld + 0x108, 0);
}

#define AV1_GCON_C8		0xf7ff
#define AV1_GCON_CC		0x71e31180
#define AV1_GCON_178		0xffcfba61

void vdec_hw_av1_lat(struct vdec_ctx *ctx, const struct vdec_av1_run *run)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_av1_state *st = ctx->av1;

	vdec_hw_init(d);
	vdec_hw_lat_prologue(d, AV1_GCON_C8, AV1_GCON_CC, AV1_GCON_178);
	vdec_w(d, LAT_MISC + 0x204, 0x40000);
	vdec_w(d, LAT_AV1 + 0xa4, 1);
	av1_bs(d, LAT_VLD, LAT_BS2, run->bs_dma, run->bs_dma >> 6,
	       (run->bs_dma + run->bs_size) >> 6);
	vdec_w(d, LAT_VLD + 0x8c, 0x100000);
	av1_poll(d, LAT_VLD + 0xf4, BIT(17), 0, "bitstream fetch");
	/* the fetch is done once vld+0e8 bit 0 (FETCHOK) is set, as MIUI
	 * and our VP8 wait: initialising the barrel shifter before that
	 * parsed stale bits now and then (about one VP9 frame in 1000: the
	 * compressed header read wrong, the context saved from it broken
	 * until the next keyframe) */
	av1_poll(d, LAT_VLD + 0xe8, BIT(0), BIT(0), "bitstream fetch done");
	vdec_w(d, LAT_VLD + 0x8c, 0x800000);
	vdec_hw_lat_racing_tail(d);
	vdec_w(d, SOC_RACING + 0xac, st->mv_tmp.dma >> 4);
	av1_tiles(run, st->tile_lat.va, run->bs_dma);
	av1_mv(d, run, true, st->mv_tmp.dma);
	av1_vld(d, run, true, st->tile_lat.dma);
	vdec_w(d, LAT_MISC + 0x2f8, 0x120);
	/* av1+0f0 (start) comes from vdec_hw_av1_start() */
}

void vdec_hw_av1_core(struct vdec_ctx *ctx, const struct vdec_av1_run *run)
{
	struct vdec_dev *d = ctx->dev;
	struct vdec_av1_state *st = ctx->av1;

	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, CORE_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);
	vdec_w(d, CORE_MISC + 0xf4, 0);
	vdec_w(d, CORE_MISC + 0xec, 1);
	vdec_w(d, SOC_RACING + 0x84, 1);
	vdec_w(d, CORE_LARB + 0xc, 1);
	vdec_w(d, CORE_VLD + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0xc8, AV1_GCON_C8);
	vdec_w(d, CORE_MISC + 0xcc, AV1_GCON_CC);
	vdec_w(d, CORE_MISC + 0x178, AV1_GCON_178);
	vdec_w(d, CORE_MISC + 0x204, 0x40000);
	vdec_w(d, CORE_MISC + 0x84, 2);
	vdec_w(d, SOC_GCON + 0x4, 2);
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, CORE_LARB + 0xc, 0x00010000);
	vdec_w(d, SOC_RACING + 0x84, 0);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 1);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 0);
	vdec_w(d, SOC_UFO_ENC + 0x90, 1);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);
	vdec_w(d, CORE_AV1 + 0xa4, 1);
	/* the core reads the LAT's copy of the tile data: window 0..8 KiB */
	av1_bs(d, CORE_VLD, CORE_BS2, 0, 0, 0x80);
	vdec_w(d, CORE_VLD_TOP + 0x2ac, 0x11);
	vdec_w(d, CORE_VLD_TOP + 0x234, 1);
	vdec_w(d, CORE_VLD_TOP + 0x23c, run->ube->dma + run->ube->size);
	vdec_w(d, CORE_VLD_TOP + 0x240, run->ube->dma);
	vdec_w(d, SOC_RACING + 0x9c, 0);
	vdec_w(d, SOC_RACING + 0x58, 2);
	vdec_w(d, CORE_VLD_TOP + 0x398, 0x113);
	vdec_w(d, SOC_RACING + 0x7c, 0);
	vdec_w(d, SOC_RACING + 0x64, 1);
	vdec_w(d, SOC_RACING + 0xb0, st->mv_tmp.dma >> 4);
	vdec_w(d, SOC_RACING + 0xb4, 1);
	vdec_w(d, CORE_VLD_TOP + 0x9c, 0x740);	/* vdec_hal_av1_set_wrapper, <= 4096x2304 */
	av1_mv(d, run, false, st->mv_tmp.dma);
	av1_iq(d, st, run);
	av1_mc(d, run);
	av1_tiles(run, st->tile_core.va, 0);
	av1_vld(d, run, false, st->tile_core.dma);
	av1_out(d, run);
	vdec_w(d, SOC_UFO_ENC + 0x8c, 0x11100);
	vdec_w(d, SOC_UFO_ENC + 0x4, 0);
	vdec_w(d, CORE_MC + 0xa60, 0);
	av1_lf(d, run);
	av1_cdef(d, run);
	/* vdec_hal_av1_set_sr_dec_params: chroma << 16 | luma */
	vdec_w(d, CORE_PP + 0xf40, av1_superres(run, true, false) << 16 |
	       av1_superres(run, false, false));
	vdec_w(d, CORE_PP + 0xf44, av1_superres(run, true, true) << 16 |
	       av1_superres(run, false, true));
	vdec_w(d, CORE_VLD_TOP + 0x2ac, 0x11);
	vdec_w(d, CORE_MISC + 0x4, 1);
	vdec_w(d, CORE_MISC + 0x2f8, 0x120);
	/* av1+0f0 (start) comes from vdec_hw_av1_start() */
}

void vdec_hw_av1_start(struct vdec_dev *d)
{
	vdec_w(d, LAT_AV1 + 0xf0, 1);
	vdec_w(d, LAT_MISC + 0xa4, 0x11);
	udelay(50);
	vdec_w(d, CORE_AV1 + 0xf0, 1);
	vdec_w(d, CORE_VLD + 0xa4, 0x11);
}
