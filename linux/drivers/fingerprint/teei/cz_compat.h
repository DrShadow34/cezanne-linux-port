/* SPDX-License-Identifier: GPL-2.0 */
/* Kernel API differences between the vendor 4.14 tree and mainline */
#ifndef CZ_COMPAT_H
#define CZ_COMPAT_H
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/sysreg.h>
#include <linux/kthread.h>

/* __flush_dcache_area(): clean + invalidate to PoC (not exported any more) */
static inline void cz_flush_dcache_area(void *addr, size_t len)
{
	size_t line = 4 << ((read_sysreg(ctr_el0) >> 16) & 0xf);
	unsigned long p = (unsigned long)addr & ~(line - 1);
	unsigned long end = (unsigned long)addr + len;

	for (; p < end; p += line)
		asm volatile("dc civac, %0" : : "r"(p) : "memory");
	dsb(sy);
}
#define __flush_dcache_area cz_flush_dcache_area
#endif
