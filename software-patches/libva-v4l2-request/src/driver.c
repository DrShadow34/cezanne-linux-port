// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: init, vtable, teardown */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <va/va_drmcommon.h>

#include "cvd.h"

int cvd_debug;

#define UNIMPL(...) { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus cvdPutSurface(VADriverContextP c, VASurfaceID s, void *draw, short sx,
			      short sy, unsigned short sw, unsigned short sh, short dx,
			      short dy, unsigned short dw, unsigned short dh,
			      VARectangle *cr, unsigned int nc, unsigned int flags) UNIMPL()
static VAStatus cvdPutImage(VADriverContextP c, VASurfaceID s, VAImageID i, int sx, int sy,
			    unsigned int sw, unsigned int sh, int dx, int dy, unsigned int dw,
			    unsigned int dh) UNIMPL()
static VAStatus cvdSetImagePalette(VADriverContextP c, VAImageID i, unsigned char *p) UNIMPL()
static VAStatus cvdQuerySubpictureFormats(VADriverContextP c, VAImageFormat *f, unsigned int *fl,
					  unsigned int *n)
{
	*n = 0;
	return VA_STATUS_SUCCESS;
}
static VAStatus cvdCreateSubpicture(VADriverContextP c, VAImageID i, VASubpictureID *s) UNIMPL()
static VAStatus cvdDestroySubpicture(VADriverContextP c, VASubpictureID s) UNIMPL()
static VAStatus cvdSetSubpictureImage(VADriverContextP c, VASubpictureID s, VAImageID i) UNIMPL()
static VAStatus cvdSetSubpictureChromakey(VADriverContextP c, VASubpictureID s, unsigned int a,
					  unsigned int b, unsigned int m) UNIMPL()
static VAStatus cvdSetSubpictureGlobalAlpha(VADriverContextP c, VASubpictureID s, float a) UNIMPL()
static VAStatus cvdAssociateSubpicture(VADriverContextP c, VASubpictureID s, VASurfaceID *t,
				       int n, short a, short b, unsigned short cw,
				       unsigned short ch, short d, short e, unsigned short f,
				       unsigned short g, unsigned int fl) UNIMPL()
static VAStatus cvdDeassociateSubpicture(VADriverContextP c, VASubpictureID s, VASurfaceID *t,
					 int n) UNIMPL()
static VAStatus cvdQueryDisplayAttributes(VADriverContextP c, VADisplayAttribute *a, int *n)
{
	*n = 0;
	return VA_STATUS_SUCCESS;
}
static VAStatus cvdGetDisplayAttributes(VADriverContextP c, VADisplayAttribute *a, int n) UNIMPL()
static VAStatus cvdSetDisplayAttributes(VADriverContextP c, VADisplayAttribute *a, int n) UNIMPL()
static VAStatus cvdLockSurface(VADriverContextP c, VASurfaceID s, unsigned int *fourcc,
			       unsigned int *ls, unsigned int *cs, unsigned int *lo,
			       unsigned int *uo, unsigned int *vo, unsigned int *bi,
			       unsigned int *bs, void **b) UNIMPL()
static VAStatus cvdUnlockSurface(VADriverContextP c, VASurfaceID s) UNIMPL()

static VAStatus cvdTerminate(VADriverContextP ctx)
{
	struct cvd_driver *d = DRV(ctx);
	struct object_base *o;
	int it;

	while ((o = object_heap_first(&d->context_heap, &it)))
		cvdDestroyContext(ctx, o->id);
	while ((o = object_heap_first(&d->image_heap, &it)))
		cvdDestroyImage(ctx, o->id);
	while ((o = object_heap_first(&d->buffer_heap, &it)))
		cvdDestroyBuffer(ctx, o->id);
	while ((o = object_heap_first(&d->surface_heap, &it))) {
		VASurfaceID id = o->id;

		cvdDestroySurfaces(ctx, &id, 1);
	}
	while ((o = object_heap_first(&d->config_heap, &it)))
		cvdDestroyConfig(ctx, o->id);
	object_heap_destroy(&d->context_heap);
	object_heap_destroy(&d->image_heap);
	object_heap_destroy(&d->buffer_heap);
	object_heap_destroy(&d->surface_heap);
	object_heap_destroy(&d->config_heap);
	if (d->video_fd >= 0)
		close(d->video_fd);
	if (d->media_fd >= 0)
		close(d->media_fd);
	if (d->heap_fd >= 0)
		close(d->heap_fd);
	pthread_mutex_destroy(&d->lock);
	free(d);
	ctx->pDriverData = NULL;
	return VA_STATUS_SUCCESS;
}

/*
 * Thread safety: FFmpeg flags the VA-API hwaccel async-safe, so frame
 * threads (VP8, VP9, HEVC) call one context from several threads. One
 * driver lock around every entry point that touches shared state; a
 * picture holds it from vaBeginPicture to its vaEndPicture so pictures of
 * different threads do not interleave. Wrappers generated from the
 * prototypes.
 */
static __thread bool cvd_in_picture;

static VAStatus LCreateConfig(VADriverContextP ctx, VAProfile p, VAEntrypoint e, VAConfigAttrib *attr, int n, VAConfigID *id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateConfig(ctx, p, e, attr, n, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDestroyConfig(VADriverContextP ctx, VAConfigID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDestroyConfig(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LQueryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile *p, VAEntrypoint *e, VAConfigAttrib *attr, int *n)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdQueryConfigAttributes(ctx, id, p, e, attr, n);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LCreateSurfaces(VADriverContextP ctx, int width, int height, int format, int n, VASurfaceID *ids)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateSurfaces(ctx, width, height, format, n, ids);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LCreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height, VASurfaceID *ids, unsigned int n, VASurfaceAttrib *attr, unsigned int nattr)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateSurfaces2(ctx, format, width, height, ids, n, attr, nattr);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDestroySurfaces(VADriverContextP ctx, VASurfaceID *ids, int n)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDestroySurfaces(ctx, ids, n);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LCreateContext(VADriverContextP ctx, VAConfigID config_id, int width, int height, int flags, VASurfaceID *surfaces, int nsurf, VAContextID *out)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateContext(ctx, config_id, width, height, flags, surfaces, nsurf, out);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDestroyContext(VADriverContextP ctx, VAContextID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDestroyContext(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int count, void *data, VABufferID *id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateBuffer(ctx, context, type, size, count, data, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LBufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned int count)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdBufferSetNumElements(ctx, id, count);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LMapBuffer(VADriverContextP ctx, VABufferID id, void **p)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdMapBuffer(ctx, id, p);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LUnmapBuffer(VADriverContextP ctx, VABufferID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdUnmapBuffer(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDestroyBuffer(VADriverContextP ctx, VABufferID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDestroyBuffer(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LBufferInfo(VADriverContextP ctx, VABufferID id, VABufferType *type, unsigned int *size, unsigned int *count)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdBufferInfo(ctx, id, type, size, count);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LBeginPicture(VADriverContextP ctx, VAContextID cid, VASurfaceID sid)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdBeginPicture(ctx, cid, sid);
	if (ret == VA_STATUS_SUCCESS)
		cvd_in_picture = true;
	else
		pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LRenderPicture(VADriverContextP ctx, VAContextID cid, VABufferID *ids, int n)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdRenderPicture(ctx, cid, ids, n);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LEndPicture(VADriverContextP ctx, VAContextID cid)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	if (!cvd_in_picture)
		pthread_mutex_lock(&d->lock);
	ret = cvdEndPicture(ctx, cid);
	cvd_in_picture = false;
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LSyncSurface(VADriverContextP ctx, VASurfaceID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdSyncSurface(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus *st)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdQuerySurfaceStatus(ctx, id, st);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LExportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t mem_type, uint32_t flags, void *descriptor)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdExportSurfaceHandle(ctx, id, mem_type, flags, descriptor);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LCreateImage(VADriverContextP ctx, VAImageFormat *fmt, int width, int height, VAImage *out)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdCreateImage(ctx, fmt, width, height, out);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDeriveImage(VADriverContextP ctx, VASurfaceID sid, VAImage *out)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDeriveImage(ctx, sid, out);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LDestroyImage(VADriverContextP ctx, VAImageID id)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdDestroyImage(ctx, id);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

static VAStatus LGetImage(VADriverContextP ctx, VASurfaceID sid, int x, int y, unsigned int w, unsigned int h, VAImageID iid)
{
	struct cvd_driver *d = DRV(ctx);
	VAStatus ret;

	pthread_mutex_lock(&d->lock);
	ret = cvdGetImage(ctx, sid, x, y, w, h, iid);
	pthread_mutex_unlock(&d->lock);
	return ret;
}

VAStatus __attribute__((visibility("default"))) VA_DRIVER_INIT_FUNC(VADriverContextP ctx);

VAStatus VA_DRIVER_INIT_FUNC(VADriverContextP ctx)
{
	struct VADriverVTable *vt = ctx->vtable;
	struct cvd_driver *d;
	const char *dbg = getenv("CEZANNE_VA_DEBUG");

	cvd_debug = dbg && *dbg && *dbg != '0';
	d = calloc(1, sizeof(*d));
	if (!d)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	{
		pthread_mutexattr_t a;

		/* recursive: wrapped entry points call each other (teardown) */
		pthread_mutexattr_init(&a);
		pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&d->lock, &a);
		pthread_mutexattr_destroy(&a);
	}
	/* the decoder, the encoder, or both; video processing needs neither */
	d->has_venc = cvd_find_encoder(d);
	if (cvd_find_devices(d) < 0) {
		d->video_fd = d->media_fd = -1;
		if (!d->has_venc) {
			cvd_log("no V4L2 stateless decoder or encoder found\n");
			free(d);
			return VA_STATUS_ERROR_OPERATION_FAILED;
		}
	}
	/* surface memory: the system dma-heap, or else (sandboxed clients)
	 * buffers of the VA display's render node */
	d->drm_fd = -1;
	if ((ctx->display_type & VA_DISPLAY_MAJOR_MASK) == VA_DISPLAY_DRM && ctx->drm_state)
		d->drm_fd = ((struct drm_state *)ctx->drm_state)->fd;
	d->heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (d->heap_fd < 0 && d->drm_fd < 0) {
		cvd_log("no /dev/dma_heap/system and no DRM display for surface memory\n");
		if (d->video_fd >= 0)
			close(d->video_fd);
		if (d->media_fd >= 0)
			close(d->media_fd);
		free(d);
		return VA_STATUS_ERROR_OPERATION_FAILED;
	}
	cvd_dbg("surface memory from %s\n", d->heap_fd >= 0 ? "dma-heap" : "the render node");
	object_heap_init(&d->config_heap, sizeof(struct cvd_config), CONFIG_ID_OFFSET);
	object_heap_init(&d->context_heap, sizeof(struct cvd_context), CONTEXT_ID_OFFSET);
	object_heap_init(&d->surface_heap, sizeof(struct cvd_surface), SURFACE_ID_OFFSET);
	object_heap_init(&d->buffer_heap, sizeof(struct cvd_buffer), BUFFER_ID_OFFSET);
	object_heap_init(&d->image_heap, sizeof(struct cvd_image), IMAGE_ID_OFFSET);
	ctx->pDriverData = d;
	ctx->version_major = VA_MAJOR_VERSION;
	ctx->version_minor = VA_MINOR_VERSION;
	ctx->max_profiles = CVD_MAX_PROFILES;
	ctx->max_entrypoints = CVD_MAX_ENTRYPOINTS;
	ctx->max_attributes = CVD_MAX_ATTRIBUTES;
	ctx->max_image_formats = CVD_MAX_IMAGE_FORMATS;
	ctx->max_subpic_formats = 1;
	ctx->max_display_attributes = 1;
	ctx->str_vendor = CVD_VENDOR;

	vt->vaTerminate = cvdTerminate;
	vt->vaQueryConfigProfiles = cvdQueryConfigProfiles;
	vt->vaQueryConfigEntrypoints = cvdQueryConfigEntrypoints;
	vt->vaGetConfigAttributes = cvdGetConfigAttributes;
	vt->vaCreateConfig = LCreateConfig;
	vt->vaDestroyConfig = LDestroyConfig;
	vt->vaQueryConfigAttributes = LQueryConfigAttributes;
	vt->vaCreateSurfaces = LCreateSurfaces;
	vt->vaCreateSurfaces2 = LCreateSurfaces2;
	vt->vaDestroySurfaces = LDestroySurfaces;
	vt->vaCreateContext = LCreateContext;
	vt->vaDestroyContext = LDestroyContext;
	vt->vaCreateBuffer = LCreateBuffer;
	vt->vaBufferSetNumElements = LBufferSetNumElements;
	vt->vaMapBuffer = LMapBuffer;
	vt->vaUnmapBuffer = LUnmapBuffer;
	vt->vaDestroyBuffer = LDestroyBuffer;
	vt->vaBufferInfo = LBufferInfo;
	vt->vaBeginPicture = LBeginPicture;
	vt->vaRenderPicture = LRenderPicture;
	vt->vaEndPicture = LEndPicture;
	vt->vaSyncSurface = LSyncSurface;
	vt->vaQuerySurfaceStatus = LQuerySurfaceStatus;
	vt->vaQuerySurfaceAttributes = cvdQuerySurfaceAttributes;
	vt->vaExportSurfaceHandle = LExportSurfaceHandle;
	vt->vaPutSurface = cvdPutSurface;
	vt->vaQueryImageFormats = cvdQueryImageFormats;
	vt->vaCreateImage = LCreateImage;
	vt->vaDeriveImage = LDeriveImage;
	vt->vaDestroyImage = LDestroyImage;
	vt->vaSetImagePalette = cvdSetImagePalette;
	vt->vaGetImage = LGetImage;
	vt->vaPutImage = cvdPutImage;
	vt->vaQuerySubpictureFormats = cvdQuerySubpictureFormats;
	vt->vaCreateSubpicture = cvdCreateSubpicture;
	vt->vaDestroySubpicture = cvdDestroySubpicture;
	vt->vaSetSubpictureImage = cvdSetSubpictureImage;
	vt->vaSetSubpictureChromakey = cvdSetSubpictureChromakey;
	vt->vaSetSubpictureGlobalAlpha = cvdSetSubpictureGlobalAlpha;
	vt->vaAssociateSubpicture = cvdAssociateSubpicture;
	vt->vaDeassociateSubpicture = cvdDeassociateSubpicture;
	vt->vaQueryDisplayAttributes = cvdQueryDisplayAttributes;
	vt->vaGetDisplayAttributes = cvdGetDisplayAttributes;
	vt->vaSetDisplayAttributes = cvdSetDisplayAttributes;
	vt->vaLockSurface = cvdLockSurface;
	vt->vaUnlockSurface = cvdUnlockSurface;
	if (ctx->vtable_vpp) {
		ctx->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
		ctx->vtable_vpp->vaQueryVideoProcFilters = cvdQueryVideoProcFilters;
		ctx->vtable_vpp->vaQueryVideoProcFilterCaps = cvdQueryVideoProcFilterCaps;
		ctx->vtable_vpp->vaQueryVideoProcPipelineCaps = cvdQueryVideoProcPipelineCaps;
	}
	cvd_dbg("initialised: h264 %d vp9 %d vp8 %d av1 %d hevc %d, encoder %d\n", d->has_h264,
		d->has_vp9, d->has_vp8, d->has_av1, d->has_hevc, d->has_venc);
	return VA_STATUS_SUCCESS;
}
