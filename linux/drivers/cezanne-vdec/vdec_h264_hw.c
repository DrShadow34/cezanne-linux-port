// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: H.264 register programming for the MT6885 decoder.
 *
 * Every picture is one LAT job (bitstream parsing, 0x16010000 block) plus
 * one core job (reconstruction, 0x16020000); they run together over the
 * on-chip "inner racing" ring, so both must be programmed before either
 * starts. The sequences below reproduce the MIUI vpud captures
 * (mainline/vcodec/trace/seg2/h264-*-*-0/-1 are the reference jobs);
 * writes the captures performed read-modify-write with the value they read
 * are reproduced as rmw, reads-only are dropped. Field layouts of the
 * avc_vld header words: vdec_h264.c.
 */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/delay.h>

#include "vdec.h"

/*
 * UBE (LAT -> core transfer) and error-map working buffers of the core,
 * MIUI rotates 3 slots. The UBE holds the LAT's output for the whole
 * picture: MIUI's 1080p slots were up to 3.8 MiB, mainline mtk-vcodec on
 * the MT8192-class hardware uses 6 MiB up to 1080p and 30 MiB above. A
 * 1 MiB buffer cut 1080p and 960x720 I pictures short.
 */
size_t vdec_hw_ube_size(u32 mb_w, u32 mb_h)
{
	return (mb_w > 120 || mb_h > 68) ? 30 * SZ_1M : 6 * SZ_1M;
}

size_t vdec_hw_err_size(u32 mb_w, u32 mb_h)
{
	return max_t(size_t, SZ_2M, (size_t)mb_w * mb_h * 256);
}

/* block bring-up, repeated before every LAT job: ungates both vdecsys
 * blocks and clears the racing pointers, as the first LAT / core job
 * prologues in the capture (later jobs carry most of it again). */
void vdec_hw_init(struct vdec_dev *d)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		vdec_w(d, SOC_RACING + 0x100 + i * 4, 0);
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0x218, 0x111);
	vdec_w(d, SOC_GCON + 0xf8, 0);
	vdec_w(d, LAT_MISC + 0xf4, 0);
	vdec_w(d, CORE_MISC + 0xf4, 0);
	vdec_w(d, SOC_GCON + 0x0, 0x00020002);	/* LAT job */
	vdec_w(d, SOC_GCON + 0x4, 0x2);		/* core job */
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);	/* core job */
	vdec_w(d, SOC_UFO_ENC + 0x8c, 0x11100);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);
	vdec_w(d, SOC_UFO_ENC + 0x4, 0);
	vdec_w(d, SOC_UFO_ENC + 0xc, 0x30);
	vdec_w(d, SOC_2000 + 0x4, 1);
	/* 10-bit output (MT2110T, "jump mode") off unless the job sets it */
	vdec_w(d, LAT_PP + 0xb90, 0);
	vdec_w(d, CORE_PP + 0xb90, 0);
	vdec_w(d, CORE_PP + 0xb8c, 0);
	vdec_w(d, CORE_MC + 0xcec, 0);
}

/*
 * LAT -> core ring watermarks (vld_top+0x200..0x22c, +0x230, +0x260..+0x268).
 * MIUI's kernel programs them once per stream (they never show in the
 * userspace trace); this is a transcription of that function (stock
 * kernel Image, 0x7c8eb8) for the NV12 / MM21 output: with
 * n = w * h * 60 / 128000, luma thresholds L(k) = max(228 - (k*n + 99) / 100, 1),
 * chroma ones C(k) = max(109 - (k*n + 399) / 400, 1) and
 * D(k) = max(109 - ceil(k*n / 200), 1). Left at zero, 1080p pictures come
 * out with scattered wrong macroblocks.
 */
void vdec_hw_ring(struct vdec_dev *d, u32 top, u32 mb_w, u32 mb_h)
{
	u32 n = mb_w * 16 * mb_h * 16 / 128000 * 60 +
		(mb_w * 16 * mb_h * 16 % 128000) * 60 / 128000;
	u32 l[10], c[10], e[10], k;

	for (k = 3; k <= 9; k++) {
		u32 ln = (k * n + 99) / 100, cn = (k * n + 399) / 400;
		u32 dn = (k * n + 199) / 200;

		l[k] = ln < 228 ? 228 - ln : 1;
		c[k] = cn < 109 ? 109 - cn : 1;
		e[k] = dn < 109 ? 109 - dn : 1;
	}
	vdec_w(d, top + 0x200, l[6] << 16 | l[7]);
	vdec_w(d, top + 0x204, l[4] << 16 | l[6]);
	vdec_w(d, top + 0x208, e[6] << 16 | e[7]);
	vdec_w(d, top + 0x20c, e[4] << 16 | e[6]);
	vdec_w(d, top + 0x210, c[6] << 16 | c[7]);
	vdec_w(d, top + 0x214, c[4] << 16 | c[6]);
	vdec_w(d, top + 0x218, l[8] << 16 | l[9]);
	vdec_w(d, top + 0x21c, l[6] << 16 | l[8]);
	vdec_w(d, top + 0x220, e[8] << 16 | e[9]);
	vdec_w(d, top + 0x224, e[6] << 16 | e[8]);
	vdec_w(d, top + 0x228, c[8] << 16 | c[9]);
	vdec_w(d, top + 0x22c, c[6] << 16 | c[8]);
	vdec_w(d, top + 0x230, l[6] << 16);	/* the job sets bit 0 */
	vdec_w(d, top + 0x260, l[3] << 16 | l[4]);
	vdec_w(d, top + 0x264, e[3] << 16 | e[4]);
	vdec_w(d, top + 0x268, c[3] << 16 | c[4]);
}


/* LAT job tail of the racing setup (counters, constants), both codecs */
void vdec_hw_lat_racing_tail(struct vdec_dev *d)
{
	vdec_w(d, SOC_RACING + 0x98, 0);
	vdec_w(d, LAT_MISC + 0x81c, 1);
	vdec_w(d, SOC_RACING + 0x54, 2);
	vdec_w(d, LAT_VLD_TOP + 0x398, 0x113);
	vdec_w(d, SOC_RACING + 0x78, 0);
	vdec_w(d, LAT_MISC + 0x850, 0);
	vdec_w(d, LAT_MISC + 0x844, 0x2000);
	vdec_w(d, SOC_RACING + 0x60, 1);
	vdec_w(d, SOC_RACING + 0x5c, 1);
	vdec_w(d, LAT_VLD_TOP + 0x230, 1);
}

void vdec_hw_racing_cfg(struct vdec_dev *d)
{
	vdec_w(d, SOC_RACING + 0x30, 1);
	vdec_w(d, SOC_RACING + 0x58, 2);
	vdec_w(d, SOC_RACING + 0x68, 0x1200);
	vdec_w(d, SOC_RACING + 0x74, 0x200);
	vdec_w(d, SOC_RACING + 0x88, 0x1200);
	vdec_w(d, SOC_RACING + 0x94, 0x100010);
	vdec_w(d, SOC_RACING + 0xac, 0xfffffff);
	vdec_w(d, SOC_RACING + 0xb0, 0xfffffff);
	vdec_w(d, SOC_RACING + 0xc8, 1);
	vdec_w(d, SOC_RACING + 0xd0, 7);
}

/* LAT job prologue shared by the codecs: gates, larb5 sleep-protect
 * handshake, the misc constants (codec-specific values) under vld+108 */
void vdec_hw_lat_prologue(struct vdec_dev *d, u32 c8, u32 cc, u32 r178)
{
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0x218, 0x111);
	vdec_w(d, SOC_GCON + 0xf8, 0);
	vdec_w(d, LAT_MISC + 0xf4, 0);
	vdec_w(d, SOC_GCON + 0x218, 0x111);
	vdec_w(d, LAT_MISC + 0xec, 1);
	vdec_w(d, LAT_LARB + 0xc, 1);
	vdec_w(d, LAT_VLD + 0x108, 0x101);
	vdec_w(d, LAT_MISC + 0xc8, c8);
	vdec_w(d, LAT_MISC + 0xcc, cc);
	vdec_w(d, LAT_MISC + 0x178, r178);
	vdec_w(d, LAT_MISC + 0x84, 2);
	vdec_w(d, LAT_VLD + 0x108, 0);
	vdec_w(d, LAT_LARB + 0xc, 0x00010000);
}

/*
 * Scaling lists (MIUI cqm captures), on each engine before its header
 * words: per list pp+504 = 1, the coefficients in raster order (as the
 * V4L2 control) as byte address / two bytes (low byte first) on
 * vld+260 / +264, pp+504 = 0. The six 4x4 lists at 16 * i, the 8x8
 * Intra Y list at 0x60, Inter Y at 0xa0 (4:2:0 has no others). The
 * engines use an uploaded list only with its flag in +094 [17:10]; with
 * bit 18 alone they use the built-in default (JVT) matrices.
 */
static void vdec_hw_h264_scaling(struct vdec_dev *d, u32 vld, u32 pp,
				 const struct v4l2_ctrl_h264_scaling_matrix *sm)
{
	unsigned int i, k, n, a;
	const u8 *l;

	if (!sm)
		return;
	for (i = 0; i < 8; i++) {
		l = i < 6 ? sm->scaling_list_4x4[i] : sm->scaling_list_8x8[i - 6];
		n = i < 6 ? 16 : 64;
		a = i < 6 ? 16 * i : 0x60 + 0x40 * (i - 6);
		vdec_w(d, pp + 0x504, 1);
		for (k = 0; k < n; k += 2) {
			vdec_w(d, vld + 0x260, a + k);
			vdec_w(d, vld + 0x264, l[k] | l[k + 1] << 8);
		}
		vdec_w(d, pp + 0x504, 0);
	}
}

/*
 * LAT job prologue (capture job 0 up to the header words). The block
 * registers are updated under vld+108 = 0x101 ... 0; larb5 and the
 * soc.4000+080 bit wrap the misc constants (SMI sleep protection while
 * the engine's clock is gated - do the handshake by hand, no SMI driver
 * user yet).
 */
void vdec_hw_h264_lat(struct vdec_ctx *ctx, const struct vdec_h264_run *run)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	unsigned int w = sps->pic_width_in_mbs_minus1 + 1;
	unsigned int h = (sps->pic_height_in_map_units_minus1 + 1) * 16;
	bool cabac = run->pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	vdec_hw_init(d);

	/* MIUI job0 order: racing counters, gcon, larb5 handshake, misc
	 * constants under the vld+108 window, then the CABAC reset pulse */
	vdec_r(d, SOC_RACING + 0x74);
	vdec_w(d, SOC_RACING + 0x100, 0);
	vdec_w(d, SOC_RACING + 0x104, 0);
	vdec_w(d, SOC_RACING + 0x108, 0);
	vdec_w(d, SOC_RACING + 0x10c, 0);
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0x218, 0x111);
	vdec_w(d, SOC_GCON + 0xf8, 0);
	vdec_w(d, SOC_GCON + 0x218, vdec_r(d, SOC_GCON + 0x218));
	vdec_w(d, LAT_MISC + 0xec, 1);

	vdec_w(d, SOC_RACING + 0x80, 1);
	vdec_w(d, LAT_LARB + 0xc, 1);
	vdec_r(d, LAT_LARB + 0xc);
	vdec_w(d, LAT_LARB + 0xc, 0x00010000);
	vdec_r(d, LAT_LARB + 0xc);
	vdec_w(d, SOC_RACING + 0x80, 0);

	/* misc constants (MIUI job0: c8/cc/178 inside the vld+108 window) */
	vdec_w(d, LAT_VLD + 0x108, 0x101);
	vdec_w(d, LAT_MISC + 0xc8, 0x0000fff7);
	vdec_w(d, LAT_MISC + 0xcc, 0x51e30180);
	vdec_w(d, LAT_MISC + 0x178, 0xfffffe60);
	vdec_w(d, LAT_MISC + 0x84, 2);
	vdec_w(d, LAT_VLD + 0x108, 0);

	/* CABAC engine reset pulse (MIUI job0: +084 bit31 1 -> 0) */
	vdec_w(d, LAT_AVC_VLD + 0x84, 0xa880c007);
	vdec_w(d, LAT_VLD_TOP + 0x54, vdec_r(d, LAT_VLD_TOP + 0x54) | 1);
	vdec_w(d, LAT_AVC_VLD + 0x2c8, 0xf900a03b);
	vdec_w(d, LAT_AVC_VLD + 0x2d0, 0x101);
	vdec_w(d, LAT_AVC_VLD + 0x2d4, 0x00204008);
	vdec_w(d, LAT_AVC_MV + 0x210, 4);
	vdec_w(d, LAT_AVC_VLD + 0x28c, 1);
	vdec_w(d, LAT_AVC_VLD + 0x28c, 0);
	vdec_w(d, LAT_VLD_TOP + 0x2ac, 0x10);
	vdec_w(d, LAT_VLD + 0x140, 0x00100014);
	vdec_w(d, LAT_VLD + 0xec, 0x10000000);
	vdec_w(d, LAT_AVC_VLD + 0x84, 0x0880c007);

	/* bitstream ring: read pointer, start and wrap end in 64-byte units
	 * (MIUI feeds a 128 KiB ring; here the whole source plane, so an
	 * access unit larger than 128 KiB does not wrap onto itself) */
	vdec_w(d, LAT_VLD + 0xb4, run->bs_dma >> 6);
	vdec_w(d, LAT_VLD + 0xb8, (run->bs_dma + run->bs_size) >> 6);
	vdec_w(d, LAT_VLD + 0xb0, run->bs_dma);
	vdec_w(d, LAT_VLD_TOP + 0x1c, 0);
	if (!cabac) {
		/* CAVLC: NAL size incl. 3-byte start code and 2 pad bytes */
		vdec_w(d, LAT_VLD_TOP + 0x18, (run->bs_len + 5) * 8);
		vdec_w(d, LAT_AVC_VLD + 0x2ec, 0);
		vdec_w(d, LAT_AVC_VLD + 0x234, 0);
	}

	vdec_hw_lat_racing_tail(d);

	vdec_hw_racing_cfg(d);

	/* LAT's UBE write range (vld_top+23c/240). MIUI's core job writes
	 * +0x23c = high end, +0x240 = low start of ONE ~4MB UBE buffer
	 * (0xffa4e000 - 0xff682000 = 0x3cc000). Mirror that here: 0x240 =
	 * buffer base, 0x23c = base + size. (The kernel also sets a ring
	 * geometry block at vld_top+0x200..0x22c that the trace never shows;
	 * testing whether the range pair alone lets LAT emit residuals.) */
	vdec_w(d, LAT_VLD_TOP + 0x23c, run->ube->dma + run->ube->size);
	vdec_w(d, LAT_VLD_TOP + 0x240, run->ube->dma);

	/* header words and geometry, then the +084 latch */
	vdec_hw_h264_scaling(d, LAT_VLD, LAT_PP, run->sm);
	vdec_w(d, LAT_AVC_VLD + 0x88, run->words.sps_word);
	vdec_w(d, LAT_VLD_TOP + 0x68,
	       (((h / 16 - 1) & 0xffff) << 16) | ((w - 1) & 0xffff));
	vdec_w(d, LAT_AVC_VLD + 0x90, run->words.pps_word);
	vdec_w(d, LAT_AVC_VLD + 0x94, run->words.ref_word);
	vdec_w(d, LAT_AVC_VLD + 0x98, run->words.slice_word);
	vdec_w(d, LAT_AVC_VLD + 0x8c, 0);
	vdec_w(d, LAT_AVC_VLD + 0x8c, 0);
	vdec_w(d, LAT_PP + 0xcac, 0);
	vdec_w(d, LAT_MISC + 0xa4, 0);
	vdec_w(d, LAT_AVC_VLD + 0x84, 0x0e80c007);
	vdec_w(d, LAT_AVC_VLD + 0x84, 0x0e80c007);

	/* tail group; +09c (slice word) lands after the latch */
	vdec_w(d, LAT_AVC_VLD + 0x26c, 0xbfdfb8bf);
	vdec_w(d, LAT_AVC_VLD + 0x280, 0xbfdfb8ff);
	vdec_w(d, LAT_AVC_VLD + 0x2d0, 0x48001101);
	vdec_w(d, LAT_AVC_VLD + 0x2d4, 0x00204008);
	vdec_w(d, LAT_AVC_VLD + 0x2e8, vdec_r(d, LAT_AVC_VLD + 0x2e8));
	vdec_w(d, LAT_AVC_VLD + 0x9c, run->words.type_word);
	if (cabac) {
		vdec_w(d, LAT_AVC_VLD + 0x2ec, 0x80000000);
		vdec_w(d, LAT_AVC_VLD + 0x234, 1);
	}
	/* MIUI job0: +06c = total MB count, +070 = aligned h <<16 | w pixels */
	vdec_w(d, LAT_VLD_TOP + 0x6c, w * h / 16);
	vdec_w(d, LAT_VLD_TOP + 0x70, h << 16 | w * 16);
	vdec_w(d, LAT_VLD_TOP + 0x9c, 0x07000800);
	vdec_w(d, LAT_PP + 0x40, 0x3011);
	vdec_w(d, LAT_AVC_VLD + 0x290, 0);
	vdec_w(d, LAT_VLD_TOP + 0x230, vdec_r(d, LAT_VLD_TOP + 0x230) | 1);
	vdec_w(d, LAT_VLD_TOP + 0x2ac, vdec_r(d, LAT_VLD_TOP + 0x2ac) | 1);
	/* avc_vld+238 (start) comes from vdec_hw_start() */
}

/*
 * Reference tables of the core, in the capture's order. Every table is
 * first filled with the current picture (32 rows), then:
 *  - the DPB: one row per reference frame, PicNum descending:
 *    mc+3dc luma, mc+b60 chroma, avc_vld+0a8 FrameNumWrap,
 *    avc_mv+000 (2*id) << 22 | 0x200000 and
 *    avc_mv+004 (2*id + 1) << 22 | 0x600000 (id: see vdec_hw_ref_info);
 *  - list table 1 = reference list 0: mc+45c, mc+be0, avc_vld+128,
 *    avc_mv+080 / +084 as above plus the top / bottom POC in the low bits;
 *  - list table 2 = reference list 1 (B) or list 0 again (P): mc+4dc,
 *    mc+c60, avc_vld+1a8, avc_mv+100 / +104, and the motion-vector buffers
 *    in pairs: avc_mv+180+8j = addr >> 4, +184+8j = the same | 2.
 * A list entry whose picture is gone, and the unused rows, stand in for
 * the current picture.
 */
struct vdec_hw_ref {
	dma_addr_t y, c, mv;
	u32 fn, pn_top, pn_bot;
	s32 poc_top, poc_bot;
};

/*
 * One picture in the core tables. MIUI's r16p capture (16 references,
 * frame_num wrapping at 32) shows what the fields hold:
 * - avc_vld+0a8 / +128 / +1a8 rows: FrameNumWrap, 19-bit two's complement
 *   (0x7ffff for -1 after the wrap); the core parses the slice header
 *   itself and needs the PicNums for the list modification;
 * - avc_mv rows [31:22]: 2 * a buffer id (+1 for the bottom field), the
 *   same for a picture as long as it is in the DPB. MIUI uses its frame
 *   buffer slot (decode index mod 17 there), which equals frame_num on
 *   short clips; 2 * FrameNumWrap went wrong after the first wrap. The
 *   capture buffer index is our slot.
 */
static void vdec_hw_ref_info(const struct vdec_h264_run *run, unsigned int e,
			     struct vdec_hw_ref *r)
{
	u32 id;

	if (e >= V4L2_H264_NUM_DPB_ENTRIES || !run->ref_y[e]) {
		r->y = run->y_dma;
		r->c = run->c_dma;
		r->mv = run->mv_dma;
		r->fn = run->dec->frame_num;
		id = run->dst->vb2_buf.index;
		r->poc_top = run->poc_top;
		r->poc_bot = run->poc_bottom;
	} else {
		const struct v4l2_h264_dpb_entry *de = &run->dec->dpb[e];

		r->y = run->ref_y[e];
		r->c = run->ref_c[e];
		r->mv = run->ref_mv[e];
		r->fn = run->ref_lt[e] ? de->frame_num : (u32)run->ref_fnw[e] & 0x7ffff;
		id = run->ref_buf[e];
		r->poc_top = de->top_field_order_cnt;
		r->poc_bot = de->bottom_field_order_cnt;
	}
	r->pn_top = (2 * id) << 22 | 0x200000;
	r->pn_bot = (2 * id + 1) << 22 | 0x600000;
}

static void vdec_hw_h264_tables(struct vdec_dev *d, const struct vdec_h264_run *run)
{
	struct vdec_hw_ref r;
	unsigned int i;

	for (i = 0; i < 32; i++) {
		vdec_w(d, CORE_MC + 0x3dc + 4 * i, run->y_dma);
		vdec_w(d, CORE_MC + 0x45c + 4 * i, run->y_dma);
		vdec_w(d, CORE_MC + 0x4dc + 4 * i, run->y_dma);
		vdec_w(d, CORE_MC + 0xb60 + 4 * i, run->c_dma);
		vdec_w(d, CORE_MC + 0xbe0 + 4 * i, run->c_dma);
		vdec_w(d, CORE_MC + 0xc60 + 4 * i, run->c_dma);
		vdec_w(d, CORE_AVC_MV + 0x180 + 4 * i, run->mv_dma >> 4);
	}
	for (i = 0; i < run->n_dpb; i++) {
		vdec_hw_ref_info(run, run->dpb_order[i], &r);
		vdec_w(d, CORE_MC + 0x3dc + 4 * i, r.y);
		vdec_w(d, CORE_MC + 0xb60 + 4 * i, r.c);
		vdec_w(d, CORE_AVC_VLD + 0xa8 + 4 * i, r.fn);
		vdec_w(d, CORE_AVC_MV + 0x000 + 8 * i, r.pn_top);
		vdec_w(d, CORE_AVC_MV + 0x004 + 8 * i, r.pn_bot);
	}
	for (i = 0; i < run->n_l0 && i < 32; i++) {
		vdec_hw_ref_info(run, run->l0[i], &r);
		vdec_w(d, CORE_MC + 0x45c + 4 * i, r.y);
		vdec_w(d, CORE_MC + 0xbe0 + 4 * i, r.c);
		vdec_w(d, CORE_AVC_VLD + 0x128 + 4 * i, r.fn);
		vdec_w(d, CORE_AVC_MV + 0x080 + 8 * i, r.pn_top | (r.poc_top & 0x1fffff));
		vdec_w(d, CORE_AVC_MV + 0x084 + 8 * i, r.pn_bot | (r.poc_bot & 0x1fffff));
	}
	for (i = 0; i < run->n_l1 && i < 16; i++) {
		vdec_hw_ref_info(run, run->l1[i], &r);
		vdec_w(d, CORE_MC + 0x4dc + 4 * i, r.y);
		vdec_w(d, CORE_MC + 0xc60 + 4 * i, r.c);
		vdec_w(d, CORE_AVC_VLD + 0x1a8 + 4 * i, r.fn);
		vdec_w(d, CORE_AVC_MV + 0x100 + 8 * i, r.pn_top | (r.poc_top & 0x1fffff));
		vdec_w(d, CORE_AVC_MV + 0x104 + 8 * i, r.pn_bot | (r.poc_bot & 0x1fffff));
		vdec_w(d, CORE_AVC_MV + 0x180 + 8 * i, r.mv >> 4);
		vdec_w(d, CORE_AVC_MV + 0x184 + 8 * i, (r.mv >> 4) | 2);
	}
}

/* core job prologue (capture job 1 up to the reference tables) */
static void vdec_hw_h264_core_pre(struct vdec_ctx *ctx,
				  const struct vdec_h264_run *run)
{
	struct vdec_dev *d = ctx->dev;

	vdec_w(d, CORE_MISC + 0x160, vdec_r(d, CORE_MISC + 0x160));
	/* ungates the core clock outputs of vdecsys (per job, MIUI): without
	 * this the whole 0x1602xxxx block stays clock-gated and dark */
	vdec_w(d, CORE_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);
	vdec_w(d, CORE_MISC + 0xf4, 0);
	vdec_w(d, CORE_MISC + 0xec, 1);
	vdec_w(d, SOC_MCORE_TOP + 0x348,
	       vdec_r(d, SOC_MCORE_TOP + 0x348) | 1);

	vdec_w(d, CORE_VLD + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0xc8, ~0);
	vdec_w(d, CORE_MISC + 0xcc, ~0);
	vdec_w(d, CORE_MISC + 0x178, ~0);
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 1);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 0);
	/* MIUI job1 pulses ufo_enc+090 1 -> 0 (leaving it set gates writes) */
	vdec_w(d, SOC_UFO_ENC + 0x90, 1);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);

	vdec_w(d, SOC_RACING + 0x84, 1);
	vdec_w(d, CORE_LARB + 0xc, 1);
	vdec_w(d, CORE_VLD + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0xc8, 0x0000fff7);
	vdec_w(d, CORE_MISC + 0xcc, 0x51e30180);
	vdec_w(d, CORE_MISC + 0x178, 0xfffffe60);
	vdec_w(d, CORE_MISC + 0x84, 2);
	vdec_w(d, SOC_GCON + 0x4, 2);	/* MIUI job1: soc gcon clear, protected */
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, CORE_LARB + 0xc, 0x00010000);
	vdec_w(d, SOC_RACING + 0x84, 0);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 1);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 0);

	vdec_w(d, CORE_VLD + 0xec, 0x10000000);
	vdec_w(d, CORE_AVC_VLD + 0x84, 1);
	vdec_w(d, CORE_AVC_VLD + 0x2c8, vdec_r(d, CORE_AVC_VLD + 0x2c8));
	vdec_w(d, CORE_VLD + 0xc0, 0x00040000);
	vdec_w(d, CORE_VLD_TOP + 0x54, vdec_r(d, CORE_VLD_TOP + 0x54) | 1);
	vdec_w(d, CORE_MC + 0x24, 6);
	vdec_w(d, CORE_AVC_VLD + 0x28c, 1);
	vdec_w(d, CORE_VLD_TOP + 0x2ac, vdec_r(d, CORE_VLD_TOP + 0x2ac) | 0x11);
	/* MIUI reads back 0x00100014 here and writes it back; on this boot
	 * chain the register holds 0x01100014 (bit 24 set), so write the
	 * captured value instead of keeping ours (the LAT gets the same
	 * constant explicitly) */
	vdec_w(d, CORE_VLD + 0x140, 0x00100014);
	vdec_w(d, CORE_VLD + 0xec, 0x10000000);
	vdec_w(d, CORE_AVC_VLD + 0x84, 1);
	/* the core consumes the LAT output ring, not the bitstream */
	vdec_w(d, CORE_VLD + 0xb4, 0);
	vdec_w(d, CORE_VLD + 0xb8, 0x80);
	vdec_w(d, CORE_VLD + 0xb0, 0);
	vdec_w(d, CORE_VLD_TOP + 0x1c, 0);
	vdec_w(d, SOC_RACING + 0x9c, 0);
	vdec_w(d, SOC_RACING + 0x58, 2);
	vdec_w(d, CORE_VLD_TOP + 0x398, 0x113);
	vdec_w(d, SOC_RACING + 0x7c, 0);
	vdec_w(d, SOC_RACING + 0x64, 1);
	vdec_w(d, CORE_AVC_VLD + 0x28c, 0);
}

/* core job tail: DPB, headers, output addresses, start (capture job 1) */
static void vdec_hw_h264_core_post(struct vdec_ctx *ctx,
				   const struct vdec_h264_run *run)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	unsigned int mb_w = sps->pic_width_in_mbs_minus1 + 1;
	unsigned int mb_h = sps->pic_height_in_map_units_minus1 + 1;
	unsigned int w = mb_w * 16, h = mb_h * 16;
	bool cabac = run->pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	unsigned int i;

	/* MIUI job1: mc+500..55c all carry the CURRENT luma address (write
	 * back ports); zeroed, the core stalls on its first reconstruction
	 * write and never raises done */
	for (i = 0; i < 23; i++)
		vdec_w(d, CORE_MC + 0x500 + 4 * i, run->y_dma);

	/* MIUI job1 order: the header-word group must land BEFORE avc_vld+084
	 * latches the config (+088's ref-count field is masked after it) */
	vdec_w(d, CORE_AVC_VLD + 0x228, 1);
	/* MIUI: 0x20 with max_num_ref_frames 8 and 16, 0x10 with 3 and 6
	 * (32 DPB field rows instead of 16?) */
	vdec_w(d, CORE_AVC_VLD + 0xa0, run->sps->max_num_ref_frames >= 8 ? 0x20 : 0x10);
	vdec_w(d, CORE_AVC_VLD + 0xa4, run->dec->frame_num);
	vdec_hw_h264_tables(d, run);
	/* current picture: top / bottom field POC and PicOrderCnt */
	vdec_w(d, CORE_AVC_MV + 0x204, run->poc_top);
	vdec_w(d, CORE_AVC_MV + 0x208, run->poc_bottom);
	vdec_w(d, CORE_AVC_MV + 0x200, min(run->poc_top, run->poc_bottom));
	vdec_w(d, CORE_MC + 0x564, 0);
	vdec_w(d, CORE_MC + 0x568, 0);
	vdec_w(d, CORE_MC + 0x56c, 0);
	vdec_hw_h264_scaling(d, CORE_VLD, CORE_PP, run->sm);
	vdec_w(d, CORE_AVC_VLD + 0x88, run->words.sps_word);
	vdec_w(d, CORE_VLD_TOP + 0x68, (mb_h - 1) << 16 | (mb_w - 1));
	vdec_w(d, CORE_AVC_VLD + 0x90, run->words.pps_word);
	vdec_w(d, CORE_AVC_VLD + 0x94, run->words.ref_word);
	vdec_w(d, CORE_AVC_VLD + 0x98, run->words.slice_word);
	vdec_w(d, CORE_AVC_VLD + 0x8c, 0);
	vdec_w(d, CORE_AVC_VLD + 0x8c, 0);
	vdec_w(d, CORE_MISC + 0x4, 0x110);
	vdec_w(d, CORE_PP + 0xcac, 0x111);
	vdec_w(d, CORE_PP + 0xcac, 0x11);
	vdec_w(d, CORE_MISC + 0xa4, 0);
	vdec_w(d, CORE_MISC + 0xa4, 0);
	vdec_w(d, CORE_AVC_VLD + 0x84, 0x00000001);
	vdec_w(d, CORE_AVC_VLD + 0x84, 0x0680c001);
	vdec_w(d, CORE_AVC_VLD + 0x84, 0x0680c001);
	vdec_w(d, CORE_AVC_VLD + 0x26c, 0xbfdfb8bf);
	vdec_w(d, CORE_AVC_VLD + 0x280, 0xbfdfb8ff);
	vdec_w(d, CORE_AVC_VLD + 0x2d0, 0x48001101);
	vdec_w(d, CORE_AVC_VLD + 0x2d4, 0x00204008);
	vdec_w(d, CORE_PP + 0x40, vdec_r(d, CORE_PP + 0x40));
	vdec_w(d, CORE_AVC_VLD + 0x2e8, vdec_r(d, CORE_AVC_VLD + 0x2e8));
	vdec_w(d, CORE_MC + 0xb5c, 0x11);

	/* geometry and output (MM21: luma >> 9, chroma >> 8) */
	vdec_w(d, CORE_MC + 0x208, w);
	/* MIUI uses the 16-aligned height here (0x440 = 1088 for 1080p), not
	 * the display height — the core's luma write-back plane size/stride is
	 * computed from this, so a raw h mis-places the luma plane. */
	vdec_w(d, CORE_MC + 0x20c, (h + 15) & ~15);
	vdec_w(d, CORE_MC + 0x220, 1);
	/* mc+224 (luma) is a 24-bit field holding byte address >> 9, while
	 * mc+228 (chroma) is >> 8. MIUI: mc+224=0x7d80e0 (<<9 = 0xfb01c000,
	 * the same luma as mc+560) and mc+228=0xfb21a0 (<<8 = 0xfb21a000).
	 * Using >>8 for luma doubles the address -> luma write-back lands
	 * nowhere (luma plane stays zero). */
	vdec_w(d, CORE_MC + 0x224, run->y_dma >> 9);
	vdec_w(d, CORE_MC + 0x228, run->c_dma >> 8);
	vdec_w(d, CORE_MC + 0x250, 1);
	vdec_w(d, CORE_MC + 0x238, 3);
	vdec_w(d, CORE_MC + 0x260, mb_w - 1);
	vdec_w(d, CORE_MC + 0x264, mb_h - 1);
	vdec_w(d, CORE_MC + 0x22c, mb_w);
	vdec_w(d, CORE_MC + 0x560, run->y_dma);
	vdec_w(d, CORE_MC + 0x90, 4);
	vdec_w(d, CORE_MC + 0x768, 4);
	vdec_w(d, CORE_PP + 0xc, 1);
	vdec_w(d, CORE_MC + 0x57c, 0);
	vdec_w(d, CORE_MC + 0x5e4, vdec_r(d, CORE_MC + 0x5e4) | 0x3000);
	vdec_w(d, CORE_MC + 0x668, 1);
	vdec_w(d, CORE_MC + 0x66c, 1);
	vdec_w(d, CORE_MC + 0x980, mb_w);
	vdec_w(d, CORE_AVC_MV + 0x20c, run->mv_dma >> 4);

	/* header words */
	vdec_w(d, CORE_AVC_VLD + 0x9c, run->words.type_word);
	vdec_w(d, CORE_AVC_VLD + 0x234, cabac ? 1 : 0);
	vdec_w(d, CORE_MC + 0x71c, vdec_r(d, CORE_MC + 0x71c));
	vdec_w(d, CORE_VLD_TOP + 0x6c, mb_w * mb_h);
	vdec_w(d, CORE_VLD_TOP + 0x70, h << 16 | w);
	vdec_w(d, CORE_MC + 0xa60, 0);
	vdec_w(d, CORE_PP + 0xb80, 0);
	vdec_w(d, CORE_AVC_MV + 0x220, vdec_r(d, CORE_AVC_MV + 0x220) | 0x20);

	vdec_hw_ring(d, CORE_VLD_TOP, mb_w, mb_h);
	vdec_w(d, CORE_VLD_TOP + 0x234, 1);
	vdec_w(d, CORE_VLD_TOP + 0x23c, run->ube->dma + run->ube->size);
	vdec_w(d, CORE_VLD_TOP + 0x240, run->ube->dma);
	vdec_w(d, CORE_AVC_VLD + 0x290, 1);
	vdec_w(d, CORE_VLD_TOP + 0x9c, 0x07000800);
	vdec_w(d, CORE_VLD_TOP + 0x230, vdec_r(d, CORE_VLD_TOP + 0x230) | 1);
	vdec_w(d, CORE_VLD_TOP + 0x2ac, vdec_r(d, CORE_VLD_TOP + 0x2ac) | 1);
	/* avc_vld+238 (start) comes from vdec_hw_start() */
}

void vdec_hw_h264_core(struct vdec_ctx *ctx, const struct vdec_h264_run *run)
{
	vdec_hw_h264_core_pre(ctx, run);
	vdec_hw_h264_core_post(ctx, run);
}

/* engine state when a job stalls: status / error counters worth a look */
void vdec_hw_dump(struct vdec_dev *d, const char *where)
{
	/* concise one-shot state snapshot for a timed-out job */
	dev_err(d->dev, "%s: lat/f4=%#x core/f4=%#x lat/a4=%#x core/a4=%#x gcon+0=%#x\n",
		where,
		vdec_r(d, LAT_VLD + 0xf4), vdec_r(d, CORE_VLD + 0xf4),
		vdec_r(d, LAT_MISC + 0xa4), vdec_r(d, CORE_MISC + 0xa4),
		vdec_r(d, CORE_GCON + 0x0));
	dev_err(d->dev, "%s: core 88=%#x 90=%#x 94=%#x 98=%#x 9c=%#x mc224=%#x mc228=%#x\n",
		where,
		vdec_r(d, CORE_AVC_VLD + 0x88), vdec_r(d, CORE_AVC_VLD + 0x90),
		vdec_r(d, CORE_AVC_VLD + 0x94), vdec_r(d, CORE_AVC_VLD + 0x98),
		vdec_r(d, CORE_AVC_VLD + 0x9c),
		vdec_r(d, CORE_MC + 0x224), vdec_r(d, CORE_MC + 0x228));
	/* registers the capture read-modify-writes: MIUI's values are
	 * pp+040 0x0100f011, avc_vld+2e8 0x1f25, avc_vld+2c8 0xf900a03b,
	 * mc+71c 0x8800ff00, vld+140 0x00100014, avc_mv+220 0xc20,
	 * mc+5e4 0x3000, vld_top+09c 0x07000800, misc+160 0xf1000 */
	dev_err(d->dev, "%s: lat vp9 st=%#x/%#x cc=%#x 138=%#x 1ec=%#x | core vp9 st=%#x/%#x\n",
		where, vdec_r(d, LAT_VP9 + 0x0), vdec_r(d, LAT_VP9 + 0x4),
		vdec_r(d, LAT_VP9 + 0xcc), vdec_r(d, LAT_VP9 + 0x138),
		vdec_r(d, LAT_VP9 + 0x1ec), vdec_r(d, CORE_VP9 + 0x0),
		vdec_r(d, CORE_VP9 + 0x4));
	dev_err(d->dev, "%s: pp40=%#x 2e8=%#x 2c8=%#x mc71c=%#x vld140=%#x mv220=%#x mc5e4=%#x top9c=%#x misc160=%#x top54=%#x\n",
		where,
		vdec_r(d, CORE_PP + 0x40), vdec_r(d, CORE_AVC_VLD + 0x2e8),
		vdec_r(d, CORE_AVC_VLD + 0x2c8), vdec_r(d, CORE_MC + 0x71c),
		vdec_r(d, CORE_VLD + 0x140), vdec_r(d, CORE_AVC_MV + 0x220),
		vdec_r(d, CORE_MC + 0x5e4), vdec_r(d, CORE_VLD_TOP + 0x9c),
		vdec_r(d, CORE_MISC + 0x160), vdec_r(d, CORE_VLD_TOP + 0x54));
	/* VP8: MB position (vpud's completion check), status words */
	dev_err(d->dev, "%s: vp8 mc28=%#x mc2c=%#x 174=%#x 15c=%#x 0b8=%#x 148=%#x 14c=%#x\n",
		where, vdec_r(d, CORE_MC + 0x28), vdec_r(d, CORE_MC + 0x2c),
		vdec_r(d, CORE_VP8 + 0x174), vdec_r(d, CORE_VP8 + 0x15c),
		vdec_r(d, CORE_VP8 + 0xb8), vdec_r(d, CORE_VP8 + 0x148),
		vdec_r(d, CORE_VP8 + 0x14c));
}

/*
 * Start both engines (LAT first, as MIUI) and arm the decode-done
 * interrupts. Called with both jobs programmed; the IRQ handler clears
 * misc+0xa4 the same way (set bit 0|4, then drop bit 4).
 */
void vdec_hw_start(struct vdec_dev *d)
{
	vdec_w(d, LAT_AVC_VLD + 0x238, 1);
	vdec_w(d, LAT_MISC + 0xa4, 0x11);
	/* MIUI starts the LAT as its own job and kicks the core shortly after,
	 * while the ring still has room; a full ring at core-start wedges it */
	udelay(50);
	vdec_w(d, CORE_AVC_VLD + 0x238, 1);
	/*
	 * Done IRQ latch (per legacy vdecdev dec_isr): BIT(16) at
	 * CORE_VLD+0xa4 = 0x160200a4 (bit4 = enable, bit0 = sw). Empirically
	 * the done STATUS bit16 then latches at CORE_MISC+0xa4 = 0x160250a4
	 * (read/cleared there by the ISR, matching legacy dec_isr's
	 * base+41*4 clear). Arming only CORE_MISC (no CORE_VLD write) left
	 * the done IRQ line silent; arming CORE_VLD makes SPI 424 fire.
	 */
	vdec_w(d, CORE_VLD + 0xa4, 0x11);
}
