// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: HEVC LAT and core register programming.
 *
 * Follows MediaTek's vpud HAL (vdec_hal_h265_set_* in MIUI's
 * libvcodecdrv.so, decompiled; checked through the emulator in
 * trace/av1/hemu.py, see NOTES.md "Video codec: HEVC"). The LAT reads the
 * picture from the first slice NAL's start code and parses every slice
 * header itself; the header words below come from the first slice.
 */
#include <linux/delay.h>

#include "vdec.h"
#include "vdec_hevc.h"

#define LAT_HEVC		0x15000		/* hevc_vld */
#define CORE_HEVC		0x28000

#define HEVC_GCON_C8		0xfeff
#define HEVC_GCON_CC		0x77e30180
#define HEVC_GCON_178		0xffddfa60

static u32 ceil_log2_min1(u32 n)
{
	u32 k = 1;

	while ((1u << k) < n)
		k++;
	return k;
}

/* vdec_hal_h265_init_auto_fetch: window and read pointer (byte exact) */
static void hevc_fetch(struct vdec_dev *d, bool lat, dma_addr_t start, dma_addr_t end,
		       dma_addr_t rd)
{
	u32 vld = lat ? LAT_VLD : CORE_VLD, top = lat ? LAT_VLD_TOP : CORE_VLD_TOP;

	vdec_w(d, top + 0x2ac, (vdec_r(d, top + 0x2ac) & ~0x10u) | 0x10);
	vdec_w(d, vld + 0x140, 0x100014);
	vdec_w(d, vld + 0xec, 0x10000000);
	vdec_w(d, (lat ? LAT_HEVC : CORE_HEVC) + 0x84, 1);
	vdec_w(d, vld + 0xb4, start >> 6);
	vdec_w(d, vld + 0xb8, end >> 6);
	vdec_w(d, vld + 0xb0, rd & ~15u);
	vdec_w(d, top + 0x1c, (rd & 15) << 3);
	vdec_w(d, top + 0x2ac, 0x11);
}

/* vdec_hal_h265_set_shdr_hevld / set_sps_hevld / set_pps_hevld /
 * set_tiles_info / set_err_conceal: the same on both engines */
static void hevc_words(struct vdec_dev *d, const struct vdec_hevc_run *run, bool lat)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_slice_params *sh = run->sh;
	u32 h = lat ? LAT_HEVC : CORE_HEVC, top = lat ? LAT_VLD_TOP : CORE_VLD_TOP;
	u32 log2_ctb = run->log2_ctb, min_cb = sps->log2_min_luma_coding_block_size_minus3 + 3;
	u32 tb_min = sps->log2_min_luma_transform_block_size_minus2 + 2;
	u32 tb_max = tb_min + sps->log2_diff_max_min_luma_transform_block_size;
	bool pcm = sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED;
	u32 l0 = 0, l1 = 0, v;
	unsigned int i;
	bool intra;

#define SPF(f) (!!(sps->flags & V4L2_HEVC_SPS_FLAG_##f))
#define PPF(f) (!!(pps->flags & V4L2_HEVC_PPS_FLAG_##f))
#define SHF(f) (!!(sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_##f))
	/* fields a slice type does not code are stale in VA's parameters: the
	 * vendor parser leaves them 0 (cabac_init_flag, mvd_l1_zero_flag) */
	intra = sh->slice_type == V4L2_HEVC_SLICE_TYPE_I;
	if (!intra)
		l0 = sh->num_ref_idx_l0_active_minus1 + 1;
	if (sh->slice_type == V4L2_HEVC_SLICE_TYPE_B)
		l1 = sh->num_ref_idx_l1_active_minus1 + 1;
	/* slice header */
	vdec_w(d, h + 0xbc, (u32)!!(run->dec->flags & V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) << 31 |
	       (sh->slice_type & 3) << 28 | (run->addr_bits & 0xf) << 20);
	vdec_w(d, h + 0xc0, (SHF(SLICE_SAO_LUMA) & SPF(SAMPLE_ADAPTIVE_OFFSET)) << 5 |
	       (SHF(SLICE_SAO_CHROMA) & SPF(SAMPLE_ADAPTIVE_OFFSET)) << 6 | (SHF(CABAC_INIT) && !intra) << 7 |
	       ((26 + pps->init_qp_minus26 + sh->slice_qp_delta) & 0x7f) << 8 |
	       ((sh->slice_cb_qp_offset + pps->pps_cb_qp_offset) & 0x1f) << 16 |
	       (u32)((sh->slice_cr_qp_offset + pps->pps_cr_qp_offset) & 0x1f) << 24);
	/* I slices: no lists, collocated_from_l0 inferred 1, as the vendor parser leaves them */
	vdec_w(d, h + 0xc4, ((l0 - 1) & 0xf) | ((l1 - 1) & 0xf) << 4 |
	       (intra ? 0 : (sh->collocated_ref_idx & 0xf) << 8 |
			    ((5 - sh->five_minus_max_num_merge_cand) & 7) << 12) |
	       (sh->slice_beta_offset_div2 & 0x1f) << 16 | (sh->slice_tc_offset_div2 & 0x1f) << 21 |
	       SHF(SLICE_TEMPORAL_MVP_ENABLED) << 26 | (intra || SHF(COLLOCATED_FROM_L0)) << 27 |
	       (SHF(MVD_L1_ZERO) && sh->slice_type == V4L2_HEVC_SLICE_TYPE_B) << 28 | SHF(SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) << 29 |
	       (u32)SHF(SLICE_DEBLOCKING_FILTER_DISABLED) << 30);
	/* the slice RPS counts (hevc_slice_rps): predicted-from set size, short-term
	 * size, long-term pictures (num_long_term_sps + num_long_term_pics),
	 * num_long_term_sps */
	vdec_w(d, h + 0xc8, (run->rps_pred & 0x1f) | (run->rps_st & 0x3f) << 8 |
	       ((run->lt_sps + run->lt_pics) & 0x1f) << 16 | (u32)(run->lt_sps & 0x1f) << 24);
	vdec_w(d, h + 0xcc, (run->n_curr & 0xf) | ((run->n_curr > 1 ? ceil_log2_min1(run->n_curr) : 0) & 7) << 4);
	vdec_w(d, h + 0xf4, (u32)sh->nal_unit_type << 8 | 0xffff0015);

	/* sequence */
	vdec_w(d, top + 0x68, (run->ctb_h - 1) << 16 | (run->ctb_w - 1));
	vdec_w(d, top + 0x70, sps->pic_width_in_luma_samples | (u32)sps->pic_height_in_luma_samples << 16);
	vdec_w(d, top + 0x94, (run->ctb_w & 0xfff) | (run->ctb_h & 0xfff) << 16);
	if (sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8) {
		vdec_w(d, top + 0x100, (sps->bit_depth_luma_minus8 & 0xf) |
		       (sps->bit_depth_chroma_minus8 & 0xf) << 4);
		/* 10-bit output on both engines */
		vdec_w(d, (lat ? LAT_PP : CORE_PP) + 0xb90, 1);
	}
	/* without PCM the vendor parser leaves 8-bit / 128 / 4096 defaults */
	vdec_w(d, h + 0xa0, (sps->chroma_format_idc & 3) | pcm << 2 | PPF(LISTS_MODIFICATION_PRESENT) << 3 |
	       SPF(AMP_ENABLED) << 4 | SPF(SAMPLE_ADAPTIVE_OFFSET) << 5 | SPF(PCM_LOOP_FILTER_DISABLED) << 6 |
	       SPF(SPS_TEMPORAL_MVP_ENABLED) << 7 |
	       ((pcm ? sps->pcm_sample_bit_depth_luma_minus1 : 7) & 0xf) << 8 |
	       ((pcm ? sps->pcm_sample_bit_depth_chroma_minus1 : 7) & 0xf) << 12 |
	       (sps->num_long_term_ref_pics_sps & 0x3f) << 16 | SPF(LONG_TERM_REF_PICS_PRESENT) << 22 |
	       (u32)(sps->num_short_term_ref_pic_sets & 0x7f) << 24);
	vdec_w(d, h + 0xa4, (ceil_log2_min1(sps->num_long_term_ref_pics_sps) & 7) << 12 |
	       (ceil_log2_min1(sps->num_short_term_ref_pic_sets) & 7) << 8 |
	       ((sps->log2_max_pic_order_cnt_lsb_minus4 + 4) & 0x1f) |
	       SPF(STRONG_INTRA_SMOOTHING_ENABLED) << 16);
	vdec_w(d, h + 0xa8, (min_cb & 7) | (log2_ctb & 7) << 4 | (tb_min & 7) << 8 | (tb_max & 7) << 12 |
	       (((pcm ? sps->log2_min_pcm_luma_coding_block_size_minus3 : 4) + 3) & 7) << 16 |
	       /* Log2MaxIpcmCbSizeY (MIUI's ipcm_A: 5; the emulated parser
	        * leaves the bare difference there, 2, and that breaks PCM) */
	       ((pcm ? sps->log2_min_pcm_luma_coding_block_size_minus3 + 3 +
		 sps->log2_diff_max_min_pcm_luma_coding_block_size : 5) & 7) << 20 |
	       (sps->max_transform_hierarchy_depth_inter & 7) << 24 |
	       (u32)(sps->max_transform_hierarchy_depth_intra & 7) << 28);
	/* picture */
	vdec_w(d, h + 0xac, PPF(SIGN_DATA_HIDING_ENABLED) | PPF(CABAC_INIT_PRESENT) << 1 |
	       PPF(CONSTRAINED_INTRA_PRED) << 2 | PPF(TRANSFORM_SKIP_ENABLED) << 3 |
	       PPF(WEIGHTED_PRED) << 4 | PPF(WEIGHTED_BIPRED) << 5 | PPF(OUTPUT_FLAG_PRESENT) << 6 |
	       PPF(TRANSQUANT_BYPASS_ENABLED) << 7 | PPF(DEPENDENT_SLICE_SEGMENT_ENABLED) << 8 |
	       PPF(ENTROPY_CODING_SYNC_ENABLED) << 9 | PPF(SLICE_SEGMENT_HEADER_EXTENSION_PRESENT) << 11 |
	       (pps->num_ref_idx_l0_default_active_minus1 & 0xf) << 16 |
	       (pps->num_ref_idx_l1_default_active_minus1 & 0xf) << 20 |
	       ((pps->log2_parallel_merge_level_minus2 + 2) & 7) << 24 |
	       (u32)(pps->num_extra_slice_header_bits & 7) << 28);
	vdec_w(d, h + 0xb0, ((pps->init_qp_minus26 + 26) & 0x7f) << 8 | PPF(CU_QP_DELTA_ENABLED) |
	       PPF(PPS_SLICE_CHROMA_QP_OFFSETS_PRESENT) << 1 |
	       ((log2_ctb - pps->diff_cu_qp_delta_depth) & 7) << 4 |
	       (pps->pps_cb_qp_offset & 0x1f) << 16 | (u32)(pps->pps_cr_qp_offset & 0x1f) << 24);
	/* loop_filter_across_tiles_enabled_flag is inferred 1 without tiles */
	vdec_w(d, h + 0xb4, PPF(TILES_ENABLED) | (pps->num_tile_columns_minus1 & 0x1f) << 8 |
	       (pps->num_tile_rows_minus1 & 0x1f) << 16 |
	       (PPF(LOOP_FILTER_ACROSS_TILES_ENABLED) || !PPF(TILES_ENABLED)) << 24);
	vdec_w(d, h + 0xb8, PPF(PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED) |
	       PPF(DEBLOCKING_FILTER_CONTROL_PRESENT) << 1 | PPF(DEBLOCKING_FILTER_OVERRIDE_ENABLED) << 2 |
	       PPF(PPS_DISABLE_DEBLOCKING_FILTER) << 3 | (pps->pps_beta_offset_div2 & 0x1f) << 4 |
	       (pps->pps_tc_offset_div2 & 0x1f) << 9);
	/* tiles: one bit per CTB column / row, set on each tile's last one */
	for (i = 0; i < 16; i++)
		vdec_w(d, h + 0x140 + 4 * i, run->col_bits[i]);
	vdec_w(d, h + 0x1d0, 0);
	for (i = 0; i < 16; i++)
		vdec_w(d, h + 0x180 + 4 * i, run->row_bits[i]);
	vdec_w(d, h + 0x1ec, 0);
	vdec_w(d, h + 0xe0, 0xfffef7fb);
	vdec_w(d, h + 0xd4, 0x4011d01);
	(void)v;
#undef SPF
#undef PPF
#undef SHF
}

static void hevc_prologue(struct vdec_dev *d, bool lat)
{
	if (lat) {
		vdec_hw_init(d);
		vdec_hw_lat_prologue(d, HEVC_GCON_C8, HEVC_GCON_CC, HEVC_GCON_178);
		return;
	}
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, CORE_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);
	vdec_w(d, CORE_MISC + 0xf4, 0);
	vdec_w(d, CORE_MISC + 0xec, 1);
	vdec_w(d, SOC_RACING + 0x84, 1);
	vdec_w(d, CORE_LARB + 0xc, 1);
	vdec_w(d, CORE_VLD + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0xc8, HEVC_GCON_C8);
	vdec_w(d, CORE_MISC + 0xcc, HEVC_GCON_CC);
	vdec_w(d, CORE_MISC + 0x178, HEVC_GCON_178);
	vdec_w(d, CORE_MISC + 0x84, 2);
	vdec_w(d, SOC_GCON + 0x4, 2);
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, CORE_LARB + 0xc, 0x00010000);
	vdec_w(d, SOC_RACING + 0x84, 0);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 1);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 0);
	vdec_w(d, SOC_UFO_ENC + 0x90, 1);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);
}

void vdec_hw_hevc_lat(struct vdec_ctx *ctx, const struct vdec_hevc_run *run)
{
	struct vdec_dev *d = ctx->dev;

	hevc_prologue(d, true);
	vdec_w(d, LAT_VLD + 0xec, 0x10000000);
	vdec_w(d, LAT_HEVC + 0x84, 1);
	hevc_fetch(d, true, run->bs_dma, run->bs_dma + run->bs_size, run->bs_dma + run->first_nal);
	vdec_hw_lat_racing_tail(d);
	hevc_words(d, run, true);
	vdec_w(d, LAT_MISC + 0x4, 0x110);
	vdec_w(d, LAT_PP + 0xcac, 0);
	/* hevc_vld+090 (start) comes from vdec_hw_hevc_start() */
}

/* four coefficients down a column of a raster-order list */
static void hevc_sl_word(struct vdec_dev *d, u32 *idx, const u8 *p, unsigned int n)
{
	vdec_w(d, CORE_VLD + 0x260, (*idx)++);
	vdec_w(d, CORE_VLD + 0x264, p[0] | p[n] << 8 | p[2 * n] << 16 | (u32)p[3 * n] << 24);
}

/*
 * vdec_hal_h265_set_sqt: the DC values, then every list through the VLD's
 * index / data port: 4x4 (6 x 4 words), 8x8, 16x16 (6 x 16), 32x32 (2 x 16),
 * an 8x8 list as its columns' top and bottom halves.
 */
static void hevc_scaling(struct vdec_dev *d, const struct vdec_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_scaling_matrix *sm = run->sm;
	const u8 *dc16, *dc32, *l;
	unsigned int s, m, i;
	u32 idx = 0;

	if (!sm || !(run->sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)) {
		vdec_w(d, CORE_PP + 0x630, 0);
		return;
	}
	dc16 = sm->scaling_list_dc_coef_16x16;
	dc32 = sm->scaling_list_dc_coef_32x32;
	vdec_w(d, CORE_PP + 0x630, 1);
	vdec_w(d, CORE_PP + 0x634, dc16[0] | dc16[1] << 8 | dc16[2] << 16 | (u32)dc16[3] << 24);
	vdec_w(d, CORE_PP + 0x638, dc16[4] | dc16[5] << 8 | dc32[0] << 16 | (u32)dc32[1] << 24);
	vdec_w(d, CORE_PP + 0x504, 1);
	for (m = 0; m < 6; m++)
		for (i = 0; i < 4; i++)
			hevc_sl_word(d, &idx, sm->scaling_list_4x4[m] + i, 4);
	for (s = 0; s < 3; s++)
		for (m = 0; m < (s == 2 ? 2 : 6); m++) {
			l = s == 0 ? sm->scaling_list_8x8[m] :
			    s == 1 ? sm->scaling_list_16x16[m] : sm->scaling_list_32x32[m];
			for (i = 0; i < 8; i++) {
				hevc_sl_word(d, &idx, l + i, 8);
				hevc_sl_word(d, &idx, l + 32 + i, 8);
			}
		}
	vdec_w(d, CORE_PP + 0x504, 0);
}

void vdec_hw_hevc_core(struct vdec_ctx *ctx, const struct vdec_hevc_run *run)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	u32 ctb = BIT(run->log2_ctb) - 1, pitch;
	unsigned int i, l;

	hevc_prologue(d, false);
	vdec_w(d, CORE_HEVC + 0x84, 1);
	vdec_w(d, CORE_VLD_TOP + 0x230, vdec_r(d, CORE_VLD_TOP + 0x230) | 2);
	/* the core reads the LAT's output: window 0..8 KiB */
	hevc_fetch(d, false, 0, 0x2000, 0);
	vdec_w(d, CORE_MISC + 0xa4, 0);
	vdec_w(d, SOC_RACING + 0x9c, 0);
	vdec_w(d, SOC_RACING + 0x58, 2);
	vdec_w(d, CORE_VLD_TOP + 0x398, 0x113);
	vdec_w(d, SOC_RACING + 0x7c, 0);
	vdec_w(d, SOC_RACING + 0x64, 1);

	/* vdec_hal_h265_set_ec_info: every slot points at a picture there is */
	vdec_w(d, CORE_MC + 0xb5c, vdec_r(d, CORE_MC + 0xb5c) | 1);
	for (i = 0; i < 16; i++) {
		vdec_w(d, CORE_MC + 0x45c + 4 * i, run->ec_y);
		vdec_w(d, CORE_MC + 0x4dc + 4 * i, run->ec_y);
		vdec_w(d, CORE_MC + 0xbe0 + 4 * i, run->ec_c);
		vdec_w(d, CORE_MC + 0xc60 + 4 * i, run->ec_c);
		vdec_w(d, CORE_AVC_MV + 4 * i, run->ec_mv >> 4);
		vdec_w(d, CORE_AVC_MV + 0x40 + 4 * i, run->ec_mv >> 4);
	}
	vdec_w(d, CORE_PP + 0x40, 0xf011);
	vdec_w(d, CORE_AVC_MV + 0x210, vdec_r(d, CORE_AVC_MV + 0x210) & ~0xf0u);
	vdec_w(d, SOC_UFO_ENC + 0x8c, 0x11100);
	vdec_w(d, SOC_UFO_ENC + 0x4, 0);
	vdec_w(d, CORE_MC + 0xa60, 0);
	vdec_w(d, CORE_PP + 0xb80, 0);
	vdec_w(d, SOC_UFO_ENC + 0xc, 0x30);

	/* vdec_hal_h265_set_ref_pic_list_reg: RefPicListTemp0 / 1 repeated */
	vdec_w(d, CORE_AVC_MV + 0x320, run->sh->slice_type == V4L2_HEVC_SLICE_TYPE_P);
	if (run->n_curr) {
		u32 lt = 0;

		for (i = 0; i < 16; i++)
			lt |= (u32)run->temp[0][i % run->n_curr].lt << i |
			      (u32)run->temp[1][i % run->n_curr].lt << (i + 16);
		vdec_w(d, CORE_AVC_MV + 0x324, lt);
		/* the long-term pictures of the DPB: id + 1 a byte, 0 ends */
		for (i = 0; i < 4; i++) {
			u32 w = 0, k;

			for (k = 0; k < 4; k++)
				if (4 * i + k < run->n_lt_ids)
					w |= (u32)((run->lt_ids[4 * i + k] + 1) & 0x1f) << (8 * k);
			vdec_w(d, CORE_AVC_MV + 0x328 + 4 * i, w);
		}
		for (l = 0; l < 2; l++)
			for (i = 0; i < 16; i++) {
				const struct vdec_hevc_ref *r = &run->temp[l][i % run->n_curr];

				vdec_w(d, CORE_AVC_MV + (l ? 0x378 : 0x338) + 4 * i,
				       ((r->id + 1) & 0x1f) << 8 | (u8)r->poc_diff);
			}
		for (l = 0; l < 2; l++)
			for (i = 0; i < 16; i++) {
				const struct vdec_hevc_ref *r = &run->temp[l][i % run->n_curr];

				vdec_w(d, CORE_AVC_MV + (l ? 0x40 : 0) + 4 * i, r->mv >> 4);
				vdec_w(d, CORE_MC + (l ? 0x4dc : 0x45c) + 4 * i, r->y);
				vdec_w(d, CORE_MC + (l ? 0xc60 : 0xbe0) + 4 * i, r->c);
			}
	}

	/* vdec_hal_h265_init_pic_info */
	pitch = (ALIGN(sps->pic_width_in_luma_samples, 64) >> 4) & ~3u;
	vdec_w(d, CORE_MC + 0x208, sps->pic_width_in_luma_samples);
	vdec_w(d, CORE_MC + 0x20c, sps->pic_height_in_luma_samples);
	vdec_w(d, CORE_MC + 0x980, pitch);
	vdec_w(d, CORE_MC + 0xcec, sps->bit_depth_luma_minus8 == 2 && sps->bit_depth_chroma_minus8 == 2);
	vdec_w(d, CORE_MC + 0x220, 1);
	vdec_w(d, CORE_MC + 0x224, run->y_dma >> 9);
	vdec_w(d, CORE_MC + 0x228, run->c_dma >> 8);
	vdec_w(d, CORE_MC + 0x22c, pitch);
	vdec_w(d, CORE_MC + 0x238, sps->chroma_format_idc ? 3 : 2);
	vdec_w(d, CORE_MC + 0x250, 1);
	vdec_w(d, CORE_MC + 0x260, (((sps->pic_width_in_luma_samples + ctb) & ~ctb) >> 4) - 1);
	vdec_w(d, CORE_MC + 0x264, (((sps->pic_height_in_luma_samples + ctb) & ~ctb) >> 4) - 1);
	vdec_w(d, CORE_AVC_MV + 0x20c, run->mv_dma >> 4);
	if (sps->bit_depth_luma_minus8)
		vdec_w(d, CORE_PP + 0xb8c, 1);		/* MT2110T ("jump mode") */
	hevc_scaling(d, run);

	hevc_words(d, run, false);
	vdec_w(d, CORE_VLD_TOP + 0x234, 1);
	vdec_w(d, CORE_VLD_TOP + 0x23c, run->ube->dma + run->ube->size);
	vdec_w(d, CORE_VLD_TOP + 0x240, run->ube->dma);
	vdec_w(d, CORE_VLD_TOP + 0x230, vdec_r(d, CORE_VLD_TOP + 0x230) | 2);
	vdec_w(d, CORE_HEVC + 0x88, 0x101);
	vdec_w(d, CORE_HEVC + 0x88, 0);
	vdec_w(d, CORE_MISC + 0x240, 0);
	vdec_w(d, CORE_MISC + 0x4, 0x110);
	vdec_w(d, CORE_PP + 0xcac, 0x11);
	/* hevc_vld+090 (start) comes from vdec_hw_hevc_start() */
}

/* bring-up: the register blocks MIUI's tracer snapshots before each start
 * (non-zero words only; no VLD / BS2 / HEVC blocks, reading those shifts) */
static int hevc_snap;
module_param(hevc_snap, int, 0644);
MODULE_PARM_DESC(hevc_snap, "print LAT / core register snapshots before the next N starts");

static void hevc_snapshot(struct vdec_dev *d, bool lat)
{
	static const u32 lblk[][2] = {
		{ 0x00000, 0xd000 }, { 0x10000, 0x1000 }, { 0x11800, 0x800 },
		{ 0x13000, 0x800 }, { 0x16000, 0x1000 },
	};
	static const u32 cblk[][2] = {
		{ 0x20800, 0x800 }, { 0x21000, 0x1000 }, { 0x23000, 0x800 },
		{ 0x24000, 0x1000 }, { 0x25000, 0x400 },
	};
	const u32 (*blk)[2] = lat ? lblk : cblk;
	unsigned int i, o, n = 0;

	for (i = 0; i < 5; i++)
		for (o = 0; o < blk[i][1]; o += 4) {
			u32 v;

			v = readl(d->regs + blk[i][0] + o);
			if (!v)
				continue;
			pr_info("hsnap %c %05x %08x\n", lat ? 'L' : 'C', blk[i][0] + o, v);
			if (!(++n % 64))
				usleep_range(1000, 2000);
		}
}

void vdec_hw_hevc_start(struct vdec_dev *d)
{
	bool snap = hevc_snap > 0;

	if (snap) {
		hevc_snap--;
		hevc_snapshot(d, true);
	}
	vdec_w(d, LAT_HEVC + 0x90, 1);
	vdec_w(d, LAT_MISC + 0xa4, 0x11);
	udelay(50);
	if (snap)
		hevc_snapshot(d, false);
	vdec_w(d, CORE_HEVC + 0x90, 1);
	vdec_w(d, CORE_VLD + 0xa4, 0x11);
}
