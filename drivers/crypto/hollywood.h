/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ATI/Nintendo Hollywood/Latte Crypto common glue
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */

#ifndef _HOLLYWOOD_CRYPTO_H
#define _HOLLYWOOD_CRYPTO_H

#include <crypto/engine.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

/* common properties */
#define HLWD_CRYPTO_CTRL  0x00
#define HLWD_CRYPTO_CTRL_EXEC BIT(31)
#define HLWD_CRYPTO_CTRL_IRQ  BIT(30)
#define HLWD_CRYPTO_CTRL_ERR  BIT(29)
#define HLWD_CRYPTO_CHUNK SZ_64K

struct hlwd_crypto {
	struct crypto_engine *engine;
	struct device *dev;
	void __iomem *regs;
	struct completion done;
	u8 *buf;
	dma_addr_t dma;
	bool failed;
};

static inline void hlwd_crypto_write(struct hlwd_crypto *h, u32 reg, u32 val)
{
	out_be32(h->regs + reg, val);
}

static irqreturn_t hlwd_crypto_irq(int irq, void *arg)
{
	struct hlwd_crypto *h = arg;
	u32 ctrl = in_be32(h->regs + HLWD_CRYPTO_CTRL);

	if (ctrl & HLWD_CRYPTO_CTRL_EXEC)
		return IRQ_NONE;

	/* acknowledge the IRQ */
	out_be32(h->regs + HLWD_CRYPTO_CTRL, ctrl & ~(HLWD_CRYPTO_CTRL_EXEC | HLWD_CRYPTO_CTRL_IRQ | HLWD_CRYPTO_CTRL_ERR));

	complete(&h->done);
	return IRQ_HANDLED;
}

static int hlwd_crypto_run(struct hlwd_crypto *h, u32 command)
{
	u32 status;

	if (h->failed)
		return -EIO;

	reinit_completion(&h->done);
	hlwd_crypto_write(h, HLWD_CRYPTO_CTRL, command);
	if (!wait_for_completion_timeout(&h->done, msecs_to_jiffies(100))) {
		status = in_be32(h->regs);
		if (status & HLWD_CRYPTO_CTRL_EXEC) {
			hlwd_crypto_write(h, HLWD_CRYPTO_CTRL, 0);
			read_poll_timeout(in_be32, status, !(status & HLWD_CRYPTO_CTRL_EXEC),
					  10, 10000, false, h->regs);
			h->failed = true;
			dev_err(h->dev, "crypto timeout; engine disabled\n");
			return -ETIMEDOUT;
		}
	}
	status = in_be32(h->regs);
	hlwd_crypto_write(h, HLWD_CRYPTO_CTRL, 0);
	return status & HLWD_CRYPTO_CTRL_ERR ? -EIO : 0;
}

static struct hlwd_crypto *hlwd_crypto_init(struct platform_device *pdev,
					      size_t bytes)
{
	struct device *dev = &pdev->dev;
	struct hlwd_crypto *h;
	u32 status;
	int ret, irq;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);

	h->dev = dev;

	h->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->regs))
		return ERR_CAST(h->regs);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ERR_PTR(ret);

	h->buf = dmam_alloc_coherent(dev, bytes, &h->dma, GFP_KERNEL);
	if (!h->buf)
		return ERR_PTR(-ENOMEM);
	if (h->dma & 63)
		return ERR_PTR(-EINVAL);

	init_completion(&h->done);
	ret = read_poll_timeout(in_be32, status, !(status & HLWD_CRYPTO_CTRL_EXEC),
				10, 100000, false, h->regs);
	if (ret)
		return ERR_PTR(ret);

	hlwd_crypto_write(h, HLWD_CRYPTO_CTRL, 0);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return ERR_PTR(irq);

	ret = devm_request_irq(dev, irq, hlwd_crypto_irq, 0, dev_name(dev), h);
	if (ret)
		return ERR_PTR(ret);

	h->engine = crypto_engine_alloc_init(dev, false);
	if (!h->engine)
		return ERR_PTR(-ENOMEM);

	ret = crypto_engine_start(h->engine);
	if (ret) {
		crypto_engine_exit(h->engine);
		return ERR_PTR(ret);
	}

	platform_set_drvdata(pdev, h);
	return h;
}

static void hlwd_crypto_exit(struct hlwd_crypto *h)
{
	crypto_engine_stop(h->engine);
	crypto_engine_exit(h->engine);
	hlwd_crypto_write(h, HLWD_CRYPTO_CTRL, 0);
}
#endif
