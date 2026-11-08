/* SPDX-License-Identifier: MIT */
/*
 * libcezanne-top: GPU and media engine usage for SoCs without PCI GPUs.
 *
 * GPU render load, clocks, temperature and memory come from DRM render
 * nodes (per-client fdinfo: drm-engine-* busy ns, drm-resident-memory),
 * devfreq and thermal zones; decoder / encoder load from V4L2 codec
 * drivers that export vcodec_* attributes on their device (cezanne-vdec:
 * vcodec_engine, vcodec_busy_ns, vcodec_clients). Loads are averages
 * over the time between two ctop_refresh() calls.
 *
 * Used by the cezanne-top CLI and by Mission Center's helper (magpie).
 */
#ifndef CTOP_H
#define CTOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTOP_MAX_ENGINES	4

/* struct ctop_gpu.valid / struct ctop_proc.valid */
enum {
	CTOP_TEMP	= 1 << 0,
	CTOP_CLOCK	= 1 << 1,
	CTOP_CLOCK_MAX	= 1 << 2,
	CTOP_MEMORY	= 1 << 3,
	CTOP_UTIL	= 1 << 4,	/* render load (needs a previous sample) */
	CTOP_DECODE	= 1 << 5,	/* a decoder exists */
	CTOP_ENCODE	= 1 << 6,	/* an encoder exists */
};

struct ctop_proc {
	int32_t pid;
	char comm[16];
	uint32_t valid;		/* CTOP_UTIL / CTOP_MEMORY / CTOP_DECODE / CTOP_ENCODE */
	uint64_t memory;	/* bytes resident in GPU buffers */
	float util, decode, encode;	/* percent */
};

struct ctop_gpu {
	char id[32];		/* device name, e.g. "13000000.gpu" */
	char name[64];		/* e.g. "Mali-G77 MC9" */
	char driver[16];
	char render_node[32];	/* e.g. "/dev/dri/renderD128" */
	uint32_t vendor_id;	/* PCI-style vendor (0x13b5 Arm), 0 if unknown */
	uint32_t device_id;	/* GPU_ID (product << 16 | revision), = Vulkan deviceID */
	uint32_t valid;
	int32_t temp_mc;	/* millidegree Celsius */
	uint32_t clock_mhz, clock_max_mhz;
	uint64_t memory_used;	/* bytes resident in GPU buffers, all clients */
	uint64_t memory_total;	/* bytes the GPU can use (system RAM: unified) */
	float util;		/* render load: busiest engine, percent */
	uint32_t engines;
	char engine_name[CTOP_MAX_ENGINES][16];
	float engine_util[CTOP_MAX_ENGINES];
	float decode, encode;	/* media engine load, percent */
	uint32_t nproc;
	const struct ctop_proc *proc;
};

struct ctop;

struct ctop *ctop_open(void);
/* sample everything; loads are over the time since the previous call */
int ctop_refresh(struct ctop *c);
uint32_t ctop_count(const struct ctop *c);
const struct ctop_gpu *ctop_gpu(const struct ctop *c, uint32_t i);
void ctop_close(struct ctop *c);

#ifdef __cplusplus
}
#endif

#endif
