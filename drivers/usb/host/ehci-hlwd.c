/*
 * drivers/usb/host/ehci-hlwd.c
 *
 * Nintendo Wii (Hollywood) USB Enhanced Host Controller Interface.
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * Based on ehci-ppc-of.c
 *
 * EHCI HCD (Host Controller Driver) for USB.
 *
 * Bus Glue for PPC On-Chip EHCI driver on the of_platform bus
 * Tested on AMCC PPC 440EPx
 *
 * Valentine Barshak <vbarshak@ru.mvista.com>
 *
 * Based on "ehci-ppc-soc.c" by Stefan Roese <sr@denx.de>
 * and "ohci-ppc-of.c" by Sylvain Munaut <tnt@246tNt.com>
 *
 * This file is licenced under the GPL.
 */

#include <linux/signal.h>

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>

#define DRV_MODULE_NAME "ehci-hlwd"
#define DRV_DESCRIPTION "Nintendo Wii EHCI Host Controller"
#define DRV_AUTHOR      "Albert Herranz"

#define HLWD_EHCI_CTL		0x0d0400cc
#define HLWD_EHCI_CTL_INTE	(1<<15)


/* called during probe() after chip reset completes */
static int ehci_hlwd_reset(struct usb_hcd *hcd)
{
	struct ehci_hcd	*ehci = hcd_to_ehci(hcd);
	void __iomem *ehci_ctl;
	int error;

	dbg_hcs_params(ehci, "reset");
	dbg_hcc_params(ehci, "reset");

	error = ehci_setup(hcd);
	if (error)
		goto out;

	ehci_ctl = ioremap(HLWD_EHCI_CTL, 4);
	if (!ehci_ctl) {
		printk(KERN_ERR __FILE__ ": ioremap failed\n");
		error = -EBUSY;
		goto out;
	}

	/* enable notification of EHCI interrupts */
	out_be32(ehci_ctl, in_be32(ehci_ctl) | HLWD_EHCI_CTL_INTE);
	iounmap(ehci_ctl);

	ehci->sbrn = 0x20;
	error = ehci_reset(ehci);
out:
	return error;
}

static const struct hc_driver ehci_hlwd_hc_driver = {
	.description		= hcd_name,
	.product_desc		= "Nintendo Wii EHCI Host Controller",
	.hcd_priv_size		= sizeof(struct ehci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq			= ehci_irq,
	.flags			= HCD_USB2 | HCD_NO_COHERENT_MEM,

	/*
	 * basic lifecycle operations
	 */
	.reset			= ehci_hlwd_reset,
	.start			= ehci_run,
	.stop			= ehci_stop,
	.shutdown		= ehci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue		= ehci_urb_enqueue,
	.urb_dequeue		= ehci_urb_dequeue,
	.endpoint_disable	= ehci_endpoint_disable,
	.endpoint_reset		= ehci_endpoint_reset,

	/*
	 * scheduling support
	 */
	.get_frame_number	= ehci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data	= ehci_hub_status_data,
	.hub_control		= ehci_hub_control,
#ifdef	CONFIG_PM
	.bus_suspend		= ehci_bus_suspend,
	.bus_resume		= ehci_bus_resume,
#endif
	.relinquish_port	= ehci_relinquish_port,
	.port_handed_over	= ehci_port_handed_over,

	.clear_tt_buffer_complete	= ehci_clear_tt_buffer_complete,
};


static int ehci_hcd_hlwd_probe(struct platform_device *op)
{
	struct device *dev = &op->dev;
	struct device_node *dn = dev->of_node;
	struct usb_hcd *hcd;
	struct ehci_hcd	*ehci = NULL;
	struct resource res;
	int irq;
	int error = -ENODEV;

	if (usb_disabled())
		goto out;

	/* big-endian registers (reversed little-endian), little-endian descriptors */
	if (!of_property_read_bool(dn, "big-endian-regs") ||
	    of_property_read_bool(dn, "big-endian-desc") ||
	    of_property_read_bool(dn, "big-endian")) {
		dev_warn(dev, "requires only 'big-endian-regs'\n");
		error = -EINVAL;
		goto out;
	}

	dev_dbg(dev, "initializing " DRV_MODULE_NAME " USB Controller\n");

	error = of_address_to_resource(dn, 0, &res);
	if (error)
		goto out;

	hcd = usb_create_hcd(&ehci_hlwd_hc_driver, dev, DRV_MODULE_NAME);
	if (!hcd) {
		error = -ENOMEM;
		goto out;
	}

	hcd->rsrc_start = res.start;
	hcd->rsrc_len = resource_size(&res);

	error = of_reserved_mem_device_init(dev);
	if (error) {
		/* satisfy coherent memory allocations from mem1 or mem2 */
		dev_warn(&op->dev, "using normal memory\n");
	}

	irq = irq_of_parse_and_map(dn, 0);
	if (irq == NO_IRQ) {
		dev_err(dev, "irq_of_parse_and_map failed\n");
		error = -EBUSY;
		goto err_irq;
	}

	hcd->regs = ioremap(hcd->rsrc_start, hcd->rsrc_len);
	if (!hcd->regs) {
		dev_err(dev, "ioremap failed\n");
		error = -EBUSY;
		goto err_ioremap;
	}

	ehci = hcd_to_ehci(hcd);
	ehci->big_endian_mmio = 1;
	ehci->caps = hcd->regs;
	ehci->regs = hcd->regs +
		HC_LENGTH(ehci, ehci_readl(ehci, &ehci->caps->hc_capbase));

	/* cache this readonly data; minimize chip reads */
	ehci->hcs_params = ehci_readl(ehci, &ehci->caps->hcs_params);

	error = usb_add_hcd(hcd, irq, 0);
	if (error == 0)
		return 0;

	iounmap(hcd->regs);
err_ioremap:
	irq_dispose_mapping(irq);
err_irq:
	of_reserved_mem_device_release(dev);
	usb_put_hcd(hcd);
out:
	return error;
}


static int ehci_hcd_hlwd_remove(struct platform_device *op)
{
	struct device *dev = &op->dev;
	struct usb_hcd *hcd = dev_get_drvdata(dev);

	dev_set_drvdata(dev, NULL);

	dev_dbg(dev, "stopping " DRV_MODULE_NAME " USB Controller\n");

	usb_remove_hcd(hcd);
	iounmap(hcd->regs);
	irq_dispose_mapping(hcd->irq);
	of_reserved_mem_device_release(dev);
	usb_put_hcd(hcd);

	return 0;
}


static void ehci_hcd_hlwd_shutdown(struct platform_device *op)
{
	struct usb_hcd *hcd = dev_get_drvdata(&op->dev);

	if (hcd->driver->shutdown)
		hcd->driver->shutdown(hcd);


}


static struct of_device_id ehci_hcd_hlwd_match[] = {
	{ .compatible = "nintendo,hollywood-usb-ehci", },
	{},
};
MODULE_DEVICE_TABLE(of, ehci_hcd_hlwd_match);

static struct platform_driver ehci_hcd_hlwd_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = ehci_hcd_hlwd_match,
	},
	.probe		= ehci_hcd_hlwd_probe,
	.remove		= ehci_hcd_hlwd_remove,
	.shutdown	= ehci_hcd_hlwd_shutdown,
};
