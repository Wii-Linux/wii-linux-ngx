// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * This file does the necessary interface mapping between the bootwrapper
 * device tree operations and the interface provided by shared source
 * files flatdevicetree.[ch].
 *
 * Copyright 2007 David Gibson, IBM Corporation.
 */

#include <stddef.h>
#include <stdio.h>
#include <page.h>
#include <libfdt.h>
#include "ops.h"

#define DEBUG	0
#define BAD_ERROR(err)	(((err) < 0) \
			 && ((err) != -FDT_ERR_NOTFOUND) \
			 && ((err) != -FDT_ERR_EXISTS))

#define check_err(err) \
	({ \
		if (BAD_ERROR(err) || ((err < 0) && DEBUG)) \
			printf("%s():%d  %s\n\r", __func__, __LINE__, \
			       fdt_strerror(err)); \
		if (BAD_ERROR(err)) \
			exit(); \
		(err < 0) ? -1 : 0; \
	})

#define offset_devp(off)	\
	({ \
		unsigned long _offset = (off); \
		check_err(_offset) ? NULL : (void *)(_offset+1); \
	})

#define devp_offset_find(devp)	(((unsigned long)(devp))-1)
#define devp_offset(devp)	(devp ? ((unsigned long)(devp))-1 : 0)

static void *fdt;
static void *buf; /* = NULL */
static fdt32_t loader_initrd_start;
static fdt32_t loader_initrd_end;

#define EXPAND_GRANULARITY	1024

static void expand_buf(int minexpand)
{
	int size = fdt_totalsize(fdt);
	int rc;

	size = _ALIGN(size + minexpand, EXPAND_GRANULARITY);
	buf = platform_ops.realloc(buf, size);
	if (!buf)
		fatal("Couldn't find %d bytes to expand device tree\n\r", size);
	rc = fdt_open_into(fdt, buf, size);
	if (rc != 0)
		fatal("Couldn't expand fdt into new buffer: %s\n\r",
		      fdt_strerror(rc));

	fdt = buf;
}

static void *fdt_wrapper_finddevice(const char *path)
{
	return offset_devp(fdt_path_offset(fdt, path));
}

static int fdt_wrapper_getprop(const void *devp, const char *name,
			       void *buf, const int buflen)
{
	const void *p;
	int len;

	p = fdt_getprop(fdt, devp_offset(devp), name, &len);
	if (!p)
		return check_err(len);
	memcpy(buf, p, min(len, buflen));
	return len;
}

static int fdt_wrapper_setprop(const void *devp, const char *name,
			       const void *buf, const int len)
{
	int rc;

	rc = fdt_setprop(fdt, devp_offset(devp), name, buf, len);
	if (rc == -FDT_ERR_NOSPACE) {
		expand_buf(len + 16);
		rc = fdt_setprop(fdt, devp_offset(devp), name, buf, len);
	}

	return check_err(rc);
}

static int fdt_wrapper_del_node(const void *devp)
{
	return fdt_del_node(fdt, devp_offset(devp));
}

static void *fdt_wrapper_get_parent(const void *devp)
{
	return offset_devp(fdt_parent_offset(fdt, devp_offset(devp)));
}

static void *fdt_wrapper_create_node(const void *devp, const char *name)
{
	int offset;

	offset = fdt_add_subnode(fdt, devp_offset(devp), name);
	if (offset == -FDT_ERR_NOSPACE) {
		expand_buf(strlen(name) + 16);
		offset = fdt_add_subnode(fdt, devp_offset(devp), name);
	}

	return offset_devp(offset);
}

static void *fdt_wrapper_find_node_by_prop_value(const void *prev,
						 const char *name,
						 const char *val,
						 int len)
{
	int offset = fdt_node_offset_by_prop_value(fdt, devp_offset_find(prev),
	                                           name, val, len);
	return offset_devp(offset);
}

static void *fdt_wrapper_find_node_by_compatible(const void *prev,
						 const char *val)
{
	int offset = fdt_node_offset_by_compatible(fdt, devp_offset_find(prev),
	                                           val);
	return offset_devp(offset);
}

static char *fdt_wrapper_get_path(const void *devp, char *buf, int len)
{
	int rc;

	rc = fdt_get_path(fdt, devp_offset(devp), buf, len);
	if (check_err(rc))
		return NULL;
	return buf;
}

static unsigned long fdt_wrapper_finalize(void)
{
	int rc;

	rc = fdt_pack(fdt);
	if (rc != 0)
		fatal("Couldn't pack flat tree: %s\n\r",
		      fdt_strerror(rc));
	return (unsigned long)fdt;
}

static int range_contains(const struct fdt_mapped_range *range,
			  unsigned long addr, unsigned long size)
{
	unsigned long offset;

	if (addr < range->start)
		return 0;

	offset = addr - range->start;
	return offset < range->size && size <= range->size - offset;
}

static int mapped_fdt_range(const struct fdt_mapped_range *ranges,
			    unsigned int nranges, unsigned long addr,
			    unsigned long size)
{
	unsigned int i;

	for (i = 0; i < nranges; i++)
		if (range_contains(&ranges[i], addr, size))
			return 1;

	return 0;
}

static int valid_loader_fdt(unsigned long r3, unsigned long r4,
			    unsigned long r5,
			    const struct fdt_mapped_range *ranges,
			    unsigned int nranges)
{
	const void *loader_fdt = (const void *)r3;

	/*
	 * The 32-bit PowerPC direct boot protocol passes the FDT in r3, the
	 * kernel physical start in r4, and zero in r5.  r4 is intentionally
	 * unconstrained since a wrapper may be loaded independently of the
	 * kernel image it contains.
	 */
	(void)r4;
	if (!r3 || r5 || (r3 & 7))
		return 0;

	/* Do not dereference a header unless it is wholly in mapped RAM. */
	if (!mapped_fdt_range(ranges, nranges, r3,
			      sizeof(struct fdt_header)))
		return 0;

	if (fdt_check_header(loader_fdt))
		return 0;

	return mapped_fdt_range(ranges, nranges, r3,
				fdt_totalsize(loader_fdt));
}

static int save_loader_initrd(const void *loader_fdt)
{
	const fdt32_t *prop;
	u64 start = 0, end = 0;
	int chosen, len, i;

	chosen = fdt_path_offset(loader_fdt, "/chosen");
	if (chosen < 0)
		return 0;

	prop = fdt_getprop(loader_fdt, chosen, "linux,initrd-start", &len);
	if (!prop || len <= 0 || len > sizeof(start) || len % sizeof(*prop))
		return 0;
	for (i = 0; i < len / sizeof(*prop); i++)
		start = (start << 32) | fdt32_to_cpu(prop[i]);

	prop = fdt_getprop(loader_fdt, chosen, "linux,initrd-end", &len);
	if (!prop || len <= 0 || len > sizeof(end) || len % sizeof(*prop))
		return 0;
	for (i = 0; i < len / sizeof(*prop); i++)
		end = (end << 32) | fdt32_to_cpu(prop[i]);

	if (start >= end || start > 0xffffffff || end > 0xffffffff)
		return 0;

	/*
	 * The direct boot protocol used here is 32-bit.  Saving these as
	 * single cells also lets the kentry hook restore them in place after
	 * main.c replaces them with the wrapper's attached initramfs.
	 */
	loader_initrd_start = cpu_to_fdt32(start);
	loader_initrd_end = cpu_to_fdt32(end);
	return 1;
}

static void loader_fdt_kentry(unsigned long fdt_addr, void *vmlinux_addr)
{
	void *kernel_fdt = (void *)fdt_addr;
	int chosen, err;

	/*
	 * main.c gives an attached initramfs priority over loader-provided
	 * data.  Restore the loader's properties immediately before entering
	 * the kernel when the loader-provided initramfs is preferred.
	 */
	chosen = fdt_path_offset(kernel_fdt, "/chosen");
	if (chosen < 0)
		fatal("Can't find /chosen in loader device tree\n");

	err = fdt_setprop_inplace(kernel_fdt, chosen, "linux,initrd-start",
				  &loader_initrd_start,
				  sizeof(loader_initrd_start));
	if (err)
		fatal("Can't restore linux,initrd-start: %s\n",
		      fdt_strerror(err));

	err = fdt_setprop_inplace(kernel_fdt, chosen, "linux,initrd-end",
				  &loader_initrd_end,
				  sizeof(loader_initrd_end));
	if (err)
		fatal("Can't restore linux,initrd-end: %s\n",
		      fdt_strerror(err));

	flush_cache(kernel_fdt, fdt_totalsize(kernel_fdt));
	((kernel_entry_t)vmlinux_addr)(fdt_addr, 0, NULL);
}

void fdt_init(void *blob)
{
	int err;
	int bufsize;

	dt_ops.finddevice = fdt_wrapper_finddevice;
	dt_ops.getprop = fdt_wrapper_getprop;
	dt_ops.setprop = fdt_wrapper_setprop;
	dt_ops.get_parent = fdt_wrapper_get_parent;
	dt_ops.create_node = fdt_wrapper_create_node;
	dt_ops.find_node_by_prop_value = fdt_wrapper_find_node_by_prop_value;
	dt_ops.find_node_by_compatible = fdt_wrapper_find_node_by_compatible;
	dt_ops.del_node = fdt_wrapper_del_node;
	dt_ops.get_path = fdt_wrapper_get_path;
	dt_ops.finalize = fdt_wrapper_finalize;

	/* Make sure the dt blob is the right version and so forth */
	fdt = blob;
	bufsize = fdt_totalsize(fdt) + EXPAND_GRANULARITY;
	buf = malloc(bufsize);
	if(!buf)
		fatal("malloc failed. can't relocate the device tree\n\r");

	err = fdt_open_into(fdt, buf, bufsize);

	if (err != 0)
		fatal("fdt_init(): %s\n\r", fdt_strerror(err));

	fdt = buf;
}

void fdt_init_from_loader(unsigned long r3, unsigned long r4,
			  unsigned long r5,
			  const struct fdt_mapped_range *ranges,
			  unsigned int nranges)
{
	void *loader_fdt = _dtb_start;

	if (valid_loader_fdt(r3, r4, r5, ranges, nranges)) {
		loader_fdt = (void *)r3;
		if (&_initrd_end > &_initrd_start &&
		    save_loader_initrd(loader_fdt))
			platform_ops.kentry = loader_fdt_kentry;
	}

	fdt_init(loader_fdt);
}
