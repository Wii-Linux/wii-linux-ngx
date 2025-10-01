// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/powerpc/boot/wii.c
 *
 * Nintendo Wii bootwrapper support
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 */

#include <stddef.h>
#include "stdio.h"
#include "types.h"
#include "io.h"
#include "ops.h"

#include "ugecon.h"

BSS_STACK(8192);

#define HW_REG(x)		((void *)(x))

#define EXI_CTRL		HW_REG(0x0d800070)
#define EXI_CTRL_ENABLE		(1<<0)

#define MEM1_TOP		(24*1024*1024)
#define XFB_RESERVED_SIZE	(1*1024*1024)

#define MEM2_TOP		(0x10000000 + 64*1024*1024)
#define FIRMWARE_DEFAULT_SIZE	(12*1024*1024)

#define VI_DCR		0x02 /* u16 */
#define VI_DCR_SCAN		(0x1<<2)
#define VI_DCR_SCAN_INTERLACED		(0<<2)
#define VI_DCR_SCAN_PROGRESSIVE		(1<<2)
#define VI_TFBL		0x1c /* u32 */
#define VI_BFBL		0x24 /* u32 */
#define SCREEN_WIDTH		640
#define COLOR_BLACK		0x10801080 /* YUYV */

struct mipc_infohdr {
	char magic[3];
	u8 version;
	u32 mem2_boundary;
	u32 ipc_in;
	size_t ipc_in_size;
	u32 ipc_out;
	size_t ipc_out_size;
};

static int mipc_check_address(u32 pa)
{
	/* only MEM2 addresses */
	if (pa < 0x10000000 || pa > 0x14000000)
		return -EINVAL;
	return 0;
}

static struct mipc_infohdr *mipc_get_infohdr(void)
{
	struct mipc_infohdr **hdrp, *hdr;

	/* 'mini' header pointer is the last word of MEM2 memory */
	hdrp = (struct mipc_infohdr **)0x13fffffc;
	if (mipc_check_address((u32)hdrp)) {
		printf("mini: invalid hdrp %08X\n", (u32)hdrp);
		hdr = NULL;
		goto out;
	}

	hdr = *hdrp;
	if (mipc_check_address((u32)hdr)) {
		printf("mini: invalid hdr %08X\n", (u32)hdr);
		hdr = NULL;
		goto out;
	}
	if (memcmp(hdr->magic, "IPC", 3)) {
		printf("mini: invalid magic\n");
		hdr = NULL;
		goto out;
	}

out:
	return hdr;
}

static int mipc_get_mem2_boundary(u32 *mem2_boundary)
{
	struct mipc_infohdr *hdr;
	int error;

	hdr = mipc_get_infohdr();
	if (!hdr) {
		error = -1;
		goto out;
	}

	if (mipc_check_address(hdr->mem2_boundary)) {
		printf("mini: invalid mem2_boundary %08X\n",
		       hdr->mem2_boundary);
		error = -EINVAL;
		goto out;
	}
	*mem2_boundary = hdr->mem2_boundary;
	error = 0;
out:
	return error;

}

/* Size of the heap without overlapping the xfb or the firmware. */
static u32 mem_heapsize(void) {
	u32 bottom = (u32)_end;
	u32 top = (bottom < MEM1_TOP ?
		MEM1_TOP - XFB_RESERVED_SIZE :
		MEM2_TOP - FIRMWARE_DEFAULT_SIZE);
	return (top > bottom ? top - bottom : 0);
}

/* Check if the memory ranges overlap. */
static bool mem_overlaps(u32 addr1, u32 size1, u32 addr2, u32 size2)
{
	return (addr1 > addr2 ?
		addr1 - addr2 < size2 :
		addr2 - addr1 < size1);
}

static void vi_fixups(void)
{
	void *vi;
	void *io_base;
	void *xfb_base;
	u32 reg[2];
	u32 xfb_start;
	u32 xfb_size;
	u32 offset;
	int len;

	vi = find_node_by_compatible(NULL, "nintendo,hollywood-vi");
	if (!vi)
		return;

	len = getprop(vi, "reg", reg, sizeof(reg));
	if (len != sizeof(reg))
		return;

	len = getprop(vi, "xfb-start", &xfb_start, sizeof(xfb_start));
	if (len != sizeof(xfb_start))
		return;

	len = getprop(vi, "xfb-size", &xfb_size, sizeof(xfb_size));
	if (len != sizeof(xfb_size))
		return;

	if ((xfb_start & 0x1f) != 0 || (xfb_size & 0x1f) != 0)
		printf("xfb is not 32 byte aligned!\n");
	if (mem_overlaps(xfb_start, xfb_size, (u32)_end, mem_heapsize()))
		printf("xfb overlaps the heap!\n");

	/* clear xfb */
	xfb_base = (void *)xfb_start;
	for (offset = xfb_start & 0x3; offset + 4 <= xfb_size; offset += 4)
		out_be32(xfb_base + offset, COLOR_BLACK);

	/* update the framebuffer address */
	io_base = (void *)reg[0];
	switch (in_be16(io_base + VI_DCR) & VI_DCR_SCAN) {
		case VI_DCR_SCAN_INTERLACED:
			out_be32(io_base + VI_TFBL, 0x10000000 | (xfb_start >> 5));
			xfb_start += 2 * SCREEN_WIDTH;	/* line length */
			out_be32(io_base + VI_BFBL, 0x10000000 | (xfb_start >> 5));
			break;
		case VI_DCR_SCAN_PROGRESSIVE:
			out_be32(io_base + VI_TFBL, 0x10000000 | (xfb_start >> 5));
			break;
	}
	printf("xfb @ %08X\n", xfb_start);
}

static void platform_fixups(void)
{
	void *mem;
	u32 reg[4];
	u32 mem2_boundary;
	int len;
	int error;

	mem = finddevice("/memory");
	if (!mem)
		fatal("Can't find memory node\n");

	/* two ranges of (address, size) words */
	len = getprop(mem, "reg", reg, sizeof(reg));
	if (len != sizeof(reg)) {
		/* nothing to do */
		goto out;
	}

	/* retrieve MEM2 boundary from 'mini' */
	error = mipc_get_mem2_boundary(&mem2_boundary);
	if (error) {
		/* if that fails use a sane value */
		mem2_boundary = MEM2_TOP - FIRMWARE_DEFAULT_SIZE;
	}

	if (mem2_boundary > reg[2] && mem2_boundary < reg[2] + reg[3]) {
		reg[3] = mem2_boundary - reg[2];
		printf("top of MEM2 @ %08X\n", reg[2] + reg[3]);
		setprop(mem, "reg", reg, sizeof(reg));
	}

out:
	vi_fixups();
	return;
}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	u32 heapsize = mem_heapsize();

	if (!heapsize)
		fatal("no heap\n");

	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init(_dtb_start);

	/*
	 * 'mini' boots the Broadway processor with EXI disabled.
	 * We need it enabled before probing for the USB Gecko.
	 */
	out_be32(EXI_CTRL, in_be32(EXI_CTRL) | EXI_CTRL_ENABLE);

	if (ug_probe())
		console_ops.write = ug_console_write;

	platform_ops.fixups = platform_fixups;
}

