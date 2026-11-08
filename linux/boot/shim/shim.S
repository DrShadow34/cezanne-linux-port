/*
 * Boot shim / chooser for MediaTek lk (Redmi K30 Ultra / cezanne).
 *
 * lk always hands the kernel its own built-in Android device tree. This shim
 * is what lk boots instead. shim.c picks the OS:
 *  - Arch (default): relocate the arm64 Image to a 2 MiB aligned address,
 *    keep a copy of lk's FDT (LKFDT_SAVE, for the modem port), copy our
 *    mainline DTB to lk's DTB slot and enter the kernel with x0 = our DTB.
 *  - MIUI, when the image carries it and the one-shot RAM flag is set
 *    (bootsel.h): install MIUI's ramdisk, point lk's FDT at it, then a
 *    trampoline moves MIUI's Image to where lk would have put it and enters
 *    it with lk's FDT, exactly like lk does.
 * The kernels are entered with the MMU off (arm64 boot protocol).
 *
 * Layout produced by mkshim.py:
 *   [ shim (padded) ][ DTB ][ Image ][ MIUI Image ][ MIUI ramdisk ]
 * The descriptor words below are patched in by mkshim.py.
 */

#define STACK_TOP	0x46f00000	/* free DRAM below TRAMP, above the blob */
#define TRAMP		0x47000000	/* trampoline copy (MIUI path) */

	.section .text
	.globl _head
_head:
	/* arm64 Image header so lk treats us like a normal kernel */
	b	start				/* code0 */
	.long	0				/* code1 */
	.quad	0x80000				/* text_offset (lk loads at base + 0x80000) */
image_size:
	.quad	0				/* patched: total image size */
	.quad	0xa				/* flags: LE, 4K pages, placement anywhere (as kbuild) */
	.quad	0
	.quad	0
	.quad	0
	.ascii	"ARM\x64"			/* magic */
	.long	0

	/* descriptors (offsets relative to _head), patched by mkshim.py */
	.globl desc
desc:
	.quad	0, 0				/* DTB offset, size */
	.quad	0, 0				/* Image */
	.quad	0, 0				/* MIUI Image (size 0: Arch only) */
	.quad	0, 0				/* MIUI ramdisk */

start:
	mov	x19, x0				/* lk's FDT */
	ldr	x0, =STACK_TOP
	mov	sp, x0
	mov	x0, x19
	adr	x1, _head
	adr	x2, boot
	bl	shim_main			/* fills struct boot, see shim.c */

	/* kernel code was written with data accesses: drop stale I-cache lines */
	dsb	sy
	ic	iallu
	dsb	sy
	isb

	adr	x9, boot
	ldr	x10, [x9, #24]			/* move len: 0 = enter in place */
	cbnz	x10, 1f
	ldr	x0, [x9]			/* x0 for the kernel */
	ldr	x4, [x9, #8]			/* entry */
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x4

	/* MIUI: the Image moves over this code; run the move from TRAMP */
1:	adr	x1, tramp
	ldr	x0, =TRAMP
	mov	x2, #(tramp_end - tramp)
2:	ldr	x3, [x1], #8
	str	x3, [x0], #8
	subs	x2, x2, #8
	b.gt	2b
	ldr	x0, =TRAMP
	mov	x1, #(tramp_end - tramp)
	bl	dcache_inval
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	ldr	x0, [x9]			/* fdt */
	ldr	x4, [x9, #8]			/* entry = move destination */
	ldr	x5, [x9, #16]			/* src */
	ldr	x6, [x9, #24]			/* len to copy */
	ldr	x7, [x9, #32]			/* footprint (image_size incl. BSS) */
	ldr	x8, =TRAMP
	br	x8

/*
 * Position independent; runs at TRAMP. x4 = dst (< src, regions may
 * overlap: copy ascending), x5 = src, x6 = len (multiple of 8),
 * x7 = footprint to invalidate, x0 = FDT for the kernel.
 */
	.balign	8
tramp:
	/* invalidate dst footprint (lines may be stale from lk) */
	mrs	x3, ctr_el0
	ubfx	x3, x3, #16, #4
	mov	x2, #4
	lsl	x2, x2, x3			/* line size */
	sub	x3, x2, #1
	bic	x10, x4, x3
	add	x11, x4, x7
1:	dc	ivac, x10
	add	x10, x10, x2
	cmp	x10, x11
	b.lo	1b
	dsb	sy
	mov	x10, x4
	mov	x11, x5
	mov	x12, x6
2:	ldr	x3, [x11], #8
	str	x3, [x10], #8
	subs	x12, x12, #8
	b.gt	2b
	dsb	sy
	sub	x3, x2, #1
	bic	x10, x4, x3
	add	x11, x4, x7
3:	dc	ivac, x10
	add	x10, x10, x2
	cmp	x10, x11
	b.lo	3b
	dsb	sy
	ic	iallu
	dsb	sy
	isb
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x4
	.balign	8
tramp_end:

/* invalidate D-cache lines covering [x0, x0 + x1) to PoC; clobbers x0-x3 */
	.globl dcache_inval
dcache_inval:
	mrs	x3, ctr_el0
	ubfx	x3, x3, #16, #4			/* DminLine: log2(words) */
	mov	x2, #4
	lsl	x2, x2, x3			/* line size in bytes */
	add	x1, x0, x1
	sub	x3, x2, #1
	bic	x0, x0, x3
1:	dc	ivac, x0
	add	x0, x0, x2
	cmp	x0, x1
	b.lo	1b
	dsb	sy
	ret

/* copy x2 bytes (multiple of 8) from x1 to x0, ascending */
	.globl copy_fwd
copy_fwd:
	cbz	x2, 2f
1:	ldr	x3, [x1], #8
	str	x3, [x0], #8
	subs	x2, x2, #8
	b.gt	1b
2:	ret

/* copy x2 bytes (multiple of 8) from x1 to x0, descending (dst > src) */
	.globl copy_bwd
copy_bwd:
	cbz	x2, 2f
	add	x1, x1, x2
	add	x0, x0, x2
1:	ldr	x3, [x1, #-8]!
	str	x3, [x0, #-8]!
	subs	x2, x2, #8
	b.gt	1b
2:	ret

	.ltorg

	.balign	8
	/* struct boot (shim.c): fdt, entry, move src, move len, footprint */
boot:	.quad	0, 0, 0, 0, 0
