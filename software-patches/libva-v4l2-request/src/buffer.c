// SPDX-License-Identifier: MIT
/* cezanne VA-API driver: VA buffers (host memory; image buffers may point
 * into a surface; coded buffers map to a segment once their encode is done) */
#include <stdlib.h>
#include <string.h>

#include "cvd.h"

VAStatus cvdCreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type,
			 unsigned int size, unsigned int count, void *data, VABufferID *id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_buffer *b;
	int bid;

	bid = object_heap_allocate(&d->buffer_heap);
	b = BUFFER(d, bid);
	if (!b)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	b->type = type;
	b->size = size;
	b->count = count;
	b->external = false;
	b->surface = VA_INVALID_ID;
	b->coded_pending = false;
	b->coded_ctx = VA_INVALID_ID;
	memset(&b->seg, 0, sizeof(b->seg));
	b->data = malloc((size_t)size * count ?: 1);
	if (!b->data) {
		object_heap_free(&d->buffer_heap, &b->base);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	if (data)
		memcpy(b->data, data, (size_t)size * count);
	*id = bid;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdBufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned int count)
{
	struct cvd_buffer *b = BUFFER(DRV(ctx), id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (count > b->count)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	b->count = count;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdMapBuffer(VADriverContextP ctx, VABufferID id, void **p)
{
	struct cvd_buffer *b = BUFFER(DRV(ctx), id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (b->type == VAEncCodedBufferType) {
		/* the bitstream once the encoder has written it */
		cvd_enc_wait_coded(DRV(ctx), b);
		b->seg.buf = b->data;
		*p = &b->seg;
		return VA_STATUS_SUCCESS;
	}
	if (b->external) {
		/* a derived image: the CPU writes or reads the surface's dma-buf
		 * (cached for heap buffers; devices read it without snooping) */
		struct cvd_surface *s = SURFACE(DRV(ctx), b->surface);

		if (s)
			cvd_surface_cpu(s, true, true);
	}
	*p = b->data;
	return VA_STATUS_SUCCESS;
}

VAStatus cvdUnmapBuffer(VADriverContextP ctx, VABufferID id)
{
	struct cvd_buffer *b = BUFFER(DRV(ctx), id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (b->external) {
		struct cvd_surface *s = SURFACE(DRV(ctx), b->surface);

		if (s)
			cvd_surface_cpu(s, false, true);	/* written back for devices */
	}
	return VA_STATUS_SUCCESS;
}

VAStatus cvdDestroyBuffer(VADriverContextP ctx, VABufferID id)
{
	struct cvd_driver *d = DRV(ctx);
	struct cvd_buffer *b = BUFFER(d, id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (b->coded_pending)
		cvd_enc_wait_coded(d, b);	/* the encoder still writes into it */
	if (!b->external)
		free(b->data);
	object_heap_free(&d->buffer_heap, &b->base);
	return VA_STATUS_SUCCESS;
}

VAStatus cvdBufferInfo(VADriverContextP ctx, VABufferID id, VABufferType *type,
		       unsigned int *size, unsigned int *count)
{
	struct cvd_buffer *b = BUFFER(DRV(ctx), id);

	if (!b)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	*type = b->type;
	*size = b->size;
	*count = b->count;
	return VA_STATUS_SUCCESS;
}
