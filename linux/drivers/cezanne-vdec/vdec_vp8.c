// SPDX-License-Identifier: GPL-2.0
/*
 * MT6885 VP8: V4L2_PIX_FMT_VP8_FRAME + V4L2_CID_STATELESS_VP8_FRAME on the
 * core engine (register sequence in vdec_vp8_hw.c).
 *
 * The core takes only the luma address of each reference and finds its
 * chroma at a fixed offset (mc+934), so VP8 capture buffers are one plane
 * with the chroma right after the 32-row-aligned luma (vdec_drv.c).
 */
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "vdec.h"

static int vp8_debug;
module_param(vp8_debug, int, 0644);
MODULE_PARM_DESC(vp8_debug, "log the next N VP8 pictures");

static const struct v4l2_ctrl_config vdec_vp8_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_VP8_FRAME },
	{
		.id = V4L2_CID_MPEG_VIDEO_VP8_PROFILE,
		.min = V4L2_MPEG_VIDEO_VP8_PROFILE_0,
		.max = V4L2_MPEG_VIDEO_VP8_PROFILE_3,
		.def = V4L2_MPEG_VIDEO_VP8_PROFILE_0,
	},
};

int vdec_vp8_ctrls_init(struct v4l2_ctrl_handler *hdl)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vdec_vp8_ctrls); i++)
		v4l2_ctrl_new_custom(hdl, &vdec_vp8_ctrls[i], NULL);
	return hdl->error;
}

/* the two working buffers (MIUI: 128 KiB apart at 320x240 and 1280x720) */
#define VP8_WB_SIZE	SZ_256K

int vdec_vp8_start(struct vdec_ctx *ctx)
{
	struct vdec_vp8_state *st;
	unsigned int i;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	ctx->vp8 = st;
	for (i = 0; i < 2; i++) {
		st->wb[i].size = VP8_WB_SIZE;
		st->wb[i].va = dma_alloc_coherent(ctx->dev->dev, VP8_WB_SIZE, &st->wb[i].dma,
						  GFP_KERNEL);
		if (!st->wb[i].va) {
			vdec_vp8_stop(ctx);
			return -ENOMEM;
		}
	}
	return 0;
}

void vdec_vp8_stop(struct vdec_ctx *ctx)
{
	struct vdec_vp8_state *st = ctx->vp8;
	unsigned int i;

	if (!st)
		return;
	for (i = 0; i < 2; i++)
		if (st->wb[i].va)
			dma_free_coherent(ctx->dev->dev, st->wb[i].size, st->wb[i].va,
					  st->wb[i].dma);
	kfree(st);
	ctx->vp8 = NULL;
}

/* RFC 6386 14.1 */
static const u8 vp8_dc_q[128] = {
	4, 5, 6, 7, 8, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17, 17,
	18, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 25, 25, 26, 27, 28,
	29, 30, 31, 32, 33, 34, 35, 36, 37, 37, 38, 39, 40, 41, 42, 43,
	44, 45, 46, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
	59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
	75, 76, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
	91, 93, 95, 96, 98, 100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118,
	122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};

static const u16 vp8_ac_q[128] = {
	4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
	36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
	52, 53, 54, 55, 56, 57, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76,
	78, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108,
	110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149, 152,
	155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209,
	213, 217, 221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

static u32 q7(int q)
{
	return clamp(q, 0, 127);
}

/*
 * Dequantization factors per segment for vld+260 / +264 (MIUI: segment
 * * 0x10 + 0 Y2, 4 Y1, 8 UV, each dc << 16 | ac), and the loop filter
 * level per segment for pp+024, as libvpx derives them.
 */
static void vdec_vp8_segments(struct vdec_vp8_run *run)
{
	const struct v4l2_ctrl_vp8_frame *f = run->frame;
	const struct v4l2_vp8_quantization *qz = &f->quant;
	bool seg = f->segment.flags & V4L2_VP8_SEGMENT_FLAG_ENABLED;
	bool delta = f->segment.flags & V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE;
	unsigned int s;

	for (s = 0; s < 4; s++) {
		int q = qz->y_ac_qi, lf = f->lf.level;
		u32 dc, ac;

		if (seg) {
			q = delta ? q + f->segment.quant_update[s] : f->segment.quant_update[s];
			lf = delta ? lf + f->segment.lf_update[s] : f->segment.lf_update[s];
		}
		q = q7(q);
		run->lf_level[s] = clamp(lf, 0, 63);
		dc = 2 * vp8_dc_q[q7(q + qz->y2_dc_delta)];
		ac = vp8_ac_q[q7(q + qz->y2_ac_delta)] * 155 / 100;
		run->dq[s][0] = dc << 16 | max(ac, 8u);
		run->dq[s][1] = vp8_dc_q[q7(q + qz->y_dc_delta)] << 16 | vp8_ac_q[q];
		dc = min_t(u32, vp8_dc_q[q7(q + qz->uv_dc_delta)], 132);
		run->dq[s][2] = dc << 16 | vp8_ac_q[q7(q + qz->uv_ac_delta)];
	}
}

static const void *vdec_ctrl(struct vdec_ctx *ctx, u32 id)
{
	struct v4l2_ctrl *c = v4l2_ctrl_find(&ctx->ctrl_hdl, id);

	return c ? c->p_cur.p : NULL;
}

/* luma of a reference by timestamp; the current picture if it is gone */
static dma_addr_t vdec_vp8_ref(struct vdec_ctx *ctx, const struct vdec_vp8_run *run, u64 ts)
{
	struct vb2_queue *cap = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	struct vb2_buffer *vb = run->key ? NULL : vb2_find_buffer(cap, ts);

	return vb ? vb2_dma_contig_plane_dma_addr(vb, 0) : run->y_dma;
}

int vdec_vp8_run(struct vdec_ctx *ctx)
{
	struct vdec_dev *d = ctx->dev;
	const struct v4l2_pix_format_mplane *cf = &ctx->dst_fmt.fmt.pix_mp;
	struct vdec_vp8_run *run;
	struct media_request *req;
	const struct v4l2_ctrl_vp8_frame *f;
	unsigned long flags;
	unsigned int i;
	u32 off;
	int ret = 0;

	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return -ENOMEM;
	run->src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	req = run->src->vb2_buf.req_obj.req;
	if (req)
		ret = v4l2_ctrl_request_setup(req, &ctx->ctrl_hdl);
	v4l2_m2m_buf_copy_metadata(run->src, run->dst);

	run->frame = f = vdec_ctrl(ctx, V4L2_CID_STATELESS_VP8_FRAME);
	if (ret || !f) {
		ret = ret ?: -EINVAL;
		goto out;
	}
	run->w = f->width;
	run->h = f->height;
	if (DIV_ROUND_UP(run->w, 16) != ctx->mb_w || DIV_ROUND_UP(run->h, 16) != ctx->mb_h) {
		dev_info_ratelimited(d->dev, "vp8: frame %ux%u does not match the format (%ux%u MBs)\n",
				     run->w, run->h, ctx->mb_w, ctx->mb_h);
		ret = -EINVAL;
		goto out;
	}
	run->key = f->flags & V4L2_VP8_FRAME_FLAG_KEY_FRAME;
	run->bs_dma = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	run->bs_len = vb2_get_plane_payload(&run->src->vb2_buf, 0);
	run->bs_size = vb2_plane_size(&run->src->vb2_buf, 0);
	run->y_dma = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->c_off = cf->plane_fmt[0].bytesperline * ALIGN(cf->height, 32);
	run->c_dma = run->y_dma + run->c_off;
	run->aux_dma = run->c_dma + run->c_off / 2;	/* after the chroma */
	run->ref_y[0] = vdec_vp8_ref(ctx, run, f->last_frame_ts);
	run->ref_y[1] = vdec_vp8_ref(ctx, run, f->golden_frame_ts);
	run->ref_y[2] = vdec_vp8_ref(ctx, run, f->alt_frame_ts);

	/* partitions: the first after the 10 / 3 header bytes, then the
	 * 3-byte sizes of all token partitions but the last, then those */
	run->nparts = f->num_dct_parts;
	if (!run->nparts || run->nparts > 8 || !is_power_of_2(run->nparts)) {
		ret = -EINVAL;
		goto out;
	}
	run->part_end = (run->key ? 10 : 3) + f->first_part_size;
	off = run->part_end + 3 * (run->nparts - 1);
	for (i = 0; i < run->nparts; i++) {
		run->part_off[i] = off;
		run->part_size[i] = f->dct_part_sizes[i];
		off += f->dct_part_sizes[i];
	}
	if (off > run->bs_len) {
		dev_info_ratelimited(d->dev, "vp8: partitions end at %u, %u bytes queued\n",
				     off, run->bs_len);
		ret = -EINVAL;
		goto out;
	}
	vdec_vp8_segments(run);

	if (vp8_debug > 0) {
		dev_info(d->dev, "vp8 %ux%u key %d ver %u q %u lf %u/%u parts %u [%u %u] flags %llx seg %x\n",
			 run->w, run->h, run->key, f->version, f->quant.y_ac_qi, f->lf.level,
			 f->lf.sharpness_level, run->nparts, f->first_part_size, f->dct_part_sizes[0],
			 f->flags, f->segment.flags);
		vp8_debug--;
	}
	ret = vdec_hw_vp8(ctx, run);
	if (ret)
		goto out;
	spin_lock_irqsave(&d->irq_lock, flags);
	d->pending = BIT(VDEC_CORE);
	d->irq_status[VDEC_CORE] = d->irq_status[VDEC_LAT] = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	vdec_hw_vp8_start(d);
out:
	if (req)
		v4l2_ctrl_request_complete(req, &ctx->ctrl_hdl);
	kfree(run);
	return ret;
}

/* the segment map lives in the core's SRAM: keep it with the stream */
void vdec_vp8_done(struct vdec_ctx *ctx, bool ok)
{
	struct vdec_dev *d = ctx->dev;

	if (vp8_debug > 0 || !ok)
		dev_info(d->dev, "vp8 done ok %d: mb %u,%u 174=%#x 15c=%#x irq %#x\n", ok,
			 vdec_r(d, CORE_MC + 0x28), vdec_r(d, CORE_MC + 0x2c),
			 vdec_r(d, CORE_VP8 + 0x174), vdec_r(d, CORE_VP8 + 0x15c),
			 d->irq_status[VDEC_CORE]);
	if (ctx->vp8)
		vdec_hw_vp8_save_seg(d, ctx->vp8->seg_map);
}
