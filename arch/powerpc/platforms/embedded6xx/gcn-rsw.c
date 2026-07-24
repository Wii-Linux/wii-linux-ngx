// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/powerpc/platforms/embedded6xx/gcn-rsw.c
 *
 * Nintendo GameCube/Wii reset switch (RSW) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Stefan Esser
 * Copyright (C) 2004,2005,2008,2009 Albert Herranz
 * Copyright (C) 2025,2026 Michael "Techflash" Garofalo
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

#define FLIPPER_ICR		0x00
#define FLIPPER_ICR_RSS		BIT(16) /* reset switch state */

#define DRV_MODULE_NAME	"gcn-rsw"
#define DRV_DESCRIPTION	"Nintendo GameCube/Wii Reset SWitch (RSW) driver"
#define DRV_AUTHOR			"Stefan Esser <se@nopiracy.de>, " \
							"Albert Herranz, " \
					"Michael \"Techflash\" Garofalo"

static char rsw_driver_version[] = "1.2t";

struct rsw_drvdata {
	void __iomem *io_base;
	unsigned int irq;

	struct device *dev;
};


/*
 * Tells if the reset button is pressed.
 */
static int rsw_is_button_pressed(void __iomem *io_base)
{
	return !(in_be32(io_base + FLIPPER_ICR) & FLIPPER_ICR_RSS);
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

static int __init rsw_probe(struct platform_device *odev)

{
	struct device *dev;
	struct rsw_drvdata *drvdata;
	int irq, retval;
	void __iomem *io_base;

	dev = &odev->dev;
	io_base = of_iomap(dev->of_node, 0);
	if (!io_base) {
		dev_err(dev, "no io memory range found\n");
		return -ENODEV;
	}

	irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!irq) {
		dev_err(dev, "no irq found\n");
		iounmap(io_base);
		return -ENODEV;
	}

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		dev_err(dev, "failed to allocate rsw_drvdata\n");
		iounmap(io_base);
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;
	drvdata->io_base = io_base;
	drvdata->irq = irq;

	retval = request_irq(irq, rsw_handler, 0,
			     DRV_MODULE_NAME, drvdata);
	if (retval) {
		dev_err(dev, "request of IRQ %d failed\n", irq);
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
	}

	return retval;
}

static void __exit rsw_remove(struct platform_device *odev)
{
	struct rsw_drvdata *drvdata = dev_get_drvdata(&odev->dev);

	if (!drvdata)
		return;

	free_irq(drvdata->irq, drvdata);
	if (drvdata->io_base) {
		iounmap(drvdata->io_base);
		drvdata->io_base = NULL;
	}
	dev_set_drvdata(&odev->dev, NULL);
	kfree(drvdata);
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
	.probe = rsw_probe,
	.remove = rsw_remove,
};

/*
 * Kernel module hooks.
 *
 */

static int __init rsw_init_module(void)
{
	pr_info("%s - version %s\n", DRV_DESCRIPTION, rsw_driver_version);

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
