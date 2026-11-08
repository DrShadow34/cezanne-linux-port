/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Force-included into the MediaTek connectivity modules: kernel API renames
 * between the GKI kernels they target and mainline 7.2.
 */
#ifndef __CONNECTIVITY_COMPAT_MAINLINE_H__
#define __CONNECTIVITY_COMPAT_MAINLINE_H__

#include <linux/string.h>
#include <linux/timer.h>
#include <linux/alarmtimer.h>
#include <linux/random.h>
#include <linux/version.h>

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif
#define del_timer(t)		timer_delete(t)
#define del_timer_sync(t)	timer_delete_sync(t)
#define get_random_int()	get_random_u32()

/* strncpy() is gone from mainline; same semantics (pad, no forced NUL) */
static inline char *compat_strncpy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return dst;
}
#define strncpy(d, s, n)	compat_strncpy(d, s, n)

/* alarmtimer: callbacks return void, start takes a relative flag */
#define alarm_start_relative(a, t)	alarm_start_timer(a, t, true)


/* sched_setscheduler() is no longer exported: vendor threads only ask for RT */
#include <linux/sched.h>
static inline int compat_sched_setscheduler(struct task_struct *p, int policy, const void *param)
{
	sched_set_fifo(p);
	return 0;
}
#define sched_setscheduler(p, policy, param) compat_sched_setscheduler(p, policy, param)

#endif
