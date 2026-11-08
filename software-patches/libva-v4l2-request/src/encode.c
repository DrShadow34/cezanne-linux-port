// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: H.264 encoding (VAEntrypointEncSlice) on the
 * cezanne-venc V4L2 stateful encoder.
 *
 * VA's encode model is stateless: the client sends sequence, picture and
 * slice parameters and picks the reference and reconstructed surfaces. The
 * hardware behind cezanne-venc is driven by its kernel driver, which keeps
 * the one reference and the reconstruction itself, runs the rate control
 * and writes SPS / PPS in front of every IDR (so no packed headers are
 * taken: VAConfigAttribEncPackedHeaders is none). The client's parameters
 * become V4L2 controls: profile, level, entropy coding, rate control mode,
 * bitrate, QPs and the frame rate; the kernel's own GOP is off and an IDR
 * picture forces a key frame. Reconstructed and reference surfaces are not
 * touched. B frames are not offered (EncMaxRefFrames: one in list 0).
 *
 * The input surface's dma-buf goes to the OUTPUT queue as it is (NV12 or
 * RGB, which the hardware converts to BT.709), each OUTPUT index kept for
 * one surface so vb2 keeps its mapping; the bitstream comes back in an
 * MMAP CAPTURE buffer and is copied into the coded buffer. vaEndPicture
 * queues; the coded buffer's vaMapBuffer and the input surface's
 * vaSyncSurface wait. The encoder finishes jobs in queue order.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "cvd.h"

#define ENC_CAP		4	/* bitstream buffers = encodes in flight */
#define ENC_SLOTS	16	/* OUTPUT indices, each kept for one surface */
#define ENC_TIMEOUT_MS	2000

struct enc_job {
	VASurfaceID surface;
	VABufferID coded;
	unsigned int cap, slot;
	uint64_t ts;
};

struct cvd_enc {
	int fd;
	VAProfile profile;
	unsigned int rc_mode;		/* VA_RC_* */
	bool started;
	/* the layout the stream was started with: every input must match */
	const struct cvd_format *fmt;
	unsigned int pitch, luma_size, out_size;
	/* the picture being assembled; the sequence persists */
	VAEncSequenceParameterBufferH264 seq;
	VAEncPictureParameterBufferH264 pic;
	VAEncSliceParameterBufferH264 slice;
	bool have_seq, have_pic, have_slice;
	VAEncMiscParameterRateControl rc;
	bool have_rc;
	unsigned int fps_num, fps_den;	/* from the frame rate parameter */
	/* what the encoder was last told (-1: nothing yet) */
	int cur_mode, cur_bitrate, cur_qpi, cur_qpp, cur_qpmin, cur_qpmax;
	unsigned int cur_fps_num, cur_fps_den;
	/* buffers */
	void *cap_map[ENC_CAP];
	size_t cap_len[ENC_CAP];
	bool cap_busy[ENC_CAP];
	bool cap_queued[ENC_CAP];	/* queued, its frame's input not (yet) */
	VASurfaceID slot_surface[ENC_SLOTS];
	uint64_t slot_used[ENC_SLOTS];
	bool slot_busy[ENC_SLOTS];
	struct enc_job jobs[ENC_CAP];
	unsigned int head, njobs;
	uint64_t ts, frames;
};

static int v4l2_level(unsigned int idc)
{
	/* V4L2_MPEG_VIDEO_H264_LEVEL_* in menu order (1b is level_idc 9) */
	static const uint8_t idcs[] = { 10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42,
					50, 51, 52 };
	unsigned int i;

	for (i = 0; i < sizeof(idcs); i++)
		if (idcs[i] == idc)
			return i;
	/* higher than the encoder's menu: its driver raises the level anyway */
	return idc > 52 ? V4L2_MPEG_VIDEO_H264_LEVEL_5_2 : V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
}

static int set_ctrls(struct cvd_enc *e, struct v4l2_ext_control *ctrls, unsigned int n)
{
	struct v4l2_ext_controls ec = {
		.which = V4L2_CTRL_WHICH_CUR_VAL, .count = n, .controls = ctrls,
	};

	if (!n)
		return 0;
	if (cvd_ioctl(e->fd, VIDIOC_S_EXT_CTRLS, &ec) < 0) {
		cvd_log("encoder controls failed: %s (control %u of %u)\n", strerror(errno),
			ec.error_idx, n);
		return -1;
	}
	return 0;
}

static void frame_rate(struct cvd_enc *e, unsigned int *num, unsigned int *den)
{
	if (e->fps_num) {
		*num = e->fps_num;
		*den = e->fps_den;
	} else if (e->have_seq && e->seq.time_scale && e->seq.num_units_in_tick) {
		/* two fields per frame in the VUI's units */
		*num = e->seq.time_scale;
		*den = e->seq.num_units_in_tick * 2;
	} else {
		*num = 30;
		*den = 1;
	}
}

/*
 * Rate control and QPs for this picture: CQP takes the picture's QP for
 * its frame type; CBR / VBR the bitrate (VBR: the target, the share of the
 * peak given as target_percentage) and the QP range. Controls are only set
 * when they change.
 */
static int update_controls(struct cvd_enc *e, bool intra)
{
	struct v4l2_ext_control ctrls[8];
	unsigned int n = 0, num, den;
	int qp = e->pic.pic_init_qp + (e->have_slice ? e->slice.slice_qp_delta : 0);
	int mode, bitrate = 0, qpmin = 0, qpmax = 51;

	if (e->rc_mode == VA_RC_CQP) {
		mode = -1;
		qp = qp < 0 ? 0 : qp > 51 ? 51 : qp;
		if (intra && qp != e->cur_qpi) {
			ctrls[n++] = (struct v4l2_ext_control){
				.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, .value = qp };
			e->cur_qpi = qp;
		}
		if (!intra && qp != e->cur_qpp) {
			ctrls[n++] = (struct v4l2_ext_control){
				.id = V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, .value = qp };
			e->cur_qpp = qp;
		}
	} else {
		uint64_t bps = e->have_rc && e->rc.bits_per_second ? e->rc.bits_per_second :
			       e->seq.bits_per_second;
		unsigned int pct = e->have_rc && e->rc.target_percentage ?
				   e->rc.target_percentage : 100;

		if (!bps)
			bps = 8000000;
		if (e->rc_mode == VA_RC_VBR && pct < 100)
			bps = bps * pct / 100;
		bitrate = bps < 64000 ? 64000 : bps > 240000000 ? 240000000 : (int)bps;
		mode = e->rc_mode == VA_RC_CBR ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR :
						 V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
		if (e->have_rc && e->rc.min_qp && e->rc.min_qp <= 51)
			qpmin = e->rc.min_qp;
		if (e->have_rc && e->rc.max_qp && e->rc.max_qp <= 51 && e->rc.max_qp >= (unsigned)qpmin)
			qpmax = e->rc.max_qp;
		else if (!qpmin)
			qpmin = 10;		/* the kernel driver's default */
	}
	if (mode != e->cur_mode) {
		ctrls[n++] = (struct v4l2_ext_control){
			.id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, .value = mode >= 0 };
		if (mode >= 0)
			ctrls[n++] = (struct v4l2_ext_control){
				.id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE, .value = mode };
		e->cur_mode = mode;
	}
	if (mode >= 0 && bitrate != e->cur_bitrate) {
		ctrls[n++] = (struct v4l2_ext_control){
			.id = V4L2_CID_MPEG_VIDEO_BITRATE, .value = bitrate };
		e->cur_bitrate = bitrate;
	}
	if (mode >= 0 && (qpmin != e->cur_qpmin || qpmax != e->cur_qpmax)) {
		ctrls[n++] = (struct v4l2_ext_control){
			.id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP, .value = qpmin };
		ctrls[n++] = (struct v4l2_ext_control){
			.id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP, .value = qpmax };
		e->cur_qpmin = qpmin;
		e->cur_qpmax = qpmax;
	}
	if (set_ctrls(e, ctrls, n) < 0)
		return -1;

	frame_rate(e, &num, &den);
	if (num != e->cur_fps_num || den != e->cur_fps_den) {
		struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };

		parm.parm.output.timeperframe.numerator = den;
		parm.parm.output.timeperframe.denominator = num;
		if (cvd_ioctl(e->fd, VIDIOC_S_PARM, &parm) < 0)
			cvd_log("encoder frame rate %u/%u: %s\n", num, den, strerror(errno));
		e->cur_fps_num = num;
		e->cur_fps_den = den;
	}
	return 0;
}

VAStatus cvd_enc_create(struct cvd_driver *d, struct cvd_context *c, struct cvd_config *cfg)
{
	struct cvd_enc *e = calloc(1, sizeof(*e));
	unsigned int i;

	if (!e)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	e->fd = open(d->venc_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (e->fd < 0) {
		cvd_log("cannot open the encoder %s: %s\n", d->venc_path, strerror(errno));
		free(e);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	e->profile = cfg->profile;
	e->rc_mode = cfg->rc_mode;
	e->cur_mode = e->cur_bitrate = e->cur_qpi = e->cur_qpp = -2;
	e->cur_qpmin = e->cur_qpmax = -2;
	for (i = 0; i < ENC_SLOTS; i++)
		e->slot_surface[i] = VA_INVALID_ID;
	c->enc = e;
	cvd_dbg("encode context %ux%u, profile %d, rate control %#x\n", c->width, c->height,
		cfg->profile, cfg->rc_mode);
	return VA_STATUS_SUCCESS;
}

/*
 * The stream starts with the first picture, when the input's layout is
 * known: raw format from the surface (its pitch must be the encoder's),
 * coded size from the sequence (the surface's otherwise), profile, level,
 * entropy coding, no GOP of the driver's own, buffers, streaming.
 */
static VAStatus enc_start(struct cvd_driver *d, struct cvd_context *c, struct cvd_enc *e,
			  struct cvd_surface *s)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_requestbuffers rb = { 0 };
	struct v4l2_ext_control ctrls[6];
	unsigned int w = s->width, h = s->height, rows, i;
	int type;

	if (!s->fmt->v4l2) {
		cvd_log("encoder input %.4s not supported\n", (const char *)&s->fmt->va);
		return VA_STATUS_ERROR_INVALID_SURFACE;
	}
	if (e->have_seq) {
		unsigned int sw = e->seq.picture_width_in_mbs * 16, sh = e->seq.picture_height_in_mbs * 16;

		if (e->seq.frame_cropping_flag) {
			sw -= 2 * (e->seq.frame_crop_left_offset + e->seq.frame_crop_right_offset);
			sh -= 2 * (e->seq.frame_crop_top_offset + e->seq.frame_crop_bottom_offset);
		}
		if (sw && sw < w)
			w = sw;
		if (sh && sh < h)
			h = sh;
	}
	/* NV12: the chroma follows the surface's luma rows */
	rows = s->fmt->rt_format == VA_RT_FORMAT_RGB32 ? h : s->luma_size / s->pitch;
	fmt.fmt.pix_mp.pixelformat = s->fmt->v4l2;
	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.height = rows;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].bytesperline = s->pitch;
	if (cvd_ioctl(e->fd, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix_mp.plane_fmt[0].bytesperline != s->pitch ||
	    fmt.fmt.pix_mp.width != w || fmt.fmt.pix_mp.height != rows) {
		cvd_log("encoder input %.4s %ux%u pitch %u: got %ux%u pitch %u (%s)\n",
			(const char *)&s->fmt->va, w, rows, s->pitch, fmt.fmt.pix_mp.width,
			fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
			strerror(errno));
		return VA_STATUS_ERROR_INVALID_SURFACE;
	}
	e->out_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (rows != h) {
		struct v4l2_selection sel = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .target = V4L2_SEL_TGT_CROP,
			.r = { 0, 0, w, h },
		};

		if (cvd_ioctl(e->fd, VIDIOC_S_SELECTION, &sel) < 0)
			cvd_log("encoder crop to %ux%u: %s\n", w, h, strerror(errno));
	}
	fmt = (struct v4l2_format){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.num_planes = 1;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = w * h * 3 / 2;
	if (cvd_ioctl(e->fd, VIDIOC_S_FMT, &fmt) < 0) {
		cvd_log("encoder output format failed: %s\n", strerror(errno));
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}

	i = 0;
	ctrls[i++] = (struct v4l2_ext_control){ .id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.value = e->profile == VAProfileH264High ? V4L2_MPEG_VIDEO_H264_PROFILE_HIGH :
			 e->profile == VAProfileH264Main ? V4L2_MPEG_VIDEO_H264_PROFILE_MAIN :
			 V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE };
	ctrls[i++] = (struct v4l2_ext_control){ .id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.value = v4l2_level(e->have_seq ? e->seq.level_idc : 0) };
	ctrls[i++] = (struct v4l2_ext_control){ .id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
		.value = e->have_pic && e->pic.pic_fields.bits.entropy_coding_mode_flag ?
			 V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC : V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC };
	/* frame types come from the client (IDR pictures force key frames) */
	ctrls[i++] = (struct v4l2_ext_control){ .id = V4L2_CID_MPEG_VIDEO_GOP_SIZE, .value = 0 };
	/* SPS / PPS in front of every IDR, as a VA driver writing its own
	 * headers does */
	ctrls[i++] = (struct v4l2_ext_control){ .id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER,
		.value = 1 };
	if (set_ctrls(e, ctrls, i) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_DMABUF;
	rb.count = ENC_SLOTS;
	if (cvd_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < ENC_CAP) {
		cvd_log("encoder input REQBUFS failed: %s\n", strerror(errno));
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	rb = (struct v4l2_requestbuffers){
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
		.count = ENC_CAP,
		/* cached CPU mapping (synced on DQBUF) for the copy out */
		.flags = V4L2_MEMORY_FLAG_NON_COHERENT,
	};
	if (cvd_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < ENC_CAP) {
		cvd_log("encoder output REQBUFS failed: %s\n", strerror(errno));
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	for (i = 0; i < ENC_CAP; i++) {
		struct v4l2_plane pl[1];
		struct v4l2_buffer b = { .type = rb.type, .memory = V4L2_MEMORY_MMAP, .index = i,
					 .m.planes = pl, .length = 1 };

		if (cvd_ioctl(e->fd, VIDIOC_QUERYBUF, &b) < 0)
			return VA_STATUS_ERROR_OPERATION_FAILED;
		e->cap_len[i] = pl[0].length;
		e->cap_map[i] = mmap(NULL, pl[0].length, PROT_READ, MAP_SHARED, e->fd,
				     pl[0].m.mem_offset);
		if (e->cap_map[i] == MAP_FAILED) {
			e->cap_map[i] = NULL;
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
	}
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (cvd_ioctl(e->fd, VIDIOC_STREAMON, &type) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (cvd_ioctl(e->fd, VIDIOC_STREAMON, &type) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	e->fmt = s->fmt;
	e->pitch = s->pitch;
	e->luma_size = s->luma_size;
	e->started = true;
	cvd_dbg("encoding %.4s %ux%u (pitch %u) into %ux%u H.264\n", (const char *)&s->fmt->va,
		w, rows, s->pitch, w, h);
	return VA_STATUS_SUCCESS;
}

static int dqbuf(struct cvd_enc *e, enum v4l2_buf_type type, unsigned int memory,
		 struct v4l2_buffer *b, struct v4l2_plane *pl)
{
	memset(b, 0, sizeof(*b));
	memset(pl, 0, sizeof(*pl));
	b->type = type;
	b->memory = memory;
	b->m.planes = pl;
	b->length = 1;
	return cvd_ioctl(e->fd, VIDIOC_DQBUF, b);
}

/*
 * The oldest encode: wait for its bitstream and its input buffer, copy the
 * bitstream into the coded buffer, release the surface.
 */
static void enc_reap(struct cvd_driver *d, struct cvd_enc *e)
{
	struct enc_job *j = &e->jobs[e->head];
	struct cvd_buffer *cb = BUFFER(d, j->coded);
	struct cvd_surface *s = SURFACE(d, j->surface);
	struct v4l2_buffer b;
	struct v4l2_plane pl;
	bool got_cap = false, got_out = false, failed = false;
	uint32_t bytes = 0, off = 0;

	while (!got_cap || !got_out) {
		struct pollfd p = { .fd = e->fd,
				    .events = (got_cap ? 0 : POLLIN) | (got_out ? 0 : POLLOUT) };
		int r = poll(&p, 1, ENC_TIMEOUT_MS);

		if (r <= 0 || (p.revents & POLLERR)) {
			cvd_log("encode of surface %#x timed out\n", j->surface);
			failed = true;
			break;
		}
		if (!got_cap && (p.revents & POLLIN) &&
		    !dqbuf(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, &b, &pl)) {
			if (b.index != j->cap)
				cvd_log("encoder returned bitstream %u, expected %u\n", b.index, j->cap);
			got_cap = true;
			off = pl.data_offset;
			bytes = pl.bytesused > off ? pl.bytesused - off : 0;
			failed |= !!(b.flags & V4L2_BUF_FLAG_ERROR);
			if (cb && b.index < ENC_CAP && e->cap_map[b.index]) {
				size_t cap = (size_t)cb->size * cb->count;

				if (bytes > cap) {
					cvd_log("coded buffer of %zu bytes too small for %u\n", cap, bytes);
					cb->seg.status |= VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK;
					bytes = cap;
				}
				memcpy(cb->data, (uint8_t *)e->cap_map[b.index] + off, bytes);
			}
		}
		if (!got_out && (p.revents & POLLOUT) &&
		    !dqbuf(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_DMABUF, &b, &pl))
			got_out = true;
	}
	if (cb) {
		cb->seg.size = bytes;
		cb->seg.bit_offset = 0;
		cb->seg.next = NULL;
		if (failed)
			cb->seg.status |= VA_CODED_BUF_STATUS_BAD_BITSTREAM;
		cb->coded_pending = false;
	}
	if (s)
		s->enc_pending = false;
	e->cap_busy[j->cap] = false;
	e->cap_queued[j->cap] = false;
	e->slot_busy[j->slot] = false;
	e->head = (e->head + 1) % ENC_CAP;
	e->njobs--;
}

void cvd_enc_wait_surface(struct cvd_driver *d, struct cvd_surface *s)
{
	struct cvd_context *c = CONTEXT(d, s->enc_ctx);

	while (s->enc_pending && c && c->kind == CVD_ENCODE && c->enc->njobs)
		enc_reap(d, c->enc);
	s->enc_pending = false;
}

void cvd_enc_wait_coded(struct cvd_driver *d, struct cvd_buffer *b)
{
	struct cvd_context *c = CONTEXT(d, b->coded_ctx);

	while (b->coded_pending && c && c->kind == CVD_ENCODE && c->enc->njobs)
		enc_reap(d, c->enc);
	b->coded_pending = false;
}

VAStatus cvd_enc_render(struct cvd_driver *d, struct cvd_context *c, struct cvd_buffer *b)
{
	struct cvd_enc *e = c->enc;

	switch (b->type) {
	case VAEncSequenceParameterBufferType:
		memcpy(&e->seq, b->data, sizeof(e->seq));
		e->have_seq = true;
		break;
	case VAEncPictureParameterBufferType:
		memcpy(&e->pic, b->data, sizeof(e->pic));
		e->have_pic = true;
		break;
	case VAEncSliceParameterBufferType:
		/* one slice: the first one's type and QP delta */
		if (!e->have_slice)
			memcpy(&e->slice, b->data, sizeof(e->slice));
		e->have_slice = true;
		break;
	case VAEncMiscParameterBufferType: {
		const VAEncMiscParameterBuffer *m = (const void *)b->data;

		if (m->type == VAEncMiscParameterTypeRateControl) {
			memcpy(&e->rc, m->data, sizeof(e->rc));
			e->have_rc = true;
		} else if (m->type == VAEncMiscParameterTypeFrameRate) {
			const VAEncMiscParameterFrameRate *f = (const void *)m->data;
			unsigned int num = f->framerate & 0xffff, den = f->framerate >> 16 & 0xffff;

			if (num) {
				e->fps_num = num;
				e->fps_den = den ? den : 1;
			}
		}
		/* HRD, quality level, ... : the kernel driver has no use for them */
		break;
	}
	default:
		/* packed headers: the kernel driver writes its own */
		break;
	}
	return VA_STATUS_SUCCESS;
}

/* an OUTPUT index for the surface: the one it had, or the least recently used */
static unsigned int enc_slot(struct cvd_enc *e, VASurfaceID id)
{
	unsigned int i, best = ENC_SLOTS;

	for (i = 0; i < ENC_SLOTS; i++)
		if (e->slot_surface[i] == id && !e->slot_busy[i])
			return i;
	for (i = 0; i < ENC_SLOTS; i++)
		if (!e->slot_busy[i] && (best == ENC_SLOTS || e->slot_used[i] < e->slot_used[best]))
			best = i;
	return best;
}

VAStatus cvd_enc_end(struct cvd_driver *d, struct cvd_context *c)
{
	struct cvd_enc *e = c->enc;
	struct cvd_surface *s = SURFACE(d, c->target);
	struct cvd_buffer *cb;
	struct v4l2_buffer b;
	struct v4l2_plane pl;
	struct enc_job *j;
	unsigned int cap, slot;
	bool idr, intra;
	VAStatus st = VA_STATUS_SUCCESS;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!e->have_pic)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	cb = BUFFER(d, e->pic.coded_buf);
	if (!cb || cb->type != VAEncCodedBufferType)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (!e->started && (st = enc_start(d, c, e, s)) != VA_STATUS_SUCCESS)
		goto out;
	if (s->fmt != e->fmt || s->pitch != e->pitch ||
	    (s->fmt->rt_format != VA_RT_FORMAT_RGB32 && s->luma_size != e->luma_size) ||
	    s->offset + e->out_size > s->size) {
		cvd_log("surface %#x (%.4s, pitch %u) does not match the stream's input\n",
			s->base.id, (const char *)&s->fmt->va, s->pitch);
		st = VA_STATUS_ERROR_INVALID_SURFACE;
		goto out;
	}
	idr = e->pic.pic_fields.bits.idr_pic_flag;
	intra = idr || (e->have_slice && (e->slice.slice_type % 5) == 2);
	if (update_controls(e, intra) < 0) {
		st = VA_STATUS_ERROR_OPERATION_FAILED;
		goto out;
	}
	if (idr && e->frames) {
		struct v4l2_ext_control k = { .id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME };

		set_ctrls(e, &k, 1);
	}
	while (e->njobs == ENC_CAP)
		enc_reap(d, e);
	for (cap = 0; cap < ENC_CAP && e->cap_busy[cap]; cap++)
		;
	slot = enc_slot(e, s->base.id);
	if (cap == ENC_CAP || slot == ENC_SLOTS) {
		st = VA_STATUS_ERROR_OPERATION_FAILED;
		goto out;
	}

	memset(&b, 0, sizeof(b));
	memset(&pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = cap;
	b.m.planes = &pl;
	b.length = 1;
	if (!e->cap_queued[cap] && cvd_ioctl(e->fd, VIDIOC_QBUF, &b) < 0) {
		cvd_log("encoder bitstream QBUF failed: %s\n", strerror(errno));
		st = VA_STATUS_ERROR_OPERATION_FAILED;
		goto out;
	}
	e->cap_queued[cap] = true;
	memset(&b, 0, sizeof(b));
	memset(&pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_DMABUF;
	b.index = slot;
	b.m.planes = &pl;
	b.length = 1;
	pl.m.fd = s->fd;
	pl.length = s->size;
	pl.data_offset = s->offset;
	pl.bytesused = s->offset + e->out_size;
	e->ts++;
	b.timestamp.tv_sec = e->ts / 1000000;
	b.timestamp.tv_usec = e->ts % 1000000;
	if (cvd_ioctl(e->fd, VIDIOC_QBUF, &b) < 0) {
		/* the bitstream buffer stays queued: the next frame uses it */
		cvd_log("encoder input QBUF failed: %s\n", strerror(errno));
		st = VA_STATUS_ERROR_OPERATION_FAILED;
		goto out;
	}
	j = &e->jobs[(e->head + e->njobs) % ENC_CAP];
	j->surface = s->base.id;
	j->coded = cb->base.id;
	j->cap = cap;
	j->slot = slot;
	j->ts = e->ts;
	e->njobs++;
	e->cap_busy[cap] = true;
	e->slot_busy[slot] = true;
	e->slot_surface[slot] = s->base.id;
	e->slot_used[slot] = e->ts;
	e->frames++;
	s->enc_pending = true;
	s->enc_ctx = c->base.id;
	memset(&cb->seg, 0, sizeof(cb->seg));
	cb->coded_pending = true;
	cb->coded_ctx = c->base.id;
out:
	e->have_pic = e->have_slice = false;
	return st;
}

void cvd_enc_destroy(struct cvd_driver *d, struct cvd_context *c)
{
	struct cvd_enc *e = c->enc;
	struct v4l2_requestbuffers rb = { 0 };
	unsigned int i;
	int type;

	if (!e)
		return;
	while (e->njobs)
		enc_reap(d, e);
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	cvd_ioctl(e->fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cvd_ioctl(e->fd, VIDIOC_STREAMOFF, &type);
	for (i = 0; i < ENC_CAP; i++)
		if (e->cap_map[i])
			munmap(e->cap_map[i], e->cap_len[i]);
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_DMABUF;
	cvd_ioctl(e->fd, VIDIOC_REQBUFS, &rb);
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	cvd_ioctl(e->fd, VIDIOC_REQBUFS, &rb);
	close(e->fd);
	free(e);
	c->enc = NULL;
}
