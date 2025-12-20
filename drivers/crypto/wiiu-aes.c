/*
 * Copyright (C) 2021 Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/crypto.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <crypto/aes.h>
#include <crypto/internal/skcipher.h>

#include <linux/io.h>
#include <linux/delay.h>

/* Static structures */

#define AES_CTRL 0
#define AES_SRC  4
#define AES_DEST 8
#define AES_KEY  12
#define AES_IV   16

#define AES_CTRL_EXEC       0x80000000
#define AES_CTRL_EXEC_RESET 0x00000000
#define AES_CTRL_EXEC_INIT  0x80000000
#define AES_CTRL_IRQ        0x40000000
#define AES_CTRL_ERR        0x20000000
#define AES_CTRL_ENA        0x10000000
#define AES_CTRL_DEC        0x08000000
#define AES_CTRL_IV         0x00001000
#define AES_CTRL_BLOCK      0x00000fff

#define OP_TIMEOUT 0x1000

#define AES_DIR_DECRYPT 0
#define AES_DIR_ENCRYPT 1

struct wiiu_aes_engine {
	struct device *dev;
	void __iomem *base;
	spinlock_t lock;
	struct skcipher_alg alg;
};

struct wiiu_aes_alg_ctx {
	struct wiiu_aes_engine* engine;
};

struct wiiu_aes_ctx {
	struct wiiu_aes_engine* engine;
	bool in_place;
	dma_addr_t src_dma;
	dma_addr_t dst_dma;
};

/* Write a 128 bit field (either a writable key or IV) */
static inline void
_writefield(struct wiiu_aes_engine *engine, u32 reg, const void *_value)
{
	const u32 *value = _value;
	int i;

	for (i = 0; i < 4; i++)
		iowrite32be(value[i], engine->base + reg);
}

static int
do_crypt(struct wiiu_aes_ctx* ctx, const void *src, void *dst, u32 len, u32 flags)
{
	struct wiiu_aes_engine *engine = ctx->engine;
	struct device *dev = engine->dev;
	u32 blocks = len >> 4;
	u32 offset = 0;
	u32 status;
	u32 counter = OP_TIMEOUT;

	/* Is dma_map_single the right API here? No highmem issues? */
	ctx->in_place = src == dst;
	if (ctx->in_place) {
		ctx->src_dma = dma_map_single(dev, (void*)src, len, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, ctx->src_dma))
			return 1;
	} else {
		ctx->src_dma = dma_map_single(dev, (void*)src, len, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, ctx->src_dma))
			return 1;

		ctx->dst_dma = dma_map_single(dev, dst, len, DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, ctx->dst_dma)) {
			dma_unmap_single(dev, ctx->src_dma, len, DMA_TO_DEVICE);
			return 1;
		}
	}

	while (blocks > 0) {
		u32 chunk_blocks = blocks - 1;
		if (chunk_blocks > 0xfff)
			chunk_blocks = 0xfff;

		/* Set the addresses for DMA */
		iowrite32be(ctx->src_dma + offset, engine->base + AES_SRC);
		if (ctx->in_place) {
			iowrite32be(ctx->src_dma + offset, engine->base + AES_DEST);
		} else {
			iowrite32be(ctx->dst_dma + offset, engine->base + AES_DEST);
		}

		/* Start the operation */
		iowrite32be(flags | chunk_blocks, engine->base + AES_CTRL);

		/* TODO: maybe enabling IRQs might be better on the CPU?  But that loop
		 * seems plenty fast so maybe not. */
		do {
			status = ioread32be(engine->base + AES_CTRL);
			cpu_relax();
		} while ((status & AES_CTRL_EXEC) && --counter);

		offset += (chunk_blocks + 1) << 4;
		blocks -= (chunk_blocks + 1);
		flags |= AES_CTRL_IV;
	}

	if (ctx->in_place) {
		dma_unmap_single(dev, ctx->src_dma, len, DMA_BIDIRECTIONAL);
	} else {
		dma_unmap_single(dev, ctx->dst_dma, len, DMA_FROM_DEVICE);
		dma_unmap_single(dev, ctx->src_dma, len, DMA_TO_DEVICE);
	}

	/* 0 means timeout, oh no! */
	return !counter;
}

static void
wiiu_aes_crypt(struct wiiu_aes_ctx* ctx, const void *src, void *dst, u32 len, u8 *iv, int dir,
               bool firstchunk)
{
	struct wiiu_aes_engine *engine = ctx->engine;
	u32 flags = 0;
	unsigned long iflags;
	int ret;

	flags |= AES_CTRL_EXEC_INIT /* | AES_CTRL_IRQ */ | AES_CTRL_ENA;

	if (dir == AES_DIR_DECRYPT)
		flags |= AES_CTRL_DEC;

	if (!firstchunk)
		flags |= AES_CTRL_IV;

	/* Start the critical section */

	spin_lock_irqsave(&engine->lock, iflags);

	if (firstchunk)
		_writefield(engine, AES_IV, iv);

	ret = do_crypt(ctx, src, dst, len, flags);
	BUG_ON(ret);

	spin_unlock_irqrestore(&engine->lock, iflags);
}

static int wiiu_setkey_skcipher(struct crypto_skcipher *tfm, const u8 *key,
                                unsigned int len)
{
	struct wiiu_aes_alg_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (len != AES_KEYSIZE_128) {
		/* not supported at all */
		return -EINVAL;
	}

	_writefield(ctx->engine, AES_KEY, key);
	return 0;
}

static int wiiu_skcipher_crypt(struct skcipher_request *req, int dir)
{
	struct wiiu_aes_ctx *ctx = skcipher_request_ctx(req);
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct wiiu_aes_alg_ctx *actx = crypto_skcipher_ctx(tfm);
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;
	char ivbuf[AES_BLOCK_SIZE];
	unsigned int ivsize;
	bool firstchunk = true;

	//init request context
	ctx->engine = actx->engine;

	/* Reset the engine */
	iowrite32be(0, ctx->engine->base + AES_CTRL);

	err = skcipher_walk_virt(&walk, req, false);
	ivsize = min(sizeof(ivbuf), walk.ivsize);

	while ((nbytes = walk.nbytes) != 0) {
		unsigned int chunkbytes = round_down(nbytes, AES_BLOCK_SIZE);
		unsigned int ret = nbytes % AES_BLOCK_SIZE;

		if (walk.total == chunkbytes && dir == AES_DIR_DECRYPT) {
			//if this is the last chunk and we're decrypting, take note of
			//the IV (which is the last ciphertext block)
			memcpy(ivbuf, walk.src.virt.addr + walk.total - ivsize, ivsize);
		}

		wiiu_aes_crypt(ctx, walk.src.virt.addr, walk.dst.virt.addr, chunkbytes,
		               walk.iv, dir, firstchunk);

		if (walk.total == chunkbytes && dir == AES_DIR_ENCRYPT) {
			//if this is the last chunk and we're encrypting, take note of
			//the IV (which is the last ciphertext block)
			memcpy(walk.iv, walk.dst.virt.addr + walk.total - ivsize, ivsize);
		} else if (walk.total == chunkbytes && dir == AES_DIR_DECRYPT) {
			memcpy(walk.iv, ivbuf, ivsize);
		}

		err = skcipher_walk_done(&walk, ret);
		firstchunk = false;
	}

	return err;
}

static int wiiu_cbc_encrypt(struct skcipher_request *req)
{
	return wiiu_skcipher_crypt(req, AES_DIR_ENCRYPT);
}

static int wiiu_cbc_decrypt(struct skcipher_request *req)
{
	return wiiu_skcipher_crypt(req, AES_DIR_DECRYPT);
}

static inline struct wiiu_aes_engine* to_wiiu_engine(struct skcipher_alg* alg) {
	return container_of(alg, struct wiiu_aes_engine, alg);
}

static int wiiu_cbc_init(struct crypto_skcipher *tfm)
{
	struct wiiu_aes_alg_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);
	struct wiiu_aes_engine *engine = to_wiiu_engine(alg);

	ctx->engine = engine;
	return 0;
}

static void wiiu_cbc_exit(struct crypto_skcipher *tfm)
{

}

static struct skcipher_alg wiiu_alg = {
	.base.cra_name		= "cbc(aes)",
	.base.cra_driver_name	= "cbc-aes-wiiu",
	.base.cra_priority	= 400,
	.base.cra_flags		= CRYPTO_ALG_KERN_DRIVER_ONLY,
	.base.cra_blocksize	= AES_BLOCK_SIZE,
	.base.cra_alignmask	= 0x1FF,
	.base.cra_ctxsize = sizeof(struct wiiu_aes_alg_ctx),
	.base.cra_module	= THIS_MODULE,
	.setkey			= wiiu_setkey_skcipher,
	.encrypt		= wiiu_cbc_encrypt,
	.decrypt		= wiiu_cbc_decrypt,
	.init			= wiiu_cbc_init,
	.exit			= wiiu_cbc_exit,
	.min_keysize		= AES_KEYSIZE_128,
	.max_keysize		= AES_KEYSIZE_128,
	.ivsize			= AES_BLOCK_SIZE,
};

static void wiiu_aes_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wiiu_aes_engine *engine = platform_get_drvdata(pdev);

	crypto_unregister_skcipher(&engine->alg);
	devm_iounmap(dev, engine->base);
	engine->base = NULL;

	return;
}

static int wiiu_aes_probe(struct platform_device *pdev)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct wiiu_aes_engine *engine = devm_kzalloc(dev, sizeof(*engine), GFP_KERNEL);

	engine->dev = dev;
	engine->alg = wiiu_alg;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	engine->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(engine->base))
		return PTR_ERR(engine->base);

	spin_lock_init(&engine->lock);

	/* Reset the engine */
	iowrite32be(0, engine->base + AES_CTRL);

	ret = crypto_register_skcipher(&engine->alg);
	if (ret)
		goto eiomap;

	platform_set_drvdata(pdev, engine);

	dev_notice(dev, "Nintendo Wii U AES engine enabled.\n");
	return 0;

 eiomap:
	devm_iounmap(dev, engine->base);

	dev_err(dev, "Nintendo Wii U AES initialization failed.\n");
	return ret;
}

static const struct of_device_id wiiu_aes_of_match[] = {
	{ .compatible = "nintendo,latte-aes", },
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, wiiu_aes_of_match);

static struct platform_driver wiiu_aes_driver = {
	.driver = {
		.name = "nintendo,latte-aes",
		.of_match_table = wiiu_aes_of_match,
	},
	.probe = wiiu_aes_probe,
	.remove = wiiu_aes_remove,
};

module_platform_driver(wiiu_aes_driver);

MODULE_AUTHOR("Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>");
MODULE_DESCRIPTION("Nintendo Wii U Hardware AES driver");
MODULE_LICENSE("GPL");
