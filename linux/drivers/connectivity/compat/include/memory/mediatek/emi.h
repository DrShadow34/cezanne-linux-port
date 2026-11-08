/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline stand-in for the vendor EMI MPU API. Mainline has no EMI MPU
 * driver; lk/ATF set up the CONNSYS region, so the calls are no-ops.
 */
#ifndef __SOC_MEDIATEK_EMI_H__
#define __SOC_MEDIATEK_EMI_H__

#include <linux/types.h>

#define MTK_EMIMPU_NO_PROTECTION	0
#define MTK_EMIMPU_SEC_RW		1
#define MTK_EMIMPU_SEC_RW_NSEC_R	2
#define MTK_EMIMPU_SEC_RW_NSEC_W	3
#define MTK_EMIMPU_SEC_R_NSEC_R		4
#define MTK_EMIMPU_FORBIDDEN		5
#define MTK_EMIMPU_SEC_R_NSEC_RW	6

struct emimpu_region_t {
	unsigned long long start;
	unsigned long long end;
	unsigned int rg_num;
	bool lock;
	unsigned int *apc;
};

static inline int mtk_emimpu_init_region(struct emimpu_region_t *r, unsigned int num)
{ r->rg_num = num; return 0; }
static inline int mtk_emimpu_set_addr(struct emimpu_region_t *r,
				      unsigned long long start, unsigned long long end)
{ r->start = start; r->end = end; return 0; }
static inline int mtk_emimpu_set_apc(struct emimpu_region_t *r,
				     unsigned int d_num, unsigned int apc)
{ return 0; }
static inline int mtk_emimpu_lock_region(struct emimpu_region_t *r, bool lock)
{ return 0; }
static inline int mtk_emimpu_set_protection(struct emimpu_region_t *r)
{ return 0; }
static inline int mtk_emimpu_free_region(struct emimpu_region_t *r) { return 0; }

#endif
