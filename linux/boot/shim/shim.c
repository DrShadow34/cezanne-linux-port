// SPDX-License-Identifier: GPL-2.0
/*
 * Boot chooser logic, called from shim.S with the MMU and D-cache off.
 * No globals: the binary has no .data/.bss (whatever follows the shim in
 * memory is the DTB). Build with -mstrict-align: with the MMU off every
 * access is Device memory, which faults on unaligned accesses.
 */
#include "bootsel.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define KERNEL_DEST	0x40200000	/* Arch: 2 MiB aligned, text_offset 0 */
#define DTB_DEST	0x4bc80000	/* lk's dtb_kernel_addr_mb slot */
#define MIUI_DEST	0x40080000	/* where lk loads (and enters) a kernel */
#define RAMDISK_ADDR	0x47c80000	/* where lk loads the boot.img ramdisk */

/*
 * Progress markers go into the ramoops console zone (same address Android's
 * 4.14 kernel reads back as /sys/fs/pstore/console-ramoops), formatted as a
 * struct persistent_ram_buffer: { u32 sig; u32 start; u32 size; u8 data[]; }
 */
#define LOG_BUF		0x4d05f000
#define LOG_SIG		0x43474244	/* PERSISTENT_RAM_SIG ^ 0 (console zone) */
/* append below this: Arch's console zone is 512 KiB, MIUI's 256 KiB */
#define LOG_KEEP_ARCH	0x7e000
#define LOG_KEEP_MIUI	0x3e000

/* Arch boot attempts without reaching userspace, then fall back to MIUI */
#define ARCH_TRIES	3

struct desc {
	u64 dtb_off, dtb_size;
	u64 kern_off, kern_size;
	u64 miui_off, miui_size;
	u64 rd_off, rd_size;
};

struct boot {
	u64 x0, entry;
	u64 move_src, move_len;	/* MIUI: Image move done by the trampoline */
	u64 footprint;
};

extern struct desc desc;
void dcache_inval(u64 addr, u64 len);
void copy_fwd(u64 dst, u64 src, u64 len);
void copy_bwd(u64 dst, u64 src, u64 len);

#define rd32(a)		(*(volatile u32 *)(a))
#define wr32(a, v)	(*(volatile u32 *)(a) = (v))
#define rd64(a)		(*(volatile u64 *)(a))
#define wr64(a, v)	(*(volatile u64 *)(a) = (v))

/*
 * persistent_ram_buffer: start is the ring's write position, size the bytes
 * held; a log that has not wrapped has start == size (kernel) or start == 0
 * (ours), and reads as data[0, size).
 */
static int log_wrapped(void)
{
	u32 start = rd32(LOG_BUF + 4);

	return start && start != rd32(LOG_BUF + 8);
}

static void log(const char *s)
{
	volatile u8 *data = (volatile u8 *)(LOG_BUF + 12);
	u32 n = rd32(LOG_BUF + 8);

	if (log_wrapped())	/* keep a wrapped ring intact */
		return;
	while (*s)
		data[n++] = *s++;
	data[n++] = '\n';
	wr32(LOG_BUF + 8, n);
	wr32(LOG_BUF + 4, 0);	/* start 0: the log is data[0, size) */
}

static void log_hex(const char *s, u64 v)
{
	char buf[64];
	int i = 0, d;

	while (*s && i < 40)
		buf[i++] = *s++;
	buf[i++] = '0';
	buf[i++] = 'x';
	for (d = 60; d >= 0; d -= 4)
		buf[i++] = "0123456789abcdef"[(v >> d) & 15];
	buf[i] = 0;
	log(buf);
}

static u32 be32(u64 a)
{
	volatile u8 *p = (volatile u8 *)a;

	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static void put_be32(u64 a, u32 v)
{
	volatile u8 *p = (volatile u8 *)a;

	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static int streq(u64 a, const char *s)
{
	volatile const u8 *p = (volatile const u8 *)a;

	while (*s)
		if (*p++ != (u8)*s++)
			return 0;
	return *p == 0;
}

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static int fdt_ok(u64 fdt)
{
	return fdt && be32(fdt) == FDT_MAGIC && be32(fdt + 4) <= LKFDT_MAX;
}

/* set a 1- or 2-cell property of /chosen in place; 0 on success */
static int fdt_set_chosen(u64 fdt, const char *name, u64 val)
{
	u64 p = fdt + be32(fdt + 8);
	u64 strings = fdt + be32(fdt + 12);
	u64 end = fdt + be32(fdt + 4);
	int depth = 0, in_chosen = 0;

	while (p < end) {
		u32 tok = be32(p);

		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			if (depth == 2)
				in_chosen = streq(p, "chosen");
			while (*(volatile u8 *)p)
				p++;
			p = (p + 4) & ~3UL;	/* NUL + pad */
			break;
		case FDT_END_NODE:
			if (depth-- == 2)
				in_chosen = 0;
			break;
		case FDT_PROP: {
			u32 len = be32(p), nameoff = be32(p + 4);
			u64 val_at = p + 8;

			if (in_chosen && depth == 2 && streq(strings + nameoff, name)) {
				if (len == 4) {
					put_be32(val_at, val);
					return 0;
				}
				if (len == 8) {
					put_be32(val_at, val >> 32);
					put_be32(val_at + 4, val);
					return 0;
				}
				return -1;
			}
			p = (val_at + len + 3) & ~3UL;
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return -1;
		}
	}
	return -1;
}

/* decided before logging starts: the log buffer depends on the OS */
static int want_miui(u64 *flag, u64 *failed)
{
	u64 w0, w1, tries;

	if (!desc.miui_size || !desc.rd_size)
		return 0;
	dcache_inval(BOOTSEL_ADDR, 32);
	w0 = rd64(BOOTSEL_ADDR);
	w1 = rd64(BOOTSEL_ADDR + 8);
	tries = rd64(BOOTSEL_ADDR + 16);
	tries = (tries >> 32) == BOOTSEL_TRIES_TAG ? tries & 0xffffffff : 0;
	*flag = w0;
	*failed = tries;
	if (w0 == BOOTSEL_MIUI && w1 == ~BOOTSEL_MIUI) {
		wr64(BOOTSEL_ADDR, 0);		/* one-shot */
		wr64(BOOTSEL_ADDR + 8, 0);
		return 1;
	}
	if (tries >= ARCH_TRIES) {
		/* Arch keeps dying before userspace: give MIUI a turn */
		wr64(BOOTSEL_ADDR + 16, 0);
		return 1;
	}
	/* cleared by cezanne-bootsel once Arch's userspace is up */
	wr64(BOOTSEL_ADDR + 16, BOOTSEL_TRIES_TAG << 32 | (tries + 1));
	return 0;
}

static u64 max(u64 a, u64 b)
{
	return a > b ? a : b;
}

static int boot_miui(u64 head, u64 lk_fdt, struct boot *b)
{
	u64 rd_len = (desc.rd_size + 7) & ~7UL;
	u64 img = head + desc.miui_off;

	if (!fdt_ok(lk_fdt)) {
		log("SHIM: no lk FDT, booting Arch");
		return -1;
	}
	if (fdt_set_chosen(lk_fdt, "linux,initrd-start", RAMDISK_ADDR) ||
	    fdt_set_chosen(lk_fdt, "linux,initrd-end", RAMDISK_ADDR + desc.rd_size)) {
		log("SHIM: lk FDT has no initrd in /chosen, booting Arch");
		return -1;
	}
	dcache_inval(lk_fdt, be32(lk_fdt + 4));

	dcache_inval(RAMDISK_ADDR, rd_len);
	copy_fwd(RAMDISK_ADDR, head + desc.rd_off, rd_len);
	dcache_inval(RAMDISK_ADDR, rd_len);
	log("SHIM: MIUI ramdisk at 0x47c80000");

	b->x0 = lk_fdt;
	b->entry = MIUI_DEST;
	b->move_src = img;
	b->move_len = desc.miui_size;
	b->footprint = max(rd64(img + 16), desc.miui_size);	/* header image_size */
	log("SHIM: entering MIUI");
	return 0;
}

static void boot_arch(u64 head, u64 lk_fdt, struct boot *b)
{
	u64 kern = head + desc.kern_off;
	u64 footprint = max(rd64(kern + 16), desc.kern_size);

	/*
	 * lk ran with caches on and may still have (even dirty) lines for the
	 * destinations; we write with the MMU off, straight to DRAM. Invalidate
	 * (not clean: that could write stale data over ours) before and after
	 * copying, so the kernel's first cached reads see what we wrote.
	 */
	dcache_inval(LKFDT_SAVE, LKFDT_MAX);
	dcache_inval(DTB_DEST, 0x80000);
	dcache_inval(KERNEL_DEST, footprint);

	/* lk's FDT first: ours goes to the same slot */
	if (fdt_ok(lk_fdt)) {
		copy_fwd(LKFDT_SAVE, lk_fdt, (be32(lk_fdt + 4) + 7) & ~7UL);
		log("SHIM: lk FDT saved at 0x4bd00000");
	} else {
		wr32(LKFDT_SAVE, 0);
	}

	copy_fwd(DTB_DEST, head + desc.dtb_off, desc.dtb_size);
	log("SHIM: dtb copied to 0x4bc80000");
	/* dst > src, regions overlap -> copy backwards */
	copy_bwd(KERNEL_DEST, kern, desc.kern_size);
	log("SHIM: kernel moved to 0x40200000");

	dcache_inval(LKFDT_SAVE, LKFDT_MAX);
	dcache_inval(DTB_DEST, 0x80000);
	dcache_inval(KERNEL_DEST, footprint);

	b->x0 = DTB_DEST;
	b->entry = KERNEL_DEST;
	b->move_len = 0;
	log("SHIM: jumping to kernel");
}

void shim_main(u64 lk_fdt, u64 head, struct boot *b)
{
	u64 flag = 0, failed = 0;
	int miui = want_miui(&flag, &failed);
	u32 size = rd32(LOG_BUF + 8);

	/*
	 * Append to the previous boot's console log (the kernel saves it as
	 * console-ramoops-0), so a boot that hung or reset the phone stays
	 * readable; start a fresh buffer if it is invalid, wrapped, or too big
	 * for the kernel we are about to start. A wrapped Arch log is left
	 * alone without our markers.
	 */
	if (miui && rd32(LOG_BUF) == LOG_SIG && !log_wrapped() &&
	    size > LOG_KEEP_MIUI && size <= LOG_KEEP_ARCH) {
		/* an Arch log too big for MIUI's zone: keep its tail */
		volatile u8 *data = (volatile u8 *)(LOG_BUF + 12);
		u32 cut = size - LOG_KEEP_MIUI, i;

		for (i = 0; i < LOG_KEEP_MIUI; i++)	/* bytes: data is not 8-aligned */
			data[i] = data[i + cut];
		size -= cut;
		wr32(LOG_BUF + 8, size);
		wr32(LOG_BUF + 4, 0);
	}
	if (rd32(LOG_BUF) != LOG_SIG || size > (miui ? LOG_KEEP_MIUI : LOG_KEEP_ARCH) ||
	    (log_wrapped() && miui)) {
		wr32(LOG_BUF, LOG_SIG);
		wr32(LOG_BUF + 4, 0);
		wr32(LOG_BUF + 8, 0);
	}
	if (!log_wrapped()) {
		log("SHIM: entered (lk -> GZ -> EL1)");
		log_hex("SHIM: bootsel ", flag);
		log_hex("SHIM: failed Arch boots ", failed);
		if (miui && flag != BOOTSEL_MIUI)
			log("SHIM: Arch failed too often, falling back to MIUI");
	}

	if (!fdt_ok(lk_fdt))
		lk_fdt = DTB_DEST;	/* lk's slot, in case x0 was not set */
	if (miui && !boot_miui(head, lk_fdt, b))
		return;
	boot_arch(head, lk_fdt, b);
}
