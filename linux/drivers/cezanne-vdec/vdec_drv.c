// SPDX-License-Identifier: GPL-2.0
/*
 * cezanne-vdec: device, V4L2 / mem2mem / request plumbing, power, IRQs.
 */
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/media.h>
#include <media/media-devnode.h>
#include <media/media-request.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>

#include "vdec.h"

#define VDEC_TIMEOUT_MS		1000

int vdec_trace;
module_param_named(trace, vdec_trace, int, 0644);
MODULE_PARM_DESC(trace, "log the register accesses of the next N jobs");
static int vdec_trace_skip;
module_param_named(trace_skip, vdec_trace_skip, int, 0644);
MODULE_PARM_DESC(trace_skip, "run this many jobs untraced before the traced ones");
/*
 * Behind the IOMMU buffers come from ordinary RAM, not a 512 MiB CMA pool,
 * and nothing else stops them: six 4K contexts took all of it and the
 * phone stalled (no swap, buffer pages are invisible to the OOM killer).
 * Past this budget REQBUFS / CREATE_BUFS fail with -ENOMEM instead. It
 * covers our buffers only; the players' linear surfaces come on top.
 */
static unsigned int vdec_mem_limit_mb;
module_param_named(mem_limit_mb, vdec_mem_limit_mb, uint, 0644);
MODULE_PARM_DESC(mem_limit_mb, "decoder buffer budget in MiB, all contexts (0: a quarter of RAM)");

/* --- formats --- */

static void vdec_fill_dst_fmt(struct vdec_ctx *ctx)
{
	struct v4l2_pix_format_mplane *src = &ctx->src_fmt.fmt.pix_mp;
	struct v4l2_format *f = &ctx->dst_fmt;
	/* H.264 / AV1 write 16x32 MM21 tiles; VP9 whole 64x64 superblocks; the
	 * HEVC core takes the vendor's 64-aligned pitch (mc+980) */
	unsigned int a = src->pixelformat == V4L2_PIX_FMT_VP9_FRAME ||
			 src->pixelformat == V4L2_PIX_FMT_HEVC_SLICE ? 64 : 16;
	unsigned int w = ALIGN(src->width, a), h = ALIGN(src->height, max(a, 32u));

	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	/*
	 * MM21: 16x32 luma tiles then 16x32 chroma tiles, the core's output
	 * layout. Not in the v4l2 core format table, so fill it by hand:
	 * plane 0 = W*H luma, plane 1 = W*H/2 chroma (4:2:0).
	 */
	/*
	 * Visible size as the format size, coded tile grid in bytesperline /
	 * sizeimage: GStreamer derives the tile rows from the format height
	 * (ceil(height / 32)) and compares the layout against one built from
	 * the display size; reporting the 16-aligned coded height here made
	 * the two disagree for cropped streams (1080 vs 1088) and the untiled
	 * picture came out displaced.
	 */
	f->fmt.pix_mp.width = ctx->disp_w;
	f->fmt.pix_mp.height = ctx->disp_h;
	f->fmt.pix_mp.pixelformat = ctx->cap_10bit ? V4L2_PIX_FMT_MT2110T : V4L2_PIX_FMT_MM21;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.num_planes = 2;
	f->fmt.pix_mp.plane_fmt[0].bytesperline = w;
	f->fmt.pix_mp.plane_fmt[0].sizeimage = w * h;
	/* MM21 plane 1 is UV-interleaved (like NV12): a chroma row is W
	 * bytes (W/2 U + W/2 V), so bytesperline is W, not W/2 (matches
	 * mtk-vcodec). */
	f->fmt.pix_mp.plane_fmt[1].bytesperline = w;
	f->fmt.pix_mp.plane_fmt[1].sizeimage = w * h / 2;
	if (ctx->cap_10bit) {
		/* MT2110T: the same tile grid, 80 bytes per 16x4 block (16 of
		 * 2-bit LSBs, 64 of MSBs): 5/4 of the MM21 sizes */
		f->fmt.pix_mp.plane_fmt[0].sizeimage = w * h * 5 / 4;
		f->fmt.pix_mp.plane_fmt[1].sizeimage = w * h / 2 * 5 / 4;
	}
	if (src->pixelformat == V4L2_PIX_FMT_VP8_FRAME) {
		/* the VP8 core finds a reference's chroma at a fixed offset
		 * from its luma: one buffer, chroma after the luma tiles */
		f->fmt.pix_mp.num_planes = 1;
		f->fmt.pix_mp.plane_fmt[0].sizeimage = w * h * 3 / 2 + VDEC_VP8_AUX(w, h);
		memset(&f->fmt.pix_mp.plane_fmt[1], 0, sizeof(f->fmt.pix_mp.plane_fmt[1]));
	}
	f->fmt.pix_mp.colorspace = src->colorspace;
	f->fmt.pix_mp.ycbcr_enc = src->ycbcr_enc;
	f->fmt.pix_mp.quantization = src->quantization;
	f->fmt.pix_mp.xfer_func = src->xfer_func;
}

static u32 vdec_max_w(u32 fmt)
{
	/* VP8: MIUI's limit (2048x1088 landscape) */
	return fmt == V4L2_PIX_FMT_VP8_FRAME ? VDEC_VP8_MAX : VDEC_MAX_W;
}

static u32 vdec_max_h(u32 fmt)
{
	return fmt == V4L2_PIX_FMT_VP8_FRAME ? VDEC_VP8_MAX : VDEC_MAX_H;
}

static void vdec_clamp_src(struct v4l2_pix_format_mplane *p)
{
	if (p->pixelformat != V4L2_PIX_FMT_VP9_FRAME && p->pixelformat != V4L2_PIX_FMT_VP8_FRAME &&
	    p->pixelformat != V4L2_PIX_FMT_AV1_FRAME && p->pixelformat != V4L2_PIX_FMT_HEVC_SLICE)
		p->pixelformat = V4L2_PIX_FMT_H264_SLICE;
	p->width = clamp(ALIGN(p->width, 16), VDEC_MIN_W, vdec_max_w(p->pixelformat));
	p->height = clamp(ALIGN(p->height, 16), VDEC_MIN_H, vdec_max_h(p->pixelformat));
	p->num_planes = 1;
	p->field = V4L2_FIELD_NONE;
	if (!p->plane_fmt[0].sizeimage)
		p->plane_fmt[0].sizeimage = p->width * p->height * 3 / 4;
	p->plane_fmt[0].sizeimage = max_t(u32, p->plane_fmt[0].sizeimage, SZ_256K);
	p->plane_fmt[0].bytesperline = 0;
}

static void vdec_reset_fmts(struct vdec_ctx *ctx)
{
	struct v4l2_pix_format_mplane *p = &ctx->src_fmt.fmt.pix_mp;

	memset(&ctx->src_fmt, 0, sizeof(ctx->src_fmt));
	ctx->src_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	p->width = 1920;
	p->height = 1088;
	p->colorspace = V4L2_COLORSPACE_REC709;
	ctx->disp_w = 1920;
	ctx->disp_h = 1080;
	vdec_clamp_src(p);
	vdec_fill_dst_fmt(ctx);
}

static int vdec_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "cezanne-vdec", sizeof(cap->driver));
	strscpy(cap->card, "MT6885 video decoder", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:cezanne-vdec", sizeof(cap->bus_info));
	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_H264_SLICE &&
			  fs->pixel_format != V4L2_PIX_FMT_VP9_FRAME &&
			  fs->pixel_format != V4L2_PIX_FMT_VP8_FRAME &&
			  fs->pixel_format != V4L2_PIX_FMT_AV1_FRAME &&
			  fs->pixel_format != V4L2_PIX_FMT_HEVC_SLICE))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise.min_width = VDEC_MIN_W;
	fs->stepwise.max_width = vdec_max_w(fs->pixel_format);
	fs->stepwise.step_width = 16;
	fs->stepwise.min_height = VDEC_MIN_H;
	fs->stepwise.max_height = vdec_max_h(fs->pixel_format);
	fs->stepwise.step_height = 16;
	return 0;
}

static int vdec_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const u32 fmts[] = { V4L2_PIX_FMT_H264_SLICE, V4L2_PIX_FMT_VP9_FRAME,
				    V4L2_PIX_FMT_VP8_FRAME, V4L2_PIX_FMT_AV1_FRAME,
				    V4L2_PIX_FMT_HEVC_SLICE };

	if (f->index >= ARRAY_SIZE(fmts))
		return -EINVAL;
	f->pixelformat = fmts[f->index];
	return 0;
}

static bool vdec_has_10bit(struct vdec_ctx *ctx)
{
	return ctx->codec == VDEC_AV1 || ctx->codec == VDEC_HEVC || ctx->codec == VDEC_VP9;
}

static int vdec_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(file);

	if (f->index > (vdec_has_10bit(ctx) ? 1 : 0))
		return -EINVAL;
	f->pixelformat = f->index ? V4L2_PIX_FMT_MT2110T : V4L2_PIX_FMT_MM21;
	return 0;
}

static int vdec_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	*f = file_to_vdec_ctx(file)->src_fmt;
	return 0;
}

static int vdec_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	*f = file_to_vdec_ctx(file)->dst_fmt;
	return 0;
}

static int vdec_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	vdec_clamp_src(&f->fmt.pix_mp);
	return 0;
}

static int vdec_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(file);
	bool tenbit = ctx->cap_10bit;

	/* capture follows the coded size; the client picks 8 or 10 bit */
	ctx->cap_10bit = f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MT2110T && vdec_has_10bit(ctx);
	vdec_fill_dst_fmt(ctx);
	*f = ctx->dst_fmt;
	ctx->cap_10bit = tenbit;
	vdec_fill_dst_fmt(ctx);
	return 0;
}

static int vdec_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)))
		return -EBUSY;
	vdec_clamp_src(&f->fmt.pix_mp);
	ctx->disp_w = min(f->fmt.pix_mp.width, vdec_max_w(f->fmt.pix_mp.pixelformat));
	ctx->disp_h = min(f->fmt.pix_mp.height, vdec_max_h(f->fmt.pix_mp.pixelformat));
	ctx->src_fmt = *f;
	ctx->codec = f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VP9_FRAME ? VDEC_VP9 :
		     f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_VP8_FRAME ? VDEC_VP8 :
		     f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_AV1_FRAME ? VDEC_AV1 :
		     f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_HEVC_SLICE ? VDEC_HEVC : VDEC_H264;
	ctx->mb_w = f->fmt.pix_mp.width / 16;
	ctx->mb_h = f->fmt.pix_mp.height / 16;
	ctx->cap_10bit = false;
	vdec_fill_dst_fmt(ctx);
	return 0;
}

static int vdec_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	ctx->cap_10bit = f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MT2110T && vdec_has_10bit(ctx);
	vdec_fill_dst_fmt(ctx);
	*f = ctx->dst_fmt;
	return 0;
}

/*
 * The capture format carries the 16x32-aligned coded size (the MM21 tile
 * grid the core writes); the visible picture inside it is reported here so
 * a client crops the tiled buffer to the right rectangle. Without it a
 * cropped stream (display size not a multiple of the tile) is detiled or
 * cropped against the coded size and comes out wrong.
 */
static int vdec_g_selection(struct file *file, void *priv,
			    struct v4l2_selection *s)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;
	s->r.left = 0;
	s->r.top = 0;
	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
		s->r.width = ctx->disp_w;
		s->r.height = ctx->disp_h;
		return 0;
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		s->r.width = ctx->dst_fmt.fmt.pix_mp.width;
		s->r.height = ctx->dst_fmt.fmt.pix_mp.height;
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * The private request ioctls run outside video_ioctl2: it would hold
 * vdev_lock, which request validation (vb2_req_prepare) takes again.
 */
static long vdec_ioctl(struct file *file, unsigned int cmd, unsigned long uarg)
{
	struct vdec_dev *d = video_drvdata(file);
	struct file *mf;
	long ret;

	if (cmd == CEZANNE_VDEC_IOC_REQUEST_QUEUE || cmd == CEZANNE_VDEC_IOC_REQUEST_REINIT) {
		int rfd = (int)uarg;
		struct media_request *req = media_request_get_by_fd(&d->mdev, rfd);

		/* only requests of our media device */
		if (IS_ERR(req))
			return PTR_ERR(req);
		media_request_put(req);
		CLASS(fd, f)(rfd);
		if (fd_empty(f))
			return -EBADF;
		return fd_file(f)->f_op->unlocked_ioctl(fd_file(f),
			cmd == CEZANNE_VDEC_IOC_REQUEST_QUEUE ? MEDIA_REQUEST_IOC_QUEUE :
								MEDIA_REQUEST_IOC_REINIT, 0);
	}
	if (cmd != CEZANNE_VDEC_IOC_REQUEST_ALLOC)
		return video_ioctl2(file, cmd, uarg);
	if (!d->media_path.dentry)
		return -ENODEV;
	mf = dentry_open(&d->media_path, O_RDWR, d->media_cred);
	if (IS_ERR(mf))
		return PTR_ERR(mf);
	/* uarg is the caller's pointer; the request fd is installed in the
	 * calling process */
	ret = mf->f_op->unlocked_ioctl(mf, MEDIA_IOC_REQUEST_ALLOC, uarg);
	fput(mf);
	return ret;
}

/*
 * Our media node, looked up once from the probing context: a sandboxed
 * caller of the private ioctl may live in a chroot without /dev. Each
 * allocation opens it briefly with the prober's credentials; an open file
 * kept here would pin the module (the node's cdev owner).
 */
static void vdec_open_media(struct vdec_dev *d)
{
	char path[32];
	int ret;

	snprintf(path, sizeof(path), "/dev/media%d", d->mdev.devnode->minor);
	ret = kern_path(path, LOOKUP_FOLLOW, &d->media_path);
	if (ret) {
		dev_warn(d->dev, "cannot find %s (%d): no request allocation through the video node\n",
			 path, ret);
		return;
	}
	d->media_cred = get_current_cred();
}

static const struct v4l2_ioctl_ops vdec_ioctl_ops = {
	.vidioc_querycap = vdec_querycap,
	.vidioc_enum_framesizes = vdec_enum_framesizes,
	.vidioc_enum_fmt_vid_out = vdec_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = vdec_enum_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = vdec_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = vdec_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = vdec_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = vdec_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = vdec_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = vdec_s_fmt_cap,
	.vidioc_g_selection = vdec_g_selection,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
};

/* --- vb2 --- */

/* --- buffer memory budget --- */

static u64 vdec_mem_limit(void)
{
	return vdec_mem_limit_mb ? (u64)vdec_mem_limit_mb << 20 :
		((u64)totalram_pages() << PAGE_SHIFT) / 4;
}

/* bytes of a buffer that we allocated (imported dma-bufs are not ours) */
static u64 vdec_buf_bytes(struct vb2_buffer *vb)
{
	u64 n = 0;
	unsigned int i;

	if (vb->memory == VB2_MEMORY_MMAP)
		for (i = 0; i < vb->num_planes; i++)
			n += PAGE_ALIGN(vb2_plane_size(vb, i));
	if (!V4L2_TYPE_IS_OUTPUT(vb->type))
		n += PAGE_ALIGN(container_of(to_vb2_v4l2_buffer(vb), struct vdec_dst_buf,
					     base.vb)->mv_size);
	return n;
}

static int vdec_mem_get(struct vdec_dev *d, u64 bytes)
{
	if (atomic64_add_return(bytes, &d->mem_used) <= vdec_mem_limit())
		return 0;
	atomic64_sub(bytes, &d->mem_used);
	dev_warn_ratelimited(d->dev, "buffer budget of %llu MiB used up\n",
			     vdec_mem_limit() >> 20);
	return -ENOMEM;
}

static void vdec_mem_put(struct vdec_dev *d, u64 bytes)
{
	atomic64_sub(bytes, &d->mem_used);
}

static int vdec_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *p = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		&ctx->src_fmt.fmt.pix_mp : &ctx->dst_fmt.fmt.pix_mp;
	unsigned int i;
	u64 one = 0;

	if (*nplanes) {
		if (*nplanes != p->num_planes)
			return -EINVAL;
		for (i = 0; i < p->num_planes; i++)
			if (sizes[i] < p->plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*nplanes = p->num_planes;
		for (i = 0; i < p->num_planes; i++)
			sizes[i] = p->plane_fmt[i].sizeimage;
	}
	/* fail early if not even one more buffer fits; buf_init enforces */
	for (i = 0; i < *nplanes; i++)
		one += PAGE_ALIGN(sizes[i]);
	if (atomic64_read(&ctx->dev->mem_used) + one > vdec_mem_limit()) {
		dev_warn_ratelimited(ctx->dev->dev, "buffer budget of %llu MiB used up\n",
				     vdec_mem_limit() >> 20);
		return -ENOMEM;
	}
	return 0;
}

static int vdec_buf_init(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vdec_dst_buf *b;
	int ret;

	if (!V4L2_TYPE_IS_OUTPUT(vb->type)) {
		b = container_of(to_vb2_v4l2_buffer(vb), struct vdec_dst_buf, base.vb);
		b->mv_size = vdec_h264_mv_size(ctx->mb_w, ctx->mb_h);
	}
	ret = vdec_mem_get(ctx->dev, vdec_buf_bytes(vb));
	if (ret || V4L2_TYPE_IS_OUTPUT(vb->type))
		return ret;
	b->mv_va = dma_alloc_coherent(ctx->dev->dev, b->mv_size, &b->mv_dma,
				      GFP_KERNEL);
	if (b->mv_va)
		return 0;
	/* no buf_cleanup after a failed buf_init */
	vdec_mem_put(ctx->dev, vdec_buf_bytes(vb));
	return -ENOMEM;
}

static void vdec_buf_cleanup(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vdec_dst_buf *b;

	vdec_mem_put(ctx->dev, vdec_buf_bytes(vb));
	if (V4L2_TYPE_IS_OUTPUT(vb->type))
		return;
	b = container_of(to_vb2_v4l2_buffer(vb), struct vdec_dst_buf, base.vb);
	if (b->mv_va)
		dma_free_coherent(ctx->dev->dev, b->mv_size, b->mv_va, b->mv_dma);
	b->mv_va = NULL;
}

static int vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *p = V4L2_TYPE_IS_OUTPUT(vb->type) ?
		&ctx->src_fmt.fmt.pix_mp : &ctx->dst_fmt.fmt.pix_mp;
	unsigned int i;

	for (i = 0; i < p->num_planes; i++) {
		if (vb2_plane_size(vb, i) < p->plane_fmt[i].sizeimage)
			return -EINVAL;
		if (!V4L2_TYPE_IS_OUTPUT(vb->type))
			vb2_set_plane_payload(vb, i, p->plane_fmt[i].sizeimage);
	}
	return 0;
}

static int vdec_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void vdec_buf_queue(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}

static void vdec_queue_cleanup(struct vb2_queue *vq, enum vb2_buffer_state state)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = V4L2_TYPE_IS_OUTPUT(vq->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			break;
		v4l2_ctrl_request_complete(vb->vb2_buf.req_obj.req, &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vb, state);
	}
}

static int vdec_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);
	int ret;

	if (!V4L2_TYPE_IS_OUTPUT(vq->type))
		return 0;
	/* hold a runtime-PM ref for the whole streaming session: the only
	 * other ref is the transient probe cycle, so without this the block
	 * autosuspends 200 ms after probe and every job runs against a
	 * powered-down core (all regs read 0, no done) */
	ret = pm_runtime_resume_and_get(ctx->dev->dev);
	if (ret)
		return ret;
	ret = vdec_h264_start(ctx);	/* the LAT -> core working buffers, all codecs */
	if (!ret && ctx->codec != VDEC_H264 && ctx->codec != VDEC_HEVC) {
		ret = ctx->codec == VDEC_VP9 ? vdec_vp9_start(ctx) :
		      ctx->codec == VDEC_AV1 ? vdec_av1_start(ctx) : vdec_vp8_start(ctx);
		if (ret)
			vdec_h264_stop(ctx);
	}
	if (ret) {
		pm_runtime_put(ctx->dev->dev);
		vdec_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void vdec_stop_streaming(struct vb2_queue *vq)
{
	struct vdec_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		vdec_vp9_stop(ctx);
		vdec_vp8_stop(ctx);
		vdec_av1_stop(ctx);
		vdec_h264_stop(ctx);
		pm_runtime_put(ctx->dev->dev);
	}
	vdec_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops vdec_qops = {
	.queue_setup = vdec_queue_setup,
	.buf_init = vdec_buf_init,
	.buf_cleanup = vdec_buf_cleanup,
	.buf_prepare = vdec_buf_prepare,
	.buf_out_validate = vdec_buf_out_validate,
	.buf_queue = vdec_buf_queue,
	.buf_request_complete = vdec_buf_request_complete,
	.start_streaming = vdec_start_streaming,
	.stop_streaming = vdec_stop_streaming,
};

static int vdec_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct vdec_ctx *ctx = priv;
	struct vdec_dev *d = ctx->dev;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->ops = &vdec_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &d->vdev_lock;
	src->dev = d->dev;
	src->supports_requests = true;
	src->requires_requests = true;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->ops = &vdec_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	/* V4L2_MEMORY_FLAG_NON_COHERENT: cached mappings for CPU readers */
	dst->allow_cache_hints = 1;
	dst->buf_struct_size = sizeof(struct vdec_dst_buf);
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &d->vdev_lock;
	dst->dev = d->dev;
	return vb2_queue_init(dst);
}

/* --- mem2mem --- */

void vdec_job_done(struct vdec_dev *d, enum vb2_buffer_state state)
{
	struct vdec_ctx *ctx = d->cur;
	unsigned long flags;
	u64 ns;

	d->cur = NULL;
	if (!ctx)
		return;
	ns = ktime_to_ns(ktime_sub(ktime_get(), d->job_start));
	spin_lock_irqsave(&d->stats_lock, flags);
	d->busy_ns += ns;
	d->jobs++;
	ctx->busy_ns += ns;
	ctx->jobs++;
	spin_unlock_irqrestore(&d->stats_lock, flags);
	/* the request's control object was completed by vdec_h264_run()
	 * right after the controls were read (completing it twice trips
	 * WARN_ON(!request_is_queued)); this returns the buffers */
	v4l2_m2m_buf_done_and_job_finish(d->m2m_dev, ctx->fh.m2m_ctx, state);
}

static void vdec_device_run(void *priv)
{
	struct vdec_ctx *ctx = priv;
	struct vdec_dev *d = ctx->dev;
	int ret, trace = vdec_trace;
	bool skip = trace > 0 && vdec_trace_skip > 0;

	if (skip) {
		vdec_trace_skip--;
		vdec_trace = 0;
	}
	/* block stays powered from probe (runtime PM is inert for this
	 * device: see vdec_probe) */
	d->cur = ctx;
	d->job_start = ktime_get();
	/* armed before the engines start: the IRQ thread cancels it */
	schedule_delayed_work(&d->watchdog, msecs_to_jiffies(VDEC_TIMEOUT_MS));
	ret = ctx->codec == VDEC_VP9 ? vdec_vp9_run(ctx) :
	      ctx->codec == VDEC_VP8 ? vdec_vp8_run(ctx) :
	      ctx->codec == VDEC_AV1 ? vdec_av1_run(ctx) :
	      ctx->codec == VDEC_HEVC ? vdec_hevc_run(ctx) : vdec_h264_run(ctx);
	if (skip)
		vdec_trace = trace;
	else if (vdec_trace > 0)
		vdec_trace--;
	if (ret && cancel_delayed_work(&d->watchdog))
		vdec_job_done(d, VB2_BUF_STATE_ERROR);
}

static const struct v4l2_m2m_ops vdec_m2m_ops = {
	.device_run = vdec_device_run,
};

static void vdec_watchdog(struct work_struct *w)
{
	struct vdec_dev *d = container_of(to_delayed_work(w), struct vdec_dev, watchdog);
	unsigned long flags;
	u32 pending;

	spin_lock_irqsave(&d->irq_lock, flags);
	pending = d->pending;
	d->pending = 0;
	spin_unlock_irqrestore(&d->irq_lock, flags);
	if (!pending)
		return;
	dev_err(d->dev, "timeout, still running: %s%s (core %#x lat %#x)\n",
		pending & BIT(VDEC_CORE) ? "core " : "",
		pending & BIT(VDEC_LAT) ? "lat" : "",
		vdec_r(d, CORE_MISC + MISC_IRQ_CFG), vdec_r(d, LAT_MISC + MISC_IRQ_CFG));
	vdec_hw_dump(d, "timeout");
	vdec_job_done(d, VB2_BUF_STATE_ERROR);
}

static irqreturn_t vdec_irq(int irq, void *data)
{
	struct vdec_dev *d = data;
	int hw = irq == d->irq[VDEC_LAT] ? VDEC_LAT : VDEC_CORE;
	u32 cfg = (hw == VDEC_LAT ? LAT_MISC : CORE_MISC) + MISC_IRQ_CFG;
	u32 st = vdec_r(d, cfg);
	bool done;

	/* clear as the BSP / mainline mtk_vcodec_dec_hw */
	vdec_w(d, cfg, vdec_r(d, cfg) | 0x11);
	vdec_w(d, cfg, vdec_r(d, cfg) & ~0x10);

	spin_lock(&d->irq_lock);
	d->irq_status[hw] = st;
	d->pending &= ~BIT(hw);
	done = !d->pending && d->cur;
	spin_unlock(&d->irq_lock);
	if (!done)
		return IRQ_HANDLED;
	return IRQ_WAKE_THREAD;
}

static irqreturn_t vdec_irq_thread(int irq, void *data)
{
	struct vdec_dev *d = data;
	bool ok;

	if (!cancel_delayed_work(&d->watchdog))
		return IRQ_HANDLED;	/* the watchdog got there first */
	/* only the core's done IRQ (SPI 424) ends a job: the LAT has
	 * finished feeding it by then (its own done is not waited for) */
	ok = d->irq_status[VDEC_CORE] & MISC_IRQ_DONE;
	if (!ok)
		dev_warn_ratelimited(d->dev, "job ended without done: core %#x lat %#x\n",
				     d->irq_status[VDEC_CORE], d->irq_status[VDEC_LAT]);
	if (d->cur && d->cur->codec == VDEC_VP9)
		vdec_vp9_done(d->cur, ok);
	else if (d->cur && d->cur->codec == VDEC_VP8)
		vdec_vp8_done(d->cur, ok);
	else if (d->cur && d->cur->codec == VDEC_AV1)
		vdec_av1_done(d->cur, ok);
	vdec_job_done(d, ok ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	return IRQ_HANDLED;
}

/* --- file --- */

static int vdec_open(struct file *f)
{
	struct vdec_dev *d = video_drvdata(f);
	struct vdec_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = d;
	vdec_reset_fmts(ctx);
	ctx->mb_w = ctx->src_fmt.fmt.pix_mp.width / 16;
	ctx->mb_h = ctx->src_fmt.fmt.pix_mp.height / 16;

	/* controls live on the file handle; requests bind to this handler
	 * (the V4L2 core accepts control ioctls through a file handle's
	 * handler without one on the video device) */
	ret = vdec_h264_ctrls_init(&ctx->ctrl_hdl);
	if (ret)
		goto err_free;
	ret = vdec_vp9_ctrls_init(&ctx->ctrl_hdl) ?: vdec_vp8_ctrls_init(&ctx->ctrl_hdl) ?:
	      vdec_av1_ctrls_init(&ctx->ctrl_hdl) ?: vdec_hevc_ctrls_init(&ctx->ctrl_hdl);
	if (ret) {
		v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
		goto err_free;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(f));
	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(d->m2m_dev, ctx, vdec_queue_init);
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

static int vdec_release(struct file *f)
{
	struct vdec_ctx *ctx = file_to_vdec_ctx(f);
	struct vdec_dev *d = ctx->dev;

	mutex_lock(&d->clients_lock);
	list_del(&ctx->client);
	mutex_unlock(&d->clients_lock);
	v4l2_fh_del(&ctx->fh, f);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations vdec_fops = {
	.owner = THIS_MODULE,
	.open = vdec_open,
	.release = vdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = vdec_ioctl,
	.mmap = v4l2_m2m_fop_mmap,
};

static int vdec_request_validate(struct media_request *req)
{
	unsigned int n = vb2_request_buffer_cnt(req);

	if (!n)
		return -ENOENT;
	if (n > 1)
		return -EINVAL;
	return vb2_request_validate(req);
}

static const struct media_device_ops vdec_media_ops = {
	.req_validate = vdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

/* --- power --- */

static int vdec_runtime_resume(struct device *dev)
{
	struct vdec_dev *d = dev_get_drvdata(dev);
	int i, ret;

	/* the core's bus lives in the MDP domain: power the smi common
	 * before the vdec domains, like the BSP's clock_on does */
	ret = pm_runtime_resume_and_get(d->smi_core);
	if (ret) {
		dev_err(dev, "smi_core resume failed %d\n", ret);
		return ret;
	}

	/* lk leaves the VDEC2 AXI firewall enabled; nothing on the core side
	 * answers until its protect bits are cleared (genpd skips this
	 * because it believes the domain was always on) */
	writel(INFRACFG_PROT_VDEC2_BITS, d->infracfg + INFRACFG_PROT_MM_2_CLR);

	for (i = 0; i < VDEC_NUM_HW; i++) {
		ret = pm_runtime_resume_and_get(d->pd[i]);
		if (ret) {
			dev_err(dev, "pd[%d] resume failed %d\n", i, ret);
			goto err_smi;
		}
		ret = pm_runtime_resume_and_get(d->larb[i]);
		if (ret) {
			dev_err(dev, "larb[%d] resume failed %d\n", i, ret);
			pm_runtime_put_sync(d->pd[i]);
			goto err_smi;
		}
	}
	return 0;
err_smi:
	while (i--) {
		pm_runtime_put_sync(d->larb[i]);
		pm_runtime_put_sync(d->pd[i]);
	}
	pm_runtime_put_sync(d->smi_core);
	return ret;
}

static int vdec_runtime_suspend(struct device *dev)
{
	struct vdec_dev *d = dev_get_drvdata(dev);
	int i;

	/*
	 * Power the subsystem down when idle: the SPM's deeper suspend modes
	 * need VDEC, VDEC2 and MDP off. Every job programs the whole hardware
	 * state (the stream start only allocates buffers), so nothing needs
	 * restoring. Early bring-up saw the block go dark after a power cycle;
	 * that was the MDPSYS SMI gates staying gated (patch 0050).
	 */
	for (i = VDEC_NUM_HW - 1; i >= 0; i--) {
		pm_runtime_put_sync(d->larb[i]);
		pm_runtime_put_sync(d->pd[i]);
	}
	pm_runtime_put_sync(d->smi_core);
	return 0;
}

/* genpd cuts the power in system suspend even mid-stream: finish the job */
static int vdec_suspend(struct device *dev)
{
	struct vdec_dev *d = dev_get_drvdata(dev);

	v4l2_m2m_suspend(d->m2m_dev);
	return 0;
}

static int vdec_resume(struct device *dev)
{
	struct vdec_dev *d = dev_get_drvdata(dev);

	v4l2_m2m_resume(d->m2m_dev);
	return 0;
}

static const struct dev_pm_ops vdec_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(vdec_suspend, vdec_resume)
	RUNTIME_PM_OPS(vdec_runtime_suspend, vdec_runtime_resume, NULL)
};

/* --- probe --- */

static struct device *vdec_get_larb(struct device *dev, int i)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
	struct platform_device *pdev;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);
	return &pdev->dev;
}

static struct device *vdec_get_smi(struct device *dev)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "mediatek,smi-core", 0);
	struct platform_device *pdev;

	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);
	return &pdev->dev;
}

/*
 * The LAT's larb5 is behind the MM IOMMU, the core's larb4 behind the MDP
 * one, and a device can name one IOMMU only: the core ports belong to the
 * "vdec-core" child. Binding it puts it in the parent's IOMMU group (same
 * domain, page table shared by both IOMMUs) and turns the larb4 ports to
 * MMU mode, so every buffer mapped for the parent is valid for both
 * engines. Without IOMMUs in the DT everything stays physical. Half a
 * setup would let one engine use IOVAs as physical addresses: refuse it.
 * Runs before the larbs are first resumed, which applies the port modes.
 */
static int vdec_attach_core(struct vdec_dev *d)
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
	np = of_get_compatible_child(d->dev->of_node, "cezanne,mt6885-vdec-core");
	core = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!core)
		return dev_err_probe(d->dev, -ENODEV, "IOMMU but no vdec-core child\n");
	if (!core->dev.driver) {
		put_device(&core->dev);
		return -EPROBE_DEFER;
	}
	g = iommu_group_get(d->dev);
	gc = iommu_group_get(&core->dev);
	ret = g && g == gc ? 0 : -EINVAL;
	if (ret)
		dev_err(d->dev, "vdec-core not in the LAT's IOMMU group\n");
	else
		dev_info(d->dev, "IOMMU group %d (LAT + core)\n", iommu_group_id(g));
	iommu_group_put(gc);
	iommu_group_put(g);
	put_device(&core->dev);
	return ret;
}

static void vdec_detach_pds(void *data)
{
	struct vdec_dev *d = data;
	int i;

	for (i = 0; i < VDEC_NUM_HW; i++)
		if (!IS_ERR_OR_NULL(d->pd[i]))
			dev_pm_domain_detach(d->pd[i], true);
}

/*
 * --- usage for monitors ---
 * vcodec_engine   "decode"
 * vcodec_busy_ns  total time jobs ran (start to completion), ns
 * vcodec_jobs     jobs run
 * vcodec_clients  one line per open file: "tgid busy_ns jobs comm"
 * vcodec_memory   bytes in decoder buffers, all clients
 * A monitor samples these twice; busy_ns delta / wall time = load.
 */
static ssize_t vcodec_engine_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "decode\n");
}
static DEVICE_ATTR_RO(vcodec_engine);

static ssize_t vcodec_busy_ns_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct vdec_dev *d = dev_get_drvdata(dev);
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
	struct vdec_dev *d = dev_get_drvdata(dev);
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
	struct vdec_dev *d = dev_get_drvdata(dev);
	struct vdec_ctx *ctx;
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
	struct vdec_dev *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&d->mem_used));
}
static DEVICE_ATTR_RO(vcodec_memory);

static struct attribute *vdec_attrs[] = {
	&dev_attr_vcodec_engine.attr,
	&dev_attr_vcodec_memory.attr,
	&dev_attr_vcodec_busy_ns.attr,
	&dev_attr_vcodec_jobs.attr,
	&dev_attr_vcodec_clients.attr,
	NULL
};
ATTRIBUTE_GROUPS(vdec);

static int vdec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	static const char * const irq_names[] = { "vdec-core", "vdec-lat" };
	struct resource *res;
	struct vdec_dev *d;
	int i, ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = dev;
	mutex_init(&d->vdev_lock);
	spin_lock_init(&d->irq_lock);
	spin_lock_init(&d->stats_lock);
	mutex_init(&d->clients_lock);
	INIT_LIST_HEAD(&d->clients);
	INIT_DELAYED_WORK(&d->watchdog, vdec_watchdog);
	platform_set_drvdata(pdev, d);

	/* overlaps the vdecsys clock controllers and larbs: no request */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < VDEC_REG_SIZE)
		return -EINVAL;
	d->regs = devm_ioremap(dev, res->start, VDEC_REG_SIZE);
	if (!d->regs)
		return -ENOMEM;
	d->infracfg = devm_ioremap(dev, INFRACFG_AO_BASE, 0x1000);
	if (!d->infracfg)
		return -ENOMEM;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	/* imported dma-bufs map to one IOVA range */
	dma_set_max_seg_size(dev, UINT_MAX);

	ret = devm_add_action_or_reset(dev, vdec_detach_pds, d);
	if (ret)
		return ret;
	/* DT order: power domains and larbs <soc+LAT, core> */
	for (i = 0; i < VDEC_NUM_HW; i++) {
		int idx = i == VDEC_LAT ? 0 : 1;

		d->larb[i] = vdec_get_larb(dev, idx);
		if (IS_ERR(d->larb[i]))
			return dev_err_probe(dev, PTR_ERR(d->larb[i]), "larb %d\n", idx);
		d->pd[i] = dev_pm_domain_attach_by_id(dev, idx);
		if (IS_ERR_OR_NULL(d->pd[i]))
			return dev_err_probe(dev, d->pd[i] ? PTR_ERR(d->pd[i]) : -ENODEV,
					     "power domain %d\n", idx);
		/* DT order: interrupts <core, LAT> */
		d->irq[i] = platform_get_irq(pdev, i == VDEC_CORE ? 0 : 1);
		if (d->irq[i] < 0)
			return d->irq[i];
		ret = devm_request_threaded_irq(dev, d->irq[i], vdec_irq, vdec_irq_thread,
						0, irq_names[i], d);
		if (ret)
			return ret;
	}
	d->smi_core = vdec_get_smi(dev);
	if (IS_ERR(d->smi_core))
		return dev_err_probe(dev, PTR_ERR(d->smi_core), "smi core\n");
	ret = vdec_attach_core(d);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 200);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &d->v4l2_dev);
	if (ret)
		return ret;
	d->m2m_dev = v4l2_m2m_init(&vdec_m2m_ops);
	if (IS_ERR(d->m2m_dev)) {
		ret = PTR_ERR(d->m2m_dev);
		goto err_v4l2;
	}

	d->mdev.dev = dev;
	strscpy(d->mdev.model, "cezanne-vdec", sizeof(d->mdev.model));
	strscpy(d->mdev.bus_info, "platform:cezanne-vdec", sizeof(d->mdev.bus_info));
	media_device_init(&d->mdev);
	d->mdev.ops = &vdec_media_ops;
	d->v4l2_dev.mdev = &d->mdev;

	d->vdev.lock = &d->vdev_lock;
	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.fops = &vdec_fops;
	d->vdev.release = video_device_release_empty;
	d->vdev.vfl_dir = VFL_DIR_M2M;
	d->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	d->vdev.ioctl_ops = &vdec_ioctl_ops;
	strscpy(d->vdev.name, "cezanne-vdec", sizeof(d->vdev.name));
	video_set_drvdata(&d->vdev, d);

	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_mdev;
	ret = v4l2_m2m_register_media_controller(d->m2m_dev, &d->vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto err_vdev;
	ret = media_device_register(&d->mdev);
	if (ret)
		goto err_m2m_mc;
	vdec_open_media(d);
	dev_info(dev, "registered %s\n", video_device_node_name(&d->vdev));
	return 0;

err_m2m_mc:
	v4l2_m2m_unregister_media_controller(d->m2m_dev);
err_vdev:
	video_unregister_device(&d->vdev);
err_mdev:
	media_device_cleanup(&d->mdev);
	v4l2_m2m_release(d->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&d->v4l2_dev);
	return ret;
}

static void vdec_remove(struct platform_device *pdev)
{
	struct vdec_dev *d = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&d->watchdog);
	if (d->media_path.dentry) {
		path_put(&d->media_path);
		put_cred(d->media_cred);
	}
	media_device_unregister(&d->mdev);
	v4l2_m2m_unregister_media_controller(d->m2m_dev);
	video_unregister_device(&d->vdev);
	media_device_cleanup(&d->mdev);
	v4l2_m2m_release(d->m2m_dev);
	v4l2_device_unregister(&d->v4l2_dev);
}

/* nothing to do: binding attaches the core's larb4 ports to the IOMMU */
static int vdec_core_probe(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id vdec_core_of_match[] = {
	{ .compatible = "cezanne,mt6885-vdec-core" },
	{ }
};
MODULE_DEVICE_TABLE(of, vdec_core_of_match);

static struct platform_driver vdec_core_driver = {
	.probe = vdec_core_probe,
	.driver = {
		.name = "cezanne-vdec-core",
		.of_match_table = vdec_core_of_match,
	},
};

static const struct of_device_id vdec_of_match[] = {
	{ .compatible = "cezanne,mt6885-vdec" },
	/* TODO: drop once the boot image carries the renamed DT node */
	{ .compatible = "cezanne,vdecdev" },
	{ }
};
MODULE_DEVICE_TABLE(of, vdec_of_match);

static struct platform_driver vdec_driver = {
	.probe = vdec_probe,
	.remove = vdec_remove,
	.driver = {
		.name = "cezanne-vdec",
		.of_match_table = vdec_of_match,
		.pm = pm_ptr(&vdec_pm_ops),
		.dev_groups = vdec_groups,
	},
};
/* the core driver first: the parent's probe expects its child bound */
static struct platform_driver * const vdec_drivers[] = {
	&vdec_core_driver,
	&vdec_driver,
};

static int __init vdec_init(void)
{
	return platform_register_drivers(vdec_drivers, ARRAY_SIZE(vdec_drivers));
}
module_init(vdec_init);

static void __exit vdec_exit(void)
{
	platform_unregister_drivers(vdec_drivers, ARRAY_SIZE(vdec_drivers));
}
module_exit(vdec_exit);
MODULE_DESCRIPTION("MT6885 V4L2 stateless video decoder (H.264, HEVC, VP9, VP8, AV1)");
MODULE_LICENSE("GPL");
