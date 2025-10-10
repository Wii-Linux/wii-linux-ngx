// SPDX-License-Identifier: GPL-2.0+
/*
 * arch/powerpc/platforms/embedded6xx/hlwd-ipc.c
 *
 * Nintendo Wii "Hollywood" IPC support.
 * Copyright (C) 2025-2026 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
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
static struct hlwd_ipc *ipc = NULL;

/*
 * Used by the MINI and IOS implementations
 * to get the IPC state
 */
struct hlwd_ipc *ipc_get_state(void)
{
	/* TODO: lock it? */
	return ipc;
}
EXPORT_SYMBOL_GPL(ipc_get_state);

/*
 * Get the IPC flavor
 */
enum ipc_flavor ipc_get_flavor(void)
{
	if (ipc)
		return ipc->flavor;
	else
		return IPC_FLAVOR_UNKNOWN;
}
EXPORT_SYMBOL_GPL(ipc_get_flavor);


/*
 * Hollywood IPC Initialization and Cleanup
 */
static int ipc_probe(struct platform_device *odev)
{
	int error = -ENOMEM, irq;
	struct hlwd_ipc *new_ipc;
	void __iomem *io_base;

	io_base = of_iomap(odev->dev.of_node, 0);
	if (!io_base) {
		pr_err("no io memory range found (%d)\n", error);
		goto err_iomap;
	}
	irq = irq_of_parse_and_map(odev->dev.of_node, 0);

	pr_info("hlwd-ipc: got address: %p, IRQ %d\n", io_base, irq);

	new_ipc = kzalloc_obj(*new_ipc);
	if (!new_ipc)
		goto err_ipc_alloc;

	/* TODO: do actual detection */
	new_ipc->flavor = IPC_FLAVOR_MINI;
	new_ipc->regs = io_base;
	ipc = new_ipc;

	error = ipc_init_mini(new_ipc);
	if (error)
		goto err_flavor_init;

	platform_set_drvdata(odev, new_ipc);
	return 0;

err_flavor_init:
	ipc = NULL;
	ipc_cleanup_mini(new_ipc);
	kfree(new_ipc);

err_ipc_alloc:
	iounmap(io_base);
err_iomap:
	return error;
}

static void ipc_remove(struct platform_device *odev)
{
	struct hlwd_ipc *old_ipc = platform_get_drvdata(odev);

	if (!old_ipc)
		return;

	ipc = NULL;
	platform_set_drvdata(odev, NULL);
	ipc_cleanup_mini(old_ipc);
	iounmap(old_ipc->regs);
	kfree(old_ipc);
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
	.remove = ipc_remove
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
