// SPDX-License-Identifier: MIT
/*
 * cezanne VA-API driver: video processing (VAProfileNone /
 * VAEntrypointVideoProc) on the CPU: copies, RGB <-> NV12 conversion
 * (BT.601 / BT.709 limited range YCbCr, full range RGB) and nearest-
 * neighbour scaling between the driver's 8-bit surface formats; no
 * filters, no rotation.
 *
 * It exists for GStreamer's vapostproc, which GNOME's screen recorder puts
 * in front of vah264enc. That pipeline normally passes the compositor's
 * dma-buf straight through (same caps in and out: the encoder imports it),
 * so this code runs only when a client really asks for a conversion.
 */
#include <stdlib.h>
#include <string.h>

#include "cvd.h"

static VAProcColorStandardType standards[] = {
	VAProcColorStandardBT601, VAProcColorStandardBT709, VAProcColorStandardSRGB,
};

VAStatus cvdQueryVideoProcFilters(VADriverContextP ctx, VAContextID context,
				  VAProcFilterType *filters, unsigned int *n)
{
	*n = 0;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdQueryVideoProcFilterCaps(VADriverContextP ctx, VAContextID context,
				     VAProcFilterType type, void *caps, unsigned int *n)
{
	*n = 0;
	return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
}

VAStatus cvdQueryVideoProcPipelineCaps(VADriverContextP ctx, VAContextID context,
				       VABufferID *filters, unsigned int nfilters,
				       VAProcPipelineCaps *caps)
{
	if (nfilters)
		return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
	caps->pipeline_flags = 0;
	caps->filter_flags = 0;
	caps->num_forward_references = 0;
	caps->num_backward_references = 0;
	caps->input_color_standards = standards;
	caps->num_input_color_standards = sizeof(standards) / sizeof(standards[0]);
	caps->output_color_standards = standards;
	caps->num_output_color_standards = sizeof(standards) / sizeof(standards[0]);
	caps->rotation_flags = 1 << VA_ROTATION_NONE;
	caps->blend_flags = 0;
	caps->mirror_flags = 0;
	caps->num_additional_outputs = 0;
	caps->min_input_width = caps->min_input_height = 2;
	caps->min_output_width = caps->min_output_height = 2;
	caps->max_input_width = caps->max_input_height = 4096;
	caps->max_output_width = caps->max_output_height = 4096;
	return VA_STATUS_SUCCESS;
}

/* the job of the picture; its regions point into the client's memory */
void cvd_vpp_render(struct cvd_context *c, struct cvd_buffer *b)
{
	if (b->type != VAProcPipelineParameterBufferType)
		return;
	memcpy(&c->vpp, b->data, sizeof(c->vpp));
	if (c->vpp.surface_region) {
		c->vpp_src = *c->vpp.surface_region;
		c->vpp.surface_region = &c->vpp_src;
	}
	if (c->vpp.output_region) {
		c->vpp_dst = *c->vpp.output_region;
		c->vpp.output_region = &c->vpp_dst;
	}
	c->have_vpp = true;
}

static VARectangle region(const VARectangle *r, const struct cvd_surface *s)
{
	VARectangle o = { 0, 0, s->width, s->height };

	if (r && r->width && r->height) {
		o = *r;
		if (o.x < 0)
			o.x = 0;
		if (o.y < 0)
			o.y = 0;
		if ((unsigned int)(o.x + o.width) > s->width)
			o.width = s->width - o.x;
		if ((unsigned int)(o.y + o.height) > s->height)
			o.height = s->height - o.y;
	}
	return o;
}

static inline uint8_t clip(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* YCbCr from full range RGB, limited range, 8 fractional bits */
struct rgb2yuv { int yr, yg, yb, ur, ug, ub, vr, vg, vb; };
static const struct rgb2yuv bt709 = { 47, 157, 16, -26, -87, 113, 113, -103, -10 };
static const struct rgb2yuv bt601 = { 66, 129, 25, -38, -74, 112, 112, -94, -18 };
/* and back: 1.164 (Y - 16), Cr -> R, Cb -> G, Cr -> G, Cb -> B */
struct yuv2rgb { int y, rv, gu, gv, bu; };
static const struct yuv2rgb bt709i = { 298, 459, 55, 136, 541 };
static const struct yuv2rgb bt601i = { 298, 409, 100, 208, 516 };

static uint8_t *plane0(const struct cvd_surface *s)
{
	return s->map + s->offset;
}

/* nearest source column / row for each destination one */
static void map_axis(unsigned int *m, unsigned int n, unsigned int src0, unsigned int srcn)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		m[i] = src0 + (unsigned int)(((uint64_t)i * srcn + srcn / (2 * n)) / n);
}

VAStatus cvd_vpp_end(struct cvd_driver *d, struct cvd_context *c)
{
	struct cvd_surface *src = c->have_vpp ? SURFACE(d, c->vpp.surface) : NULL;
	struct cvd_surface *dst = SURFACE(d, c->target);
	bool srgb, drgb, b601 = c->vpp.output_color_standard == VAProcColorStandardBT601;
	VARectangle sr, dr;
	unsigned int *xm, *ym, x, y;

	if (!c->have_vpp)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (!src || !dst)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	cvd_surface_flush(d, src);	/* decoded: detiled */
	cvd_surface_wait(d, dst);	/* the encoder may still read it */
	if (!src->map || !dst->map)
		return VA_STATUS_ERROR_OPERATION_FAILED;
	if (src->p010 || dst->p010)
		return VA_STATUS_ERROR_UNIMPLEMENTED;
	srgb = src->fmt->rt_format == VA_RT_FORMAT_RGB32;
	drgb = dst->fmt->rt_format == VA_RT_FORMAT_RGB32;
	if (!srgb)
		b601 = c->vpp.surface_color_standard == VAProcColorStandardBT601;
	sr = region(c->vpp.surface_region, src);
	dr = region(c->vpp.output_region, dst);
	if (!sr.width || !sr.height || !dr.width || !dr.height)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	xm = malloc(sizeof(*xm) * (dr.width + dr.height));
	if (!xm)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	ym = xm + dr.width;
	map_axis(xm, dr.width, sr.x, sr.width);
	map_axis(ym, dr.height, sr.y, sr.height);
	cvd_surface_cpu(src, true, false);
	cvd_surface_cpu(dst, true, true);

	if (srgb && drgb) {
		const struct cvd_format *sf = src->fmt, *df = dst->fmt;
		bool same = sf == df && sr.width == dr.width && sr.height == dr.height;

		for (y = 0; y < dr.height; y++) {
			const uint8_t *s = plane0(src) + (size_t)ym[y] * src->pitch;
			uint8_t *o = plane0(dst) + (size_t)(dr.y + y) * dst->pitch + dr.x * 4;

			if (same) {
				memcpy(o, s + sr.x * 4, dr.width * 4);
				continue;
			}
			for (x = 0; x < dr.width; x++, o += 4) {
				const uint8_t *p = s + xm[x] * 4;

				o[df->r] = p[sf->r];
				o[df->g] = p[sf->g];
				o[df->b] = p[sf->b];
				o[3] = sf->va == VA_FOURCC_BGRA || sf->va == VA_FOURCC_RGBA ? p[3] : 255;
			}
		}
	} else if (srgb) {
		/* RGB -> NV12: Y per pixel, CbCr from the 2x2 block */
		const struct rgb2yuv *k = b601 ? &bt601 : &bt709;
		const struct cvd_format *sf = src->fmt;
		uint8_t *luma = plane0(dst), *chroma = plane0(dst) + dst->luma_size;

		for (y = 0; y < dr.height; y++) {
			const uint8_t *s = plane0(src) + (size_t)ym[y] * src->pitch;
			uint8_t *o = luma + (size_t)(dr.y + y) * dst->pitch + dr.x;

			for (x = 0; x < dr.width; x++) {
				const uint8_t *p = s + xm[x] * 4;

				o[x] = clip(16 + ((k->yr * p[sf->r] + k->yg * p[sf->g] +
						   k->yb * p[sf->b] + 128) >> 8));
			}
		}
		for (y = 0; y < dr.height; y += 2) {
			const uint8_t *s0 = plane0(src) + (size_t)ym[y] * src->pitch;
			const uint8_t *s1 = plane0(src) +
					    (size_t)ym[y + 1 < dr.height ? y + 1 : y] * src->pitch;
			uint8_t *o = chroma + (size_t)((dr.y + y) / 2) * dst->pitch + (dr.x & ~1u);

			for (x = 0; x < dr.width; x += 2) {
				unsigned int x1 = x + 1 < dr.width ? x + 1 : x;
				const uint8_t *p[4] = { s0 + xm[x] * 4, s0 + xm[x1] * 4,
							s1 + xm[x] * 4, s1 + xm[x1] * 4 };
				int r = 0, g = 0, b = 0, i;

				for (i = 0; i < 4; i++) {
					r += p[i][sf->r];
					g += p[i][sf->g];
					b += p[i][sf->b];
				}
				o[x] = clip(128 + ((k->ur * r + k->ug * g + k->ub * b + 512) >> 10));
				o[x + 1] = clip(128 + ((k->vr * r + k->vg * g + k->vb * b + 512) >> 10));
			}
		}
	} else if (!drgb) {
		/* NV12 -> NV12 */
		for (y = 0; y < dr.height; y++) {
			const uint8_t *s = plane0(src) + (size_t)ym[y] * src->pitch;
			uint8_t *o = plane0(dst) + (size_t)(dr.y + y) * dst->pitch + dr.x;

			if (sr.width == dr.width) {
				memcpy(o, s + sr.x, dr.width);
				continue;
			}
			for (x = 0; x < dr.width; x++)
				o[x] = s[xm[x]];
		}
		for (y = 0; y < dr.height; y += 2) {
			const uint8_t *s = plane0(src) + src->luma_size +
					   (size_t)(ym[y] / 2) * src->pitch;
			uint8_t *o = plane0(dst) + dst->luma_size +
				     (size_t)((dr.y + y) / 2) * dst->pitch + (dr.x & ~1u);

			for (x = 0; x < dr.width; x += 2) {
				o[x] = s[xm[x] & ~1u];
				o[x + 1] = s[(xm[x] & ~1u) + 1];
			}
		}
	} else {
		/* NV12 -> RGB */
		const struct yuv2rgb *k = b601 ? &bt601i : &bt709i;
		const struct cvd_format *df = dst->fmt;

		for (y = 0; y < dr.height; y++) {
			const uint8_t *l = plane0(src) + (size_t)ym[y] * src->pitch;
			const uint8_t *ch = plane0(src) + src->luma_size + (size_t)(ym[y] / 2) * src->pitch;
			uint8_t *o = plane0(dst) + (size_t)(dr.y + y) * dst->pitch + dr.x * 4;

			for (x = 0; x < dr.width; x++, o += 4) {
				int yy = k->y * (l[xm[x]] - 16) + 128;
				int u = ch[xm[x] & ~1u] - 128, v = ch[(xm[x] & ~1u) + 1] - 128;

				o[df->r] = clip((yy + k->rv * v) >> 8);
				o[df->g] = clip((yy - k->gu * u - k->gv * v) >> 8);
				o[df->b] = clip((yy + k->bu * u) >> 8);
				o[3] = 255;
			}
		}
	}

	cvd_surface_cpu(dst, false, true);
	cvd_surface_cpu(src, false, false);
	free(xm);
	c->have_vpp = false;
	return VA_STATUS_SUCCESS;
}
