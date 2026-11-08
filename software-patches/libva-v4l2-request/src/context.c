// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: contexts; decode ones here (V4L2 format, queues,
 * requests), encode ones in encode.c, video processing ones need nothing */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/media.h>

#include "cvd.h"

int cvd_start_capture(struct cvd_driver *d, struct cvd_context *c)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (c->cap_streaming)
		return 0;
	if (cvd_ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		cvd_log("capture STREAMON failed: %s\n", strerror(errno));
		return -1;
	}
	c->cap_streaming = true;
	return 0;
}

/*
 * Map capture buffer i. MM21 has two memory planes, except for VP8 where
 * the kernel driver keeps the chroma in the same buffer after the luma
 * (the VP8 core finds a reference's chroma at a fixed offset).
 */
/* capture format MM21 or MT2110T (10 bit) and the geometry the detile uses */
int cvd_capture_format(struct cvd_driver *d, struct cvd_context *c, bool tenbit)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };

	if (cvd_ioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0)
		goto fail;
	fmt.fmt.pix_mp.pixelformat = tenbit ? V4L2_PIX_FMT_MT2110T : V4L2_PIX_FMT_MM21;
	if (cvd_ioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0 || !fmt.fmt.pix_mp.num_planes ||
	    fmt.fmt.pix_mp.num_planes > 2 ||
	    (fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MT2110T) != tenbit)
		goto fail;
	c->cap_10bit = tenbit;
	c->cap_planes = fmt.fmt.pix_mp.num_planes;
	c->cap_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (c->cap_planes == 2) {
		/* MT2110T planes are 5/4 of the MM21 ones */
		c->cap_luma_h = fmt.fmt.pix_mp.plane_fmt[0].sizeimage / c->cap_stride;
		c->cap_chroma_h = fmt.fmt.pix_mp.plane_fmt[1].sizeimage / c->cap_stride;
		if (tenbit) {
			c->cap_luma_h = c->cap_luma_h * 4 / 5;
			c->cap_chroma_h = c->cap_chroma_h * 4 / 5;
		}
	} else {
		/* luma rows: the height in 32-row tiles; chroma half of that */
		c->cap_luma_h = (fmt.fmt.pix_mp.height + 31) & ~31u;
		c->cap_chroma_h = c->cap_luma_h / 2;
	}
	return 0;
fail:
	cvd_log("capture format (%s) failed: %s\n", tenbit ? "MT2110T" : "MM21", strerror(errno));
	return -1;
}

int cvd_map_capture(struct cvd_driver *d, struct cvd_context *c, unsigned int i)
{
	struct v4l2_plane pl[2];
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				 .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = pl,
				 .length = c->cap_planes };
	unsigned int p;

	if (cvd_ioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
		cvd_log("capture QUERYBUF failed: %s\n", strerror(errno));
		return -1;
	}
	for (p = 0; p < c->cap_planes; p++) {
		c->cap[i].len[p] = pl[p].length;
		c->cap[i].map[p] = mmap(NULL, pl[p].length, PROT_READ, MAP_SHARED,
					c->fd, pl[p].m.mem_offset);
		if (c->cap[i].map[p] == MAP_FAILED)
			return -1;
	}
	if (c->cap_planes == 1) {
		c->cap[i].map[1] = (uint8_t *)c->cap[i].map[0] +
				   (size_t)c->cap_stride * c->cap_luma_h;
		c->cap[i].len[1] = 0;
	}
	return 0;
}

static void context_teardown(struct cvd_driver *d, struct cvd_context *c)
{
	struct v4l2_requestbuffers rb = { .memory = V4L2_MEMORY_MMAP };
	int type;
	unsigned int i, p;

	/* everything in flight collected, then frames still shown after the
	 * decoder goes away keep their picture */
	while (c->nin)
		cvd_reap(d, c);
	for (i = 0; i < c->ncap; i++) {
		struct cvd_surface *s = c->cap[i].owner != VA_INVALID_ID ?
					SURFACE(d, c->cap[i].owner) : NULL;

		if (s && s->dirty && s->detile_ctx == (VAContextID)c->base.id)
			cvd_surface_flush(d, s);
	}
	free(c->grain_src);
	c->grain_src = NULL;
	c->grain_src_size = 0;
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	cvd_ioctl(c->fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cvd_ioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < c->ncap; i++) {
		struct cvd_surface *s = c->cap[i].owner != VA_INVALID_ID ?
					SURFACE(d, c->cap[i].owner) : NULL;

		if (s && s->cap == (int)i)
			s->cap = -1;
		for (p = 0; p < c->cap_planes; p++)
			if (c->cap[i].map[p] && c->cap[i].map[p] != MAP_FAILED)
				munmap(c->cap[i].map[p], c->cap[i].len[p]);
	}
	for (i = 0; i < c->nout; i++) {
		if (c->out_map[i] && c->out_map[i] != MAP_FAILED)
			munmap(c->out_map[i], c->out_len[i]);
		if (c->req_fd[i] >= 0)
			close(c->req_fd[i]);
	}
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	cvd_ioctl(c->fd, VIDIOC_REQBUFS, &rb);
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cvd_ioctl(c->fd, VIDIOC_REQBUFS, &rb);
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	free(c->bs);
}

VAStatus cvdCreateContext(VADriverContextP ctx, VAConfigID config_id, int width, int height,
			  int flags, VASurfaceID *surfaces, int nsurf, VAContextID *out)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_config *cfg = CONFIG(d, config_id);
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_requestbuffers rb = { .memory = V4L2_MEMORY_MMAP };
	struct cvd_context *c;
	unsigned int i;
	int cid, type, n;

	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	cid = object_heap_allocate(&d->context_heap);
	c = CONTEXT(d, cid);
	if (!c)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	memset((char *)c + sizeof(c->base), 0, sizeof(*c) - sizeof(c->base));
	c->fd = -1;
	c->config = config_id;
	c->profile = cfg->profile;
	c->width = width;
	c->height = height;
	c->target = VA_INVALID_ID;
	if (cfg->entrypoint == VAEntrypointVideoProc) {
		c->kind = CVD_VPP;
		*out = cid;
		return VA_STATUS_SUCCESS;
	}
	if (cfg->entrypoint == VAEntrypointEncSlice) {
		VAStatus st;

		c->kind = CVD_ENCODE;
		st = cvd_enc_create(d, c, cfg);
		if (st != VA_STATUS_SUCCESS) {
			object_heap_free(&d->context_heap, &c->base);
			return st;
		}
		*out = cid;
		return VA_STATUS_SUCCESS;
	}
	c->kind = CVD_DECODE;
	c->fd = open(d->video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	for (i = 0; i < CVD_MAX_OUT; i++)
		c->req_fd[i] = -1;
	for (i = 0; i < CVD_MAX_CAP; i++)
		c->cap[i].owner = VA_INVALID_ID;
	c->config = config_id;
	c->profile = cfg->profile;
	c->width = width;
	c->height = height;
	c->target = VA_INVALID_ID;
	c->pixfmt = cfg->profile == VAProfileVP9Profile0 ||
		    cfg->profile == VAProfileVP9Profile2 ? V4L2_PIX_FMT_VP9_FRAME :
		    cfg->profile == VAProfileVP8Version0_3 ? V4L2_PIX_FMT_VP8_FRAME :
		    cfg->profile == VAProfileAV1Profile0 ? V4L2_PIX_FMT_AV1_FRAME :
		    cfg->profile == VAProfileHEVCMain ||
		    cfg->profile == VAProfileHEVCMain10 ? V4L2_PIX_FMT_HEVC_SLICE :
							     V4L2_PIX_FMT_H264_SLICE;
	cvd_vp9_reset(&c->vp9);
	c->bs_cap = (size_t)width * height + 65536;
	c->bs = malloc(c->bs_cap);
	if (!c->bs || c->fd < 0)
		goto fail;

	fmt.fmt.pix_mp.pixelformat = c->pixfmt;
	fmt.fmt.pix_mp.width = width;
	fmt.fmt.pix_mp.height = height;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = c->bs_cap;
	if (cvd_ioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
		cvd_log("VIDIOC_S_FMT(output %ux%u) failed: %s\n", width, height, strerror(errno));
		goto fail;
	}
	/* 10-bit capture for a 10-bit config or surfaces; dynamic pools decide
	 * with the first surface bound (cvd_surface_bind_capture) */
	if (cvd_capture_format(d, c, cfg->rt_format == VA_RT_FORMAT_YUV420_10 ||
			       (nsurf > 0 && SURFACE(d, surfaces[0]) && SURFACE(d, surfaces[0])->p010)) < 0)
		goto fail;
	cvd_dbg("context %ux%u fmt %.4s, capture stride %u, %u/%u rows%s\n", width, height,
		(char *)&c->pixfmt, c->cap_stride, c->cap_luma_h, c->cap_chroma_h,
		c->cap_10bit ? ", 10 bit" : "");

	/* bitstream buffers and one request each */
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.count = CVD_MAX_OUT;
	if (cvd_ioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || !rb.count) {
		cvd_log("output REQBUFS failed\n");
		goto fail;
	}
	c->nout = rb.count;
	for (i = 0; i < c->nout; i++) {
		struct v4l2_plane pl[1];
		struct v4l2_buffer b = { .type = rb.type, .memory = V4L2_MEMORY_MMAP,
					 .index = i, .m.planes = pl, .length = 1 };

		if (cvd_ioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
			cvd_log("output QUERYBUF failed\n");
			goto fail;
		}
		c->out_len[i] = pl[0].length;
		c->out_map[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
				     c->fd, pl[0].m.mem_offset);
		if (c->out_map[i] == MAP_FAILED)
			goto fail;
		c->req_fd[i] = cvd_request_alloc(d);
		if (c->req_fd[i] < 0) {
			cvd_log("request allocation failed\n");
			goto fail;
		}
	}

	/* capture buffers, bound to the first given surfaces (more are
	 * created or taken back on demand: cvd_surface_bind_capture) */
	n = nsurf < CVD_CAP_PREALLOC ? nsurf : CVD_CAP_PREALLOC;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.count = n;
	/* cached CPU mapping (synced on DQBUF), as in cvd_surface_bind_capture */
	rb.flags = getenv("CEZANNE_VA_COHERENT") ? 0 : V4L2_MEMORY_FLAG_NON_COHERENT;
	if (cvd_ioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0) {
		cvd_log("capture REQBUFS(%u) failed\n", n);
		goto fail;
	}
	for (i = 0; i < rb.count; i++) {
		struct cvd_surface *s = (int)i < nsurf ? SURFACE(d, surfaces[i]) : NULL;

		if (cvd_map_capture(d, c, i) < 0)
			goto fail;
		if (s) {
			c->cap[i].owner = s->base.id;
			s->cap = i;
		}
	}
	c->ncap = rb.count;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (cvd_ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		cvd_log("output STREAMON failed\n");
		goto fail;
	}
	/* vb2 cannot start a queue without buffers: with no surfaces given
	 * (FFmpeg's dynamic pools) the capture queue starts with the first
	 * buffer created for a surface */
	if (c->ncap && cvd_start_capture(d, c) < 0)
		goto fail;
	*out = cid;
	return VA_STATUS_SUCCESS;
fail:
	cvd_log("context creation failed: %s\n", strerror(errno));
	context_teardown(d, c);
	object_heap_free(&d->context_heap, &c->base);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}

VAStatus cvdDestroyContext(VADriverContextP ctx, VAContextID id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_context *c = CONTEXT(d, id);

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (c->kind == CVD_ENCODE)
		cvd_enc_destroy(d, c);
	else if (c->kind == CVD_DECODE)
		context_teardown(d, c);
	object_heap_free(&d->context_heap, &c->base);
	return VA_STATUS_SUCCESS;
}
