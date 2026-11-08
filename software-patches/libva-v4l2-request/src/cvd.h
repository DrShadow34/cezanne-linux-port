/* SPDX-License-Identifier: MIT */
/*
 * cezanne VA-API driver: VA-API on top of a V4L2 stateless (request API)
 * decoder, written for the MT6885 cezanne-vdec driver (MM21 output), the
 * cezanne-venc V4L2 stateful H.264 encoder, and a CPU video processor.
 * Fork of Bootlin's libva-v4l2-request (object heap, overall shape).
 *
 * Model: every VA surface is a linear dma-buf (dma-heap, or imported) that
 * clients can export to EGL; the decoder writes its tiled MM21 capture
 * buffer, which is detiled into the surface once the picture is decoded.
 * The encoder reads a surface's dma-buf directly (NV12 or RGB).
 */
#ifndef CVD_H
#define CVD_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_dec_vp9.h>
#include <va/va_dec_vp8.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_enc_h264.h>
#include <va/va_vpp.h>
#include <linux/videodev2.h>

#include "object_heap.h"

#define CVD_VENDOR		"cezanne V4L2 request VA driver"
/* what libva callers allocate for our lists: must cover every entry the
 * query functions write (9 profiles overflowed an 8-entry array into the
 * next heap object, e.g. FFmpeg's frames-context ref in Firefox) */
#define CVD_MAX_PROFILES	16
#define CVD_MAX_ENTRYPOINTS	2
#define CVD_MAX_ATTRIBUTES	8
#define CVD_MAX_IMAGE_FORMATS	6
#define CVD_MAX_OUT		8	/* bitstream buffers / requests = decodes in flight */
#define CVD_MAX_CAP		32
/* capture buffers created with a context; more on demand, but a buffer
 * whose picture is no longer referenced is taken back first (one per
 * surface cost ~22 x 12.5 MiB per 4K FFmpeg context) */
#define CVD_CAP_PREALLOC	8
#define CVD_MAX_REFS		16	/* H.264 DPB; HEVC 15, VP9 / AV1 8, VP8 3 */
#define CVD_MAX_SLICES		256

#define CONFIG_ID_OFFSET	0x01000000
#define CONTEXT_ID_OFFSET	0x02000000
#define SURFACE_ID_OFFSET	0x04000000
#define BUFFER_ID_OFFSET	0x08000000
#define IMAGE_ID_OFFSET		0x10000000

/* surface pixel formats: VA fourcc, DRM fourcc, V4L2 encoder input format;
 * RGB formats are named by byte order in memory (VA) */
struct cvd_format {
	uint32_t va, drm, v4l2;
	unsigned int rt_format, bpp;	/* bytes per pixel of plane 0 */
	uint8_t r, g, b;		/* RGB: byte offsets of the channels */
};
const struct cvd_format *cvd_format(uint32_t va_fourcc);
const struct cvd_format *cvd_format_from_drm(uint32_t drm_fourcc);
extern const struct cvd_format cvd_formats[];
extern const unsigned int cvd_nformats;

extern int cvd_debug;
#define cvd_log(...) do { fprintf(stderr, "cezanne-va: " __VA_ARGS__); } while (0)
#define cvd_dbg(...) do { if (cvd_debug) cvd_log(__VA_ARGS__); } while (0)
/* debug line with the wall-clock time (lines up with MOZ_LOG timestamps) */
#define cvd_dbgt(fmt, ...) do { if (cvd_debug) { struct timespec _t;			\
	clock_gettime(CLOCK_REALTIME, &_t);						\
	cvd_log("%02ld:%02ld.%06ld " fmt, (long)(_t.tv_sec / 60 % 60), (long)(_t.tv_sec % 60),	\
		_t.tv_nsec / 1000, ##__VA_ARGS__); } } while (0)

#include <time.h>
/* monotonic microseconds, for the per-frame timing debug lines */
static inline uint64_t cvd_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* cezanne-vdec private ioctl: a media request through the video node */
#define CEZANNE_VDEC_IOC_REQUEST_ALLOC	_IO('V', BASE_VIDIOC_PRIVATE + 0)
#define CEZANNE_VDEC_IOC_REQUEST_QUEUE	_IO('V', BASE_VIDIOC_PRIVATE + 1)
#define CEZANNE_VDEC_IOC_REQUEST_REINIT	_IO('V', BASE_VIDIOC_PRIVATE + 2)

struct cvd_driver {
	pthread_mutex_t lock;		/* see driver.c: entry points are locked */
	struct object_heap config_heap, context_heap, surface_heap;
	struct object_heap buffer_heap, image_heap;
	/* video_fd: the probe and the request ioctls; each context opens the
	 * node again (video_path) for its own queues and controls */
	int video_fd, media_fd, heap_fd;
	char video_path[32];
	int drm_fd;			/* the VA display's render node (panfrost) */
	bool has_h264, has_vp9, has_vp8, has_av1, has_hevc;
	/* the stateful H.264 encoder (cezanne-venc), opened per context */
	char venc_path[32];
	bool has_venc;
};

struct cvd_config {
	struct object_base base;
	VAProfile profile;
	VAEntrypoint entrypoint;
	unsigned int rt_format;
	unsigned int rc_mode;		/* encode: VA_RC_* */
};

struct cvd_surface {
	struct object_base base;
	unsigned int width, height;
	/* linear NV12, P010 for 10 bit, or packed RGB (fmt->bpp 4) */
	const struct cvd_format *fmt;
	bool p010;
	int fd;
	uint8_t *map;
	size_t size;
	/* plane 0 starts at offset (imported buffers), the chroma of NV12 /
	 * P010 at luma_size */
	unsigned int pitch, luma_size, offset;
	bool imported;			/* the client's dma-buf (DRM PRIME 2) */
	/* being encoded: the encoder reads it until enc_ctx collects the job */
	bool enc_pending;
	VAContextID enc_ctx;
	/* V4L2 capture buffer bound to this surface (-1: none yet) */
	int cap;
	uint64_t ts;			/* timestamp of its last decode, ns */
	VASurfaceStatus status;
	bool decoded;
	/* decoded but not yet detiled into map: done on first access (export,
	 * sync, image), so frames nobody shows (AV1 / VP9 hidden references)
	 * cost no CPU copy; detile_ctx is the context holding its capture */
	bool dirty;
	VAContextID detile_ctx;
	/* queued to the decoder, not collected yet (decoding is asynchronous:
	 * vaEndPicture queues, readers wait); failed: the decoder flagged it */
	bool pending, failed;
	uint8_t av1_order_hint;		/* AV1: OrderHint of its last decode */
};

struct cvd_capbuf {
	void *map[2];
	size_t len[2];
	VASurfaceID owner;
};

struct cvd_slice {
	uint32_t offset, size;
};

struct cvd_vp9_state {
	int8_t ref_deltas[4], mode_deltas[2];
	uint8_t seg_tree[7], seg_pred[3];
	uint8_t seg_enabled_mask[8];
	int16_t seg_data[8][4];
	bool seg_abs;
};

struct cvd_enc;

enum cvd_kind { CVD_DECODE, CVD_ENCODE, CVD_VPP };

struct cvd_context {
	struct object_base base;
	enum cvd_kind kind;
	struct cvd_enc *enc;		/* CVD_ENCODE: encode.c */
	VAProcPipelineParameterBuffer vpp;	/* CVD_VPP: the picture's job, */
	VARectangle vpp_src, vpp_dst;		/* with its regions copied */
	bool have_vpp;
	/* this context's own open file of the decoder: two contexts on one
	 * file collide (S_FMT EBUSY when a player creates the next context,
	 * e.g. for a resolution switch, before destroying the old one) */
	int fd;
	VAConfigID config;
	VAProfile profile;
	unsigned int width, height;
	uint32_t pixfmt;
	/* capture geometry (MM21, or MT2110T for 10 bit) */
	unsigned int cap_planes, cap_stride, cap_luma_h, cap_chroma_h;
	bool cap_10bit;
	struct cvd_capbuf cap[CVD_MAX_CAP];
	unsigned int ncap;
	bool cap_streaming;
	/* bitstream buffers and requests */
	void *out_map[CVD_MAX_OUT];
	size_t out_len[CVD_MAX_OUT];
	int req_fd[CVD_MAX_OUT];
	/* decodes in flight, oldest first: surface, bitstream buffer and the
	 * pictures it references (their capture buffers stay theirs) */
	struct {
		VASurfaceID surface;
		unsigned int out;
		VASurfaceID refs[CVD_MAX_REFS];
		unsigned int nrefs;
	} inflight[CVD_MAX_OUT];
	unsigned int in_head, nin;
	unsigned int nout, next_out;
	uint64_t ts_counter;
	/* the picture being assembled, and every picture it may reference
	 * (the codec's whole reference set, not only this picture's lists) */
	VASurfaceID target;
	VASurfaceID refs[CVD_MAX_REFS];
	unsigned int nrefs;
	uint8_t *bs;
	size_t bs_len, bs_cap;
	bool have_pic, have_iq, have_slice;
	union {
		VAPictureParameterBufferH264 h264;
		VADecPictureParameterBufferVP9 vp9;
		VAPictureParameterBufferVP8 vp8;
		VADecPictureParameterBufferAV1 av1;
		VAPictureParameterBufferHEVC hevc;
	} pic;
	VASliceParameterBufferHEVC hevc_slice;	/* first slice */
	VAProbabilityDataBufferVP8 vp8_probs;
	VAIQMatrixBufferVP8 vp8_iq;
	VASliceParameterBufferVP8 vp8_slice;
	bool have_probs;
	VAIQMatrixBufferH264 iq;
	VAIQMatrixBufferHEVC hevc_iq;
	uint8_t *grain_src;		/* AV1: cached copy of the frame for film grain */
	size_t grain_src_size;
	VASliceParameterBufferH264 h264_slice;	/* first slice */
	VASliceParameterBufferVP9 vp9_slice;
	struct cvd_slice pending[CVD_MAX_SLICES];	/* params waiting for data */
	unsigned int npending;
	struct cvd_vp9_state vp9;
	/* AV1 tiles: offsets into the slice data buffer until it arrives,
	 * then into bs */
	struct v4l2_ctrl_av1_tile_group_entry av1_tiles[V4L2_AV1_MAX_TILE_COUNT];
	unsigned int av1_ntiles, av1_pending;
};

struct cvd_buffer {
	struct object_base base;
	VABufferType type;
	unsigned int size, count;
	uint8_t *data;
	bool external;			/* data points into a surface */
	VASurfaceID surface;
	/* VAEncCodedBufferType: what vaMapBuffer returns once the encode
	 * that fills it (coded_ctx) is collected */
	VACodedBufferSegment seg;
	bool coded_pending;
	VAContextID coded_ctx;
};

struct cvd_image {
	struct object_base base;
	VAImage image;
};

#define DRV(ctx) ((struct cvd_driver *)(ctx)->pDriverData)
#define CONFIG(d, id) ((struct cvd_config *)object_heap_lookup(&(d)->config_heap, id))
#define CONTEXT(d, id) ((struct cvd_context *)object_heap_lookup(&(d)->context_heap, id))
#define SURFACE(d, id) ((struct cvd_surface *)object_heap_lookup(&(d)->surface_heap, id))
#define BUFFER(d, id) ((struct cvd_buffer *)object_heap_lookup(&(d)->buffer_heap, id))
#define IMAGE(d, id) ((struct cvd_image *)object_heap_lookup(&(d)->image_heap, id))

/* v4l2.c */
int cvd_ioctl(int fd, unsigned long req, void *arg);
int cvd_find_devices(struct cvd_driver *d);
bool cvd_find_encoder(struct cvd_driver *d);
int cvd_request_alloc(struct cvd_driver *d);
int cvd_request_queue(struct cvd_driver *d, int req);
int cvd_request_reinit(struct cvd_driver *d, int req);
bool cvd_has_output_format(int fd, uint32_t pixfmt);

/* surface.c */
VAStatus cvd_surface_bind_capture(struct cvd_driver *d, struct cvd_context *c,
				  struct cvd_surface *s);
void cvd_detile(struct cvd_context *c, struct cvd_surface *s);
VAStatus cvd_surface_flush(struct cvd_driver *d, struct cvd_surface *s);
void cvd_surface_wait(struct cvd_driver *d, struct cvd_surface *s);
void cvd_reap(struct cvd_driver *d, struct cvd_context *c);
void cvd_detile_to(struct cvd_context *c, const struct cvd_surface *s, uint8_t *map, int fd);

/* encode.c */
VAStatus cvd_enc_create(struct cvd_driver *d, struct cvd_context *c, struct cvd_config *cfg);
void cvd_enc_destroy(struct cvd_driver *d, struct cvd_context *c);
VAStatus cvd_enc_render(struct cvd_driver *d, struct cvd_context *c, struct cvd_buffer *b);
VAStatus cvd_enc_end(struct cvd_driver *d, struct cvd_context *c);
void cvd_enc_wait_surface(struct cvd_driver *d, struct cvd_surface *s);
void cvd_enc_wait_coded(struct cvd_driver *d, struct cvd_buffer *b);

/* vpp.c */
void cvd_vpp_render(struct cvd_context *c, struct cvd_buffer *b);
VAStatus cvd_vpp_end(struct cvd_driver *d, struct cvd_context *c);
VAStatus cvdQueryVideoProcFilters(VADriverContextP, VAContextID, VAProcFilterType *,
				  unsigned int *);
VAStatus cvdQueryVideoProcFilterCaps(VADriverContextP, VAContextID, VAProcFilterType,
				     void *, unsigned int *);
VAStatus cvdQueryVideoProcPipelineCaps(VADriverContextP, VAContextID, VABufferID *,
				       unsigned int, VAProcPipelineCaps *);

/* surface.c: CPU access to a surface's dma-buf (DMA_BUF_IOCTL_SYNC) */
void cvd_surface_cpu(struct cvd_surface *s, bool start, bool write);

/* context.c */
int cvd_start_capture(struct cvd_driver *d, struct cvd_context *c);
int cvd_map_capture(struct cvd_driver *d, struct cvd_context *c, unsigned int i);
int cvd_capture_format(struct cvd_driver *d, struct cvd_context *c, bool tenbit);
unsigned int cvd_rt_formats(VAProfile p);

/* picture.c */
VAStatus cvd_submit(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s,
		    struct v4l2_ext_control *ctrls, unsigned int nctrls);

/* h264.c / vp9.c */
VAStatus cvd_h264_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s);
VAStatus cvd_vp9_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s);
VAStatus cvd_vp8_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s);
VAStatus cvd_av1_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s);
void cvd_av1_film_grain(struct cvd_surface *dst, const struct cvd_surface *src,
			const VADecPictureParameterBufferAV1 *pp);
VAStatus cvd_hevc_end(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s);
void cvd_vp9_reset(struct cvd_vp9_state *st);

/* the entry points */
VAStatus cvdQueryConfigProfiles(VADriverContextP, VAProfile *, int *);
VAStatus cvdQueryConfigEntrypoints(VADriverContextP, VAProfile, VAEntrypoint *, int *);
VAStatus cvdGetConfigAttributes(VADriverContextP, VAProfile, VAEntrypoint, VAConfigAttrib *, int);
VAStatus cvdCreateConfig(VADriverContextP, VAProfile, VAEntrypoint, VAConfigAttrib *, int, VAConfigID *);
VAStatus cvdDestroyConfig(VADriverContextP, VAConfigID);
VAStatus cvdQueryConfigAttributes(VADriverContextP, VAConfigID, VAProfile *, VAEntrypoint *,
				  VAConfigAttrib *, int *);
VAStatus cvdCreateSurfaces2(VADriverContextP, unsigned int, unsigned int, unsigned int,
			    VASurfaceID *, unsigned int, VASurfaceAttrib *, unsigned int);
VAStatus cvdCreateSurfaces(VADriverContextP, int, int, int, int, VASurfaceID *);
VAStatus cvdDestroySurfaces(VADriverContextP, VASurfaceID *, int);
VAStatus cvdSyncSurface(VADriverContextP, VASurfaceID);
VAStatus cvdQuerySurfaceStatus(VADriverContextP, VASurfaceID, VASurfaceStatus *);
VAStatus cvdQuerySurfaceAttributes(VADriverContextP, VAConfigID, VASurfaceAttrib *, unsigned int *);
VAStatus cvdExportSurfaceHandle(VADriverContextP, VASurfaceID, uint32_t, uint32_t, void *);
VAStatus cvdCreateContext(VADriverContextP, VAConfigID, int, int, int, VASurfaceID *, int,
			  VAContextID *);
VAStatus cvdDestroyContext(VADriverContextP, VAContextID);
VAStatus cvdCreateBuffer(VADriverContextP, VAContextID, VABufferType, unsigned int,
			 unsigned int, void *, VABufferID *);
VAStatus cvdBufferSetNumElements(VADriverContextP, VABufferID, unsigned int);
VAStatus cvdMapBuffer(VADriverContextP, VABufferID, void **);
VAStatus cvdUnmapBuffer(VADriverContextP, VABufferID);
VAStatus cvdDestroyBuffer(VADriverContextP, VABufferID);
VAStatus cvdBufferInfo(VADriverContextP, VABufferID, VABufferType *, unsigned int *,
		       unsigned int *);
VAStatus cvdBeginPicture(VADriverContextP, VAContextID, VASurfaceID);
VAStatus cvdRenderPicture(VADriverContextP, VAContextID, VABufferID *, int);
VAStatus cvdEndPicture(VADriverContextP, VAContextID);
VAStatus cvdQueryImageFormats(VADriverContextP, VAImageFormat *, int *);
VAStatus cvdCreateImage(VADriverContextP, VAImageFormat *, int, int, VAImage *);
VAStatus cvdDeriveImage(VADriverContextP, VASurfaceID, VAImage *);
VAStatus cvdDestroyImage(VADriverContextP, VAImageID);
VAStatus cvdGetImage(VADriverContextP, VASurfaceID, int, int, unsigned int, unsigned int,
		     VAImageID);

#endif
