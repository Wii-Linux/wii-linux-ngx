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
#include <libfdt.h>

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

static fdt32_t loader_initrd_start;
static fdt32_t loader_initrd_end;

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

static bool mem_contains(u32 addr, u32 size, u32 start, u32 end)
{
	return addr >= start && addr < end && size <= end - addr;
}

static bool valid_loader_fdt(unsigned long r3, unsigned long r4,
			     unsigned long r5)
{
	const void *fdt = (const void *)r3;
	u32 size;

	/*
	 * The 32-bit PowerPC direct boot protocol passes the FDT in r3, the
	 * kernel physical start in r4, and zero in r5.  r4 is intentionally
	 * unconstrained here since the wrapper may be loaded independently
	 * of the kernel image it contains.
	 */
	if (!r3 || r5 || (r3 & 7))
		return false;

	/* Do not dereference a header unless it is wholly in mapped RAM. */
	if (!mem_contains(r3, sizeof(struct fdt_header), 0, MEM1_TOP) &&
	    !mem_contains(r3, sizeof(struct fdt_header), 0x10000000, MEM2_TOP))
		return false;

	if (fdt_check_header(fdt))
		return false;

	size = fdt_totalsize(fdt);
	return mem_contains(r3, size, 0, MEM1_TOP) ||
	       mem_contains(r3, size, 0x10000000, MEM2_TOP);
}

static bool save_loader_initrd(const void *fdt)
{
	const fdt32_t *prop;
	u64 start = 0, end = 0;
	int chosen, len, i;

	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return false;

	prop = fdt_getprop(fdt, chosen, "linux,initrd-start", &len);
	if (!prop || len <= 0 || len > sizeof(start) || len % sizeof(*prop))
		return false;
	for (i = 0; i < len / sizeof(*prop); i++)
		start = (start << 32) | fdt32_to_cpu(prop[i]);

	prop = fdt_getprop(fdt, chosen, "linux,initrd-end", &len);
	if (!prop || len <= 0 || len > sizeof(end) || len % sizeof(*prop))
		return false;
	for (i = 0; i < len / sizeof(*prop); i++)
		end = (end << 32) | fdt32_to_cpu(prop[i]);

	if (start >= end || start > 0xffffffff || end > 0xffffffff)
		return false;

	/*
	 * Wii RAM is 32-bit addressed.  Saving these as single cells also
	 * lets wii_kentry restore them in-place after the tree is packed.
	 */
	loader_initrd_start = cpu_to_fdt32(start);
	loader_initrd_end = cpu_to_fdt32(end);
	return true;
}

static void wii_kentry(unsigned long fdt_addr, void *vmlinux_addr)
{
	void *fdt = (void *)fdt_addr;
	int chosen, err;

	/*
	 * main.c gives an attached initramfs priority over loader-provided
	 * data.  Restore the bootloader's properties immediately before
	 * entering the kernel, as the attached initramfs is not preferred here.
	 */
	chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		fatal("Can't find /chosen in loader device tree\n");

	err = fdt_setprop_inplace(fdt, chosen, "linux,initrd-start",
				  &loader_initrd_start,
				  sizeof(loader_initrd_start));
	if (err)
		fatal("Can't restore linux,initrd-start: %s\n",
		      fdt_strerror(err));

	err = fdt_setprop_inplace(fdt, chosen, "linux,initrd-end",
				  &loader_initrd_end,
				  sizeof(loader_initrd_end));
	if (err)
		fatal("Can't restore linux,initrd-end: %s\n",
		      fdt_strerror(err));

	flush_cache(fdt, fdt_totalsize(fdt));
	((kernel_entry_t)vmlinux_addr)(fdt_addr, 0, NULL);
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
	u32 reg[6];
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

	if (mem2_boundary > reg[4] && mem2_boundary < reg[4] + reg[5]) {
		reg[5] = mem2_boundary - reg[4];
		printf("top of MEM2 @ %08X\n", reg[4] + reg[5]);
		setprop(mem, "reg", reg, sizeof(reg));
	}

out:
	vi_fixups();
	return;
}

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	u32 heapsize = mem_heapsize();
	void *fdt = _dtb_start;

	if (!heapsize)
		fatal("no heap\n");

	simple_alloc_init(_end, heapsize, 32, 64);

	if (valid_loader_fdt(r3, r4, r5)) {
		fdt = (void *)r3;
		if (&_initrd_end > &_initrd_start && save_loader_initrd(fdt))
			platform_ops.kentry = wii_kentry;
	}
	fdt_init(fdt);

	/*
	 * 'mini' boots the Broadway processor with EXI disabled.
	 * We need it enabled before probing for the USB Gecko.
	 */
	out_be32(EXI_CTRL, in_be32(EXI_CTRL) | EXI_CTRL_ENABLE);

	if (ug_probe())
		console_ops.write = ug_console_write;

	platform_ops.fixups = platform_fixups;
}

