/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline stand-in for the vendor kernel's built-in WMT adapter. On the
 * vendor kernel these bridges let built-in code (thermal, SDIO, debugfs) call
 * into the connectivity modules; nothing on mainline uses them.
 */
#ifndef WMT_BUILD_IN_ADAPTER_H
#define WMT_BUILD_IN_ADAPTER_H

#include <linux/fs.h>

typedef int (*wmt_bridge_thermal_query_cb)(void);
typedef int (*wmt_bridge_trigger_assert_cb)(void);
typedef void (*wmt_bridge_connsys_clock_fail_dump_cb)(void);
typedef int (*wmt_bridge_conninfra_reg_readable)(void);
typedef int (*wmt_bridge_conninfra_reg_is_bus_hang)(void);

struct wmt_platform_bridge {
	wmt_bridge_thermal_query_cb thermal_query_cb;
	wmt_bridge_trigger_assert_cb trigger_assert_cb;
	wmt_bridge_connsys_clock_fail_dump_cb clock_fail_dump_cb;
	wmt_bridge_conninfra_reg_readable conninfra_reg_readable_cb;
	wmt_bridge_conninfra_reg_is_bus_hang conninfra_reg_is_bus_hang_cb;
};

struct wmt_platform_dbg_bridge {
	ssize_t (*write_cb)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_cb)(struct file *, char __user *, size_t, loff_t *);
};

static inline void wmt_export_platform_bridge_register(const struct wmt_platform_bridge *cb) {}
static inline void wmt_export_platform_bridge_unregister(void) {}
static inline void wmt_export_platform_dbg_bridge_register(const struct wmt_platform_dbg_bridge *cb) {}
static inline void wmt_export_platform_dbg_bridge_unregister(void) {}

#endif
