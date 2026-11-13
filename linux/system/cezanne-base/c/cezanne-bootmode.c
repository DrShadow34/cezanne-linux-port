// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Prints lk's boot mode for this boot: 0 normal, 8 power-off charging (the
 * phone was off and a charger powered it on), 9 low-battery charging, ...
 * (MediaTek BOOTMODE). lk passes it in its FDT as /chosen "atag,boot" (u32
 * size, u32 tag 0x41000802, u32 mode, little-endian); the boot shim keeps
 * that FDT in the lk-fdt carve-out (reserved-memory, no-map), read here
 * through /dev/mem. No-map memory is mapped as device memory: only aligned
 * 32-bit loads, into a copy that is parsed.
 * Exit 1 when the FDT or the property is missing.
 */
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define RESERVED	"/sys/firmware/devicetree/base/reserved-memory/lk-fdt@*/reg"
#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* the carve-out's base and size from the kernel's device tree */
static int region(uint64_t *base, uint64_t *size)
{
	uint8_t reg[16];
	glob_t g;
	int fd, n = 0;

	if (glob(RESERVED, 0, NULL, &g) || !g.gl_pathc)
		return -1;
	fd = open(g.gl_pathv[0], O_RDONLY);
	globfree(&g);
	if (fd < 0)
		return -1;
	n = read(fd, reg, sizeof(reg));
	close(fd);
	if (n != sizeof(reg))
		return -1;
	*base = be64(reg);
	*size = be64(reg + 8);
	return 0;
}

/* aligned 32-bit copy out of device memory */
static void copy32(uint32_t *dst, const volatile uint32_t *src, size_t bytes)
{
	size_t i;

	for (i = 0; i < bytes / 4; i++)
		dst[i] = src[i];
}

int main(void)
{
	uint64_t base, size;
	const volatile uint32_t *map;
	uint8_t *fdt;
	uint32_t total, off_struct, off_strings, p;
	int fd, depth = 0, chosen = 0;

	if (region(&base, &size) || size < 64 || base & 0xfff)
		return 1;
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0)
		return 1;
	map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, base);
	close(fd);
	if (map == MAP_FAILED)
		return 1;
	fdt = malloc(size);
	if (!fdt)
		return 1;
	copy32((uint32_t *)fdt, map, 64);
	total = be32(fdt + 4);
	if (be32(fdt) != FDT_MAGIC || total > size || total < 64)
		return 1;
	copy32((uint32_t *)fdt, map, (total + 3) & ~3u);
	munmap((void *)map, size);
	off_struct = be32(fdt + 8);
	off_strings = be32(fdt + 12);

	for (p = off_struct; p + 4 <= total;) {
		uint32_t tok = be32(fdt + p);

		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)fdt + p;
			size_t len = strnlen(name, total - p);

			depth++;
			if (depth == 2)
				chosen = !strcmp(name, "chosen");
			p = (p + len + 4) & ~3u;
			break;
		}
		case FDT_END_NODE:
			if (depth-- == 2)
				chosen = 0;
			break;
		case FDT_PROP: {
			uint32_t len, nameoff;
			const uint8_t *v;

			if (p + 8 > total)
				return 1;
			len = be32(fdt + p);
			nameoff = be32(fdt + p + 4);
			v = fdt + p + 8;
			p = (p + 8 + len + 3) & ~3u;
			if (p > total || off_strings + nameoff >= total)
				return 1;
			if (chosen && depth == 2 && len >= 12 &&
			    !strcmp((const char *)fdt + off_strings + nameoff, "atag,boot")) {
				printf("%u\n", v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24);
				return 0;
			}
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 1;
		}
	}
	return 1;
}
