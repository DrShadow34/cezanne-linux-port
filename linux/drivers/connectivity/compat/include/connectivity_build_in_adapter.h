/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline stand-in for the vendor kernel's built-in connectivity adapter.
 * The hooks it exported (SCP/conap, clock buffer debug, dump helpers) do not
 * exist on mainline; the conninfra paths that need them are debug only.
 */
#ifndef CONNECTIVITY_BUILD_IN_ADAPTER_H
#define CONNECTIVITY_BUILD_IN_ADAPTER_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/sched/clock.h>
#include <linux/string.h>

/* show_stack() is not exported to modules */
#define KERNEL_show_stack(task, sp)	sched_show_task(task)

static inline void connectivity_export_clk_buf_show_status_info(void) {}
#define KERNEL_clk_buf_show_status_info connectivity_export_clk_buf_show_status_info

static inline void connectivity_export_dump_thread_state(const char *name) {}


/* SCP/conap does not exist here; accept any arguments */
#define connectivity_export_conap_scp_init(...)			0
#define connectivity_export_conap_scp_deinit(...)		0
#define connectivity_export_conap_scp_state_change(...)		0
#define connectivity_export_conap_scp_trigger_cmd(...)		0
#define connectivity_export_conap_scp_trigger_dfd_cmd(...)	0
#define connectivity_export_conap_scp_clr_dfd_buffer(...)	0

#endif
