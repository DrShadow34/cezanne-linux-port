// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: VP9 register programming, transcribed from the MIUI
 * capture of a 320x240 profile-0 decode (trace/caps/cap-vp9.webm, jobs in
 * trace/vp9/). NOTES "VP9 register map" lists what each write is. The
 * LAT reads the whole frame from the start of the source buffer; its
 * second bitstream reader (bs2) is pointed at the tile data.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/unaligned.h>
#include <linux/slab.h>

#include "vdec.h"
#include <linux/module.h>

static int vp9_save_shift;
module_param(vp9_save_shift, int, 0644);
static int vp9_save_debug;
static uint vp9_0a8_xor;
module_param(vp9_0a8_xor, uint, 0644);
static int vp9_save_delay;
module_param(vp9_save_delay, int, 0644);
module_param(vp9_save_debug, int, 0644);

#define VP9_GCON_C8	0x0000fdff
#define VP9_GCON_CC	0x71e31182
#define VP9_GCON_178	0xfffff041

/* --- probability memory: port A (+198 address, +19c data), port B (+1a0, +1a4) --- */

static void vp9_port_a(struct vdec_dev *d, u32 addr)
{
	vdec_w(d, LAT_VP9 + 0x198, 0x10000 | addr);
}

static void vp9_port_b(struct vdec_dev *d, u32 addr)
{
	vdec_w(d, LAT_VP9 + 0x1a0, 0x10000 | addr);
}

static u32 le_word(const u8 *p, unsigned int n)
{
	u32 w = 0;
	unsigned int i;

	for (i = 0; i < n; i++)
		w |= (u32)p[i] << (8 * i);
	return w;
}

/* n bytes as little-endian words through port A, padded to `words` words */
static void vp9_a_bytes(struct vdec_dev *d, const u8 *p, unsigned int n, unsigned int words)
{
	unsigned int i;

	for (i = 0; i < words; i++) {
		unsigned int k = i * 4 < n ? min(4u, n - i * 4) : 0;

		vdec_w(d, LAT_VP9 + 0x19c, k ? le_word(p + i * 4, k) : 0);
	}
}

static void vp9_b_bytes(struct vdec_dev *d, const u8 *p, unsigned int n)
{
	vdec_w(d, LAT_VP9 + 0x1a4, le_word(p, n));
}

static void vp9_write_probs(struct vdec_dev *d, const struct v4l2_vp9_frame_context *c)
{
	const u8 *coef = (const u8 *)c->coef;
	unsigned int i, k;

	vp9_port_a(d, 0);
	vp9_port_b(d, 0);
	/* 96 coefficient bands of 18 bytes: 16 through A, the last 2 through B */
	for (k = 0; k < 4 * 2 * 2 * 6; k++) {
		vp9_a_bytes(d, coef + 18 * k, 16, 4);
		vp9_b_bytes(d, coef + 18 * k + 16, 2);
	}
	vp9_port_a(d, 0x1d0);
	for (i = 0; i < 10; i++)
		vp9_a_bytes(d, c->uv_mode[i], 9, 4);
	vp9_port_a(d, 0x180);
	for (i = 0; i < 4; i++)
		vp9_a_bytes(d, c->y_mode[i], 9, 4);
	vp9_port_a(d, 0x190);
	for (i = 0; i < 4; i++)
		vp9_a_bytes(d, c->interp_filter[i], 2, 4);
	vp9_port_b(d, 0x60);
	for (i = 0; i < 16; i++)
		vp9_b_bytes(d, c->partition[i], 3);
	vp9_port_b(d, 0x7e);
	vp9_b_bytes(d, c->is_inter, 4);
	vp9_port_a(d, 0x1a8);
	vp9_a_bytes(d, c->comp_mode, 5, 2);
	vp9_port_a(d, 0x1ac);
	vp9_a_bytes(d, c->comp_ref, 5, 2);
	vp9_port_a(d, 0x1b0);
	vp9_a_bytes(d, (const u8 *)c->single_ref, 10, 3);
	vp9_port_b(d, 0x78);
	for (i = 0; i < 2; i++)
		vp9_b_bytes(d, c->tx8[i], 1);
	for (i = 0; i < 2; i++)
		vp9_b_bytes(d, c->tx16[i], 2);
	for (i = 0; i < 2; i++)
		vp9_b_bytes(d, c->tx32[i], 3);
	vp9_port_b(d, 0x77);
	vp9_b_bytes(d, c->skip, 3);
	vp9_port_b(d, 0x70);
	for (i = 0; i < 7; i++)
		vp9_b_bytes(d, c->inter_mode[i], 3);
	vp9_port_a(d, 0x1b4);
	vp9_a_bytes(d, c->mv.joint, 3, 1);
	vp9_port_a(d, 0x1b8);
	for (i = 0; i < 2; i++) {
		u8 b[11];

		/* MIUI writes the sign probability as a leading byte before
		 * each component's classes (the hardware's mv_classes layout
		 * is sign + classes[10]); omitting it shifts every class */
		b[0] = c->mv.sign[i];
		memcpy(b + 1, c->mv.classes[i], 10);
		vp9_a_bytes(d, b, 11, 4);
	}
	for (i = 0; i < 2; i++) {
		u8 b[11];

		b[0] = c->mv.class0_bit[i];
		memcpy(b + 1, c->mv.bits[i], 10);
		vp9_a_bytes(d, b, 11, 4);
	}
	for (i = 0; i < 2; i++) {
		u8 b[11];

		memcpy(b, c->mv.class0_fr[i], 6);
		memcpy(b + 6, c->mv.fr[i], 3);
		b[9] = c->mv.class0_hp[i];
		b[10] = c->mv.hp[i];
		vp9_a_bytes(d, b, 11, 4);
	}
}

/*
 * vp9+0a8: frame mode word (MIUI vdec_hal_vp9_set_uncompress_info builds
 * it from ten header fields). Known: bit 0 inter frame, bit 3
 * frame_parallel_decoding_mode (also stops the symbol counting the
 * backward adaptation needs), [26:24] interpolation filter
 * (4 = switchable), bit 31 set on inter frames of the capture. The rest
 * stays at the captured values until a capture shows them.
 */
/* vp9+0c4: tile columns - 1 (MIUI: 0, 1, 3 for 1, 2, 4 columns) */
static u32 vp9_tile_cols_reg(const struct v4l2_ctrl_vp9_frame *f)
{
	return (1u << f->tile_cols_log2) - 1;
}

static u32 vp9_mode_word(const struct vdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	u32 w = 0;

	if (run->inter) {
		w |= BIT(0) | (u32)(f->interpolation_filter & 7) << 24;
		if (f->flags & V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV)
			w |= BIT(31);
	}
	if (f->flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)
		w |= BIT(3);
	if (f->bit_depth == 10)
		w |= BIT(17);		/* the vendor's (bit_depth == 10) << 1 in byte 2 */
	if (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) {
		w |= BIT(8);
		if (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP)
			w |= BIT(9);
		if (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE)
			w |= BIT(10);
	}
	return w;
}

/* segmentation: tree and prediction probabilities, the engine's map */
static void vp9_seg(struct vdec_dev *d, u32 blk, u32 top, dma_addr_t map,
		    const struct v4l2_ctrl_vp9_frame *f)
{
	const u8 *t = f->seg.tree_probs, *p = f->seg.pred_probs;
	bool temporal = f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE;

	if (!(f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED))
		return;
	vdec_w(d, blk + 0x1f0, t[0] | t[1] << 8 | t[2] << 16 | (u32)t[3] << 24);
	vdec_w(d, blk + 0x1f4, t[4] | t[5] << 8 | t[6] << 16);
	vdec_w(d, blk + 0x1f8, temporal ? p[0] | p[1] << 8 | p[2] << 16 : 0xffffff);
	vdec_w(d, top + 0x118, map);
	vdec_w(d, top + 0x11c, map);
}

/*
 * Tile descriptors, 4 words each into the vp9+104 FIFO (mainline
 * vdec_vp9_slice_setup_tile_buffer layout): size in bits plus the bit
 * offset of the 16-byte aligned start, the aligned address, the bit
 * offset inside it, (superblock rows - 1) << 6 | (superblock cols - 1).
 * Every tile but the last is preceded by a 4-byte big-endian size.
 */
static unsigned int vp9_tile_start(unsigned int idx, unsigned int mis, unsigned int log2)
{
	unsigned int sbs = (mis + 7) >> 3;
	unsigned int off = ((idx * sbs) >> log2) << 3;

	return min(off, mis);
}

static void vp9_write_tiles(struct vdec_dev *d, const struct vdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	const u8 *va = vb2_plane_vaddr(&run->src->vb2_buf, 0);
	unsigned int rows = 1 << f->tile_rows_log2, cols = 1 << f->tile_cols_log2;
	unsigned int mi_rows = DIV_ROUND_UP(run->h, 8), mi_cols = DIV_ROUND_UP(run->w, 8);
	u32 off = f->uncompressed_header_size + f->compressed_header_size;
	unsigned int i, j;

	for (i = 0; i < rows; i++) {
		unsigned int sb_rows = (vp9_tile_start(i + 1, mi_rows, f->tile_rows_log2) -
					vp9_tile_start(i, mi_rows, f->tile_rows_log2) + 7) >> 3;

		for (j = 0; j < cols; j++) {
			unsigned int sb_cols = (vp9_tile_start(j + 1, mi_cols, f->tile_cols_log2) -
						vp9_tile_start(j, mi_cols, f->tile_cols_log2) + 7) >> 3;
			u32 size;
			dma_addr_t pa;

			if (i == rows - 1 && j == cols - 1) {
				size = run->bs_len > off ? run->bs_len - off : 0;
			} else {
				if (!va || off + 4 > run->bs_len) {
					dev_warn_ratelimited(d->dev, "vp9: tile %u/%u header past the data (off %u, %u bytes)\n",
							     i, j, off, run->bs_len);
					return;
				}
				size = get_unaligned_be32(va + off);
				off += 4;
				if (off + size > run->bs_len) {
					dev_warn_ratelimited(d->dev, "vp9: tile %u/%u size %u at %u past %u bytes (data %*ph)\n",
							     i, j, size, off - 4, run->bs_len, 16, va + off - 4);
					return;
				}
			}
			pa = run->bs_dma + off;
			if (sb_rows) {
				vdec_w(d, LAT_VP9 + 0x104, (size << 3) + ((off << 3) & 0x7f));
				vdec_w(d, LAT_VP9 + 0x104, pa & ~0xf);
				vdec_w(d, LAT_VP9 + 0x104, (pa << 3) & 0x7f);
				vdec_w(d, LAT_VP9 + 0x104, ((sb_rows - 1) & 0x1ff) << 6 |
							   ((sb_cols - 1) & 0x3f));
			}
			off += size;
		}
	}
}

/* --- LAT job --- */

/*
 * The LAT's VP9 front end doubles as the driver's bit reader, as in MIUI:
 * reading +000 peeks at the next 32 bits, reading +4*n consumes n bits
 * (n <= 16 here, as MIUI), +138 counts the bits consumed since the last
 * +144 pulse. After the bool decoder is started (+110), +084 / +088 read a
 * 1- / 2-bit literal through it.
 */
static void vp9_skip_bits(struct vdec_dev *d, u32 bits)
{
	while (bits) {
		u32 n = min(bits, 16u);

		vdec_r(d, LAT_VP9 + 4 * n);
		bits -= n;
	}
}

static int vp9_poll(struct vdec_dev *d, u32 reg, u32 mask, u32 want, const char *what)
{
	u32 v;
	int ret = readl_poll_timeout_atomic(d->regs + reg, v, (v & mask) == want, 1, 2000);

	if (ret)
		dev_err_ratelimited(d->dev, "vp9: %s timed out (%#x)\n", what, v);
	return ret;
}

static bool vp9_lossless(const struct v4l2_ctrl_vp9_frame *f)
{
	return !f->quant.base_q_idx && !f->quant.delta_q_y_dc &&
	       !f->quant.delta_q_uv_dc && !f->quant.delta_q_uv_ac;
}

static bool vp9_compound_allowed(const struct v4l2_ctrl_vp9_frame *f)
{
	u8 b = f->ref_frame_sign_bias;	/* bit i = sign bias of LAST + i */

	return ((b >> 1) & 1) != (b & 1) || ((b >> 2) & 1) != (b & 1);
}

/*
 * vp9+0b0, written after the compressed header is parsed (MIUI, from a
 * two-pass alt-ref capture): [7:0] reference_mode, [15:8] comp_fixed_ref
 * (libvpx keeps it from the last frame that allowed compound prediction,
 * 0 before any), [23:16] 8 when this frame allows compound prediction,
 * [31:24] 9 once comp_fixed_ref is set. Only the fixed = ALTREF layout
 * (libvpx's alt-ref sign bias) was observed.
 */
static u32 vp9_comp_word(struct vdec_dev *d, struct vdec_vp9_state *st,
			 const struct vdec_vp9_run *run, u32 refmode)
{
	u8 b = run->frame->ref_frame_sign_bias;
	bool allowed = run->inter && vp9_compound_allowed(run->frame);

	if (allowed) {
		st->comp_fixed = (b & 1) == ((b >> 1) & 1) ? 3 :
				 (b & 1) == ((b >> 2) & 1) ? 2 : 1;
		if (st->comp_fixed != 3)
			dev_info_ratelimited(d->dev, "vp9: compound with fixed reference %u (unverified)\n",
					     st->comp_fixed);
	}
	if (!run->inter)
		return 0;
	return (refmode & 0xff) | (u32)st->comp_fixed << 8 | (allowed ? 8u << 16 : 0) |
	       (st->comp_fixed ? 9u << 24 : 0);
}


void vdec_hw_vp9_lat(struct vdec_ctx *ctx, const struct vdec_vp9_run *run)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	const struct v4l2_ctrl_vp9_compressed_hdr *h = run->hdr;
	u32 sb_w = DIV_ROUND_UP(run->w, 64), sb_h = DIV_ROUND_UP(run->h, 64);
	u32 flags = vp9_mode_word(run) ^ vp9_0a8_xor;
	u32 tx_mode = V4L2_VP9_TX_MODE_ONLY_4X4, refmode, v;
	unsigned int i;

	vdec_hw_init(d);
	vdec_hw_lat_prologue(d, VP9_GCON_C8, VP9_GCON_CC, VP9_GCON_178);
	vdec_w(d, LAT_MISC + 0x4, 3);
	vdec_w(d, LAT_VP9 + 0xa4, 1);
	vdec_w(d, LAT_VP9 + 0x1a8, 1);
	vp9_poll(d, LAT_VP9 + 0x1a8, 1, 0, "init");
	/* both bitstream readers see the whole source plane */
	vdec_w(d, LAT_BS2 + 0xb4, run->bs_dma >> 6);
	vdec_w(d, LAT_BS2 + 0xb8, (run->bs_dma + run->bs_size) >> 6);
	vdec_w(d, LAT_BS2 + 0xec, 0x10000000);
	vdec_w(d, LAT_VLD + 0xec, 0x10000000);
	vdec_w(d, LAT_VLD + 0xb4, run->bs_dma >> 6);
	vdec_w(d, LAT_VLD + 0xb8, (run->bs_dma + run->bs_size) >> 6);
	/* read pointer at the frame start, fetch, then the barrel shifter */
	vdec_w(d, LAT_VLD + 0xb0, run->bs_dma);
	vdec_w(d, LAT_VLD + 0x108, 0x100);
	vdec_w(d, LAT_VLD + 0x108, 0);
	vdec_w(d, LAT_VLD + 0x8c, 0x100000);
	vp9_poll(d, LAT_VLD + 0xf4, BIT(17), 0, "bitstream fetch");
	/* the fetch is done once vld+0e8 bit 0 (FETCHOK) is set, as MIUI
	 * and our VP8 wait: initialising the barrel shifter before that
	 * parsed stale bits now and then (about one VP9 frame in 1000: the
	 * compressed header read wrong, the context saved from it broken
	 * until the next keyframe) */
	vp9_poll(d, LAT_VLD + 0xe8, BIT(0), BIT(0), "bitstream fetch done");
	vdec_w(d, LAT_VLD + 0x8c, 0x800000);
	vdec_w(d, LAT_VP9 + 0x144, 1);
	vdec_w(d, LAT_VP9 + 0x144, 0);
	/* the uncompressed header was parsed by the client: step over it */
	vp9_skip_bits(d, f->uncompressed_header_size * 8);
	v = vdec_r(d, LAT_VP9 + 0x138);
	if (v != f->uncompressed_header_size * 8)
		dev_dbg(d->dev, "vp9: bit counter %u after %u header bytes\n",
			v, f->uncompressed_header_size);

	vdec_w(d, LAT_VLD_TOP + 0x70, run->h << 16 | run->w);
	if (f->bit_depth == 10) {
		vdec_w(d, LAT_VLD_TOP + 0x100, 0x22);	/* as AV1 / HEVC */
		vdec_w(d, LAT_PP + 0xb90, 1);
	}
	vdec_w(d, LAT_VLD_TOP + 0x68, (sb_h - 1) << 16 | (sb_w - 1));
	vdec_w(d, LAT_VP9 + 0xa4, 1);
	vdec_w(d, LAT_VP9 + 0xa8, flags);
	vdec_w(d, LAT_VP9 + 0xac, 0);
	vp9_seg(d, LAT_VP9, LAT_VLD_TOP, ctx->vp9->lat_seg.dma, f);
	vp9_write_probs(d, &run->probs);

	/* compressed header: bool decoder at the current position */
	vdec_w(d, LAT_VP9 + 0x144, 1);
	vdec_w(d, LAT_VP9 + 0x144, 0);
	vdec_w(d, LAT_VP9 + 0x110, 1);
	vp9_poll(d, LAT_VP9 + 0x110, BIT(16), BIT(16), "bool decoder");
	if (!vp9_lossless(f)) {
		tx_mode = vdec_r(d, LAT_VP9 + 0x88);		/* L(2) */
		if (tx_mode == V4L2_VP9_TX_MODE_ALLOW_32X32)
			tx_mode += vdec_r(d, LAT_VP9 + 0x84);	/* + L(1) */
		/* VA-API clients send no compressed header: the LAT's own
		 * reading is authoritative */
		if (h->tx_mode && tx_mode != h->tx_mode)
			dev_dbg(d->dev, "vp9: tx_mode %u, header says %u\n", tx_mode, h->tx_mode);
	}
	vdec_w(d, LAT_VP9 + 0xb4, tx_mode);
	/* probability groups in syntax order: one pulse each, wait for it */
	/*
	 * Probability groups in syntax order: one pulse each, wait for it.
	 * frame_reference_mode() sits before group 6: its literals decide
	 * groups 6 (comp_inter), 7 (single_ref) and 8 (comp_ref).
	 */
	refmode = V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE;
	for (i = 0; i < 14; i++) {
		bool on = run->groups & BIT(i);

		if (i == 0)
			on = tx_mode == V4L2_VP9_TX_MODE_SELECT;
		if (i >= 6 && i <= 8) {
			if (!run->inter)
				continue;
			if (i == 6 && vp9_compound_allowed(f) && vdec_r(d, LAT_VP9 + 0x84))
				refmode = vdec_r(d, LAT_VP9 + 0x84) ?
					  V4L2_VP9_REFERENCE_MODE_SELECT :
					  V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE;
			on = (i == 6 && refmode == V4L2_VP9_REFERENCE_MODE_SELECT) ||
			     (i == 7 && refmode != V4L2_VP9_REFERENCE_MODE_COMPOUND_REFERENCE) ||
			     (i == 8 && refmode != V4L2_VP9_REFERENCE_MODE_SINGLE_REFERENCE);
		}
		if (!on)
			continue;
		vdec_w(d, LAT_VP9 + 0xc8, BIT(i));
		vdec_w(d, LAT_VP9 + 0xc8, 0);
		vp9_poll(d, LAT_VP9 + 0xcc, BIT(i), BIT(i), "probability group");
	}
	ctx->vp9->hw_tx_mode = tx_mode;
	ctx->vp9->hw_refmode = refmode;

	vdec_hw_lat_racing_tail(d);
	vdec_hw_racing_cfg(d);
	/* MIUI's VP9 LAT does not program the UBE range (only the core does);
	 * writing it here misroutes the LAT->core residuals */
	ctx->vp9->comp_word = vp9_comp_word(d, ctx->vp9, run, refmode);
	vdec_w(d, LAT_VP9 + 0xb0, ctx->vp9->comp_word);
	vdec_w(d, LAT_AVC_MV + 0x3c4, run->use_prev_mvs);
	vdec_w(d, LAT_AVC_MV + 0x3c0, ctx->vp9->lat_mv.dma >> 4);
	vdec_w(d, LAT_AVC_MV + 0x20c, ctx->vp9->lat_mv.dma >> 4);
	vdec_w(d, LAT_VP9 + 0x1a8, 1);
	vp9_poll(d, LAT_VP9 + 0x1a8, 1, 0, "pre-start");
	vdec_w(d, LAT_PP + 0xcac, 0);
	vdec_w(d, LAT_VP9 + 0xc4, vp9_tile_cols_reg(f));
	vdec_w(d, LAT_VP9 + 0x100, 0);
	/* tile data: bits from a 16-byte aligned base, its address, the bit
	 * offset of the tile data inside those 16 bytes */
	vp9_write_tiles(d, run);
	/* MIUI writes these values on every VP9 LAT job (bit 26 is clear
	 * after an H.264 job) */
	vdec_w(d, LAT_VLD + 0x140, 0x05100014);
	vdec_w(d, LAT_BS2 + 0x140, 0x07100014);
	vdec_w(d, LAT_VP9 + 0xbc, 0x10111);
	/* +0b8 (start) comes from vdec_hw_vp9_start() */
}

/*
 * Core tile columns: (superblock columns - 1) of each tile column, one byte
 * each, four per vp9+104 word (MIUI: 0x4 for one 320-wide tile, 0x404 for
 * two 320-wide ones).
 */
static void vp9_core_tiles(struct vdec_dev *d, const struct vdec_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	unsigned int cols = 1 << f->tile_cols_log2, mi_cols = DIV_ROUND_UP(run->w, 8), j;
	u32 w = 0;

	for (j = 0; j < cols; j++) {
		unsigned int s0 = vp9_tile_start(j, mi_cols, f->tile_cols_log2);
		unsigned int s1 = vp9_tile_start(j + 1, mi_cols, f->tile_cols_log2);

		w |= (((s1 - s0 + 7) >> 3) - 1 & 0xff) << (8 * (j & 3));
		if ((j & 3) == 3 || j == cols - 1) {
			vdec_w(d, CORE_VP9 + 0x104, w);
			w = 0;
		}
	}
}

/* --- core job --- */

void vdec_hw_vp9_core(struct vdec_ctx *ctx, const struct vdec_vp9_run *run)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_ctrl_vp9_frame *f = run->frame;
	u32 sb_w = DIV_ROUND_UP(run->w, 64), sb_h = DIV_ROUND_UP(run->h, 64);
	u32 mi_w = DIV_ROUND_UP(run->w, 8), mi_h = DIV_ROUND_UP(run->h, 8);
	u32 flags = vp9_mode_word(run) ^ vp9_0a8_xor;
	unsigned int i;

	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, CORE_GCON + 0x18, 0x111);
	vdec_w(d, SOC_GCON + 0xf0, 0x40000004);
	vdec_w(d, CORE_MISC + 0xf4, 0);
	vdec_w(d, SOC_GCON + 0x18, 0x111);
	vdec_w(d, CORE_GCON + 0x18, 0x111);
	vdec_w(d, CORE_MISC + 0xec, 1);
	vdec_w(d, CORE_LARB + 0xc, 1);
	vdec_w(d, CORE_VLD + 0x108, 0x101);
	vdec_w(d, CORE_MISC + 0xc8, VP9_GCON_C8);
	vdec_w(d, CORE_MISC + 0xcc, VP9_GCON_CC);
	vdec_w(d, CORE_MISC + 0x178, VP9_GCON_178);
	vdec_w(d, CORE_MISC + 0x84, 2);
	vdec_w(d, SOC_GCON + 0x4, 2);
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, CORE_LARB + 0xc, 0x00010000);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 1);
	vdec_w(d, SOC_UFO_ENC + 0x1e4, 0);
	vdec_w(d, SOC_UFO_ENC + 0x90, 1);
	vdec_w(d, SOC_UFO_ENC + 0x90, 0);
	vdec_w(d, CORE_MISC + 0x4, 3);
	vdec_w(d, CORE_VP9 + 0xa4, 1);
	vdec_w(d, CORE_VP9 + 0x1a8, 1);
	vp9_poll(d, CORE_VP9 + 0x1a8, 1, 0, "core init");
	vdec_w(d, CORE_BS2 + 0xb4, 0);
	vdec_w(d, CORE_BS2 + 0xb8, 0x80);
	vdec_w(d, CORE_BS2 + 0xec, 0x10000000);
	vdec_w(d, CORE_VLD + 0xec, 0x10000000);
	vdec_w(d, CORE_VLD + 0xb4, 0);
	vdec_w(d, CORE_VLD + 0xb8, 0x80);
	vdec_w(d, CORE_VLD + 0xb0, 0);
	vdec_w(d, CORE_VLD + 0x108, 0x100);
	vdec_w(d, CORE_VLD + 0x108, 0);
	vdec_w(d, CORE_VLD_TOP + 0x2ac, 0x11);
	vdec_w(d, CORE_VP9 + 0x144, 1);
	vdec_w(d, CORE_VP9 + 0x144, 0);
	/* tx_mode as the LAT read it (the LAT job is programmed first) */
	vdec_w(d, CORE_VP9 + 0xb4, ctx->vp9->hw_tx_mode);
	vdec_w(d, CORE_VLD_TOP + 0x70, run->h << 16 | run->w);
	if (f->bit_depth == 10)
		vdec_w(d, CORE_VLD_TOP + 0x100, 0x22);
	vdec_w(d, CORE_VLD_TOP + 0x68, (sb_h - 1) << 16 | (sb_w - 1));
	vdec_w(d, CORE_VP9 + 0xa4, 1);
	vdec_w(d, CORE_VP9 + 0xa8, flags);
	vdec_w(d, CORE_VP9 + 0xac, 0);
	/* loop filter: level, reference and mode deltas as bytes */
	vdec_w(d, CORE_PP + 0xd8, (u32)f->lf.level << 16 | 1);
	if (f->lf.flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED) {
		vdec_w(d, CORE_PP + 0xdc, (u8)f->lf.ref_deltas[0] | (u8)f->lf.ref_deltas[1] << 8 |
		       (u8)f->lf.ref_deltas[2] << 16 | (u32)(u8)f->lf.ref_deltas[3] << 24);
		vdec_w(d, CORE_PP + 0xe0, (u8)f->lf.mode_deltas[0] | (u8)f->lf.mode_deltas[1] << 8);
	} else {
		vdec_w(d, CORE_PP + 0xdc, 0);
		vdec_w(d, CORE_PP + 0xe0, 0);
	}
	vdec_w(d, CORE_VLD_TOP + 0x234, 1);
	vdec_w(d, CORE_VLD_TOP + 0x23c, run->ube->dma + run->ube->size);
	vdec_w(d, CORE_VLD_TOP + 0x240, run->ube->dma);
	vdec_w(d, CORE_VP9 + 0xb0, ctx->vp9->comp_word);
	vdec_w(d, CORE_AVC_MV + 0x3c4, run->use_prev_mvs);
	vdec_w(d, CORE_AVC_MV + 0x3c0, ctx->vp9->core_mv.dma >> 4);
	vdec_w(d, CORE_AVC_MV + 0x20c, ctx->vp9->core_mv.dma >> 4);
	vdec_w(d, CORE_MC + 0x208, run->w);
	vdec_w(d, CORE_MC + 0x20c, run->h);
	vdec_w(d, CORE_MC + 0xd28, run->ref_y[0]);
	vdec_w(d, CORE_MC + 0xd34, run->ref_c[0]);
	vdec_w(d, CORE_MC + 0xd2c, run->ref_y[1]);
	vdec_w(d, CORE_MC + 0xd38, run->ref_c[1]);
	vdec_w(d, CORE_MC + 0xd30, run->ref_y[2]);
	vdec_w(d, CORE_MC + 0xd3c, run->ref_c[2]);
	if (run->inter) {
		for (i = 0; i < 3; i++) {
			vdec_w(d, CORE_MC + 0xd4c + 4 * i, run->w << 16 | run->h);
			vdec_w(d, CORE_MC + 0xd70 + 4 * i, sb_w * 4);
		}
		vdec_w(d, CORE_MC + 0xd7c, 0x07770777);
		vdec_w(d, CORE_MC + 0xd80, 0);
	}
	vdec_w(d, CORE_MC + 0xd88, mi_w << 16 | mi_h);
	vdec_w(d, CORE_MC + 0x220, 1);
	vdec_w(d, CORE_MC + 0x224, run->y_dma >> 9);
	vdec_w(d, CORE_MC + 0x228, run->c_dma >> 8);
	vdec_w(d, CORE_MC + 0xb5c, 1);
	vdec_w(d, CORE_MC + 0x22c, sb_w * 4);
	vdec_w(d, CORE_MC + 0x238, 3);
	vdec_w(d, CORE_MC + 0x250, 1);
	/* the core works in whole superblocks (MIUI 320x240: 19 / 15) */
	vdec_w(d, CORE_MC + 0x260, sb_w * 4 - 1);
	vdec_w(d, CORE_MC + 0x264, sb_h * 4 - 1);
	vdec_w(d, CORE_PP + 0xf0, run->h << 16 | run->w);
	if (f->bit_depth == 10) {
		/* vdec_hal_vp9_set_hw_info: 10-bit (MT2110T) output */
		vdec_w(d, CORE_PP + 0xb8c, 1);
		vdec_w(d, CORE_PP + 0xb90, 1);
		vdec_w(d, CORE_MC + 0xcec, 1);
	}
	/* quantizer rows: 8 segments x (Y, UV), ac << 16 | dc */
	vdec_w(d, CORE_PP + 0x504, 1);
	for (i = 0; i < 16; i++) {
		vdec_w(d, CORE_VLD + 0x260, i);
		vdec_w(d, CORE_VLD + 0x264, run->quant[i]);
	}
	vdec_w(d, CORE_PP + 0x504, 0);
	vdec_w(d, SOC_UFO_ENC + 0x8c, 0x11100);
	vdec_w(d, SOC_UFO_ENC + 0x4, 0);
	vdec_w(d, CORE_MC + 0xa60, 0);
	vdec_w(d, CORE_PP + 0xb80, 0);
	vdec_w(d, SOC_UFO_ENC + 0xc, 0x30);
	vdec_w(d, SOC_RACING + 0x9c, 0);
	vdec_w(d, SOC_RACING + 0x58, 2);
	vdec_w(d, CORE_VLD_TOP + 0x398, 0x113);
	vdec_w(d, SOC_RACING + 0x7c, 0);
	vdec_w(d, SOC_RACING + 0x64, 1);
	vdec_w(d, CORE_PP + 0xcac, 0x11);
	vdec_w(d, CORE_MISC + 0x4, 0x110);
	vdec_w(d, CORE_VLD_TOP + 0x124, 0x07200010);
	vdec_w(d, CORE_VP9 + 0xc4, vp9_tile_cols_reg(f));
	vdec_w(d, CORE_VP9 + 0x100, 0x80);
	vp9_core_tiles(d, run);
	vdec_w(d, CORE_VLD + 0x140, 0x01100014);
	vdec_w(d, CORE_BS2 + 0x140, 0x01100014);
	vdec_w(d, CORE_VP9 + 0xbc, 0x10111);
	vdec_w(d, CORE_VP9 + 0x12c, 0);
	if (f->seg.flags & V4L2_VP9_SEGMENTATION_FLAG_ENABLED) {
		unsigned int k;

		/* only ALT_Q (the quantizer rows) is known; the per-segment
		 * loop filter levels presumably go to pp+0e4..0ec */
		for (k = 0; k < 8; k++)
			if (f->seg.feature_enabled[k] & ~BIT(V4L2_VP9_SEG_LVL_ALT_Q))
				dev_info_ratelimited(d->dev, "vp9: segment feature %#x not programmed\n",
						     f->seg.feature_enabled[k]);
		vdec_w(d, CORE_PP + 0xe4, 0);
		vdec_w(d, CORE_PP + 0xe8, 0);
		vdec_w(d, CORE_PP + 0xec, 0);
		vp9_seg(d, CORE_VP9, CORE_VLD_TOP, ctx->vp9->core_seg.dma, f);
	}
	vdec_hw_ring(d, CORE_VLD_TOP, ctx->mb_w, ctx->mb_h);
	/* +0b8 (start) comes from vdec_hw_vp9_start() */
}

void vdec_hw_vp9_start(struct vdec_dev *d)
{
	vdec_w(d, LAT_VP9 + 0xb8, 1);
	vdec_w(d, LAT_MISC + 0xa4, 0x11);
	udelay(50);
	vdec_w(d, CORE_VP9 + 0xb8, 1);
	vdec_w(d, CORE_VLD + 0xa4, 0x11);
}

/*
 * After a picture: the LAT writes its updated context to a, then its
 * counts to b (MIUI: command 2 then 1, done flag polled in +1ec).
 */
int vdec_hw_vp9_save(struct vdec_dev *d, dma_addr_t a, dma_addr_t b)
{
	static const u32 cmd[2] = { 2, 1 };
	dma_addr_t dst[2] = { a, b };
	unsigned int i;
	u32 v;
	int ret;

	for (i = 0; i < 2; i++) {
		u32 before = vdec_r(d, LAT_VP9 + 0x1ec);
		ktime_t t0 = ktime_get();

		vdec_w(d, LAT_VP9 + 0x1dc, cmd[i]);
		vdec_w(d, LAT_VP9 + 0x1e0, dst[i] >> vp9_save_shift);
		vdec_w(d, LAT_VP9 + 0x1e8, cmd[i]);
		/* the done flag still shows the previous dump: wait for it to
		 * drop (MIUI reads 0 first), then for the new one */
		readl_poll_timeout(d->regs + LAT_VP9 + 0x1ec, v, !(v & 1), 1, 200);
		ret = readl_poll_timeout(d->regs + LAT_VP9 + 0x1ec, v, v & 1, 5, 5000);
		if (vp9_save_delay)
			usleep_range(vp9_save_delay, vp9_save_delay + 100);
		if (vp9_save_debug)
			pr_info("vdec vp9 save cmd %u: 1ec before %#x after %#x, %lld us, ret %d\n",
				cmd[i], before, v, ktime_us_delta(ktime_get(), t0), ret);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Symbol counts of the last picture, read through the LAT's index / data
 * pairs (MIUI vdec_hal_vp9_get_counts_table): 16 KiB in the layout of
 * struct vdec_vp9_counts, unused ranges zero.
 */
void vdec_hw_vp9_read_counts(struct vdec_dev *d, u32 *buf)
{
	static const struct { u16 port, words, off; } seg[] = {
		{ 0x1b0, 0x240, 0x0000 },	/* eob_branch */
		{ 0x1b8, 0x400, 0x1000 },	/* coef_probs ... */
		{ 0x1c0, 0x400, 0x2000 },
		{ 0x1c8, 0x220, 0x3000 },	/* ... mode and mv counts */
	};
	unsigned int i, k;

	memset(buf, 0, SZ_16K);
	for (i = 0; i < ARRAY_SIZE(seg); i++)
		for (k = 0; k < seg[i].words; k++) {
			vdec_w(d, LAT_VP9 + seg[i].port, k);
			buf[seg[i].off / 4 + k] = vdec_r(d, LAT_VP9 + seg[i].port + 4);
		}
}

/*
 * The context the LAT ended the picture with (compressed-header deltas
 * applied), read back through the probability ports at the addresses
 * vp9_write_probs() writes (MIUI vdec_hal_vp9_get_prob_table).
 */
static void vp9_unpack(const u32 *w, unsigned int nw, u8 *dst, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n && i / 4 < nw; i++)
		dst[i] = w[i / 4] >> (8 * (i & 3));
}

void vdec_hw_vp9_read_probs(struct vdec_dev *d, struct v4l2_vp9_frame_context *c)
{
	u32 *a, *b;
	u8 tmp[11];
	unsigned int i, k;

	a = kmalloc_array(0x200 + 0x80, sizeof(u32), GFP_KERNEL);
	if (!a)
		return;
	b = a + 0x200;
	for (i = 0; i < 0x200; i++) {
		vdec_w(d, LAT_VP9 + 0x198, i);
		a[i] = vdec_r(d, LAT_VP9 + 0x19c);
	}
	for (i = 0; i < 0x80; i++) {
		vdec_w(d, LAT_VP9 + 0x1a0, i);
		b[i] = vdec_r(d, LAT_VP9 + 0x1a4);
	}
	for (k = 0; k < 96; k++) {
		u8 *band = (u8 *)c->coef + 18 * k;

		vp9_unpack(a + 4 * k, 4, band, 16);
		vp9_unpack(b + k, 1, band + 16, 2);
	}
	for (i = 0; i < 10; i++)
		vp9_unpack(a + 0x1d0 + 4 * i, 4, c->uv_mode[i], 9);
	for (i = 0; i < 4; i++)
		vp9_unpack(a + 0x180 + 4 * i, 4, c->y_mode[i], 9);
	for (i = 0; i < 4; i++)
		vp9_unpack(a + 0x190 + 4 * i, 4, c->interp_filter[i], 2);
	for (i = 0; i < 16; i++)
		vp9_unpack(b + 0x60 + i, 1, c->partition[i], 3);
	vp9_unpack(b + 0x7e, 1, c->is_inter, 4);
	vp9_unpack(a + 0x1a8, 2, c->comp_mode, 5);
	vp9_unpack(a + 0x1ac, 2, c->comp_ref, 5);
	vp9_unpack(a + 0x1b0, 3, (u8 *)c->single_ref, 10);
	for (i = 0; i < 2; i++)
		vp9_unpack(b + 0x78 + i, 1, c->tx8[i], 1);
	for (i = 0; i < 2; i++)
		vp9_unpack(b + 0x7a + i, 1, c->tx16[i], 2);
	for (i = 0; i < 2; i++)
		vp9_unpack(b + 0x7c + i, 1, c->tx32[i], 3);
	vp9_unpack(b + 0x77, 1, c->skip, 3);
	for (i = 0; i < 7; i++)
		vp9_unpack(b + 0x70 + i, 1, c->inter_mode[i], 3);
	vp9_unpack(a + 0x1b4, 1, c->mv.joint, 3);
	for (i = 0; i < 2; i++) {
		vp9_unpack(a + 0x1b8 + 4 * i, 4, tmp, 11);
		c->mv.sign[i] = tmp[0];
		memcpy(c->mv.classes[i], tmp + 1, 10);
		vp9_unpack(a + 0x1c0 + 4 * i, 4, tmp, 11);
		c->mv.class0_bit[i] = tmp[0];
		memcpy(c->mv.bits[i], tmp + 1, 10);
		vp9_unpack(a + 0x1c8 + 4 * i, 4, tmp, 11);
		memcpy(c->mv.class0_fr[i], tmp, 6);
		memcpy(c->mv.fr[i], tmp + 6, 3);
		c->mv.class0_hp[i] = tmp[9];
		c->mv.hp[i] = tmp[10];
	}
	kfree(a);
}
