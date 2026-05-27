// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drivers/block/gcn-aram.c
 *
 * Nintendo GameCube Auxiliary RAM (ARAM) block driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2005,2007,2008,2009 Albert Herranz
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on previous work by Franz Lehner.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/dma-mapping.h>
#include <linux/hdreg.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>


#define DRV_MODULE_NAME "gcn-aram"
#define DRV_DESCRIPTION "Nintendo GameCube Auxiliary RAM (ARAM) block driver"
#define DRV_AUTHOR      "Todd Jeffreys <todd@voidpointer.org>, " \
			"Albert Herranz"

static char aram_driver_version[] = "4.0i";

/*
 * Hardware.
 */
#define ARAM_DMA_ALIGN		0x1f	/* 32 bytes */

#define DSP_CSR			0x00a
#define  DSP_CSR_PIINT		BIT(1)
#define  DSP_CSR_AIDINT		BIT(3)
#define  DSP_CSR_ARINT		BIT(5)
#define  DSP_CSR_ARINTMASK	BIT(6)
#define  DSP_CSR_DSPINT		BIT(7)
#define  DSP_CSR_DSPDMA		BIT(9)

#define AR_DMA_MMADDR		0x020
#define AR_DMA_ARADDR		0x024
#define AR_DMA_CNT		0x028
#define  AR_READ		BIT(31)
#define  AR_WRITE		0

/*
 * Driver settings
 */
#define ARAM_NAME		"gcn-aram"

#define ARAM_SECTOR_SIZE	PAGE_SIZE
#define ARAM_DMA_BUF_SIZE	PAGE_SIZE


/*
 * Driver data.
 */
struct aram_drvdata {
	spinlock_t			io_lock;

	void __iomem			*io_base;
	int				irq;

	u64 size;
	struct gendisk			*disk;

	struct request			*req;	/* protected by ->io_lock */
	dma_addr_t			bounce_dma;
	size_t				dma_len;

	struct blk_mq_tag_set		tag_set;

	void				*bounce_buf;
	struct device			*dev;
};

static int gcn_aram_major;

static inline u16 aram_readw(struct aram_drvdata *drvdata, u32 reg)
{
	return ioread16be(drvdata->io_base + reg);
}

static inline void aram_writew(struct aram_drvdata *drvdata, u32 reg, u16 val)
{
	iowrite16be(val, drvdata->io_base + reg);
}

static inline void aram_writel(struct aram_drvdata *drvdata, u32 reg, u32 val)
{
	iowrite32be(val, drvdata->io_base + reg);
}

static void aram_ack_irq(struct aram_drvdata *drvdata, u16 csr)
{
	/* Ack only ARAM state; AI and DSP share this register and IRQ line. */
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	aram_writew(drvdata, DSP_CSR, csr);
}

static void aram_start_dma(struct aram_drvdata *drvdata, sector_t sector,
			   enum req_op op)
{
	u32 aram_addr = lower_32_bits((u64)sector << SECTOR_SHIFT);
	u32 aram_dir = op == REQ_OP_READ ? AR_READ : AR_WRITE;

	WARN_ON_ONCE((drvdata->bounce_dma & ARAM_DMA_ALIGN) ||
		     (drvdata->dma_len & ARAM_DMA_ALIGN));

	aram_writel(drvdata, AR_DMA_MMADDR, lower_32_bits(drvdata->bounce_dma));
	aram_writel(drvdata, AR_DMA_ARADDR, aram_addr);
	aram_writel(drvdata, AR_DMA_CNT, aram_dir | drvdata->dma_len);
}

static blk_status_t aram_copy_to_bounce(struct aram_drvdata *drvdata,
					struct request *req)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	size_t offset = 0;

	rq_for_each_segment(bvec, req, iter) {
		void *src;

		if (offset + bvec.bv_len > ARAM_DMA_BUF_SIZE)
			return BLK_STS_IOERR;

		src = bvec_kmap_local(&bvec);
		memcpy((u8 *)drvdata->bounce_buf + offset, src, bvec.bv_len);
		kunmap_local(src);
		offset += bvec.bv_len;
	}

	return BLK_STS_OK;
}

static blk_status_t aram_copy_from_bounce(struct aram_drvdata *drvdata,
					  struct request *req, size_t len)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	size_t offset = 0;

	rq_for_each_segment(bvec, req, iter) {
		void *dst;

		if (offset + bvec.bv_len > len)
			return BLK_STS_IOERR;

		dst = bvec_kmap_local(&bvec);
		memcpy(dst, (u8 *)drvdata->bounce_buf + offset, bvec.bv_len);
		kunmap_local(dst);
		offset += bvec.bv_len;
	}

	return BLK_STS_OK;
}

static irqreturn_t aram_irq_handler(int irq, void *data)
{
	struct aram_drvdata *drvdata = data;
	struct request *req;
	blk_status_t status = BLK_STS_OK;
	size_t dma_len;
	u16 csr;

	spin_lock(&drvdata->io_lock);

	csr = aram_readw(drvdata, DSP_CSR);
	if (!(csr & DSP_CSR_ARINT)) {
		spin_unlock(&drvdata->io_lock);
		return IRQ_NONE;
	}

	aram_ack_irq(drvdata, csr);

	req = drvdata->req;
	dma_len = drvdata->dma_len;
	drvdata->req = NULL;
	drvdata->dma_len = 0;

	spin_unlock(&drvdata->io_lock);

	if (!req) {
		dev_err(drvdata->dev, "ignoring ARAM interrupt with no request\n");
		return IRQ_HANDLED;
	}

	if (req_op(req) == REQ_OP_READ)
		status = aram_copy_from_bounce(drvdata, req, dma_len);

	blk_mq_end_request(req, status);
	return IRQ_HANDLED;
}

static blk_status_t aram_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct aram_drvdata *drvdata = hctx->queue->queuedata;
	struct request *req = bd->rq;
	sector_t sector = blk_rq_pos(req);
	u64 aram_addr = (u64)sector << SECTOR_SHIFT;
	size_t len = blk_rq_bytes(req);
	enum req_op op = req_op(req);
	blk_status_t status;
	unsigned long flags;

	if (op != REQ_OP_READ && op != REQ_OP_WRITE)
		return BLK_STS_NOTSUPP;

	spin_lock_irqsave(&drvdata->io_lock, flags);
	if (drvdata->req) {
		spin_unlock_irqrestore(&drvdata->io_lock, flags);
		return BLK_STS_RESOURCE;
	}
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	blk_mq_start_request(req);

	if (len > ARAM_DMA_BUF_SIZE || (len & ARAM_DMA_ALIGN) ||
	    (aram_addr & ARAM_DMA_ALIGN)) {
		status = BLK_STS_IOERR;
		goto end;
	}

	if (aram_addr + len > drvdata->size) {
		dev_err(drvdata->dev, "bad access: block=%llu size=%zu\n",
			(unsigned long long)sector, len);
		status = BLK_STS_IOERR;
		goto end;
	}

	if (op == REQ_OP_WRITE) {
		status = aram_copy_to_bounce(drvdata, req);
		if (status != BLK_STS_OK)
			goto end;
	}

	spin_lock_irqsave(&drvdata->io_lock, flags);
	drvdata->req = req;
	drvdata->dma_len = len;
	aram_start_dma(drvdata, sector, op);
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	return BLK_STS_OK;

end:
	blk_mq_end_request(req, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops aram_mq_ops = {
	.queue_rq = aram_queue_rq,
};

static void aram_quiesce(struct aram_drvdata *drvdata)
{
	unsigned long flags;
	u16 csr;

	spin_lock_irqsave(&drvdata->io_lock, flags);
	csr = aram_readw(drvdata, DSP_CSR);
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT | DSP_CSR_ARINTMASK);
	aram_writew(drvdata, DSP_CSR, csr);
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	while (aram_readw(drvdata, DSP_CSR) & DSP_CSR_DSPDMA)
		cpu_relax();
}

static int aram_enable_irq(struct aram_drvdata *drvdata)
{
	unsigned long flags;
	u16 csr;

	spin_lock_irqsave(&drvdata->io_lock, flags);
	csr = aram_readw(drvdata, DSP_CSR);
	csr |= DSP_CSR_ARINT | DSP_CSR_ARINTMASK | DSP_CSR_PIINT;
	csr &= ~(DSP_CSR_AIDINT | DSP_CSR_DSPINT);
	aram_writew(drvdata, DSP_CSR, csr);
	spin_unlock_irqrestore(&drvdata->io_lock, flags);

	return 0;
}

static int aram_get_size(struct device_node *dsp_np, u64 *size)
{
	struct device_node *aram_np;
	u64 addr;
	int ret;

	aram_np = of_get_compatible_child(dsp_np, "nintendo,flipper-aram");
	if (!aram_np)
		return -ENODEV;

	ret = of_property_read_reg(aram_np, 0, &addr, size);
	of_node_put(aram_np);
	if (ret)
		return ret;

	if (!*size || *size > U32_MAX)
		return -EINVAL;

	return 0;
}

static int aram_open(struct gendisk *disk, blk_mode_t mode)
{
	return 0;
}

static void aram_release(struct gendisk *disk)
{
}

static int aram_getgeo(struct gendisk *disk, struct hd_geometry *geo)
{
	geo->heads = 4;
	geo->sectors = 16;
	geo->cylinders = get_capacity(disk) / (geo->heads * geo->sectors);
	return 0;
}

static const struct block_device_operations aram_fops = {
	.owner		= THIS_MODULE,
	.open		= aram_open,
	.release	= aram_release,
	.getgeo		= aram_getgeo,
};

static int aram_probe(struct platform_device *pdev)
{
	struct queue_limits lim = {
		.logical_block_size	= ARAM_SECTOR_SIZE,
		.physical_block_size	= ARAM_SECTOR_SIZE,
		.io_min			= ARAM_SECTOR_SIZE,
		.dma_alignment		= ARAM_DMA_ALIGN,
		.max_hw_sectors		= ARAM_DMA_BUF_SIZE >> SECTOR_SHIFT,
		.max_segments		= 1,
		.max_segment_size	= ARAM_DMA_BUF_SIZE,
	};
	struct device *dev = &pdev->dev;
	struct aram_drvdata *drvdata;
	u64 size;
	int ret;

	ret = aram_get_size(dev->of_node, &size);
	if (ret)
		return ret;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = dev;
	drvdata->size = size;

	spin_lock_init(&drvdata->io_lock);
	platform_set_drvdata(pdev, drvdata);

	drvdata->io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(drvdata->io_base))
		return PTR_ERR(drvdata->io_base);

	drvdata->irq = platform_get_irq(pdev, 0);
	if (drvdata->irq < 0)
		return drvdata->irq;

	drvdata->bounce_buf = dmam_alloc_coherent(dev, ARAM_DMA_BUF_SIZE,
					       &drvdata->bounce_dma, GFP_KERNEL);
	if (!drvdata->bounce_buf)
		return -ENOMEM;

	ret = devm_request_irq(dev, drvdata->irq, aram_irq_handler, IRQF_SHARED,
			       ARAM_NAME, drvdata);
	if (ret)
		return dev_err_probe(dev, ret, "request IRQ %d failed\n",
				     drvdata->irq);

	ret = blk_mq_alloc_sq_tag_set(&drvdata->tag_set, &aram_mq_ops, 1, 0);
	if (ret)
		return ret;

	drvdata->disk = blk_mq_alloc_disk(&drvdata->tag_set, &lim, drvdata);
	if (IS_ERR(drvdata->disk)) {
		ret = PTR_ERR(drvdata->disk);
		goto err_free_tag_set;
	}

	drvdata->disk->major = gcn_aram_major;
	drvdata->disk->first_minor = 0;
	drvdata->disk->minors = 1;
	drvdata->disk->fops = &aram_fops;
	drvdata->disk->private_data = drvdata;
	drvdata->disk->queue->queuedata = drvdata;
	strscpy(drvdata->disk->disk_name, ARAM_NAME, DISK_NAME_LEN);
	set_capacity(drvdata->disk, drvdata->size >> SECTOR_SHIFT);

	ret = aram_enable_irq(drvdata);
	if (ret)
		goto err_put_disk;

	ret = add_disk(drvdata->disk);
	if (ret)
		goto err_quiesce;

	dev_info(dev, "GameCube ARAM block device registered, %llu bytes\n",
		 drvdata->size);
	return 0;

err_quiesce:
	aram_quiesce(drvdata);
err_put_disk:
	put_disk(drvdata->disk);
err_free_tag_set:
	blk_mq_free_tag_set(&drvdata->tag_set);
	return ret;
}

static void aram_remove(struct platform_device *pdev)
{
	struct aram_drvdata *drvdata = platform_get_drvdata(pdev);

	if (drvdata->disk)
		del_gendisk(drvdata->disk);
	aram_quiesce(drvdata);
	put_disk(drvdata->disk);
	blk_mq_free_tag_set(&drvdata->tag_set);
}

static void aram_shutdown(struct platform_device *pdev)
{
	aram_quiesce(platform_get_drvdata(pdev));
}


static const struct of_device_id aram_of_match[] = {
	{ .compatible = "nintendo,flipper-dsp" },
	{ }
};


MODULE_DEVICE_TABLE(of, aram_of_match);

static struct platform_driver aram_driver = {
	.driver = {
		.name = ARAM_NAME,
		.owner = THIS_MODULE,
		.of_match_table = aram_of_match,
	},
	.probe = aram_probe,
	.remove = aram_remove,
	.shutdown = aram_shutdown,
};

/*
 * Module interfaces.
 *
 */

static int __init aram_init_module(void)
{
	int ret;

	gcn_aram_major = register_blkdev(0, ARAM_NAME);
	if (gcn_aram_major < 0)
		return gcn_aram_major;

	ret = platform_driver_register(&aram_driver);
	if (ret)
		unregister_blkdev(gcn_aram_major, ARAM_NAME);

	return ret;
}

static void __exit aram_exit_module(void)
{
	platform_driver_unregister(&aram_driver);
	unregister_blkdev(gcn_aram_major, ARAM_NAME);
}

module_init(aram_init_module);
module_exit(aram_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
