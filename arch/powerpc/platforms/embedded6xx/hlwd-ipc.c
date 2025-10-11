/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * arch/powerpc/platforms/embedded6xx/hlwd-ipc.c
 *
 * Nintendo Wii "Hollywood" IPC support.
 * Copyright (C) 2025 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
 *
 * Based in part on arch/powerpc/platforms/embedded6xx/starlet-ipc.c:
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 */

#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <asm/hlwd-ipc.h>
#include <asm/hlwd-ipc-mini.h>

#define DRV_MODULE_NAME "hlwd-ipc"
#define DRV_AUTHOR "Michael \"Techflash\" Garofalo <officialTechflashYT@gmail.com>"
#define DRV_DESCRIPTION "Nintendo Wii \"Hollywood\" IPC support"

/*
 * Shared IPC state (used by both MINI and IOS implementations)
 */
static struct hlwd_ipc *ipc;

/*
 * Used by the MINI and IOS implementations
 * to get the IPC state
 */
struct hlwd_ipc *ipc_get_state(void)
{
	/* TODO: lock it? */
	return ipc;
}

/*
 * Get the IPC flavor
 */
enum ipc_flavor ipc_get_flavor(void)
{
	return ipc->flavor;
}


/*
 * Hollywood IPC Initialization and Cleanup
 */
static int ipc_probe(struct platform_device *odev)
{
	struct resource mem;
	int error, irq, io_size;
	void __iomem *io_base;

	error = of_address_to_resource(odev->dev.of_node, 0, &mem);
	if (error) {
		pr_err("no io memory range found (%d)\n", error);
		goto out;
	}
	irq = irq_of_parse_and_map(odev->dev.of_node, 0);

	io_size = mem.end - mem.start + 1;
	pr_info("hlwd-ipc: got address: 0x%08x, size %d, IRQ %d\n", (u32)mem.start, io_size, irq);
	io_base = ioremap(mem.start, io_size);
	if (!io_base)
		return -ENOMEM;
	
	ipc = kzalloc(sizeof(struct hlwd_ipc), GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	/* TODO: do actual detection */
	ipc->flavor = IPC_FLAVOR_MINI;
	ipc->regs = io_base;

	dev_set_drvdata(&odev->dev, ipc);

	error = ipc_init_mini(ipc);
out:
	return error;
}

static void ipc_shutdown(struct platform_device *odev)
{
	return;
}

static void ipc_remove(struct platform_device *odev)
{
	struct hlwd_ipc *ipc;
	ipc = dev_get_drvdata(&odev->dev);

	iounmap(ipc->regs);
	kfree(ipc);
	return;
}

/*
 * DT matches
 */
static struct of_device_id ipc_of_match[] = {
	{ .compatible = "nintendo,hollywood-ipc" },
	{ },
};

MODULE_DEVICE_TABLE(of, ipc_of_match);

static struct platform_driver ipc_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = ipc_of_match,
	},
	.probe = ipc_probe,
	.remove = ipc_remove,
	.shutdown = ipc_shutdown,
};

/*
 * Kernel module interface hooks.
 */
static int __init ipc_init_module(void)
{
	pr_info("%s loading\n", DRV_DESCRIPTION);

	return platform_driver_register(&ipc_of_driver);
}

static void __exit ipc_exit_module(void)
{
	platform_driver_unregister(&ipc_of_driver);
}

module_init(ipc_init_module);
module_exit(ipc_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
