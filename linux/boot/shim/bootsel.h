/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One-shot OS selection for the boot chooser shim, shared with the kernel
 * side (cezanne-bootsel). The flag must live in DRAM that survives a warm
 * reboot: preloader fills reserved carve-outs (aee_debug_kinfo, minirdump,
 * ...) with a test pattern on reset, but keeps the pstore region. So it sits
 * at the end of the ramoops pmsg zone (0x4d0e0000, 64 KiB): unused on Arch,
 * overwritten by Android's logging on MIUI. The shim clears it when it acts
 * on it.
 */
#ifndef CEZANNE_BOOTSEL_H
#define CEZANNE_BOOTSEL_H

#define BOOTSEL_ADDR	0x4d0eff00
#define BOOTSEL_MIUI	0x544f4f4249554d49ULL	/* "IMUIBOOT" read as LE u64 */
/* word 1 holds ~word 0, so leftover RAM contents never match by accident */
/* word 2: failed Arch boot attempts, tag in the upper half ("ARCH") */
#define BOOTSEL_TRIES_TAG	0x41524348ULL

/* where the Arch path keeps lk's FDT (reserved in our DT; rewritten each boot) */
#define LKFDT_SAVE	0x4bd00000
#define LKFDT_MAX	0x80000

#endif
