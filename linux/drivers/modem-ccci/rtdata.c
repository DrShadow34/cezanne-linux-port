// SPDX-License-Identifier: GPL-2.0
/*
 * Handshake 1: the MD asks which of its 64 runtime features the AP
 * supports (struct md_query_ap_feature in CCIF SRAM); the AP answers with
 * one TLV per feature in the runtime-data share memory and a summary in
 * SRAM (BSP ccci_modem.c ccci_md_prepare_runtime_data, modem_sys1.c
 * md_cd_send_runtime_data_v2). Values are the MT6885 layout; they match
 * byte for byte what MIUI 14's kernel sends (except seed and time).
 */
#include <linux/io.h>
#include <linux/random.h>
#include <linux/timekeeping.h>

#include "ccci.h"

enum {
	FEATURE_NOT_EXIST, FEATURE_NOT_SUPPORT, FEATURE_MUST_SUPPORT,
	FEATURE_OPTIONAL_SUPPORT, FEATURE_SUPPORT_BACKWARD_COMPAT,
};

/* runtime feature ids (BSP enum MD_CCCI_RUNTIME_FEATURE_ID) */
enum {
	BOOT_INFO = 0, EXCEPTION_SHARE_MEMORY, CCIF_SHARE_MEMORY,
	MISC_INFO_HIF_DMA_REMAP = 5, MISC_INFO_RTC_32K_LESS,
	MISC_INFO_RANDOM_SEED_NUM, MISC_INFO_GPS_COCLOCK, MISC_INFO_SBP_ID,
	MISC_INFO_CCCI, MISC_INFO_CLIB_TIME, MISC_INFO_C2K,
	MD_IMAGE_START_MEMORY, CCB_SHARE_MEMORY = 15, DHL_RAW_SHARE_MEMORY,
	DT_NETD_SHARE_MEMORY, DT_USB_SHARE_MEMORY, EE_AFTER_EPOF,
	AP_CCMNI_MTU, LWA_SHARE_MEMORY = 25, AUDIO_RAW_SHARE_MEMORY,
	MULTI_MD_MPU, CCISM_SHARE_MEMORY_EXP, MD_CONSYS_SHARE_MEMORY = 30,
	MD_USIP_SHARE_MEMORY, MD_MTEE_SHARE_MEMORY_ENABLE,
	MD_WIFI_PROXY_SHARE_MEMORY = 35, NVRAM_CACHE_SHARE_MEMORY,
	MD_MEM_AP_VIEW_INF = 38,
};

struct rt_feature {
	u8 id;
	u8 support;		/* support_mask:4, version:4 */
	u8 reserved[2];
	u32 len;
} __packed;

/* CCB data (DHL, MD monitor, META users) for ccb_gear_id 12 */
#define CCB_DATA_SIZE		0x178000
#define DHL_RAW_SIZE		0x3e00000
#define MD_MEM_SIZE		0x1e000000	/* MD bank 0 at AP 0xde000000 */
#define MD_MEM_PHYS		0xde000000
#define AP_CCMNI_MTU_VAL	(3584 - 16)	/* NET_RX_BUF - ccci_header */

static u8 *put(u8 *p, int id, u8 support, const void *data, u32 len)
{
	struct rt_feature f = { .id = id, .support = support, .len = len };

	memcpy(p, &f, sizeof(f));
	p += sizeof(f);
	if (len)
		memcpy(p, data, len);
	return p + len;
}

/* data for a feature the AP supports; returns its length, or -1 */
static int feature_data(struct ccci_md *md, int id, u32 *d)
{
	struct timespec64 ts;

	memset(d, 0, 48);
	switch (id) {
	case BOOT_INFO:		/* boot channel CONTROL_RX, normal boot */
	case MISC_INFO_HIF_DMA_REMAP:
	case MISC_INFO_RTC_32K_LESS:
	case MISC_INFO_GPS_COCLOCK:
	case EE_AFTER_EPOF:
		return 16;
	case EXCEPTION_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_MDCCCI_DBG;
		d[1] = 0x10000;
		return 8;
	case CCIF_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_CCISM_MCU;
		d[1] = md->rings_size;
		return 8;
	case MISC_INFO_RANDOM_SEED_NUM:
		d[0] = get_random_u32();
		return 16;
	case MISC_INFO_SBP_ID:
		d[0] = 0;		/* sbp code */
		d[1] = 0x7d;		/* world-mode bitmap of the ubin */
		return 16;
	case MISC_INFO_CCCI:
		d[0] = BIT(0) | BIT(1);	/* sequence check, MD status polling */
		return 16;
	case MISC_INFO_CLIB_TIME:
		ktime_get_real_ts64(&ts);
		d[0] = lower_32_bits(ts.tv_sec);
		d[1] = upper_32_bits(ts.tv_sec);
		return 16;		/* time zone, dst: 0 */
	case MISC_INFO_C2K:
		d[0] = BIT(2);		/* CDMA2000 enabled */
		return 16;
	case MD_IMAGE_START_MEMORY:
		d[0] = MD_MEM_PHYS;
		d[1] = md->md_img_size;
		return 8;
	case CCB_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_CCB_CTRL;
		d[1] = 0x1000;
		d[2] = C_MD_VIEW + C_CCB_DHL;
		d[3] = CCB_DATA_SIZE;
		return 16;
	case DHL_RAW_SHARE_MEMORY:
		d[0] = C_MD_VIEW + C_RAW_DHL;
		d[1] = DHL_RAW_SIZE;
		return 8;
	case DT_NETD_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_NETD;
		d[1] = 0x2000;
		return 8;
	case DT_USB_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_USB;
		d[1] = 0x1000;
		return 8;
	case AP_CCMNI_MTU:
		d[0] = AP_CCMNI_MTU_VAL;
		return 4;
	case LWA_SHARE_MEMORY:	/* no LWA region */
		return 8;
	case AUDIO_RAW_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_AUDIO;
		d[1] = 0xd000;
		return 8;
	case MULTI_MD_MPU:
		return 0;
	case CCISM_SHARE_MEMORY_EXP:
		d[0] = NC_MD_VIEW + NC_CCISM_MCU_EXP;
		d[1] = NC_CCISM_MCU_EXP_SIZE;
		return 8;
	case MD_CONSYS_SHARE_MEMORY:
		d[0] = C_MD_VIEW + C_MD_CONSYS;
		d[1] = C_NVRAM_CACHE - C_MD_CONSYS;
		return 8;
	case MD_USIP_SHARE_MEMORY:
		d[0] = C_MD_VIEW + C_USIP;
		d[1] = C_SIZE - C_USIP;
		return 8;
	case MD_MTEE_SHARE_MEMORY_ENABLE:
		return 4;		/* no MTEE */
	case MD_WIFI_PROXY_SHARE_MEMORY:
		d[0] = NC_MD_VIEW + NC_WIFI_PROXY;
		d[1] = 0x8000;
		return 8;
	case NVRAM_CACHE_SHARE_MEMORY:
		d[0] = C_MD_VIEW + C_NVRAM_CACHE;
		d[1] = 0x700000;
		return 8;
	case MD_MEM_AP_VIEW_INF:
		/* { MD view, size, AP physical (u64) } per bank */
		d[0] = 0;
		d[1] = MD_MEM_SIZE;
		d[2] = MD_MEM_PHYS;
		d[4] = NC_MD_VIEW;
		d[5] = NC_SIZE;
		d[6] = NC_PHYS;
		d[8] = C_MD_VIEW;
		d[9] = C_SIZE;
		d[10] = C_PHYS;
		return 48;
	}
	return -1;
}

/* the merge rule of the BSP: what MD asks for vs. what the AP has */
static u8 merge(u8 md_set, bool ap_has)
{
	u8 mask = md_set & 0xf, ver = md_set >> 4;

	switch (mask) {
	case FEATURE_NOT_EXIST:
	case FEATURE_MUST_SUPPORT:
		return md_set;
	case FEATURE_NOT_SUPPORT:
		return 0;
	case FEATURE_OPTIONAL_SUPPORT:
		return ap_has && ver == 0 ? FEATURE_MUST_SUPPORT : FEATURE_NOT_SUPPORT;
	case FEATURE_SUPPORT_BACKWARD_COMPAT:
		return ap_has ? FEATURE_MUST_SUPPORT : FEATURE_NOT_SUPPORT;
	}
	return 0;
}

static int prepare(struct ccci_md *md, const struct md_query_ap_feature *q)
{
	u8 *base = md->nc + NC_RUNTIME_DATA, *p = base;
	u32 d[12];
	int i, len;

	if (q->head_pattern != MD_FEATURE_QUERY_PATTERN ||
	    q->tail_pattern != MD_FEATURE_QUERY_PATTERN) {
		dev_err(md->dev, "HS1: bad feature query %#x/%#x\n",
			q->head_pattern, q->tail_pattern);
		return -EINVAL;
	}
	for (i = 0; i < FEATURE_COUNT; i++) {
		u8 sup;

		len = feature_data(md, i, d);
		sup = merge(q->feature_set[i], len >= 0);
		if ((q->feature_set[i] & 0xf) == FEATURE_MUST_SUPPORT && len < 0) {
			dev_err(md->dev, "HS1: MD needs feature %d\n", i);
			return -EINVAL;
		}
		if ((sup & 0xf) != FEATURE_MUST_SUPPORT)
			len = 0;
		if (p + sizeof(struct rt_feature) + len > base + RUNTIME_AP_SIZE)
			return -ENOSPC;
		p = put(p, i, sup, d, len);
	}
	wmb();
	return p - base;
}

void ccci_send_runtime_data(struct ccci_md *md, const void *query)
{
	struct ap_query_md_feature_v2_1 a = {
		.head_pattern = AP_FEATURE_QUERY_PATTERN,
		.share_memory_support = 2,	/* MULTI_MD_MPU_SUPPORT */
		.ap_runtime_data_addr = NC_MD_VIEW + NC_RUNTIME_DATA,
		.ap_runtime_data_size = RUNTIME_AP_SIZE,
		.md_runtime_data_addr = NC_MD_VIEW + NC_RUNTIME_DATA + RUNTIME_AP_SIZE,
		.md_runtime_data_size = RUNTIME_MD_SIZE,
		.noncached_mpu_start_addr = NC_MD_VIEW,
		.noncached_mpu_total_size = NC_SIZE,
		.cached_mpu_start_addr = C_MD_VIEW,
		.cached_mpu_total_size = C_SIZE,
		.tail_pattern = AP_FEATURE_QUERY_PATTERN,
	};
	struct ccci_header h = {
		.data = { 0, sizeof(h) + sizeof(a) },
		.channel = CCCI_CONTROL_TX,
		.reserved = MD_INIT_CHK_ID,
	};
	int n = prepare(md, query);

	if (n < 0)
		return;
	/* MPU size covers AP-MD1 and MD1-MD3 share memory */
	a.feature_set[1] = 1 << 4;
	ccci_sram_write(md, SRAM_UP_HEADER, &h, sizeof(h));
	ccci_sram_write(md, SRAM_AP_RT_DATA, &a, sizeof(a));
	wmb();
	ccci_ccif_send_ch(md, CCIF_CH_SRAM);
	dev_info(md->dev, "HS1: runtime data sent (%d bytes)\n", n);
}
