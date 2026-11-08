// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: picture assembly and request submission */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/media.h>

#include "cvd.h"

VAStatus cvdBeginPicture(VADriverContextP ctx, VAContextID cid, VASurfaceID sid)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_context *c = CONTEXT(d, cid);
	struct cvd_surface *s = SURFACE(d, sid);

	cvd_dbgt("timing: begin surface %u\n", sid);
	if (!c)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	c->target = sid;
	c->bs_len = 0;
	c->have_pic = c->have_iq = c->have_slice = c->have_probs = false;
	c->have_vpp = false;
	c->npending = 0;
	c->av1_ntiles = c->av1_pending = 0;
	s->status = VASurfaceRendering;
	return VA_STATUS_SUCCESS;
}

static VAStatus append(struct cvd_context *c, const uint8_t *p, size_t n, bool startcode)
{
	size_t need = c->bs_len + n + 3;

	if (need > c->bs_cap) {
		uint8_t *nb = realloc(c->bs, need * 2);

		if (!nb)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		c->bs = nb;
		c->bs_cap = need * 2;
	}
	if (startcode) {
		c->bs[c->bs_len++] = 0;
		c->bs[c->bs_len++] = 0;
		c->bs[c->bs_len++] = 1;
	}
	memcpy(c->bs + c->bs_len, p, n);
	c->bs_len += n;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdRenderPicture(VADriverContextP ctx, VAContextID cid, VABufferID *ids, int n)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_context *c = CONTEXT(d, cid);
	bool vp9 = c && (c->profile == VAProfileVP9Profile0 || c->profile == VAProfileVP9Profile2);
	bool vp8 = c && c->profile == VAProfileVP8Version0_3;
	bool av1 = c && c->profile == VAProfileAV1Profile0;
	bool hevc = c && (c->profile == VAProfileHEVCMain || c->profile == VAProfileHEVCMain10);
	int i;

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	for (i = 0; i < n; i++) {
		struct cvd_buffer *b = BUFFER(d, ids[i]);
		unsigned int k;

		if (!b)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		if (c->kind == CVD_ENCODE) {
			VAStatus st = cvd_enc_render(d, c, b);

			if (st != VA_STATUS_SUCCESS)
				return st;
			continue;
		}
		if (c->kind == CVD_VPP) {
			cvd_vpp_render(c, b);
			continue;
		}
		switch (b->type) {
		case VAPictureParameterBufferType:
			memcpy(&c->pic, b->data, vp9 ? sizeof(c->pic.vp9) :
				 vp8 ? sizeof(c->pic.vp8) :
				 av1 ? sizeof(c->pic.av1) :
				 hevc ? sizeof(c->pic.hevc) : sizeof(c->pic.h264));
			c->have_pic = true;
			break;
		case VAProbabilityBufferType:
			if (vp8) {
				memcpy(&c->vp8_probs, b->data, sizeof(c->vp8_probs));
				c->have_probs = true;
			}
			break;
		case VAIQMatrixBufferType:
			if (vp8) {
				memcpy(&c->vp8_iq, b->data, sizeof(c->vp8_iq));
				c->have_iq = true;
			} else if (hevc) {
				memcpy(&c->hevc_iq, b->data, sizeof(c->hevc_iq));
				c->have_iq = true;
			} else if (!vp9) {
				memcpy(&c->iq, b->data, sizeof(c->iq));
				c->have_iq = true;
			}
			break;
		case VASliceParameterBufferType:
			if (av1) {
				/* one entry per tile; offsets into the next data buffer */
				for (k = 0; k < b->count && c->av1_ntiles < V4L2_AV1_MAX_TILE_COUNT; k++) {
					const VASliceParameterBufferAV1 *sp =
						(const void *)(b->data + (size_t)k * b->size);
					struct v4l2_ctrl_av1_tile_group_entry *t =
						&c->av1_tiles[c->av1_ntiles++];

					t->tile_offset = sp->slice_data_offset;
					t->tile_size = sp->slice_data_size;
					t->tile_row = sp->tile_row;
					t->tile_col = sp->tile_column;
				}
				c->have_slice = true;
				break;
			}
			for (k = 0; k < b->count && c->npending < CVD_MAX_SLICES; k++) {
				const void *e = b->data + (size_t)k * b->size;

				if (vp8) {
					const VASliceParameterBufferVP8 *sp = e;

					/* VP8: the first partition starts at the
					 * offset, the data runs to the buffer's end */
					if (!c->have_slice)
						c->vp8_slice = *sp;
					c->pending[c->npending].offset = sp->slice_data_offset;
					c->pending[c->npending].size = ~0u;
				} else if (vp9) {
					const VASliceParameterBufferVP9 *sp = e;

					if (!c->have_slice)
						c->vp9_slice = *sp;
					c->pending[c->npending].offset = sp->slice_data_offset;
					c->pending[c->npending].size = sp->slice_data_size;
				} else if (hevc) {
					const VASliceParameterBufferHEVC *sp = e;

					if (!c->have_slice)
						c->hevc_slice = *sp;
					c->pending[c->npending].offset = sp->slice_data_offset;
					c->pending[c->npending].size = sp->slice_data_size;
				} else {
					const VASliceParameterBufferH264 *sp = e;

					if (!c->have_slice)
						c->h264_slice = *sp;
					c->pending[c->npending].offset = sp->slice_data_offset;
					c->pending[c->npending].size = sp->slice_data_size;
				}
				c->have_slice = true;
				c->npending++;
			}
			break;
		case VASliceDataBufferType:
			if (av1) {
				/* the tile group whole: tiles index into it */
				size_t total = (size_t)b->size * b->count;

				for (k = c->av1_pending; k < c->av1_ntiles; k++) {
					struct v4l2_ctrl_av1_tile_group_entry *t = &c->av1_tiles[k];

					if ((size_t)t->tile_offset + t->tile_size > total)
						return VA_STATUS_ERROR_INVALID_BUFFER;
					t->tile_offset += c->bs_len;
				}
				c->av1_pending = c->av1_ntiles;
				if (append(c, b->data, total, false))
					return VA_STATUS_ERROR_ALLOCATION_FAILED;
				break;
			}
			for (k = 0; k < c->npending; k++) {
				struct cvd_slice *sl = &c->pending[k];
				size_t total = (size_t)b->size * b->count;

				if (sl->size == ~0u && sl->offset <= total)
					sl->size = total - sl->offset;
				if ((size_t)sl->offset + sl->size > total)
					return VA_STATUS_ERROR_INVALID_BUFFER;
				if (append(c, b->data + sl->offset, sl->size, !vp9 && !vp8))
					return VA_STATUS_ERROR_ALLOCATION_FAILED;
			}
			c->npending = 0;
			break;
		default:
			break;
		}
	}
	return VA_STATUS_SUCCESS;
}

static void add_ref(struct cvd_context *c, VASurfaceID id)
{
	unsigned int i;

	if (id == VA_INVALID_SURFACE || c->nrefs == CVD_MAX_REFS)
		return;
	for (i = 0; i < c->nrefs; i++)
		if (c->refs[i] == id)
			return;
	c->refs[c->nrefs++] = id;
}

/*
 * Every surface the stream may still reference from this picture on: the
 * H.264 / HEVC DPB references (all of them, also those only later pictures
 * use), the VP9 / AV1 reference slots, VP8's last / golden / altref. A
 * picture that drops out of this set can never become a reference again,
 * so its capture buffer can go to another surface once it is detiled.
 */
static void pic_refs(struct cvd_context *c)
{
	unsigned int i;

	c->nrefs = 0;
	switch (c->profile) {
	case VAProfileVP9Profile0:
	case VAProfileVP9Profile2:
		for (i = 0; i < 8; i++)
			add_ref(c, c->pic.vp9.reference_frames[i]);
		break;
	case VAProfileAV1Profile0:
		for (i = 0; i < 8; i++)
			add_ref(c, c->pic.av1.ref_frame_map[i]);
		break;
	case VAProfileVP8Version0_3:
		add_ref(c, c->pic.vp8.last_ref_frame);
		add_ref(c, c->pic.vp8.golden_ref_frame);
		add_ref(c, c->pic.vp8.alt_ref_frame);
		break;
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		for (i = 0; i < 15; i++)
			if (!(c->pic.hevc.ReferenceFrames[i].flags & VA_PICTURE_HEVC_INVALID))
				add_ref(c, c->pic.hevc.ReferenceFrames[i].picture_id);
		break;
	default:
		for (i = 0; i < 16; i++)
			if (!(c->pic.h264.ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID))
				add_ref(c, c->pic.h264.ReferenceFrames[i].picture_id);
		break;
	}
}

VAStatus cvdEndPicture(VADriverContextP ctx, VAContextID cid)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_context *c = CONTEXT(d, cid);
	struct cvd_surface *s;
	VAStatus st;

	if (!c)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	s = SURFACE(d, c->target);
	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (c->kind != CVD_DECODE) {
		st = c->kind == CVD_ENCODE ? cvd_enc_end(d, c) : cvd_vpp_end(d, c);
		s->status = VASurfaceReady;
		c->target = VA_INVALID_ID;
		return st;
	}
	if (!c->have_pic || !c->bs_len) {
		s->status = VASurfaceReady;
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	}
	pic_refs(c);
	st = cvd_surface_bind_capture(d, c, s);
	if (!st)
		st = c->profile == VAProfileVP9Profile0 ||
		     c->profile == VAProfileVP9Profile2 ? cvd_vp9_end(d, c, s) :
		     c->profile == VAProfileVP8Version0_3 ? cvd_vp8_end(d, c, s) :
		     c->profile == VAProfileAV1Profile0 ? cvd_av1_end(d, c, s) :
		     c->profile == VAProfileHEVCMain ||
		     c->profile == VAProfileHEVCMain10 ? cvd_hevc_end(d, c, s) :
							    cvd_h264_end(d, c, s);
	s->status = VASurfaceReady;
	c->target = VA_INVALID_ID;
	return st;
}

/*
 * The oldest decode in flight: wait for its request, dequeue both buffers,
 * mark its surface done (or failed). The decoder runs jobs in queue order.
 */
void cvd_reap(struct cvd_driver *d, struct cvd_context *c)
{
	unsigned int out = c->inflight[c->in_head].out;
	struct cvd_surface *s = SURFACE(d, c->inflight[c->in_head].surface);
	int req = c->req_fd[out];
	struct pollfd pfd = { .fd = req, .events = POLLPRI };
	struct v4l2_plane pl[2];
	struct v4l2_buffer b;
	bool failed = false;
	uint64_t t0 = cvd_us();

	if (!c->nin)
		return;
	c->in_head = (c->in_head + 1) % CVD_MAX_OUT;
	c->nin--;
	if (poll(&pfd, 1, 2000) <= 0) {
		cvd_log("decode timed out\n");
		failed = true;
	}
	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = pl;
	b.length = 1;
	cvd_ioctl(c->fd, VIDIOC_DQBUF, &b);
	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.m.planes = pl;
	b.length = 2;
	if (cvd_ioctl(c->fd, VIDIOC_DQBUF, &b) < 0) {
		cvd_log("capture DQBUF failed: %s\n", strerror(errno));
		failed = true;
	} else {
		if (b.flags & V4L2_BUF_FLAG_ERROR) {
			cvd_dbg("decode error flagged\n");
			failed = true;
		}
		/* the decoder completes jobs in queue order, one capture each */
		if (s && (int)b.index != s->cap)
			cvd_log("capture buffer %u done, expected %d (surface %#x)\n",
				b.index, s->cap, s->base.id);
	}
	cvd_request_reinit(d, req);
	cvd_dbgt("timing: collected surface %u, waited %llu us%s\n",
		 c->inflight[(c->in_head + CVD_MAX_OUT - 1) % CVD_MAX_OUT].surface,
		 (unsigned long long)(cvd_us() - t0), failed ? " (failed)" : "");
	if (!s)
		return;
	s->pending = false;
	s->failed = failed;
	if (getenv("CEZANNE_VA_DUMP") && s->cap >= 0) {
		/* bring-up: the raw capture planes of every frame, appended */
		FILE *f = fopen(getenv("CEZANNE_VA_DUMP"), "ab");
		unsigned int p;

		for (p = 0; f && p < c->cap_planes; p++)
			fwrite(c->cap[s->cap].map[p], 1, c->cap[s->cap].len[p], f);
		if (f)
			fclose(f);
	}
}

/*
 * One decode: controls into the request, capture buffer and bitstream
 * queued, request queued; collected later (cvd_reap) by whoever reads the
 * surface. Firefox times vaEndPicture as the decode: an AV1 packet with
 * four hidden frames and the shown one took 16+ ms when every picture was
 * waited for here, and Firefox drops to software after a few late frames.
 */
VAStatus cvd_submit(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s,
		    struct v4l2_ext_control *ctrls, unsigned int nctrls)
{
	unsigned int idx, slot;
	int req;
	struct v4l2_ext_controls ec = {
		.which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = nctrls,
		.controls = ctrls,
	};
	struct v4l2_plane pl[2];
	struct v4l2_buffer b;
	uint64_t ts;

	/* a free bitstream buffer, and this surface's previous decode done */
	while (c->nin == c->nout)
		cvd_reap(d, c);
	while (s->pending && c->nin)
		cvd_reap(d, c);
	idx = c->next_out;
	req = c->req_fd[idx];
	ec.request_fd = req;
	c->next_out = (c->next_out + 1) % c->nout;
	if (c->bs_len > c->out_len[idx]) {
		cvd_log("bitstream of %zu bytes does not fit (%zu)\n", c->bs_len, c->out_len[idx]);
		return VA_STATUS_ERROR_NOT_ENOUGH_BUFFER;
	}
	memcpy(c->out_map[idx], c->bs, c->bs_len);
	if (getenv("CEZANNE_VA_BSDUMP")) {
		/* bring-up: every submitted bitstream, appended */
		FILE *f = fopen(getenv("CEZANNE_VA_BSDUMP"), "ab");

		if (f) {
			fwrite(c->bs, 1, c->bs_len, f);
			fclose(f);
		}
	}
	if (cvd_ioctl(c->fd, VIDIOC_S_EXT_CTRLS, &ec) < 0) {
		cvd_log("VIDIOC_S_EXT_CTRLS failed: %s (control %u)\n", strerror(errno), ec.error_idx);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	memset(&b, 0, sizeof(b));
	memset(pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = s->cap;
	b.m.planes = pl;
	b.length = 2;
	if (cvd_ioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
		cvd_log("capture QBUF failed: %s\n", strerror(errno));
		goto reinit_fail;
	}
	ts = ++c->ts_counter;
	memset(&b, 0, sizeof(b));
	memset(pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = idx;
	b.m.planes = pl;
	b.length = 1;
	pl[0].bytesused = c->bs_len;
	b.flags = V4L2_BUF_FLAG_REQUEST_FD;
	b.request_fd = req;
	b.timestamp.tv_sec = ts / 1000000;
	b.timestamp.tv_usec = ts % 1000000;
	if (cvd_ioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
		cvd_log("output QBUF failed: %s\n", strerror(errno));
		goto reinit_fail;
	}
	if (cvd_request_queue(d, req) < 0) {
		cvd_log("request queue failed: %s\n", strerror(errno));
		goto reinit_fail;
	}
	slot = (c->in_head + c->nin) % CVD_MAX_OUT;
	c->inflight[slot].surface = s->base.id;
	c->inflight[slot].out = idx;
	memcpy(c->inflight[slot].refs, c->refs, sizeof(c->refs));
	c->inflight[slot].nrefs = c->nrefs;
	c->nin++;
	/* later jobs may reference it at once: the decoder keeps queue order */
	s->ts = ts * 1000;	/* the ns timestamp vb2 gives the capture buffer */
	s->decoded = true;
	s->pending = true;
	s->failed = false;
	/* the detile into the surface waits for its first reader */
	s->dirty = true;
	s->detile_ctx = c->base.id;
	return VA_STATUS_SUCCESS;
reinit_fail:
	cvd_request_reinit(d, req);
	return VA_STATUS_ERROR_OPERATION_FAILED;
}
