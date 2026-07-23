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

#define MEM2_TOP		(0x10000000 + 64*1024*1024)
#define FIRMWARE_DEFAULT_SIZE	(12*1024*1024)

#define VI_BASE			HW_REG(0x0c002000)
#define VI_VTR			0x00 /* u16 */
#define VI_VTR_ACV		0x3ff0
#define VI_VTO			0x0c /* u32 */
#define VI_VTE			0x10 /* u32 */

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

/* Size of the heap without overlapping the firmware. */
static u32 mem_heapsize(void) {
	u32 bottom = (u32)_end;
	u32 top = (bottom < MEM1_TOP ?
		MEM1_TOP :
		MEM2_TOP - FIRMWARE_DEFAULT_SIZE);
	return (top > bottom ? top - bottom : 0);
}

/* Blank active video without changing the inherited sync timing. */
static void vi_set_black(void)
{
	u16 vtr = in_be16(VI_BASE + VI_VTR);
	u32 acv = (vtr & VI_VTR_ACV) >> 4;
	u32 vto, vte;

	if (!acv)
		return;

	vto = in_be32(VI_BASE + VI_VTO);
	vte = in_be32(VI_BASE + VI_VTE);
	/* Move active half-lines into blanking, as VIDEO_SetBlack() does. */
	out_be16(VI_BASE + VI_VTR, vtr & ~VI_VTR_ACV);
	out_be32(VI_BASE + VI_VTO, vto + (2 << 16) + 2 * acv - 2);
	out_be32(VI_BASE + VI_VTE, vte + (2 << 16) + 2 * acv - 2);
}

static void platform_fixups(void)
{
	void *mem;
	u32 reg[6];
	u32 mem2_boundary;
	int len;
	int error;

	mem = finddevice("/memory");
	if (!mem)
		fatal("Can't find memory node\n");

	/* three ranges of (address, size) words */
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

	/*
	 * The bootloader may already reserve MINI below 64 MiB while exposing
	 * additional MEM2 above it (for example on devkits or Wii U "vWii").
	 * Do not discard that RAM when the complete firmware region is
	 * covered by a reservation.
	 */
	if (mem2_boundary >= 0x10000000 && mem2_boundary < MEM2_TOP &&
	    fdt_range_is_reserved(mem2_boundary, MEM2_TOP - mem2_boundary))
		goto out;

	if (mem2_boundary > reg[4] && mem2_boundary < reg[4] + reg[5]) {
		reg[5] = mem2_boundary - reg[4];
		printf("top of MEM2 @ %08X\n", reg[4] + reg[5]);
		setprop(mem, "reg", reg, sizeof(reg));
	}

out:
	return;
}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	static const struct fdt_mapped_range mapped_ram[] = {
		{ 0, MEM1_TOP },
		{ 0x10000000, MEM2_TOP - 0x10000000 },
	};
	u32 heapsize = mem_heapsize();

	if (!heapsize)
		fatal("no heap\n");

	/* Stop VI from fetching the loader's XFB until our own 'gcnfb' initializes */
	vi_set_black();
	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init_from_loader(r3, r4, r5, mapped_ram, ARRAY_SIZE(mapped_ram));

	/*
	 * 'mini' boots the Broadway processor with EXI disabled.
	 * We need it enabled before probing for the USB Gecko.
	 */
	out_be32(EXI_CTRL, in_be32(EXI_CTRL) | EXI_CTRL_ENABLE);

	if (ug_probe())
		console_ops.write = ug_console_write;

	platform_ops.fixups = platform_fixups;
}
