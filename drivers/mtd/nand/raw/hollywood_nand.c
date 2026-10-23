// SPDX-License-Identifier: GPL-2.0-only
/*
 * ATI/Nintendo Hollywood/Latte NAND Interface
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on code from NPLL:
 *   Copyright (C) 2026 Techflash
 *
 *   Mainly based on MINI's NAND code:
 *     Copyright (C) 2008, 2009	Haxx Enterprises <bushing@gmail.com>
 *     Copyright (C) 2008, 2009	Sven Peter <svenpeter@gmail.com>
 *     Copyright (C) 2008, 2009	Hector Martin "marcan" <marcan@marcansoft.com>
 *
 *   Wii U specifics based on linux-loader's NAND code:
 *     Copyright (C) 2021          rw-r-r-0644
 *     Copyright (C) 2016          SALT
 *     Copyright (C) 2016          Daz Jones <daz@dazzozo.com>
 *
 *     Copyright (C) 2008, 2009    Haxx Enterprises <bushing@gmail.com>
 *     Copyright (C) 2008, 2009    Sven Peter <svenpeter@gmail.com>
 *     Copyright (C) 2008, 2009    Hector Martin "marcan" <marcan@marcansoft.com>
 */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/rawnand.h>
#include <linux/nintendo-nand-ecc.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>

#define NAND_CTRL      0x00
#define NAND_CTRL_EXEC         BIT(31)
#define NAND_CTRL_IRQ          BIT(30)
#define NAND_CTRL_ERR          BIT(29)
#define NAND_CTRL_WAIT         BIT(15)
#define NAND_CTRL_WR           BIT(14)
#define NAND_CTRL_RD           BIT(13)
#define NAND_CTRL_ECC          BIT(12)
#define NAND_CONFIG    0x04
#define NAND_CONFIG_LATTE_WP   BIT(31)
#define NAND_CONFIG_ENABLE     BIT(27)
#define NAND_CONFIG_LATTE_INIT 0x743e3eff
#define NAND_CONFIG_LATTE_NORM 0x5d0f1eff
#define NAND_CONFIG_HLWD       0x4b3e0e7f
#define NAND_ADDR1     0x08
#define NAND_ADDR2     0x0c
#define NAND_DATA      0x10
#define NAND_ECC       0x14
#define NAND_BANK      0x18
#define NAND_BANK_CTRL 0x30

#define PAGE_BYTES 2048
#define OOB_BYTES 64
#define BANK_BYTES SZ_512M

static bool write_enable;
module_param(write_enable, bool, 0400);
MODULE_PARM_DESC(write_enable, "Enable NAND writes including ALL boot regions (default false)");

struct hlwd_nand;
struct hlwd_nand_chip {
	struct nand_chip chip;
	struct hlwd_nand *ctrl;
	u32 bank;
};

struct hlwd_nand {
	struct nand_controller controller;
	struct device *dev;
	void __iomem *regs;
	struct completion done;
	u32 status;
	bool latte;
	bool failed;
	u8 *data;
	dma_addr_t data_dma;
	u8 *spare;
	dma_addr_t spare_dma;
	struct hlwd_nand_chip chips[2];
	unsigned int nchips;
};

static u32 hlwd_nand_read(struct hlwd_nand *h, unsigned int reg)
{
	return ioread32be(h->regs + reg);
}

static void hlwd_nand_write(struct hlwd_nand *h, unsigned int reg, u32 val)
{
	iowrite32be(val, h->regs + reg);
}

static void hlwd_nand_select(struct hlwd_nand_chip *c, bool writable)
{
	struct hlwd_nand *h = c->ctrl;

	if (h->latte) {
		hlwd_nand_write(h, NAND_CTRL, 0);
		hlwd_nand_write(h, NAND_CONFIG, 0);
		hlwd_nand_write(h, NAND_CONFIG, NAND_CONFIG_LATTE_NORM | (writable ? 0 : NAND_CONFIG_LATTE_WP));
		hlwd_nand_write(h, NAND_BANK, c->bank | BIT(2));
	}
}

static irqreturn_t hlwd_nand_irq(int irq, void *arg)
{
	struct hlwd_nand *h = arg;
	u32 status = hlwd_nand_read(h, NAND_CTRL);

	if (status & NAND_CTRL_EXEC)
		return IRQ_NONE;

	WRITE_ONCE(h->status, status);
	hlwd_nand_write(h, NAND_CTRL, ~NAND_CTRL_EXEC);
	complete(&h->done);
	return IRQ_HANDLED;
}

static int hlwd_nand_command(struct hlwd_nand *h, u8 cmd, u32 addr,
			u32 flags, unsigned int len)
{
	u32 status;
	int ret;

	if (h->failed)
		return -EIO;
	if (hlwd_nand_read(h, NAND_CTRL) & NAND_CTRL_EXEC)
		return -EBUSY;

	hlwd_nand_write(h, NAND_CTRL, ~NAND_CTRL_EXEC);
	hlwd_nand_write(h, NAND_CTRL, 0);
	reinit_completion(&h->done);
	hlwd_nand_write(h, NAND_CTRL, NAND_CTRL_EXEC | NAND_CTRL_IRQ | (addr << 24) |
		   ((u32)cmd << 16) | flags | len);

	if (!wait_for_completion_timeout(&h->done, msecs_to_jiffies(500))) {
		status = hlwd_nand_read(h, NAND_CTRL);
		if (status & NAND_CTRL_EXEC) {
			hlwd_nand_write(h, NAND_CTRL, 0);
			ret = read_poll_timeout(hlwd_nand_read, status, !(status & NAND_CTRL_EXEC),
					       10, 10000, false, h, NAND_CTRL);
			h->failed = true;
			dev_err(h->dev, "NAND timeout; controller disabled (%d)\n", ret);
			return -ETIMEDOUT;
		}
	} else
		status = READ_ONCE(h->status);

	return status & NAND_CTRL_ERR ? -EIO : 0;
}

static void hlwd_nand_dma(struct hlwd_nand *h)
{
	hlwd_nand_write(h, NAND_DATA, lower_32_bits(h->data_dma));
	hlwd_nand_write(h, NAND_ECC, lower_32_bits(h->spare_dma));
}

static int hlwd_nand_read_page_raw(struct nand_chip *chip, u8 *buf,
			      int oob_required, int page)
{
	struct hlwd_nand_chip *c = nand_get_controller_data(chip);
	struct hlwd_nand *h = c->ctrl;
	int ret;

	if (page < 0 || page >= BANK_BYTES / PAGE_BYTES)
		return -EINVAL;

	hlwd_nand_select(c, false);
	hlwd_nand_write(h, NAND_ADDR1, 0);
	hlwd_nand_write(h, NAND_ADDR2, page);
	ret = hlwd_nand_command(h, NAND_CMD_READ0, 0x1f, 0, 0);
	if (ret)
		return ret;
	hlwd_nand_dma(h);
	ret = hlwd_nand_command(h, NAND_CMD_READSTART, 0,
			        NAND_CTRL_WAIT | NAND_CTRL_RD | NAND_CTRL_ECC,
			        PAGE_BYTES + OOB_BYTES);
	if (ret)
		return ret;
	if (buf)
		memcpy(buf, h->data, PAGE_BYTES);
	memcpy(chip->oob_poi, h->spare, OOB_BYTES);
	return 0;
}

static int hlwd_nand_read_page(struct nand_chip *chip, u8 *buf,
			  int oob_required, int page)
{
	struct hlwd_nand_chip *c = nand_get_controller_data(chip);
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret = hlwd_nand_read_page_raw(chip, buf, 1, page);

	if (ret)
		return ret;

	ret = nintendo_nand_ecc_correct(buf, c->ctrl->spare + 48,
					c->ctrl->spare + 64);

	if (ret < 0) {
		mtd->ecc_stats.failed++;
		return 0; /* NAND core converts ecc_stats.failed to -EBADMSG */
	}

	mtd->ecc_stats.corrected += ret;
	return !!ret; /* At most one corrected bit per 512-byte ECC step */
}

static int hlwd_nand_read_oob(struct nand_chip *chip, int page)
{
	return hlwd_nand_read_page_raw(chip, NULL, 1, page);
}

static int hlwd_program(struct nand_chip *chip, const u8 *buf, int page,
			bool raw, bool oob_only)
{
	struct hlwd_nand_chip *c = nand_get_controller_data(chip);
	struct hlwd_nand *h = c->ctrl;
	u8 status;
	int ret;

	if (!write_enable)
		return -EROFS;
	if (page < 0 || page >= BANK_BYTES / PAGE_BYTES)
		return -EINVAL;

	if (oob_only)
		memcpy(h->data, chip->oob_poi, OOB_BYTES);
	else {
		memcpy(h->data, buf, PAGE_BYTES);
		memcpy(h->spare, chip->oob_poi, OOB_BYTES);
		if (!raw)
			nintendo_nand_ecc_calculate(buf, h->spare + 48);
	}

	hlwd_nand_select(c, true);
	hlwd_nand_write(h, NAND_ADDR1, oob_only ? PAGE_BYTES : 0);
	hlwd_nand_write(h, NAND_ADDR2, page);
	hlwd_nand_dma(h);
	ret = hlwd_nand_command(h, NAND_CMD_SEQIN, 0x1f, NAND_CTRL_WR,
			   oob_only ? OOB_BYTES : PAGE_BYTES + OOB_BYTES);

	if (!ret)
		ret = hlwd_nand_command(h, NAND_CMD_PAGEPROG, 0, NAND_CTRL_WAIT, 0);
	if (!ret) {
		/* nand_status_op selects this bank read-only through exec_op */
		ret = nand_status_op(chip, &status);
		if (!ret && (status & NAND_STATUS_FAIL))
			ret = -EIO;
	}
	hlwd_nand_select(c, false);
	return ret;
}

static int hlwd_nand_write_page_raw(struct nand_chip *chip, const u8 *buf,
			       int oob_required, int page)
{
	return hlwd_program(chip, buf, page, true, false);
}

static int hlwd_nand_write_page(struct nand_chip *chip, const u8 *buf,
			   int oob_required, int page)
{
	return hlwd_program(chip, buf, page, false, false);
}

static int hlwd_nand_write_oob(struct nand_chip *chip, int page)
{
	return hlwd_program(chip, NULL, page, true, true);
}

/* Page I/O uses the custom callbacks; only identification/status/erase here */
static int hlwd_nand_exec_op(struct nand_chip *chip, const struct nand_operation *op,
			bool check_only)
{
	struct hlwd_nand_chip *c = nand_get_controller_data(chip);
	struct hlwd_nand *h = c->ctrl;
	const struct nand_op_instr *a = NULL, *d = NULL, *in;
	u8 cmd;
	u32 page = 0;
	unsigned int i;
	int ret;

	if (!op->ninstrs || op->cs || op->instrs[0].type != NAND_OP_CMD_INSTR)
		return -EOPNOTSUPP;
	cmd = op->instrs[0].ctx.cmd.opcode;
	for (i = 1; i < op->ninstrs; i++) {
		in = &op->instrs[i];

		switch (in->type) {
		case NAND_OP_ADDR_INSTR:
			if (a)
				return -EOPNOTSUPP;
			a = in;
			break;
		case NAND_OP_DATA_IN_INSTR:
			if (d || in->ctx.data.len > 64)
				return -EOPNOTSUPP;
			d = in;
			break;
		case NAND_OP_CMD_INSTR:
			if (cmd != NAND_CMD_ERASE1 ||
			    in->ctx.cmd.opcode != NAND_CMD_ERASE2)
				return -EOPNOTSUPP;
			break;
		case NAND_OP_WAITRDY_INSTR:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (cmd == NAND_CMD_READID) {
		if (!a || a->ctx.addr.naddrs != 1)
			return -EOPNOTSUPP;
	} else if (cmd == NAND_CMD_ERASE1) {
		if (!a || a->ctx.addr.naddrs < 2 || a->ctx.addr.naddrs > 3 || d)
			return -EOPNOTSUPP;

		for (i = 0; i < a->ctx.addr.naddrs; i++)
			page |= (u32)a->ctx.addr.addrs[i] << (i * 8);

		if (page >= BANK_BYTES / PAGE_BYTES || (page & 63))
			return -EINVAL;
	} else if (cmd != NAND_CMD_RESET && cmd != NAND_CMD_STATUS)
		return -EOPNOTSUPP;

	if (check_only)
		return 0;
	if (cmd == NAND_CMD_ERASE1 && !write_enable)
		return -EROFS;

	hlwd_nand_select(c, cmd == NAND_CMD_ERASE1);
	if (cmd == NAND_CMD_ERASE1) {
		hlwd_nand_write(h, NAND_ADDR2, page);
		ret = hlwd_nand_command(h, cmd, 0x1c, 0, 0);
		if (!ret)
			ret = hlwd_nand_command(h, NAND_CMD_ERASE2, 0, NAND_CTRL_WAIT, 0);
	} else if (cmd == NAND_CMD_RESET)
		ret = hlwd_nand_command(h, cmd, 0, NAND_CTRL_WAIT, 0);
	else {
		hlwd_nand_write(h, NAND_ADDR1, a ? a->ctx.addr.addrs[0] : 0);
		hlwd_nand_write(h, NAND_ADDR2, 0);
		hlwd_nand_dma(h);
		ret = hlwd_nand_command(h, cmd, a ? 1 : 0, NAND_CTRL_RD, 64);
		if (!ret && d)
			memcpy(d->ctx.data.buf.in, h->data, d->ctx.data.len);
	}

	hlwd_nand_select(c, false);
	return ret;
}

static int hlwd_nand_oob_ecc(struct mtd_info *mtd, int section,
			struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = 48;
	region->length = 16;
	return 0;
}

static int hlwd_nand_oob_free(struct mtd_info *mtd, int section,
			 struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	/* FIXME: ?????????? */
	region->offset = 1; /* Preserve the factory bad-block marker */
	region->length = 47;

	return 0;
}

static const struct mtd_ooblayout_ops hlwd_nand_ooblayout = {
	.ecc = hlwd_nand_oob_ecc,
	.free = hlwd_nand_oob_free,
};

static int hlwd_nand_attach(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);

	if (mtd->writesize != PAGE_BYTES || mtd->oobsize != OOB_BYTES ||
	    mtd->erasesize != PAGE_BYTES * 64 || mtd->size != BANK_BYTES)
		return -ENODEV;

	if (chip->bbt_options & NAND_BBT_USE_FLASH)
		return -EINVAL;

	chip->options |= NAND_NO_SUBPAGE_WRITE;
	chip->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
	chip->ecc.algo = NAND_ECC_ALGO_HAMMING;
	chip->ecc.size = 512;
	chip->ecc.bytes = 4;
	chip->ecc.strength = 1;
	chip->ecc.read_page = hlwd_nand_read_page;
	chip->ecc.read_page_raw = hlwd_nand_read_page_raw;
	chip->ecc.write_page = hlwd_nand_write_page;
	chip->ecc.write_page_raw = hlwd_nand_write_page_raw;
	chip->ecc.read_oob = hlwd_nand_read_oob;
	chip->ecc.read_oob_raw = hlwd_nand_read_oob;
	chip->ecc.write_oob = hlwd_nand_write_oob;
	chip->ecc.write_oob_raw = hlwd_nand_write_oob;
	mtd_set_ooblayout(mtd, &hlwd_nand_ooblayout);

	return 0;
}

static const struct nand_controller_ops hlwd_nand_ops = {
	.exec_op = hlwd_nand_exec_op,
	.attach_chip = hlwd_nand_attach,
};

static int hlwd_nand_init_hw(struct hlwd_nand *h)
{
	u32 status;
	unsigned int i, j;
	int ret;

	if (!h->latte) {
		hlwd_nand_write(h, NAND_CONFIG, NAND_CONFIG_ENABLE);
		hlwd_nand_write(h, NAND_CONFIG, NAND_CONFIG_HLWD);
		return 0;
	}

	/* Latte needs special initialization */
	for (j = 0; j < 2; j++) {
		hlwd_nand_write(h, NAND_BANK_CTRL, 0);
		ret = read_poll_timeout(hlwd_nand_read, status, !(status & NAND_CTRL_EXEC),
				       10, 10000, false, h, NAND_BANK_CTRL);
		if (ret)
			return ret;

		for (i = 0x40; i < 0x100; i += 4)
			hlwd_nand_write(h, i, 0);

		hlwd_nand_write(h, NAND_CTRL, 0);
		hlwd_nand_write(h, NAND_CONFIG, NAND_CONFIG_LATTE_INIT | NAND_CONFIG_ENABLE |
				NAND_CONFIG_LATTE_WP);
		hlwd_nand_write(h, NAND_BANK, BIT(2) | (j ? 3 : 1));
		ret = hlwd_nand_command(h, NAND_CMD_RESET, 0, NAND_CTRL_WAIT, 0);
		if (ret)
			return ret;
	}
	return 0;
}

static int hlwd_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hlwd_nand *h;
	int ret, irq;
	struct hlwd_nand_chip *c;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	u32 bank;


	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->dev = dev;
	h->latte = of_device_is_compatible(dev->of_node, "nintendo,latte-nand");
	h->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->regs))
		return PTR_ERR(h->regs);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	h->data = dmam_alloc_coherent(dev, PAGE_BYTES, &h->data_dma, GFP_KERNEL);
	h->spare = dmam_alloc_coherent(dev, 128, &h->spare_dma, GFP_KERNEL);
	if (!h->data || !h->spare)
		return -ENOMEM;

	if ((h->data_dma & 31) || (h->spare_dma & 127))
		return -EINVAL;

	init_completion(&h->done);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, hlwd_nand_irq, 0, dev_name(dev), h);
	if (ret)
		return ret;

	ret = hlwd_nand_init_hw(h);
	if (ret)
		return ret;

	nand_controller_init(&h->controller);
	h->controller.ops = &hlwd_nand_ops;
	/* Latte keeps WP asserted until our program/erase callbacks run */
	h->controller.controller_wp = h->latte;
	platform_set_drvdata(pdev, h);
	for_each_available_child_of_node_scoped(dev->of_node, np) {
		if (h->nchips == ARRAY_SIZE(h->chips) ||
		    of_property_read_u32(np, "reg", &bank) ||
		    (!h->latte && bank) ||
		    (h->latte && bank != 1 && bank != 2)) {
			ret = -EINVAL;
			goto cleanup;
		}
		if (h->nchips && h->chips[0].bank == bank) {
			ret = -EINVAL;
			goto cleanup;
		}

		c = &h->chips[h->nchips];
		c->ctrl = h;
		c->bank = bank;
		c->chip.controller = &h->controller;
		c->chip.ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;

		nand_set_controller_data(&c->chip, c);
		nand_set_flash_node(&c->chip, np);

		mtd = nand_to_mtd(&c->chip);
		mtd->dev.parent = dev;
		mtd->name = !h->latte ? "wii-nand" :
			bank == 2 ? "wiiu-slc" : "wiiu-slccmpt";

		ret = nand_scan(&c->chip, 1);
		if (ret)
			goto cleanup;

		if (!write_enable)
			mtd->flags &= ~MTD_WRITEABLE;

		ret = mtd_device_register(mtd, NULL, 0);
		if (ret) {
			nand_cleanup(&c->chip);
			goto cleanup;
		}

		h->nchips++;
	}

	if (h->nchips)
		return 0;

	ret = -ENODEV;

cleanup:
	while (h->nchips) {
		h->nchips--;
		chip = &h->chips[h->nchips].chip;

		mtd_device_unregister(nand_to_mtd(chip));
		nand_cleanup(chip);
	}
	return ret;
}

static void hlwd_nand_shutdown(struct platform_device *pdev)
{
	struct hlwd_nand *h = platform_get_drvdata(pdev);

	hlwd_nand_write(h, NAND_CTRL, 0);
	if (h->latte)
		hlwd_nand_write(h, NAND_CONFIG, NAND_CONFIG_LATTE_NORM | NAND_CONFIG_LATTE_WP);
}

static void hlwd_nand_remove(struct platform_device *pdev)
{
	struct hlwd_nand *h = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < h->nchips; i++) {
		WARN_ON(mtd_device_unregister(nand_to_mtd(&h->chips[i].chip)));
		nand_cleanup(&h->chips[i].chip);
	}
	hlwd_nand_shutdown(pdev);
}

static const struct of_device_id hlwd_nand_match[] = {
	{ .compatible = "nintendo,hollywood-nand" },
	{ .compatible = "nintendo,latte-nand" },
	{}
};
MODULE_DEVICE_TABLE(of, hlwd_nand_match);

static struct platform_driver hlwd_nand_driver = {
	.probe = hlwd_nand_probe,
	.remove = hlwd_nand_remove,
	.shutdown = hlwd_nand_shutdown,
	.driver = {
		.name = "hollywood_nand",
		.of_match_table = hlwd_nand_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(hlwd_nand_driver);
MODULE_DESCRIPTION("ATI/Nintendo Hollywood/Latte NAND controller");
MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_LICENSE("GPL");
