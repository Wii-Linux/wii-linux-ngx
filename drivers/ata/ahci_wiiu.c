/*
 * AHCI SATA platform driver
 *
 * Copyright 2004-2005  Red Hat, Inc.
 *   Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2010  MontaVista Software, LLC.
 *   Anton Vorontsov <avorontsov@ru.mvista.com>
 * Copyright 2021-2025 linux-wiiu Contributors
 *   Ash Logan <ash@heyquark.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/libata.h>
#include <linux/ahci_platform.h>
#include <linux/pci_ids.h>
#include "ahci.h"

//wiiu-specific vendor registers
#define HOST_VND_IRQ_STAT 0x400
#define HOST_VND_IRQ_MASK 0x404

#define VND_IRQ_PORT0 0x08
#define VND_IRQ_PORT1 0x20
//other vendor irqs: 0x01 (disc inserted?) 0x02 (unknown)

#define DRV_NAME "wiiu-ahci"

static const struct ata_port_info ahci_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_platform_ops,
};

static const struct ata_port_info ahci_port_info_nolpm = {
	.flags		= AHCI_FLAG_COMMON | ATA_FLAG_NO_LPM,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_platform_ops,
};

static struct scsi_host_template ahci_platform_sht = {
	AHCI_SHT(DRV_NAME),
};

static irqreturn_t wiiu_ahci_irq_intr(int irq, void *dev_instance)
{
	struct ata_host *host = dev_instance;
	struct ahci_host_priv *hpriv;
	unsigned int rc = 0;
	void __iomem *mmio;
	u32 irq_stat, irq_masked;
	u32 vnd_irq_stat, vnd_irq_mask, vnd_irq_masked;

	hpriv = host->private_data;
	mmio = hpriv->mmio;

	vnd_irq_stat = readl(mmio + HOST_VND_IRQ_STAT);
	vnd_irq_mask = readl(mmio + HOST_VND_IRQ_MASK);

	vnd_irq_masked = vnd_irq_stat & vnd_irq_mask;

	/* sigh.  0xffffffff is a valid return from h/w */
	irq_stat = readl(mmio + HOST_IRQ_STAT);
	if (!irq_stat) {
		if (vnd_irq_stat) {
			writel(vnd_irq_stat, mmio + HOST_VND_IRQ_STAT);
		}
		return IRQ_NONE;
	}

	irq_masked = irq_stat & hpriv->port_map;

	spin_lock(&host->lock);

	rc = ahci_handle_port_intr(host, irq_masked);

	/* HOST_IRQ_STAT behaves as level triggered latch meaning that
	 * it should be cleared after all the port events are cleared;
	 * otherwise, it will raise a spurious interrupt after each
	 * valid one.  Please read section 10.6.2 of ahci 1.1 for more
	 * information.
	 *
	 * Also, use the unmasked value to clear interrupt as spurious
	 * pending event on a dummy port might cause screaming IRQ.
	 */
	writel(irq_stat, mmio + HOST_IRQ_STAT);

	//ack vendor
	writel(vnd_irq_stat, mmio + HOST_VND_IRQ_STAT);

	spin_unlock(&host->lock);

	return IRQ_RETVAL(rc);
}

static int ahci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	int rc;

	hpriv = ahci_platform_get_resources(pdev,
					    AHCI_PLATFORM_GET_RESETS);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);

	rc = ahci_platform_enable_resources(hpriv);
	if (rc)
		return rc;

	hpriv->irq_handler = wiiu_ahci_irq_intr;
	writel(0xffffffff, hpriv->mmio + HOST_VND_IRQ_STAT);
	writel(VND_IRQ_PORT0 | VND_IRQ_PORT1, hpriv->mmio + HOST_VND_IRQ_MASK);

	rc = ahci_platform_init_host(pdev, hpriv, &ahci_port_info,
				     &ahci_platform_sht);
	if (rc)
		goto disable_resources;

	dev_err(dev, "hello wiiu ok\n");

	return 0;
disable_resources:
	ahci_platform_disable_resources(hpriv);
	return rc;
}

static SIMPLE_DEV_PM_OPS(ahci_pm_ops, ahci_platform_suspend,
			 ahci_platform_resume);

static const struct of_device_id ahci_of_match[] = {
	{ .compatible = "nintendo,latte-ahci", },
	{},
};
MODULE_DEVICE_TABLE(of, ahci_of_match);

static struct platform_driver ahci_driver = {
	.probe = ahci_probe,
	.remove = ata_platform_remove_one,
	.shutdown = ahci_platform_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = ahci_of_match,
		.pm = &ahci_pm_ops,
	},
};
module_platform_driver(ahci_driver);

MODULE_DESCRIPTION("AHCI SATA Wii U driver");
MODULE_AUTHOR("Ash Logan <ash@heyquark.com>");
MODULE_LICENSE("GPL");
