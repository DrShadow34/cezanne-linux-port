/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline stand-in for the vendor display notifier. There is no MTK DRM
 * driver here, so Wi-Fi gets no screen on/off hints (power save stays on its
 * own heuristics).
 */
#pragma once
#include <linux/notifier.h>

#define MTK_DISP_EARLY_EVENT_BLANK	0x00
#define MTK_DISP_EVENT_BLANK		0x01
#define MTK_DISP_BLANK_UNBLANK		0x00
#define MTK_DISP_BLANK_POWERDOWN	0x01

static inline int mtk_disp_notifier_register(const char *source,
					     struct notifier_block *nb)
{ return 0; }
static inline int mtk_disp_notifier_unregister(struct notifier_block *nb)
{ return 0; }
