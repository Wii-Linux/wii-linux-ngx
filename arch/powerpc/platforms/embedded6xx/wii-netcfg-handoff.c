// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii bootloader network configuration handoff
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * The bootloader places an 8 byte magic value followed by the literal
 * contents of /shared2/sys/net/02/config.dat in reserved memory.  Userspace
 * can read the file through /dev/wii-netcfg and explicitly consume it
 * once the configuration has been imported.
 */

#define pr_fmt(fmt) "wii-netcfg-handoff: " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define WII_NETCFG_MAGIC_SIZE	8
#define WII_NETCFG_SIZE		0x1b5c
#define WII_NETCFG_HANDOFF_SIZE	(WII_NETCFG_SIZE + WII_NETCFG_MAGIC_SIZE)

static_assert(WII_NETCFG_HANDOFF_SIZE <= SZ_8K);

static const u8 wii_netcfg_magic[WII_NETCFG_MAGIC_SIZE] = {
	'W', 'I', 'I', 'N', 'E', 'T', 'C', '\0'
};

static DEFINE_MUTEX(wii_netcfg_lock);
static phys_addr_t wii_netcfg_base;
static size_t wii_netcfg_size;
static void *wii_netcfg_mapping;
static bool wii_netcfg_opened, wii_netcfg_was_read;
static struct miscdevice wii_netcfg_miscdev;

static void wii_netcfg_release_memory(void)
{
	u8 *mapping = wii_netcfg_mapping;
	int error;

	lockdep_assert_held(&wii_netcfg_lock);

	if (!mapping)
		return;

	/* Invalidate the handoff before wiping the remaining contents. */
	memzero_explicit(mapping, WII_NETCFG_MAGIC_SIZE);

	/* Make the invalid magic visible before erasing the payload. */
	wmb();
	memzero_explicit(mapping + WII_NETCFG_MAGIC_SIZE, wii_netcfg_size - WII_NETCFG_MAGIC_SIZE);

	memunmap(mapping);
	wii_netcfg_mapping = NULL;

	/* Free it back to the system, no point int keeping it around. */
	error = memblock_phys_free(wii_netcfg_base, SZ_8K);
	if (error)
		pr_warn("failed to remove reserved-memory range: %d\n", error);

	wii_netcfg_base = 0;
	wii_netcfg_size = 0;
}

static int wii_netcfg_open(struct inode *inode, struct file *file)
{
	int error = 0;

	mutex_lock(&wii_netcfg_lock);
	if (!wii_netcfg_mapping)
		error = -ENODATA;
	else if (wii_netcfg_opened)
		error = -EBUSY;
	else
		wii_netcfg_opened = true;
	mutex_unlock(&wii_netcfg_lock);

	return error;
}

static int wii_netcfg_release(struct inode *inode, struct file *file)
{
	mutex_lock(&wii_netcfg_lock);
	wii_netcfg_opened = false;
	mutex_unlock(&wii_netcfg_lock);

	if (wii_netcfg_was_read) {
		wii_netcfg_release_memory();
		misc_deregister(&wii_netcfg_miscdev);
	}

	return 0;
}

static ssize_t wii_netcfg_read(struct file *file, char __user *buffer,
			       size_t count, loff_t *offset)
{
	ssize_t ret = count;

	mutex_lock(&wii_netcfg_lock);
	if (!wii_netcfg_mapping) {
		ret = -ENODATA;
		goto out;
	}

	if (*offset != 0 || count != WII_NETCFG_SIZE || wii_netcfg_was_read) {
		ret = -EINVAL;
		goto out;
	}

	if (copy_to_user(buffer,
			(u8 *)wii_netcfg_mapping + WII_NETCFG_MAGIC_SIZE,
			WII_NETCFG_SIZE)) {
		ret = -EFAULT;
		goto out;
	}

	wii_netcfg_was_read = true;

out:
	mutex_unlock(&wii_netcfg_lock);
	return ret;
}

static const struct file_operations wii_netcfg_fops = {
	.owner		= THIS_MODULE,
	.open		= wii_netcfg_open,
	.release	= wii_netcfg_release,
	.read		= wii_netcfg_read,
};

static struct miscdevice wii_netcfg_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "wii-netcfg",
	.fops	= &wii_netcfg_fops,
	.mode	= 0600,
};

static int __init wii_netcfg_reserved_mem_init(unsigned long node, struct reserved_mem *rmem)
{
	if (wii_netcfg_size) {
		pr_err("multiple handoff regions specified\n");
		return -EBUSY;
	}

	if (rmem->size != WII_NETCFG_HANDOFF_SIZE ||
	    !IS_ALIGNED(rmem->base, PAGE_SIZE)) {
		pr_err("handoff region must be %lu bytes and page-aligned\n",
		       (unsigned long)WII_NETCFG_HANDOFF_SIZE);
		return -EINVAL;
	}

	wii_netcfg_base = rmem->base;
	wii_netcfg_size = rmem->size;
	return 0;
}

static const struct reserved_mem_ops wii_netcfg_reserved_mem_ops = {
	.node_init = wii_netcfg_reserved_mem_init,
};

RESERVEDMEM_OF_DECLARE(wii_netcfg, "nintendo,wii-netcfg", &wii_netcfg_reserved_mem_ops);

static int __init wii_netcfg_handoff_init(void)
{
	int error;

	if (!wii_netcfg_size)
		return 0;

	wii_netcfg_mapping = memremap(wii_netcfg_base, wii_netcfg_size, MEMREMAP_WB);
	if (!wii_netcfg_mapping) {
		pr_err("failed to map handoff memory\n");
		return -ENOMEM;
	}

	if (memcmp(wii_netcfg_mapping, wii_netcfg_magic, WII_NETCFG_MAGIC_SIZE)) {
		mutex_lock(&wii_netcfg_lock);
		wii_netcfg_release_memory();
		mutex_unlock(&wii_netcfg_lock);
		pr_info("no configuration present; handoff memory released\n");
		return 0;
	}

	error = misc_register(&wii_netcfg_miscdev);
	if (error) {
		pr_err("failed to register misc device: %d\n", error);
		mutex_lock(&wii_netcfg_lock);
		wii_netcfg_release_memory();
		mutex_unlock(&wii_netcfg_lock);
		return error;
	}

	pr_info("configuration available in /dev/%s\n", wii_netcfg_miscdev.name);
	return 0;
}
device_initcall(wii_netcfg_handoff_init);
