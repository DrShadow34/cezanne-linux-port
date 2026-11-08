// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: NV12 / P010 / RGB images */
#include <string.h>

#include "cvd.h"

static const VAImageFormat nv12 = {
	.fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 12,
};
static const VAImageFormat p010 = {
	.fourcc = VA_FOURCC_P010, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 24,
};
/* masks as libva clients (GStreamer's va format map) expect them */
static const VAImageFormat rgb[] = {
#define RGB32(f, d, r, g, b, a) { .fourcc = f, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 32, \
	.depth = d, .red_mask = r, .green_mask = g, .blue_mask = b, .alpha_mask = a }
	RGB32(VA_FOURCC_BGRX, 24, 0x00ff0000, 0x0000ff00, 0x000000ff, 0),
	RGB32(VA_FOURCC_BGRA, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000),
	RGB32(VA_FOURCC_RGBX, 24, 0x000000ff, 0x0000ff00, 0x00ff0000, 0),
	RGB32(VA_FOURCC_RGBA, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000),
#undef RGB32
};
#define NRGB (sizeof(rgb) / sizeof(rgb[0]))

_Static_assert(2 + NRGB <= CVD_MAX_IMAGE_FORMATS, "CVD_MAX_IMAGE_FORMATS too small");

static const VAImageFormat *image_format(uint32_t fourcc)
{
	unsigned int i;

	if (fourcc == VA_FOURCC_NV12)
		return &nv12;
	if (fourcc == VA_FOURCC_P010)
		return &p010;
	for (i = 0; i < NRGB; i++)
		if (rgb[i].fourcc == fourcc)
			return &rgb[i];
	return NULL;
}

VAStatus cvdQueryImageFormats(VADriverContextP ctx, VAImageFormat *list, int *n)
{
	unsigned int i;

	list[0] = nv12;
	list[1] = p010;
	for (i = 0; i < NRGB; i++)
		list[2 + i] = rgb[i];
	*n = 2 + NRGB;
	return VA_STATUS_SUCCESS;
}

static VAStatus image_new(struct cvd_driver *d, struct cvd_image **out)
{
	int id = object_heap_allocate(&d->image_heap);

	*out = IMAGE(d, id);
	if (!*out)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	memset(&(*out)->image, 0, sizeof((*out)->image));
	(*out)->image.image_id = id;
	(*out)->image.buf = VA_INVALID_ID;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdCreateImage(VADriverContextP ctx, VAImageFormat *fmt, int width, int height,
			VAImage *out)
{
	struct cvd_driver *d = DRV(ctx);
	const VAImageFormat *f = image_format(fmt->fourcc);
	bool ten = fmt->fourcc == VA_FOURCC_P010;
	unsigned int pitch = (width * (ten ? 2 : 1) + 63) & ~63, lh = (height + 1) & ~1;
	struct cvd_image *img;
	VAStatus st;

	if (!f)
		return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	st = image_new(d, &img);
	if (st)
		return st;
	img->image.format = *f;
	img->image.width = width;
	img->image.height = height;
	if (f->bits_per_pixel == 32) {
		pitch = (width * 4 + 63) & ~63;
		img->image.num_planes = 1;
		img->image.pitches[0] = pitch;
		img->image.data_size = pitch * height;
	} else {
		img->image.num_planes = 2;
		img->image.pitches[0] = img->image.pitches[1] = pitch;
		img->image.offsets[1] = pitch * lh;
		img->image.data_size = pitch * lh * 3 / 2;
	}
	st = cvdCreateBuffer(ctx, 0, VAImageBufferType, img->image.data_size, 1, NULL,
			     &img->image.buf);
	if (st) {
		object_heap_free(&d->image_heap, &img->base);
		return st;
	}
	*out = img->image;
	return VA_STATUS_SUCCESS;
}

/* an image whose buffer is the surface itself (no copy) */
VAStatus cvdDeriveImage(VADriverContextP ctx, VASurfaceID sid, VAImage *out)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_surface *s = SURFACE(d, sid);
	struct cvd_image *img;
	struct cvd_buffer *b;
	VABufferID bid;
	VAStatus st;

	cvd_surface_flush(d, s);
	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!s->map)		/* an imported dma-buf without a CPU mapping */
		return VA_STATUS_ERROR_OPERATION_FAILED;
	st = image_new(d, &img);
	if (st)
		return st;
	bid = object_heap_allocate(&d->buffer_heap);
	b = BUFFER(d, bid);
	if (!b) {
		object_heap_free(&d->image_heap, &img->base);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	b->type = VAImageBufferType;
	b->size = s->size;
	b->count = 1;
	b->data = s->map;
	b->external = true;
	b->surface = sid;
	img->image.format = *image_format(s->fmt->va);
	img->image.buf = bid;
	img->image.width = s->width;
	img->image.height = s->height;
	img->image.pitches[0] = s->pitch;
	img->image.offsets[0] = s->offset;
	if (s->fmt->rt_format == VA_RT_FORMAT_RGB32) {
		img->image.num_planes = 1;
		img->image.data_size = s->pitch * s->height;
	} else {
		img->image.num_planes = 2;
		img->image.pitches[1] = s->pitch;
		img->image.offsets[1] = s->offset + s->luma_size;
		img->image.data_size = s->luma_size * 3 / 2;
	}
	*out = img->image;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdDestroyImage(VADriverContextP ctx, VAImageID id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_image *img = IMAGE(d, id);

	if (!img)
		return VA_STATUS_ERROR_INVALID_IMAGE;
	if (img->image.buf != VA_INVALID_ID)
		cvdDestroyBuffer(ctx, img->image.buf);
	object_heap_free(&d->image_heap, &img->base);
	return VA_STATUS_SUCCESS;
}

VAStatus cvdGetImage(VADriverContextP ctx, VASurfaceID sid, int x, int y, unsigned int w,
		     unsigned int h, VAImageID iid)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_surface *s = SURFACE(d, sid);
	struct cvd_image *img = IMAGE(d, iid);
	struct cvd_buffer *b;
	unsigned int r, bpp;

	if (!s)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	cvd_surface_flush(d, s);
	if (!img || !(b = BUFFER(d, img->image.buf)))
		return VA_STATUS_ERROR_INVALID_IMAGE;
	if (img->image.format.fourcc != s->fmt->va)
		return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
	bpp = s->fmt->bpp;
	if (x < 0 || y < 0 || x + w > s->width || y + h > s->height ||
	    w > img->image.width || h > img->image.height || !s->map)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (s->fmt->rt_format == VA_RT_FORMAT_RGB32) {
		cvd_surface_cpu(s, true, false);
		for (r = 0; r < h; r++)
			memcpy(b->data + img->image.offsets[0] + r * img->image.pitches[0],
			       s->map + s->offset + (y + r) * s->pitch + x * bpp, w * bpp);
		cvd_surface_cpu(s, false, false);
		return VA_STATUS_SUCCESS;
	}
	for (r = 0; r < h; r++)
		memcpy(b->data + img->image.offsets[0] + r * img->image.pitches[0],
		       s->map + s->offset + (y + r) * s->pitch + x * bpp, w * bpp);
	for (r = 0; r < (h + 1) / 2; r++)
		memcpy(b->data + img->image.offsets[1] + r * img->image.pitches[1],
		       s->map + s->offset + s->luma_size + (y / 2 + r) * s->pitch + (x & ~1) * bpp,
		       ((w + 1) & ~1u) * bpp);
	return VA_STATUS_SUCCESS;
}
