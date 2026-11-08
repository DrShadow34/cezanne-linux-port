// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-venc: V4L2 stateful (mem2mem) H.264 encoder for the MT6885 VENC.
 * Both cores: a stream's frames alternate between them and run two at a
 * time, the hardware keeping each frame behind the one it references. The
 * NBM engines work in the MM SRAM (venc_sram.c). Raw frames in (NV12 /
 * NV21 / I420 / YV12, single or multi-planar), Annex B out, one coded
 * buffer per frame.
 */
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "venc.h"

#define RGB(f, bits)	{ V4L2_PIX_FMT_##f, 1, 1, VENC_FMT_NV12, false, VENC_RGB_EN | (bits), \
			  (bits) & VENC_RGB_4BYTES ? 4 : 3 }
#define RGB4	VENC_RGB_4BYTES
#define AFIRST	VENC_RGB_ALPHA_FIRST
#define BGR	VENC_RGB_BGR

static const struct venc_fmt venc_src_fmts[] = {
	{ V4L2_PIX_FMT_NV12, 1, 2, VENC_FMT_NV12, false },
	{ V4L2_PIX_FMT_NV21, 1, 2, VENC_FMT_NV21, false },
	{ V4L2_PIX_FMT_YUV420, 1, 3, VENC_FMT_3PLANE, false },
	{ V4L2_PIX_FMT_YVU420, 1, 3, VENC_FMT_3PLANE, true },
	{ V4L2_PIX_FMT_NV12M, 2, 2, VENC_FMT_NV12, false },
	{ V4L2_PIX_FMT_NV21M, 2, 2, VENC_FMT_NV21, false },
	{ V4L2_PIX_FMT_YUV420M, 3, 3, VENC_FMT_3PLANE, false },
	{ V4L2_PIX_FMT_YVU420M, 3, 3, VENC_FMT_3PLANE, true },
	/* RGB, converted by the hardware; names in V4L2 terms, memory order after */
	RGB(RGBA32, RGB4),		/* R G B A */
	RGB(RGBX32, RGB4),		/* R G B X */
	RGB(ABGR32, RGB4 | BGR),	/* B G R A */
	RGB(XBGR32, RGB4 | BGR),	/* B G R X */
	RGB(ARGB32, RGB4 | AFIRST),	/* A R G B */
	RGB(XRGB32, RGB4 | AFIRST),	/* X R G B */
	RGB(BGRA32, RGB4 | AFIRST | BGR),	/* A B G R */
	RGB(BGRX32, RGB4 | AFIRST | BGR),	/* X B G R */
	RGB(RGB24, 0),			/* R G B */
	RGB(BGR24, BGR),		/* B G R */
};

#undef RGB
#undef RGB4
#undef AFIRST
#undef BGR

static int venc_trace;
module_param_named(trace, venc_trace, int, 0644);
MODULE_PARM_DESC(trace, "log the timing of the next N frames");

static unsigned int venc_ncores = VENC_CORES;
module_param_named(cores, venc_ncores, uint, 0444);
MODULE_PARM_DESC(cores, "cores to encode on, 1 or 2 (default 2)");

static const struct v4l2_event venc_eos_event = {
	.type = V4L2_EVENT_EOS,
};

static const struct venc_fmt *venc_find_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(venc_src_fmts); i++)
		if (venc_src_fmts[i].fourcc == fourcc)
			return &venc_src_fmts[i];
	return NULL;
}

/* --- formats --- */

/*
 * Raw formats keep the frame's height and the natural plane layout, as
 * FFmpeg's v4l2m2m (copies linesize x format height out of its frame) and
 * GStreamer (overran its own buffer when sizeimage exceeded the layout)
 * need. The hardware does not read past the last row: +028 tells it the
 * padding of the last macroblock row (no IOMMU faults at 1080 lines).
 * Pitches are multiples of 16 (32 for three planes, whose chroma pitch is
 * half the luma pitch). RGB pitches (+1f0, bytes) any multiple of 16 bytes
 * from the row size up, the hardware reads no further than the frame.
 */
static void venc_fill_src(struct v4l2_pix_format_mplane *p, const struct venc_fmt *f)
{
	u32 pitch, row;
	unsigned int i;

	p->width = clamp_t(u32, ALIGN(p->width, 2), VENC_MIN_W, VENC_MAX_W);
	p->height = clamp_t(u32, ALIGN(p->height, 2), VENC_MIN_H, VENC_MAX_H);
	/* portrait sizes are fine up to the macroblocks of 4096x2304 */
	if (DIV_ROUND_UP(p->width, 16) * DIV_ROUND_UP(p->height, 16) > VENC_MAX_MBS)
		p->height = VENC_MAX_MBS / DIV_ROUND_UP(p->width, 16) * 16;
	row = f->rgb ? p->width * f->bpp : p->width;
	pitch = ALIGN(max(p->plane_fmt[0].bytesperline, row), f->comps == 3 ? 32 : 16);
	if (pitch > (f->rgb ? 4 * VENC_MAX_W : 8176))
		pitch = ALIGN(row, f->comps == 3 ? 32 : 16);
	p->pixelformat = f->fourcc;
	p->field = V4L2_FIELD_NONE;
	p->num_planes = f->planes;
	memset(p->plane_fmt, 0, sizeof(p->plane_fmt));
	p->plane_fmt[0].bytesperline = pitch;
	if (f->rgb) {
		p->plane_fmt[0].sizeimage = pitch * p->height;
		return;
	}
	if (f->planes == 1) {
		p->plane_fmt[0].sizeimage = pitch * p->height * 3 / 2;
		return;
	}
	p->plane_fmt[0].sizeimage = pitch * p->height;
	for (i = 1; i < f->planes; i++) {
		p->plane_fmt[i].bytesperline = f->comps == 3 ? pitch / 2 : pitch;
		p->plane_fmt[i].sizeimage = p->plane_fmt[i].bytesperline * p->height / 2;
	}
}

static void venc_fill_dst(struct venc_ctx *ctx, struct v4l2_pix_format_mplane *p)
{
	u32 want = p->plane_fmt[0].sizeimage;

	p->pixelformat = V4L2_PIX_FMT_H264;
	p->width = ctx->crop.width;
	p->height = ctx->crop.height;
	p->field = V4L2_FIELD_NONE;
	p->num_planes = 1;
	p->plane_fmt[0].bytesperline = 0;
	/* one byte a pixel and the parameter sets fit a frame at any QP we use */
	p->plane_fmt[0].sizeimage = clamp_t(u32, max(want, p->width * p->height + SZ_64K),
					    SZ_512K, SZ_32M);
	p->plane_fmt[0].sizeimage = ALIGN(p->plane_fmt[0].sizeimage, SZ_4K);
	p->colorspace = ctx->colorspace;
	p->xfer_func = ctx->xfer_func;
	p->ycbcr_enc = ctx->ycbcr_enc;
	p->quantization = ctx->quant;
}

static void venc_reset_fmts(struct venc_ctx *ctx)
{
	ctx->src_fmt = &venc_src_fmts[0];
	memset(&ctx->src, 0, sizeof(ctx->src));
	ctx->src.width = 1280;
	ctx->src.height = 720;
	ctx->src.colorspace = V4L2_COLORSPACE_REC709;
	venc_fill_src(&ctx->src, ctx->src_fmt);
	ctx->colorspace = ctx->src.colorspace;
	ctx->crop = (struct v4l2_rect){ 0, 0, ctx->src.width, ctx->src.height };
	memset(&ctx->dst, 0, sizeof(ctx->dst));
	venc_fill_dst(ctx, &ctx->dst);
	ctx->tpf = (struct v4l2_fract){ 1, 30 };
}

static int venc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "cezanne-venc", sizeof(cap->driver));
	strscpy(cap->card, "MT6885 H.264 encoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:cezanne-venc", sizeof(cap->bus_info));
	return 0;
}

static int venc_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(venc_src_fmts))
		return -EINVAL;
	f->pixelformat = venc_src_fmts[f->index].fourcc;
	return 0;
}

static int venc_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_H264;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int venc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_H264 &&
			  !venc_find_fmt(fs->pixel_format)))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){ VENC_MIN_W, VENC_MAX_W, 2,
						       VENC_MIN_H, VENC_MAX_H, 2 };
	return 0;
}

static int venc_enum_frameintervals(struct file *file, void *priv,
				    struct v4l2_frmivalenum *fi)
{
	if (fi->index || (fi->pixel_format != V4L2_PIX_FMT_H264 &&
			  !venc_find_fmt(fi->pixel_format)))
		return -EINVAL;
	fi->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
	fi->stepwise.min = (struct v4l2_fract){ 1, 240 };
	fi->stepwise.max = (struct v4l2_fract){ 1, 1 };
	fi->stepwise.step = (struct v4l2_fract){ 1, 1 };
	return 0;
}

static int venc_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = file_to_venc_ctx(file)->src;
	return 0;
}

static int venc_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = file_to_venc_ctx(file)->dst;
	return 0;
}

static int venc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	const struct venc_fmt *vf = venc_find_fmt(f->fmt.pix_mp.pixelformat);

	venc_fill_src(&f->fmt.pix_mp, vf ?: &venc_src_fmts[0]);
	return 0;
}

static int venc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	venc_fill_dst(file_to_venc_ctx(file), &f->fmt.pix_mp);
	return 0;
}

static int venc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	venc_try_fmt_out(file, priv, f);
	ctx->src_fmt = venc_find_fmt(p->pixelformat);
	ctx->src = *p;
	ctx->colorspace = p->colorspace;
	ctx->xfer_func = p->xfer_func;
	ctx->ycbcr_enc = p->ycbcr_enc;
	ctx->quant = p->quantization;
	/* RGB comes out as BT.709 limited range YCbCr (venc_h264_run) */
	if (ctx->src_fmt->rgb) {
		ctx->colorspace = V4L2_COLORSPACE_REC709;
		ctx->xfer_func = V4L2_XFER_FUNC_709;
		ctx->ycbcr_enc = V4L2_YCBCR_ENC_709;
		ctx->quant = V4L2_QUANTIZATION_LIM_RANGE;
	}
	ctx->crop = (struct v4l2_rect){ 0, 0, p->width, p->height };
	/* the coded format follows the raw one */
	venc_fill_dst(ctx, &ctx->dst);
	return 0;
}

static int venc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	venc_fill_dst(ctx, &f->fmt.pix_mp);
	ctx->dst = f->fmt.pix_mp;
	return 0;
}

static int venc_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		s->r = ctx->crop;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r = (struct v4l2_rect){ 0, 0, ctx->src.width, ctx->src.height };
		return 0;
	default:
		return -EINVAL;
	}
}

/* the visible part: top left of the frame, whole macroblocks or not */
static int venc_s_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT || s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;
	if (vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	s->r.left = 0;
	s->r.top = 0;
	s->r.width = clamp_t(u32, ALIGN(s->r.width, 2), VENC_MIN_W, ctx->src.width);
	s->r.height = clamp_t(u32, ALIGN(s->r.height, 2), VENC_MIN_H, ctx->src.height);
	ctx->crop = s->r;
	if (!vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		venc_fill_dst(ctx, &ctx->dst);
	return 0;
}

static int venc_g_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);

	if (V4L2_TYPE_IS_OUTPUT(a->type)) {
		a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
		a->parm.output.timeperframe = ctx->tpf;
	} else {
		a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		a->parm.capture.timeperframe = ctx->tpf;
	}
	return 0;
}

static int venc_s_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);
	struct v4l2_fract *t = V4L2_TYPE_IS_OUTPUT(a->type) ? &a->parm.output.timeperframe :
							       &a->parm.capture.timeperframe;

	if (!t->numerator || !t->denominator)
		*t = ctx->tpf;
	/* 1 to 240 frames a second */
	if ((u64)t->numerator * 240 < t->denominator)
		*t = (struct v4l2_fract){ 1, 240 };
	if (t->numerator > t->denominator)
		*t = (struct v4l2_fract){ 1, 1 };
	ctx->tpf = *t;
	return venc_g_parm(file, priv, a);
}

static int venc_encoder_cmd(struct file *file, void *priv, struct v4l2_encoder_cmd *ec)
{
	struct venc_ctx *ctx = file_to_venc_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret < 0)
		return ret;
	if (!vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    !vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return 0;
	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
	if (ret < 0)
		return ret;
	if (ec->cmd == V4L2_ENC_CMD_STOP && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &venc_eos_event);
	if (ec->cmd == V4L2_ENC_CMD_START && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
	return 0;
}

static int venc_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops venc_ioctl_ops = {
	.vidioc_querycap = venc_querycap,
	.vidioc_enum_framesizes = venc_enum_framesizes,
	.vidioc_enum_frameintervals = venc_enum_frameintervals,
	.vidioc_enum_fmt_vid_out = venc_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = venc_enum_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = venc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = venc_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = venc_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = venc_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = venc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = venc_s_fmt_cap,
	.vidioc_g_selection = venc_g_selection,
	.vidioc_s_selection = venc_s_selection,
	.vidioc_g_parm = venc_g_parm,
	.vidioc_s_parm = venc_s_parm,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = venc_encoder_cmd,
	.vidioc_subscribe_event = venc_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* --- vb2 --- */

static int venc_queue_setup(struct vb2_queue *vq, unsigned int *nbufs, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *p = V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src : &ctx->dst;
	unsigned int i;

	if (*nplanes) {
		if (*nplanes != p->num_planes)
			return -EINVAL;
		for (i = 0; i < p->num_planes; i++)
			if (sizes[i] < p->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}
	*nplanes = p->num_planes;
	for (i = 0; i < p->num_planes; i++)
		sizes[i] = p->plane_fmt[i].sizeimage;
	return 0;
}

static u64 venc_buf_bytes(struct vb2_buffer *vb)
{
	u64 n = 0;
	unsigned int i;

	if (vb->memory == VB2_MEMORY_MMAP)
		for (i = 0; i < vb->num_planes; i++)
			n += PAGE_ALIGN(vb2_plane_size(vb, i));
	return n;
}

static int venc_buf_init(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	atomic64_add(venc_buf_bytes(vb), &ctx->dev->mem_used);
	return 0;
}

static void venc_buf_cleanup(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	atomic64_sub(venc_buf_bytes(vb), &ctx->dev->mem_used);
}

static int venc_buf_prepare(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *p = V4L2_TYPE_IS_OUTPUT(vb->type) ? &ctx->src : &ctx->dst;
	unsigned int i;

	for (i = 0; i < p->num_planes; i++) {
		if (vb2_plane_size(vb, i) < p->plane_fmt[i].sizeimage)
			return -EINVAL;
		/* source addresses in 16-byte units, the output 128-byte aligned */
		if (vb2_dma_contig_plane_dma_addr(vb, i) &
		    (V4L2_TYPE_IS_OUTPUT(vb->type) ? 15 : VENC_BS_ALIGN - 1))
			return -EINVAL;
	}
	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		for (i = 0; i < p->num_planes; i++)
			if (!vb2_get_plane_payload(vb, i))
				vb2_set_plane_payload(vb, i, p->plane_fmt[i].sizeimage);
	}
	return 0;
}

static void venc_buf_queue(struct vb2_buffer *vb)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	/* a coded buffer queued after the stream stopped ends empty, LAST */
	if (V4L2_TYPE_IS_CAPTURE(vb->type) && vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->field = V4L2_FIELD_NONE;
		vbuf->sequence = ctx->sequence_cap++;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		v4l2_event_queue_fh(&ctx->fh, &venc_eos_event);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void venc_return_bufs(struct venc_ctx *ctx, struct vb2_queue *q,
			     enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						    v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			return;
		v4l2_m2m_buf_done(vb, state);
	}
}

static int venc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);
	int ret;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_CAPTURE(q->type)) {
		ctx->sequence_cap = 0;
		return 0;
	}
	ctx->sequence_out = 0;
	ret = pm_runtime_resume_and_get(ctx->dev->dev);
	if (ret)
		goto err;
	ret = venc_h264_start(ctx);
	if (ret) {
		pm_runtime_put_autosuspend(ctx->dev->dev);
		goto err;
	}
	return 0;
err:
	venc_return_bufs(ctx, q, VB2_BUF_STATE_QUEUED);
	return ret;
}

static bool venc_idle(struct venc_ctx *ctx)
{
	unsigned long flags;
	bool idle;

	spin_lock_irqsave(&ctx->dev->hw_lock, flags);
	idle = !ctx->job_count;
	spin_unlock_irqrestore(&ctx->dev->hw_lock, flags);
	return idle;
}

static void venc_stop_streaming(struct vb2_queue *q)
{
	struct venc_ctx *ctx = vb2_get_drv_priv(q);
	struct venc_dev *d = ctx->dev;

	/* frames on the cores use buffers of both queues (watchdogs end hangs) */
	if (!wait_event_timeout(ctx->idle_wq, venc_idle(ctx),
				msecs_to_jiffies(3 * VENC_TIMEOUT_MS)))
		dev_err(d->dev, "frames still running at stream off\n");
	/* and the completion is past its last use of ctx */
	mutex_lock(&d->done_lock);
	mutex_unlock(&d->done_lock);
	venc_return_bufs(ctx, q, VB2_BUF_STATE_ERROR);
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		if (v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
			v4l2_event_queue_fh(&ctx->fh, &venc_eos_event);
		venc_h264_stop(ctx);
		pm_runtime_put_autosuspend(ctx->dev->dev);
	}
}

static const struct vb2_ops venc_qops = {
	.queue_setup = venc_queue_setup,
	.buf_init = venc_buf_init,
	.buf_cleanup = venc_buf_cleanup,
	.buf_prepare = venc_buf_prepare,
	.buf_queue = venc_buf_queue,
	.start_streaming = venc_start_streaming,
	.stop_streaming = venc_stop_streaming,
};

static int venc_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct venc_ctx *ctx = priv;
	struct venc_dev *d = ctx->dev;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	/* user pointers too: behind the IOMMU they need not be contiguous */
	src->io_modes = VB2_MMAP | VB2_DMABUF | VB2_USERPTR;
	src->drv_priv = ctx;
	src->ops = &venc_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &d->vdev_lock;
	src->dev = d->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->ops = &venc_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &d->vdev_lock;
	dst->dev = d->dev;
	return vb2_queue_init(dst);
}

/* --- jobs --- */

/*
 * device_run starts a context's next frame on its core (frames alternate
 * between the cores) and ends the m2m job at once, so the next frame, of
 * this context or another, can start on the other core. The cores' IRQs
 * complete frames in each context's order. The buffers of running frames
 * stay at the head of the m2m queues until then: the first job_count
 * buffers of both queues are the running frames' (kept so under hw_lock),
 * and the drain helpers find the last source buffer as with one job.
 */

/* the n-th buffer of a ready queue, 0 the head */
static struct vb2_v4l2_buffer *venc_nth_buf(struct v4l2_m2m_queue_ctx *q, unsigned int n)
{
	struct vb2_v4l2_buffer *vb = NULL;
	struct v4l2_m2m_buffer *b;
	unsigned long flags;

	spin_lock_irqsave(&q->rdy_spinlock, flags);
	list_for_each_entry(b, &q->rdy_queue, list)
		if (!n--) {
			vb = &b->vb;
			break;
		}
	spin_unlock_irqrestore(&q->rdy_spinlock, flags);
	return vb;
}

static bool venc_header_pending(struct venc_ctx *ctx)
{
	return ctx->c_header->val == V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE && !ctx->header_sent;
}

/* under the m2m job lock: a frame not yet running, and its core free */
static int venc_job_ready(void *priv)
{
	struct venc_ctx *ctx = priv;
	struct venc_dev *d = ctx->dev;
	struct v4l2_m2m_ctx *m = ctx->fh.m2m_ctx;
	unsigned long flags;
	int ready;

	spin_lock_irqsave(&d->hw_lock, flags);
	if (venc_header_pending(ctx))
		ready = !ctx->job_count;
	else
		ready = ctx->job_count < d->ncores &&
			v4l2_m2m_num_src_bufs_ready(m) > ctx->job_count &&
			v4l2_m2m_num_dst_bufs_ready(m) > ctx->job_count &&
			!d->core[venc_next_core(ctx)].job;
	spin_unlock_irqrestore(&d->hw_lock, flags);
	return ready;
}

/* busy time per core, over the core count: 100 % = both cores saturated */
static void venc_account(struct venc_dev *d, struct venc_ctx *ctx, u64 ns)
{
	unsigned long flags;

	ns = div_u64(ns, d->ncores);
	spin_lock_irqsave(&d->stats_lock, flags);
	d->busy_ns += ns;
	d->jobs++;
	ctx->busy_ns += ns;
	ctx->jobs++;
	spin_unlock_irqrestore(&d->stats_lock, flags);
}

/* the context's finished frames back to userspace, oldest first */
static void venc_complete(struct venc_ctx *ctx)
{
	struct venc_dev *d = ctx->dev;
	struct v4l2_m2m_ctx *m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src, *dst;
	struct venc_job job;
	unsigned long flags;

	mutex_lock(&d->done_lock);
	for (;;) {
		spin_lock_irqsave(&d->hw_lock, flags);
		if (!ctx->job_count || !ctx->frames[ctx->job_head].done) {
			spin_unlock_irqrestore(&d->hw_lock, flags);
			break;
		}
		job = ctx->frames[ctx->job_head];
		src = v4l2_m2m_src_buf_remove(m);
		dst = v4l2_m2m_dst_buf_remove(m);
		ctx->job_head = (ctx->job_head + 1) % VENC_CORES;
		ctx->job_count--;
		spin_unlock_irqrestore(&d->hw_lock, flags);
		WARN_ON(src != job.src || dst != job.dst);

		if (job.state == VB2_BUF_STATE_DONE)
			venc_h264_done(ctx, &job);
		else
			ctx->force_key = true;	/* later frames reference garbage */
		if (dst) {
			if (src)
				v4l2_m2m_buf_copy_metadata(src, dst);
			vb2_set_plane_payload(&dst->vb2_buf, 0, job.state == VB2_BUF_STATE_DONE ?
					      job.hdr_off + job.bytes : 0);
			dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
					V4L2_BUF_FLAG_LAST);
			dst->flags |= job.idr ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
			dst->sequence = ctx->sequence_cap++;
			if (src && v4l2_m2m_is_last_draining_src_buf(m, src)) {
				dst->flags |= V4L2_BUF_FLAG_LAST;
				v4l2_event_queue_fh(&ctx->fh, &venc_eos_event);
				v4l2_m2m_mark_stopped(m);
			}
			v4l2_m2m_buf_done(dst, job.state);
		}
		if (src) {
			src->sequence = ctx->sequence_out++;
			v4l2_m2m_buf_done(src, job.state);
		}
	}
	wake_up(&ctx->idle_wq);
	mutex_unlock(&d->done_lock);
}

/* a core is free: any context may have a frame for it */
static void venc_schedule_all(struct venc_dev *d)
{
	struct venc_ctx *ctx;

	mutex_lock(&d->clients_lock);
	list_for_each_entry(ctx, &d->clients, client)
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	mutex_unlock(&d->clients_lock);
}

/* the core's frame finished or failed */
static void venc_core_done(struct venc_core *c, enum vb2_buffer_state state, u32 bytes)
{
	struct venc_dev *d = c->d;
	u64 ns = ktime_to_ns(ktime_sub(ktime_get(), c->start));
	struct venc_job *job;
	unsigned long flags;

	spin_lock_irqsave(&d->hw_lock, flags);
	job = c->job;
	c->job = NULL;
	if (job) {
		job->state = state;
		job->bytes = bytes;
		job->done = true;
	}
	spin_unlock_irqrestore(&d->hw_lock, flags);
	if (!job)
		return;
	/* the context lives on until its last frame is completed */
	venc_account(d, job->ctx, ns);
	venc_complete(job->ctx);
	venc_schedule_all(d);
}

/* header mode "separate": the parameter sets alone, before the first frame */
static void venc_header_job(struct venc_ctx *ctx)
{
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);

	if (out)
		memcpy(out, ctx->hdr, ctx->hdr_len);
	vb2_set_plane_payload(&dst->vb2_buf, 0, out ? ctx->hdr_len : 0);
	if (src)
		v4l2_m2m_buf_copy_metadata(src, dst);
	dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_LAST);
	dst->sequence = ctx->sequence_cap++;
	ctx->header_sent = true;
	v4l2_m2m_buf_done(dst, out ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

static void venc_device_run(void *priv)
{
	struct venc_ctx *ctx = priv;
	struct venc_dev *d = ctx->dev;
	struct v4l2_m2m_ctx *m = ctx->fh.m2m_ctx;
	struct venc_job *job = NULL;
	struct venc_core *c;
	unsigned long flags;
	unsigned int n;

	if (venc_header_pending(ctx)) {
		venc_header_job(ctx);
		return;
	}
	spin_lock_irqsave(&d->hw_lock, flags);
	c = &d->core[venc_next_core(ctx)];
	n = ctx->job_count;
	/* another context may have taken the core since job_ready */
	if (!c->job && n < d->ncores) {
		job = &ctx->frames[(ctx->job_head + n) % VENC_CORES];
		*job = (struct venc_job){
			.ctx = ctx,
			.core = c->id,
			.src = venc_nth_buf(&m->out_q_ctx, n),
			.dst = venc_nth_buf(&m->cap_q_ctx, n),
		};
		if (job->src && job->dst) {
			c->job = job;
			ctx->job_count++;
		} else {
			job = NULL;
		}
	}
	spin_unlock_irqrestore(&d->hw_lock, flags);
	if (job) {
		c->start = ktime_get();
		c->irq_status = 0;
		schedule_delayed_work(&c->watchdog, msecs_to_jiffies(VENC_TIMEOUT_MS));
		venc_h264_run(ctx, job);
	}
	v4l2_m2m_job_finish(d->m2m_dev, m);
}

static const struct v4l2_m2m_ops venc_m2m_ops = {
	.device_run = venc_device_run,
	.job_ready = venc_job_ready,
};

static void venc_watchdog(struct work_struct *w)
{
	struct venc_core *c = container_of(to_delayed_work(w), struct venc_core, watchdog);

	dev_err(c->d->dev, "core %u: timeout (status %#x)\n", c->id, venc_r(c, VENC_IRQ_STATUS));
	venc_hw_reset(c);
	venc_core_done(c, VB2_BUF_STATE_ERROR, 0);
}

static irqreturn_t venc_irq(int irq, void *data)
{
	struct venc_core *c = data;
	u32 st = venc_r(c, VENC_IRQ_STATUS);

	c->t_irq = ktime_get();
	/* the BSP ISR acks each set status bit */
	venc_w(c, VENC_IRQ_ACK, st);
	c->irq_status |= st;
	if (!(st & (VENC_IRQ_FRAME | VENC_IRQ_DRAM_FULL)))
		return IRQ_HANDLED;
	return IRQ_WAKE_THREAD;
}

static irqreturn_t venc_irq_thread(int irq, void *data)
{
	struct venc_core *c = data;
	u32 st = c->irq_status, bytes = venc_r(c, VENC_BS_BYTES);

	if (!cancel_delayed_work(&c->watchdog))
		return IRQ_HANDLED;	/* the watchdog got there first */
	venc_w(c, VENC_CLK_EN, 0);
	if (st & VENC_IRQ_DRAM_FULL) {
		/* the coded buffer was too small: the frame is lost */
		dev_warn_ratelimited(c->d->dev, "core %u: coded buffer full\n", c->id);
		venc_hw_reset(c);
		venc_core_done(c, VB2_BUF_STATE_ERROR, 0);
		return IRQ_HANDLED;
	}
	if (venc_trace > 0) {
		venc_trace--;
		dev_info(c->d->dev, "core %u frame: start->irq %lld us, ->thread %lld us, %u bytes, status %#x\n",
			 c->id, ktime_us_delta(c->t_irq, c->start),
			 ktime_us_delta(ktime_get(), c->start), bytes, st);
	}
	venc_core_done(c, VB2_BUF_STATE_DONE, bytes);
	return IRQ_HANDLED;
}

/* --- file --- */

static int venc_open(struct file *f)
{
	struct venc_dev *d = video_drvdata(f);
	struct venc_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = d;
	init_waitqueue_head(&ctx->idle_wq);
	ret = venc_h264_ctrls_init(ctx);
	if (ret)
		goto err_free;
	venc_reset_fmts(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(f));
	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(d->m2m_dev, ctx, venc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_fh;
	}
	v4l2_fh_add(&ctx->fh, f);
	ctx->tgid = task_tgid_nr(current);
	get_task_comm(ctx->comm, current->group_leader);
	mutex_lock(&d->clients_lock);
	list_add_tail(&ctx->client, &d->clients);
	mutex_unlock(&d->clients_lock);
	return 0;

err_fh:
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
err_free:
	kfree(ctx);
	return ret;
}

static int venc_release(struct file *f)
{
	struct venc_ctx *ctx = file_to_venc_ctx(f);
	struct venc_dev *d = ctx->dev;

	mutex_lock(&d->clients_lock);
	list_del(&ctx->client);
	mutex_unlock(&d->clients_lock);
	v4l2_fh_del(&ctx->fh, f);
	mutex_lock(&d->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&d->vdev_lock);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations venc_fops = {
	.owner = THIS_MODULE,
	.open = venc_open,
	.release = venc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* --- power --- */

/*
 * The NBM engines' ports (RDMA, RDMA_LITE, WDMA, WDMA_LITE, FCS_NBM R / W)
 * to the MM SRAM's SMI common: BSP smi_sysram_enable, after each larb
 * power-up (only bit 16 of the field sticks). Without it, their accesses
 * to 0x1e000000 go to DRAM, wrapped into RAM the kernel owns.
 */
static const u8 venc_sram_ports[] = { 5, 6, 13, 14, 21, 22 };
#define SMI_LARB_OSTD_MON(p)		(0x280 + (p) * 4)
#define SMI_LARB_INT_OSTD_MON(p)	(0x780 + (p) * 4)
#define SMI_LARB_NONSEC_CON(p)		(0x380 + (p) * 4)
#define SMI_LARB_SYSRAM			GENMASK(19, 16)

static int venc_route_sram(struct venc_dev *d, bool on)
{
	unsigned int i, k;
	u32 val;

	for (i = 0; i < VENC_CORES; i++)
		for (k = 0; k < ARRAY_SIZE(venc_sram_ports); k++) {
			void __iomem *l = d->larb_regs[i];
			u8 p = venc_sram_ports[k];

			if (on && (readl(l + SMI_LARB_OSTD_MON(p)) ||
				   readl(l + SMI_LARB_INT_OSTD_MON(p)))) {
				dev_err(d->dev, "larb %u port %u busy: not routed to the SRAM\n",
					i, p);
				return -EBUSY;
			}
			val = readl(l + SMI_LARB_NONSEC_CON(p)) & ~SMI_LARB_SYSRAM;
			writel(val | (on ? SMI_LARB_SYSRAM : 0), l + SMI_LARB_NONSEC_CON(p));
		}
	return 0;
}

/* both cores' domains and larbs, the MM SRAM and the routing to it */
static int venc_runtime_resume(struct device *dev)
{
	struct venc_dev *d = dev_get_drvdata(dev);
	int i, ret;

	for (i = 0; i < VENC_CORES; i++) {
		ret = pm_runtime_resume_and_get(d->pd[i]);
		if (ret)
			goto err;
		ret = pm_runtime_resume_and_get(d->larb[i]);
		if (ret) {
			pm_runtime_put_sync(d->pd[i]);
			goto err;
		}
	}
	ret = venc_sram_on(d->sram);
	if (ret)
		goto err;
	ret = venc_route_sram(d, true);
	if (ret) {
		venc_route_sram(d, false);
		venc_sram_off(d->sram);
		goto err;
	}
	return 0;
err:
	while (i--) {
		pm_runtime_put_sync(d->larb[i]);
		pm_runtime_put_sync(d->pd[i]);
	}
	dev_err(dev, "power on failed: %d\n", ret);
	return ret;
}

static int venc_runtime_suspend(struct device *dev)
{
	struct venc_dev *d = dev_get_drvdata(dev);
	int i;

	venc_route_sram(d, false);
	venc_sram_off(d->sram);
	for (i = VENC_CORES - 1; i >= 0; i--) {
		pm_runtime_put_sync(d->larb[i]);
		pm_runtime_put_sync(d->pd[i]);
	}
	return 0;
}

static const struct dev_pm_ops venc_pm_ops = {
	RUNTIME_PM_OPS(venc_runtime_suspend, venc_runtime_resume, NULL)
};

/*
 * --- usage for monitors (as cezanne-vdec) ---
 * vcodec_engine   "encode"
 * vcodec_busy_ns  total time frames took (start to completion) over the
 *                 number of cores, ns
 * vcodec_jobs     frames encoded
 * vcodec_clients  one line per open file: "tgid busy_ns jobs comm"
 * vcodec_memory   bytes in encoder buffers, all clients
 */
static ssize_t vcodec_engine_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "encode\n");
}
static DEVICE_ATTR_RO(vcodec_engine);

static ssize_t vcodec_busy_ns_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct venc_dev *d = dev_get_drvdata(dev);
	unsigned long flags;
	u64 ns;

	spin_lock_irqsave(&d->stats_lock, flags);
	ns = d->busy_ns;
	spin_unlock_irqrestore(&d->stats_lock, flags);
	return sysfs_emit(buf, "%llu\n", ns);
}
static DEVICE_ATTR_RO(vcodec_busy_ns);

static ssize_t vcodec_jobs_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct venc_dev *d = dev_get_drvdata(dev);
	unsigned long flags;
	u64 n;

	spin_lock_irqsave(&d->stats_lock, flags);
	n = d->jobs;
	spin_unlock_irqrestore(&d->stats_lock, flags);
	return sysfs_emit(buf, "%llu\n", n);
}
static DEVICE_ATTR_RO(vcodec_jobs);

static ssize_t vcodec_clients_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct venc_dev *d = dev_get_drvdata(dev);
	struct venc_ctx *ctx;
	unsigned long flags;
	u64 ns, n;
	int len = 0;

	mutex_lock(&d->clients_lock);
	list_for_each_entry(ctx, &d->clients, client) {
		spin_lock_irqsave(&d->stats_lock, flags);
		ns = ctx->busy_ns;
		n = ctx->jobs;
		spin_unlock_irqrestore(&d->stats_lock, flags);
		len += sysfs_emit_at(buf, len, "%d %llu %llu %s\n", ctx->tgid, ns, n, ctx->comm);
	}
	mutex_unlock(&d->clients_lock);
	return len;
}
static DEVICE_ATTR_RO(vcodec_clients);

static ssize_t vcodec_memory_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct venc_dev *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&d->mem_used));
}
static DEVICE_ATTR_RO(vcodec_memory);

static struct attribute *venc_attrs[] = {
	&dev_attr_vcodec_engine.attr,
	&dev_attr_vcodec_busy_ns.attr,
	&dev_attr_vcodec_jobs.attr,
	&dev_attr_vcodec_clients.attr,
	&dev_attr_vcodec_memory.attr,
	NULL
};
ATTRIBUTE_GROUPS(venc);

/* --- probe --- */

/* larb i and its registers (mtk-smi owns them; the SRAM routing bits are ours) */
static struct device *venc_larb(struct venc_dev *d, int i)
{
	struct device_node *np = of_parse_phandle(d->dev->of_node, "mediatek,larbs", i);
	struct platform_device *pdev;
	struct resource *res;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	d->larb_regs[i] = res ? devm_ioremap(d->dev, res->start, resource_size(res)) : NULL;
	if (!d->larb_regs[i])
		return ERR_PTR(-ENOMEM);
	return &pdev->dev;
}

/*
 * Core 0's larb7 is behind the MM IOMMU, core 1's larb8 behind the MDP one,
 * and a device names one IOMMU only: core 1's ports belong to the
 * "venc-core1" child, which must land in the parent's IOMMU group (one
 * domain, page table shared by both IOMMUs). As in cezanne-vdec.
 */
static int venc_attach_core1(struct venc_dev *d)
{
	struct device_node *np;
	struct platform_device *core;
	struct iommu_group *g, *gc;
	int ret;

	if (!device_iommu_mapped(d->dev))
		return 0;
	ret = devm_of_platform_populate(d->dev);
	if (ret)
		return ret;
	np = of_get_compatible_child(d->dev->of_node, "cezanne,mt6885-venc-core1");
	core = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!core)
		return dev_err_probe(d->dev, -ENODEV, "IOMMU but no venc-core1 child\n");
	if (!core->dev.driver) {
		put_device(&core->dev);
		return -EPROBE_DEFER;
	}
	g = iommu_group_get(d->dev);
	gc = iommu_group_get(&core->dev);
	ret = g && g == gc ? 0 : -EINVAL;
	if (ret)
		dev_err(d->dev, "venc-core1 not in core 0's IOMMU group\n");
	iommu_group_put(gc);
	iommu_group_put(g);
	put_device(&core->dev);
	return ret;
}

static void venc_detach_pds(void *data)
{
	struct venc_dev *d = data;
	int i;

	for (i = 0; i < 2; i++)
		if (!IS_ERR_OR_NULL(d->pd[i]))
			dev_pm_domain_detach(d->pd[i], true);
}

static int venc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct venc_dev *d;
	int i, ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	d->ncores = clamp(venc_ncores, 1u, (unsigned int)VENC_CORES);
	mutex_init(&d->vdev_lock);
	mutex_init(&d->clients_lock);
	mutex_init(&d->done_lock);
	spin_lock_init(&d->hw_lock);
	spin_lock_init(&d->stats_lock);
	INIT_LIST_HEAD(&d->clients);
	platform_set_drvdata(pdev, d);

	for (i = 0; i < VENC_CORES; i++) {
		struct venc_core *c = &d->core[i];

		c->d = d;
		c->id = i;
		INIT_DELAYED_WORK(&c->watchdog, venc_watchdog);
		c->regs = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(c->regs))
			return PTR_ERR(c->regs);
	}
	/* without the SRAM the NBM traffic would land in DRAM: no encoder then */
	d->sram = venc_sram_find(dev);
	if (IS_ERR(d->sram))
		return dev_err_probe(dev, PTR_ERR(d->sram), "MM SRAM (cezanne,mmsram)\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	/* imported dma-bufs map to one IOVA range */
	dma_set_max_seg_size(dev, UINT_MAX);
	ret = venc_attach_core1(d);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, venc_detach_pds, d);
	if (ret)
		return ret;
	for (i = 0; i < VENC_CORES; i++) {
		struct venc_core *c = &d->core[i];

		d->larb[i] = venc_larb(d, i);
		if (IS_ERR(d->larb[i]))
			return dev_err_probe(dev, PTR_ERR(d->larb[i]), "larb %d\n", i);
		d->pd[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(d->pd[i]))
			return dev_err_probe(dev, d->pd[i] ? PTR_ERR(d->pd[i]) : -ENODEV,
					     "power domain %d\n", i);
		c->irq = platform_get_irq(pdev, i);
		if (c->irq < 0)
			return c->irq;
		ret = devm_request_threaded_irq(dev, c->irq, venc_irq, venc_irq_thread, 0,
						i ? "venc-core1" : "venc-core0", c);
		if (ret)
			return ret;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &d->v4l2_dev);
	if (ret)
		return ret;
	d->m2m_dev = v4l2_m2m_init(&venc_m2m_ops);
	if (IS_ERR(d->m2m_dev)) {
		ret = PTR_ERR(d->m2m_dev);
		goto err_v4l2;
	}
	d->vdev.lock = &d->vdev_lock;
	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.fops = &venc_fops;
	d->vdev.release = video_device_release_empty;
	d->vdev.vfl_dir = VFL_DIR_M2M;
	d->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	d->vdev.ioctl_ops = &venc_ioctl_ops;
	strscpy(d->vdev.name, "cezanne-venc", sizeof(d->vdev.name));
	video_set_drvdata(&d->vdev, d);
	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	dev_info(dev, "registered %s, %u core%s%s, MM SRAM\n", video_device_node_name(&d->vdev),
		 d->ncores, d->ncores > 1 ? "s" : "", device_iommu_mapped(dev) ? ", IOMMU" : "");
	return 0;

err_m2m:
	v4l2_m2m_release(d->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&d->v4l2_dev);
	return ret;
}

static void venc_remove(struct platform_device *pdev)
{
	struct venc_dev *d = platform_get_drvdata(pdev);

	video_unregister_device(&d->vdev);
	cancel_delayed_work_sync(&d->core[0].watchdog);
	cancel_delayed_work_sync(&d->core[1].watchdog);
	v4l2_m2m_release(d->m2m_dev);
	v4l2_device_unregister(&d->v4l2_dev);
}

static const struct of_device_id venc_of_match[] = {
	{ .compatible = "cezanne,mt6885-venc" },
	{ }
};
MODULE_DEVICE_TABLE(of, venc_of_match);

static struct platform_driver venc_driver = {
	.probe = venc_probe,
	.remove = venc_remove,
	.driver = {
		.name = "cezanne-venc",
		.of_match_table = venc_of_match,
		.pm = pm_ptr(&venc_pm_ops),
		.dev_groups = venc_groups,
	},
};

/* nothing to do: binding attaches core 1's larb8 ports to the IOMMU */
static int venc_core1_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id venc_core1_of_match[] = {
	{ .compatible = "cezanne,mt6885-venc-core1" },
	{ }
};
MODULE_DEVICE_TABLE(of, venc_core1_of_match);

static struct platform_driver venc_core1_driver = {
	.probe = venc_core1_probe,
	.driver = {
		.name = "cezanne-venc-core1",
		.of_match_table = venc_core1_of_match,
	},
};

/* core 1 and the SRAM first: the encoder's probe expects them bound */
static struct platform_driver * const venc_drivers[] = {
	&venc_core1_driver,
	&venc_sram_driver,
	&venc_driver,
};

static int __init venc_init(void)
{
	return platform_register_drivers(venc_drivers, ARRAY_SIZE(venc_drivers));
}
module_init(venc_init);

static void __exit venc_exit(void)
{
	platform_unregister_drivers(venc_drivers, ARRAY_SIZE(venc_drivers));
}
module_exit(venc_exit);

MODULE_DESCRIPTION("MT6885 V4L2 stateful H.264 encoder");
MODULE_LICENSE("GPL");
