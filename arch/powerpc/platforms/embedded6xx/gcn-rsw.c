/*
 * arch/powerpc/platforms/embedded6xx/gcn-rsw.c
 *
 * Nintendo GameCube/Wii reset switch (RSW) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Stefan Esser
 * Copyright (C) 2004,2005,2008,2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/reboot.h>

/* for flipper hardware registers */
#include "flipper-pic.h"

#define FLIPPER_ICR		0x00
#define FLIPPER_ICR_RSS		(1<<16) /* reset switch state */

#define DRV_MODULE_NAME	"gcn-rsw"
#define DRV_DESCRIPTION	"Nintendo GameCube/Wii Reset SWitch (RSW) driver"
#define DRV_AUTHOR			"Stefan Esser <se@nopiracy.de>, " \
							"Albert Herranz"

static char rsw_driver_version[] = "1.1t";

#define drv_printk(level, format, arg...) \
	printk(level DRV_MODULE_NAME ": " format , ## arg)

struct rsw_drvdata {
	spinlock_t lock;

	void __iomem *io_base;
	unsigned int irq;

	struct device *dev;
};


/*
 * Tells if the reset button is pressed.
 */
static int rsw_is_button_pressed(void __iomem *io_base)
{
	u32 icr = in_be32(io_base + FLIPPER_ICR);

	drv_printk(KERN_INFO, "%x\n", icr);
	return !(icr & FLIPPER_ICR_RSS);
}

/*
 * Handles the interrupt associated to the reset button.
 */
static irqreturn_t rsw_handler(int irq, void *data)
{
	struct rsw_drvdata *drvdata = (struct rsw_drvdata *)data;

	if (!rsw_is_button_pressed(drvdata->io_base)) {
		/* nothing to do */
		return IRQ_HANDLED;
	}

	/* reboot the system */
	ctrl_alt_del();

	return IRQ_HANDLED;
}

/*
 * Setup routines.
 *
 */

static int rsw_init(struct rsw_drvdata *drvdata, struct resource *mem, int irq)
{
	int retval;

	drvdata->io_base = ioremap(mem->start, mem->end - mem->start + 1);
	drvdata->irq = irq;

	retval = request_irq(drvdata->irq, rsw_handler, 0,
			     DRV_MODULE_NAME, drvdata);
	if (retval) {
		drv_printk(KERN_ERR, "request of IRQ %d failed\n",
			   drvdata->irq);
	}
	return retval;
}

static void rsw_exit(struct rsw_drvdata *drvdata)
{
	free_irq(drvdata->irq, drvdata);
	if (drvdata->io_base) {
		iounmap(drvdata->io_base);
		drvdata->io_base = NULL;
	}
}

/*
 * Driver model helper routines.
 *
 */

static int rsw_do_probe(struct device *dev, struct resource *mem, int irq)
{
	struct rsw_drvdata *drvdata;
	int retval;

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		drv_printk(KERN_ERR, "failed to allocate rsw_drvdata\n");
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;

	retval = rsw_init(drvdata, mem, irq);
	if (retval) {
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
	}
	return retval;
}

static int rsw_do_remove(struct device *dev)
{
	struct rsw_drvdata *drvdata = dev_get_drvdata(dev);

	if (drvdata) {
		rsw_exit(drvdata);
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
		return 0;
	}
	return -ENODEV;
}

/*
 * OF platform driver hooks.
 *
 */

static int __init rsw_of_probe(struct platform_device *odev)

{
	struct resource mem;
	int retval;
	int irq;

	retval = of_address_to_resource(odev->dev.of_node, 0, &mem);
	if (retval) {
		drv_printk(KERN_ERR, "no io memory range found\n");
		return -ENODEV;
	}

	irq = irq_of_parse_and_map(odev->dev.of_node, 0);
	if (!irq) {
		drv_printk(KERN_ERR, "no irq found\n");
		return -ENODEV;
	}

	return rsw_do_probe(&odev->dev, &mem, irq);
}

static void __exit rsw_of_remove(struct platform_device *odev)
{
	rsw_do_remove(&odev->dev);
}

static struct of_device_id rsw_of_match[] = {
	{.compatible = "nintendo,flipper-pi"},
	{.compatible = "nintendo,hollywood-pi"},
	{},
};

MODULE_DEVICE_TABLE(of, rsw_of_match);

static struct platform_driver rsw_of_driver __refdata = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = rsw_of_match,
	},
	.probe = rsw_of_probe,
	.remove = rsw_of_remove,
};

/*
 * Kernel module hooks.
 *
 */

static int __init rsw_init_module(void)
{
	drv_printk(KERN_INFO, "%s - version %s\n", DRV_DESCRIPTION,
		   rsw_driver_version);

	return platform_driver_register(&rsw_of_driver);
}

static void __exit rsw_exit_module(void)
{
	platform_driver_unregister(&rsw_of_driver);
}

module_init(rsw_init_module);
module_exit(rsw_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
