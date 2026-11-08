// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: surfaces (linear NV12 / P010 / RGB dma-bufs, own or
 * imported), export, detiling */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <drm_fourcc.h>
#include <drm.h>
#include <drm/panfrost_drm.h>
#include <va/va_drmcommon.h>

#include "cvd.h"

#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* RGB by byte order in memory: VA BGRX = DRM XRGB8888 = V4L2 XBGR32 */
const struct cvd_format cvd_formats[] = {
#define YUV(f, d, v, rt, n) { .va = f, .drm = d, .v4l2 = v, .rt_format = rt, .bpp = n }
#define RGB(f, d, v, r_, g_, b_) { .va = f, .drm = d, .v4l2 = v, .rt_format = VA_RT_FORMAT_RGB32, \
	.bpp = 4, .r = r_, .g = g_, .b = b_ }
	YUV(VA_FOURCC_NV12, DRM_FORMAT_NV12, V4L2_PIX_FMT_NV12, VA_RT_FORMAT_YUV420, 1),
	YUV(VA_FOURCC_P010, DRM_FORMAT_P010, 0, VA_RT_FORMAT_YUV420_10, 2),
	RGB(VA_FOURCC_BGRX, DRM_FORMAT_XRGB8888, V4L2_PIX_FMT_XBGR32, 2, 1, 0),
	RGB(VA_FOURCC_BGRA, DRM_FORMAT_ARGB8888, V4L2_PIX_FMT_ABGR32, 2, 1, 0),
	RGB(VA_FOURCC_RGBX, DRM_FORMAT_XBGR8888, V4L2_PIX_FMT_RGBX32, 0, 1, 2),
	RGB(VA_FOURCC_RGBA, DRM_FORMAT_ABGR8888, V4L2_PIX_FMT_RGBA32, 0, 1, 2),
#undef YUV
#undef RGB
};
const unsigned int cvd_nformats = sizeof(cvd_formats) / sizeof(cvd_formats[0]);

const struct cvd_format *cvd_format(uint32_t va_fourcc)
{
	unsigned int i;

	for (i = 0; i < cvd_nformats; i++)
		if (cvd_formats[i].va == va_fourcc)
			return &cvd_formats[i];
	return NULL;
}

const struct cvd_format *cvd_format_from_drm(uint32_t drm_fourcc)
{
	unsigned int i;

	for (i = 0; i < cvd_nformats; i++)
		if (cvd_formats[i].drm == drm_fourcc)
			return &cvd_formats[i];
	return NULL;
}

void cvd_surface_cpu(struct cvd_surface *s, bool start, bool write)
{
	struct dma_buf_sync sync = {
		(start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
		(write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ)
	};

	if (s->fd >= 0)
		cvd_ioctl(s->fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static void surface_free(struct cvd_driver *d, struct cvd_surface *s)
{
	if (s->map)
		munmap(s->map, s->size);
	if (s->fd >= 0)
		close(s->fd);
	object_heap_free(&d->surface_heap, &s->base);
}

/* a dma-buf: from the system heap, or a panfrost buffer exported as one */
static int alloc_dmabuf(struct cvd_driver *d, size_t size)
{
	if (d->heap_fd >= 0) {
		struct dma_heap_allocation_data alloc = {
			.len = size, .fd_flags = O_RDWR | O_CLOEXEC,
		};

		if (cvd_ioctl(d->heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0)
			return -1;
		return alloc.fd;
	} else {
		struct drm_panfrost_create_bo bo = { .size = size, .flags = PANFROST_BO_NOEXEC };
		struct drm_prime_handle prime = { .flags = DRM_CLOEXEC | DRM_RDWR };
		struct drm_gem_close cl = { 0 };

		if (cvd_ioctl(d->drm_fd, DRM_IOCTL_PANFROST_CREATE_BO, &bo) < 0)
			return -1;
		prime.handle = bo.handle;
		cl.handle = bo.handle;
		if (cvd_ioctl(d->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
			cvd_ioctl(d->drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);
			return -1;
		}
		cvd_ioctl(d->drm_fd, DRM_IOCTL_GEM_CLOSE, &cl);	/* the dma-buf keeps it */
		return prime.fd;
	}
}

static struct cvd_surface *surface_new(struct cvd_driver *d, const struct cvd_format *fmt,
				      unsigned int width, unsigned int height)
{
	int sid = object_heap_allocate(&d->surface_heap);
	struct cvd_surface *s = SURFACE(d, sid);

	if (!s)
		return NULL;
	memset((char *)s + sizeof(s->base), 0, sizeof(*s) - sizeof(s->base));
	s->fd = -1;
	s->fmt = fmt;
	s->width = width;
	s->height = height;
	s->p010 = fmt->va == VA_FOURCC_P010;
	s->cap = -1;
	s->status = VASurfaceReady;
	s->detile_ctx = VA_INVALID_ID;
	s->enc_ctx = VA_INVALID_ID;
	return s;
}

/*
 * A client's dma-buf as a surface (VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2):
 * one linear object, NV12 / P010 with equal pitches or one RGB plane. The
 * fd is duplicated; the CPU mapping is for the video processor.
 */
static VAStatus import_surface(struct cvd_driver *d, const VADRMPRIMESurfaceDescriptor *desc,
			       VASurfaceID *id)
{
	const struct cvd_format *fmt = cvd_format(desc->fourcc);
	unsigned int off[2] = { 0 }, pitch[2] = { 0 }, np = 0, l, p;
	uint64_t mod = desc->objects[0].drm_format_modifier;
	struct cvd_surface *s;

	if (!fmt || desc->num_objects != 1 ||
	    (mod != DRM_FORMAT_MOD_LINEAR && mod != DRM_FORMAT_MOD_INVALID)) {
		cvd_log("import of %.4s, %u objects, modifier %#llx not supported\n",
			(const char *)&desc->fourcc, desc->num_objects, (unsigned long long)mod);
		return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	}
	for (l = 0; l < desc->num_layers && l < 4; l++)
		for (p = 0; p < desc->layers[l].num_planes && p < 4 && np < 2; p++, np++) {
			off[np] = desc->layers[l].offset[p];
			pitch[np] = desc->layers[l].pitch[p];
		}
	if (np != (fmt->rt_format == VA_RT_FORMAT_RGB32 ? 1u : 2u) ||
	    (np == 2 && (pitch[1] != pitch[0] || off[1] < off[0])))
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	s = surface_new(d, fmt, desc->width, desc->height);
	if (!s)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	s->imported = true;
	s->offset = off[0];
	s->pitch = pitch[0];
	s->luma_size = np == 2 ? off[1] - off[0] : 0;
	s->fd = fcntl(desc->objects[0].fd, F_DUPFD_CLOEXEC, 0);
	s->size = desc->objects[0].size;
	if (s->fd >= 0 && !s->size) {
		off_t end = lseek(s->fd, 0, SEEK_END);

		s->size = end > 0 ? (size_t)end : 0;
	}
	if (s->fd < 0 || !s->size) {
		surface_free(d, s);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
	if (s->map == MAP_FAILED)
		s->map = mmap(NULL, s->size, PROT_READ, MAP_SHARED, s->fd, 0);
	if (s->map == MAP_FAILED)
		s->map = NULL;		/* the encoder needs no CPU mapping */
	*id = s->base.id;
	cvd_dbg("imported %.4s %ux%u pitch %u offset %u size %zu as surface %#x\n",
		(const char *)&fmt->va, s->width, s->height, s->pitch, s->offset, s->size, *id);
	return VA_STATUS_SUCCESS;
}

VAStatus cvdCreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width,
			    unsigned int height, VASurfaceID *ids, unsigned int n,
			    VASurfaceAttrib *attr, unsigned int nattr)
{
	struct cvd_driver *d = DRV(ctx);
	const struct cvd_format *fmt = NULL;
	const VADRMPRIMESurfaceDescriptor *desc = NULL;
	uint32_t memtype = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
	unsigned int i, a;
	uint64_t t0 = cvd_us();

	for (a = 0; a < nattr; a++) {
		if (attr[a].type == VASurfaceAttribPixelFormat) {
			fmt = cvd_format(attr[a].value.value.i);
			if (!fmt)
				return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
		} else if (attr[a].type == VASurfaceAttribMemoryType) {
			memtype = attr[a].value.value.i;
		} else if (attr[a].type == VASurfaceAttribExternalBufferDescriptor) {
			desc = attr[a].value.value.p;
		}
	}
	if (memtype == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
		if (!desc || n != 1)
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		return import_surface(d, desc, ids);
	}
	if (memtype != VA_SURFACE_ATTRIB_MEM_TYPE_VA)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	if (!fmt)
		fmt = cvd_format(format == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 :
				 format == VA_RT_FORMAT_RGB32 ? VA_FOURCC_BGRX : VA_FOURCC_NV12);
	if (!(fmt->rt_format & format))
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	for (i = 0; i < n; i++) {
		struct cvd_surface *s = surface_new(d, fmt, width, height);

		if (!s)
			goto fail;
		if (fmt->rt_format == VA_RT_FORMAT_RGB32) {
			s->pitch = ALIGN(width * 4, 64);
			s->size = ALIGN((size_t)s->pitch * height, 4096);
		} else {
			s->pitch = ALIGN(width * fmt->bpp, 64);
			s->luma_size = s->pitch * ALIGN(height, 2);
			s->size = ALIGN(s->luma_size * 3 / 2, 4096);
		}
		s->fd = alloc_dmabuf(d, s->size);
		if (s->fd < 0) {
			cvd_log("allocation of %zu bytes failed: %s\n", s->size, strerror(errno));
			surface_free(d, s);
			goto fail;
		}
		s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
		if (s->map == MAP_FAILED) {
			s->map = NULL;
			surface_free(d, s);
			goto fail;
		}
		ids[i] = s->base.id;
	}
	cvd_dbgt("timing: %u surface(s) %ux%u created in %llu us\n", n, width, height,
		(unsigned long long)(cvd_us() - t0));
	return VA_STATUS_SUCCESS;
fail:
	cvdDestroySurfaces(ctx, ids, i);
	return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

VAStatus cvdCreateSurfaces(VADriverContextP ctx, int width, int height, int format, int n,
			   VASurfaceID *ids)
{
	return cvdCreateSurfaces2(ctx, format, width, height, ids, n, NULL, 0);
}

VAStatus cvdDestroySurfaces(VADriverContextP ctx, VASurfaceID *ids, int n)
{
	struct cvd_driver *d = DRV(ctx);
	int i;

	for (i = 0; i < n; i++) {
		struct cvd_surface *s = SURFACE(d, ids[i]);

		if (!s)
			return VA_STATUS_ERROR_INVALID_SURFACE;
		cvd_surface_wait(d, s);	/* its decode is still writing */
		surface_free(d, s);
	}
	return VA_STATUS_SUCCESS;
}

VAStatus cvdSyncSurface(VADriverContextP ctx, VASurfaceID id)
{
	/* vaEndPicture only queues: wait for the decode, detile */
	cvd_dbgt("timing: sync surface %u\n", id);
	return cvd_surface_flush(DRV(ctx), SURFACE(DRV(ctx), id));
}

VAStatus cvdQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus *st)
{
	struct cvd_surface *s = SURFACE(DRV(ctx), id);

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	*st = s->pending || s->enc_pending ? VASurfaceRendering : s->status;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdQuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config,
				   VASurfaceAttrib *attr, unsigned int *n)
{
	struct cvd_config *cfg = CONFIG(DRV(ctx), config);
	unsigned int rt = cfg ? cfg->rt_format : VA_RT_FORMAT_YUV420;
	VASurfaceAttrib list[16];
	unsigned int count = 0, i;
	VASurfaceAttrib decode[] = {
		{ VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
		  { VAGenericValueTypeInteger,
		    { .i = rt & VA_RT_FORMAT_YUV420 ? VA_FOURCC_NV12 : VA_FOURCC_P010 } } },
		/* the 10-bit format second (dropped from the list when not supported) */
		{ VASurfaceAttribPixelFormat, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
		  { VAGenericValueTypeInteger, { .i = VA_FOURCC_P010 } } },
		{ VASurfaceAttribMinWidth, VA_SURFACE_ATTRIB_GETTABLE,
		  { VAGenericValueTypeInteger, { .i = 64 } } },
		{ VASurfaceAttribMaxWidth, VA_SURFACE_ATTRIB_GETTABLE,
		  { VAGenericValueTypeInteger, { .i = 4096 } } },
		{ VASurfaceAttribMinHeight, VA_SURFACE_ATTRIB_GETTABLE,
		  { VAGenericValueTypeInteger, { .i = 64 } } },
		{ VASurfaceAttribMaxHeight, VA_SURFACE_ATTRIB_GETTABLE,
		  { VAGenericValueTypeInteger, { .i = 2304 } } },
		{ VASurfaceAttribMemoryType, VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
		  { VAGenericValueTypeInteger,
		    { .i = VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 } } },
	};

	if (cfg && cfg->entrypoint != VAEntrypointVLD) {
		/* encoder input and video processing: NV12 and RGB, own or
		 * imported, up to the encoder's 4096 on either side */
		for (i = 0; i < cvd_nformats; i++)
			if (cvd_formats[i].rt_format & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_RGB32))
				list[count++] = (VASurfaceAttrib){ VASurfaceAttribPixelFormat,
					VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
					{ VAGenericValueTypeInteger, { .i = cvd_formats[i].va } } };
		list[count++] = decode[2];
		list[count++] = decode[3];
		list[count++] = decode[4];
		list[count] = decode[5];
		list[count++].value.value.i = 4096;
		list[count++] = decode[6];
	} else {
		count = sizeof(decode) / sizeof(decode[0]);
		memcpy(list, decode, sizeof(decode));
		if (!(rt & VA_RT_FORMAT_YUV420) || !(rt & VA_RT_FORMAT_YUV420_10)) {
			memmove(&list[1], &list[2], (count - 2) * sizeof(list[0]));
			count--;
		}
	}
	if (!attr) {
		*n = count;
		return VA_STATUS_SUCCESS;
	}
	if (*n < count) {
		*n = count;
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	}
	memcpy(attr, list, count * sizeof(list[0]));
	*n = count;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdExportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t mem_type,
				uint32_t flags, void *descriptor)
{
	VADRMPRIMESurfaceDescriptor *desc = descriptor;
	struct cvd_surface *s = SURFACE(DRV(ctx), id);
	int fd;

	cvd_dbgt("timing: export surface %u\n", id);
	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (cvd_surface_flush(DRV(ctx), s) == VA_STATUS_ERROR_INVALID_SURFACE)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
	fd = dup(s->fd);
	if (fd < 0)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	memset(desc, 0, sizeof(*desc));
	desc->fourcc = s->fmt->va;
	desc->width = s->width;
	desc->height = s->height;
	desc->num_objects = 1;
	desc->objects[0].fd = fd;
	desc->objects[0].size = s->size;
	desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
	if (s->fmt->rt_format == VA_RT_FORMAT_RGB32) {
		desc->num_layers = 1;
		desc->layers[0].drm_format = s->fmt->drm;
		desc->layers[0].num_planes = 1;
		desc->layers[0].offset[0] = s->offset;
		desc->layers[0].pitch[0] = s->pitch;
	} else if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
		desc->num_layers = 2;
		desc->layers[0].drm_format = s->p010 ? DRM_FORMAT_R16 : DRM_FORMAT_R8;
		desc->layers[0].num_planes = 1;
		desc->layers[0].offset[0] = s->offset;
		desc->layers[0].pitch[0] = s->pitch;
		desc->layers[1].drm_format = s->p010 ? DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
		desc->layers[1].num_planes = 1;
		desc->layers[1].offset[0] = s->offset + s->luma_size;
		desc->layers[1].pitch[0] = s->pitch;
	} else {
		desc->num_layers = 1;
		desc->layers[0].drm_format = s->p010 ? DRM_FORMAT_P010 : DRM_FORMAT_NV12;
		desc->layers[0].num_planes = 2;
		desc->layers[0].offset[0] = s->offset;
		desc->layers[0].pitch[0] = s->pitch;
		desc->layers[0].offset[1] = s->offset + s->luma_size;
		desc->layers[0].pitch[1] = s->pitch;
	}
	return VA_STATUS_SUCCESS;
}

/*
 * MM21 -> NV12. Luma: 16x32 tiles (512 bytes), chroma: 16x16 tiles of
 * interleaved CbCr (256 bytes), both in raster order of tiles with the
 * capture format's bytesperline as the tiled width.
 */
static void detile_plane(uint8_t *dst, unsigned int dpitch, const uint8_t *src,
			 unsigned int stride, unsigned int th, unsigned int w, unsigned int h)
{
	unsigned int xt = stride / 16, tsize = 16 * th, y, tx;
	unsigned int ncols = (w + 15) / 16;

	for (y = 0; y < h; y++) {
		const uint8_t *row = src + (y / th) * xt * tsize + (y % th) * 16;
		uint8_t *d = dst + y * dpitch;

		for (tx = 0; tx < ncols; tx++)
			memcpy(d + tx * 16, row + tx * tsize, 16);
	}
}

/*
 * MT2110T -> P010: the MM21 tile grid (16x32 luma, 16x16 interleaved CbCr),
 * each tile 80 bytes per 16x4 block: 16 bytes holding the 2 low bits of the
 * block's columns (row j in bits 2j..2j+1), then the 4 rows of high 8 bits.
 *
 * The AV1 core writes the same blocks in 128-byte pieces: per 640 bytes
 * (16-byte units u0..u39) two halves of 16 rows, each half two 8-row groups
 * of 10 units whose first 8 (low bits, rows 0-3, low bits, rows 4-5) stay
 * in place while the last 2 (rows 6-7) go to a shared block in the middle:
 * [group 0][group 1][rows 6-7 of the four groups][group 2][group 3].
 * mt2110_row() gives where a row's low and high bits start.
 */
static void mt2110_row(bool av1, unsigned int r, unsigned int half, unsigned int *lo,
		       unsigned int *hi)
{
	unsigned int g = r / 8, q = r % 8, main = (half ? 24 : 0) + 8 * g;

	if (!av1) {
		/* bytes from the tile */
		*lo = r / 4 * 80;
		*hi = *lo + 16 + 16 * (r % 4);
		return;
	}
	/* bytes from the 640-byte period; r is the row in its 16-row half */
	*lo = 16 * (main + (q < 4 ? 0 : 5));
	*hi = 16 * (q < 4 ? main + 1 + q : q < 6 ? main + 2 + q : (half ? 20 : 16) + 2 * g + q - 6);
}

static void detile_plane_10(uint8_t *dst, unsigned int dpitch, const uint8_t *src,
			    unsigned int stride, unsigned int th, unsigned int w, unsigned int h,
			    bool av1)
{
	unsigned int xt = stride / 16, tsize = 20 * th, y, tx, k;
	unsigned int ncols = (w + 15) / 16;

	for (y = 0; y < h; y++) {
		unsigned int r = y % th, j = y % 4;
		size_t rowbase = (size_t)(y / th) * xt * tsize;
		uint16_t *d = (uint16_t *)(dst + y * dpitch);

		for (tx = 0; tx < ncols; tx++) {
			size_t t = rowbase + (size_t)tx * tsize, base = t;
			unsigned int lo, hi;
			const uint8_t *l, *m;

			if (av1) {
				size_t half = t + r / 16 * 320;

				base = half - half % 640;
				mt2110_row(true, r % 16, half % 640 / 320, &lo, &hi);
			} else {
				mt2110_row(false, r, 0, &lo, &hi);
			}
			l = src + base + lo;
			m = src + base + hi;
			for (k = 0; k < 16; k++)
				d[tx * 16 + k] = (m[k] << 2 | (l[k] >> (2 * j) & 3)) << 6;
		}
	}
}

/* s's capture buffer, detiled into an NV12 / P010 buffer of s's layout (fd -1: not a dma-buf) */
void cvd_detile_to(struct cvd_context *c, const struct cvd_surface *s, uint8_t *map, int fd)
{
	struct cvd_capbuf *cb = &c->cap[s->cap];
	struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
	unsigned int w = s->width < c->cap_stride ? s->width : c->cap_stride;
	unsigned int h = s->height, bpp = s->p010 ? 2 : 1;

	if (h > c->cap_luma_h)
		h = c->cap_luma_h;
	if (s->pitch / bpp < ALIGN(w, 16))
		w = s->pitch / bpp & ~15u;
	if (fd >= 0)
		cvd_ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	if (s->p010 && c->cap_10bit) {
		bool av1 = c->profile == VAProfileAV1Profile0;

		detile_plane_10(map, s->pitch, cb->map[0], c->cap_stride, 32, w, h, av1);
		detile_plane_10(map + s->luma_size, s->pitch, cb->map[1], c->cap_stride, 16, w,
				(h + 1) / 2, av1);
	} else {
		detile_plane(map, s->pitch, cb->map[0], c->cap_stride, 32, w, h);
		detile_plane(map + s->luma_size, s->pitch, cb->map[1], c->cap_stride, 16, w,
			     (h + 1) / 2);
	}
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
	if (fd >= 0)
		cvd_ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void cvd_detile(struct cvd_context *c, struct cvd_surface *s)
{
	cvd_detile_to(c, s, s->map, s->fd);
	s->dirty = false;
}

/* the surface's decode finished (collected), if it is in flight */
void cvd_surface_wait(struct cvd_driver *d, struct cvd_surface *s)
{
	struct cvd_context *c = s ? CONTEXT(d, s->detile_ctx) : NULL;

	while (s && s->pending && c && c->nin)
		cvd_reap(d, c);
	if (s)
		s->pending = false;
	if (s && s->enc_pending)
		cvd_enc_wait_surface(d, s);
}

/* the surface ready for readers: decode collected, detile done */
VAStatus cvd_surface_flush(struct cvd_driver *d, struct cvd_surface *s)
{
	struct cvd_context *c;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	cvd_surface_wait(d, s);
	if (!s->dirty)
		return s->failed ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
	s->dirty = false;
	c = CONTEXT(d, s->detile_ctx);
	if (c && s->cap >= 0 && (unsigned int)s->cap < c->ncap &&
	    c->cap[s->cap].owner == (VASurfaceID)s->base.id) {
		uint64_t t0 = cvd_us();

		cvd_detile(c, s);
		cvd_dbgt("timing: detile surface %u %llu us\n", s->base.id,
			 (unsigned long long)(cvd_us() - t0));
	}
	return s->failed ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
}

/* the surface owning capture buffer i of c, if it still does */
static struct cvd_surface *cap_owner(struct cvd_driver *d, struct cvd_context *c, unsigned int i)
{
	struct cvd_surface *o = c->cap[i].owner != VA_INVALID_ID ? SURFACE(d, c->cap[i].owner) : NULL;

	/* a surface decoded by another context since has its picture there */
	return o && o->cap == (int)i && (!o->decoded || o->detile_ctx == (VAContextID)c->base.id) ?
	       o : NULL;
}

/* referenced by the picture being submitted or by a decode in flight */
static bool cap_needed(struct cvd_context *c, VASurfaceID id)
{
	unsigned int i, k;

	for (i = 0; i < c->nrefs; i++)
		if (c->refs[i] == id)
			return true;
	for (k = 0; k < c->nin; k++) {
		unsigned int slot = (c->in_head + k) % CVD_MAX_OUT;

		if (c->inflight[slot].surface == id)
			return true;
		for (i = 0; i < c->inflight[slot].nrefs; i++)
			if (c->inflight[slot].refs[i] == id)
				return true;
	}
	return false;
}

/*
 * A capture buffer whose picture nothing references any more: the least
 * recently decoded one, preferably already detiled (else detiled now, the
 * copy its reader would make later). Its surface keeps the picture in its
 * own memory and loses the capture buffer.
 */
static int cap_reclaim(struct cvd_driver *d, struct cvd_context *c, struct cvd_surface *s)
{
	struct cvd_surface *o, *best = NULL;
	unsigned int i;
	int bi = -1;

	for (i = 0; i < c->ncap; i++) {
		o = cap_owner(d, c, i);
		if (!o || o == s || o->pending || cap_needed(c, o->base.id))
			continue;
		if (!best || (best->dirty && !o->dirty) ||
		    (best->dirty == o->dirty && o->ts < best->ts)) {
			best = o;
			bi = i;
		}
	}
	if (!best)
		return -1;
	if (best->dirty)
		cvd_surface_flush(d, best);
	best->cap = -1;
	c->cap[bi].owner = VA_INVALID_ID;
	return bi;
}

/*
 * A capture buffer for this surface: its own, a free one, one taken back
 * from a picture no longer referenced, or a new one. When the budget
 * refuses a new one, the oldest decode in flight is waited for (its
 * picture may then be given up) before failing.
 */
VAStatus cvd_surface_bind_capture(struct cvd_driver *d, struct cvd_context *c,
				  struct cvd_surface *s)
{
	struct v4l2_create_buffers cb = { 0 };
	unsigned int i;
	int r;

	if (s->cap >= 0 && (unsigned int)s->cap < c->ncap && c->cap[s->cap].owner == (VASurfaceID)s->base.id)
		return VA_STATUS_SUCCESS;
	/* the first buffer of a dynamic pool fixes 8 or 10 bit */
	if (!c->ncap && s->p010 != c->cap_10bit && cvd_capture_format(d, c, s->p010) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	for (;;) {
		for (i = 0; i < c->ncap; i++)
			if (!cap_owner(d, c, i))
				break;
		if (i < c->ncap)
			break;
		r = cap_reclaim(d, c, s);
		if (r >= 0) {
			i = r;
			break;
		}
		if (cvd_debug) {
			unsigned int np = 0, nn = 0, nx = 0;

			for (i = 0; i < c->ncap; i++) {
				struct cvd_surface *o = cap_owner(d, c, i);

				if (!o)
					nx++;
				else if (o->pending)
					np++;
				else if (cap_needed(c, o->base.id))
					nn++;
			}
			cvd_log("grow: %u buffers, %u pending, %u referenced, %u other, %u in flight, %u refs\n",
				c->ncap, np, nn, nx, c->nin, c->nrefs);
		}
		if (c->ncap < CVD_MAX_CAP) {
			uint64_t t0 = cvd_us();

			cb.count = 1;
			cb.memory = V4L2_MEMORY_MMAP;
			/* cached CPU mapping (synced on DQBUF): the detile reads 16
			 * bytes per tile row, far too slow from an uncached mapping */
			cb.flags = getenv("CEZANNE_VA_COHERENT") ? 0 : V4L2_MEMORY_FLAG_NON_COHERENT;
			cb.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			if (cvd_ioctl(c->fd, VIDIOC_G_FMT, &cb.format) == 0 &&
			    cvd_ioctl(c->fd, VIDIOC_CREATE_BUFS, &cb) == 0 && cb.count == 1) {
				i = cb.index;
				if (cvd_map_capture(d, c, i) < 0)
					return VA_STATUS_ERROR_ALLOCATION_FAILED;
				c->ncap = i + 1;
				cvd_dbgt("timing: capture buffer %u created in %llu us\n", i,
					 (unsigned long long)(cvd_us() - t0));
				break;
			}
			cvd_dbg("VIDIOC_CREATE_BUFS failed: %s\n", strerror(errno));
		}
		if (!c->nin) {
			cvd_log("no capture buffer for surface %#x (%u in use)\n", s->base.id, c->ncap);
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
		cvd_reap(d, c);
	}
	c->cap[i].owner = s->base.id;
	s->cap = i;
	if (cvd_start_capture(d, c) < 0)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	return VA_STATUS_SUCCESS;
}
