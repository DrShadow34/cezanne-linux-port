/* SPDX-License-Identifier: GPL-2.0 */
/* 4.14 BSP sensor hub sources on a 7.x kernel; force-included (Kbuild) */
#ifndef __COMPAT_SENSOR_H__
#define __COMPAT_SENSOR_H__

#include <linux/version.h>
#include <linux/timekeeping.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/arch_timer.h>

#define ktime_get_boot_ns()		ktime_get_boottime_ns()
#define strlcpy(d, s, n)		strscpy(d, s, n)
#define arch_counter_get_cntvct()	__arch_counter_get_cntvct()
#define PDE_DATA(inode)			pde_data(inode)

/*
 * Not exported; true on arm64, where the arch timer keeps counting in
 * suspend so timekeeping never injects RTC sleep time.
 */
#define timekeeping_rtc_skipresume()	true

#endif
