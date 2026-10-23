// SPDX-License-Identifier: GPL-2.0-only
/*
 * ATI/Nintendo Hollywood/Latte AES Engine
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on code from NPLL:
 * Copyright (C) 2026 Techflash
 */

#include <crypto/aes.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/of.h>
#include "hollywood.h"

#define AES_CTRL  0x00
#define AES_SRC   0x04
#define AES_DEST  0x08
#define AES_KEY   0x0c
#define AES_IV    0x10

#define HLWD_AES_DMA_ALIGN 64

#define AES_CTRL_EXEC BIT(31)
#define AES_CTRL_IRQ  BIT(30)
#define AES_CTRL_ENA  BIT(28)
#define AES_CTRL_DEC  BIT(27)

static struct hlwd_crypto *aes_device;
static DEFINE_MUTEX(aes_device_lock);

struct hlwd_aes_ctx {
	struct crypto_skcipher *fallback;
	u8 key[AES_KEYSIZE_128];
	bool use_fallback;
};

struct hlwd_aes_req {
	bool decrypt;
	/* a fallback request has a variable-sized context */
	struct skcipher_request fallback;
};

static int hlwd_aes_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int len)
{
	struct hlwd_aes_ctx *ctx = crypto_skcipher_ctx(tfm);
	int ret;

	if (len != AES_KEYSIZE_128 && len != AES_KEYSIZE_192 && len != AES_KEYSIZE_256)
		return -EINVAL;

	crypto_skcipher_clear_flags(ctx->fallback, CRYPTO_TFM_REQ_MASK);
	crypto_skcipher_set_flags(ctx->fallback, crypto_skcipher_get_flags(tfm) &
				 CRYPTO_TFM_REQ_MASK);
	ret = crypto_skcipher_setkey(ctx->fallback, key, len);
	if (ret)
		return ret;

	ctx->use_fallback = len != AES_KEYSIZE_128;
	memzero_explicit(ctx->key, sizeof(ctx->key));
	if (!ctx->use_fallback)
		memcpy(ctx->key, key, sizeof(ctx->key));

	return 0;
}

static int hlwd_aes_crypt(struct skcipher_request *req, bool decrypt)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct hlwd_aes_ctx *ctx = crypto_skcipher_ctx(tfm);
	struct hlwd_aes_req *rctx = skcipher_request_ctx(req);

	if (req->cryptlen % AES_BLOCK_SIZE)
		return -EINVAL;
	if (!req->cryptlen)
		return 0;
	if (ctx->use_fallback) {
		skcipher_request_set_tfm(&rctx->fallback, ctx->fallback);
		skcipher_request_set_callback(&rctx->fallback, req->base.flags,
					      req->base.complete, req->base.data);
		skcipher_request_set_crypt(&rctx->fallback, req->src, req->dst,
					   req->cryptlen, req->iv);
		return decrypt ? crypto_skcipher_decrypt(&rctx->fallback) :
				 crypto_skcipher_encrypt(&rctx->fallback);
	}

	rctx->decrypt = decrypt;
	return crypto_transfer_skcipher_request_to_engine(aes_device->engine, req);
}

static int hlwd_aes_encrypt(struct skcipher_request *req)
{
	return hlwd_aes_crypt(req, false);
}

static int hlwd_aes_decrypt(struct skcipher_request *req)
{
	return hlwd_aes_crypt(req, true);
}

static void hlwd_aes_load(struct hlwd_crypto *h, const u8 *key, const u8 *iv)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		hlwd_crypto_write(h, AES_KEY, ((const u32 *)key)[i]);
		hlwd_crypto_write(h, AES_IV, ((const u32 *)iv)[i]);
	}
}

static void hlwd_aes_clear(struct hlwd_crypto *h)
{
	unsigned int i;

	/* Clear the key/IV shift registers after each request */
	for (i = 0; i < 4; i++) {
		hlwd_crypto_write(h, AES_KEY, 0);
		hlwd_crypto_write(h, AES_IV, 0);
	}
}

static int hlwd_aes_exec(struct hlwd_crypto *h, struct hlwd_aes_ctx *ctx,
			 bool decrypt, const u8 *iv, dma_addr_t src,
			 dma_addr_t dst, unsigned int len)
{
	hlwd_aes_load(h, ctx->key, iv);
	hlwd_crypto_write(h, AES_SRC, lower_32_bits(src));
	hlwd_crypto_write(h, AES_DEST, lower_32_bits(dst));
	return hlwd_crypto_run(h, AES_CTRL_EXEC | AES_CTRL_ENA | AES_CTRL_IRQ |
			       (decrypt ? AES_CTRL_DEC : 0) |
			       (len / AES_BLOCK_SIZE - 1));
}

/* Fallback (slow): stage everything through the (uncached) coherent bounce buffer */
static int hlwd_aes_do_bounce(struct skcipher_request *req,
			      struct hlwd_aes_ctx *ctx, bool decrypt, u8 *iv)
{
	struct hlwd_crypto *h = aes_device;
	u8 next_iv[AES_BLOCK_SIZE];
	unsigned int off = 0, len;
	int ret = 0;

	while (off < req->cryptlen) {
		len = min_t(unsigned int, req->cryptlen - off, HLWD_CRYPTO_CHUNK);
		scatterwalk_map_and_copy(h->buf, req->src, off, len, 0);
		if (decrypt)
			memcpy(next_iv, h->buf + len - AES_BLOCK_SIZE, sizeof(next_iv));

		ret = hlwd_aes_exec(h, ctx, decrypt, iv, h->dma,
				    h->dma + HLWD_CRYPTO_CHUNK, len);
		if (ret)
			break;

		if (!decrypt)
			memcpy(next_iv, h->buf + HLWD_CRYPTO_CHUNK + len - AES_BLOCK_SIZE,
			       sizeof(next_iv));

		scatterwalk_map_and_copy(h->buf + HLWD_CRYPTO_CHUNK, req->dst, off, len, 1);
		memcpy(iv, next_iv, AES_BLOCK_SIZE);
		off += len;
	}

	memzero_explicit(h->buf, min_t(unsigned int, req->cryptlen, HLWD_CRYPTO_CHUNK));
	memzero_explicit(h->buf + HLWD_CRYPTO_CHUNK,
			 min_t(unsigned int, req->cryptlen, HLWD_CRYPTO_CHUNK));
	return ret;
}

/*
 * When the request's buffers satisfy the engine's alignment rules we can map
 * them directly and avoid the bounce copies (slow)
 */
static bool hlwd_aes_sg_direct(struct scatterlist *sg, unsigned int remaining)
{
	unsigned int len;

	while (remaining) {
		if (!sg)
			return false;

		len = min(sg->length, remaining);
		if (!IS_ALIGNED(sg_phys(sg), HLWD_AES_DMA_ALIGN) ||
		    !IS_ALIGNED(len, HLWD_AES_DMA_ALIGN))
			return false;

		remaining -= len;
		sg = sg_next(sg);
	}
	return true;
}

static dma_addr_t hlwd_aes_map(struct device *dev, struct scatterlist *sg,
			       unsigned int off, unsigned int len,
			       enum dma_data_direction dir)
{
	phys_addr_t phys = sg_phys(sg) + off;

	/* An entry may span several contiguous pages, so don't trust sg->offset */
	return dma_map_page(dev, pfn_to_page(PHYS_PFN(phys)), offset_in_page(phys),
			    len, dir);
}

static int hlwd_aes_do_direct(struct skcipher_request *req,
			      struct hlwd_aes_ctx *ctx, bool decrypt, u8 *iv)
{
	struct hlwd_crypto *h = aes_device;
	struct device *dev = h->dev;
	struct scatterlist *ssg = req->src, *dsg = req->dst;
	bool inplace = req->src == req->dst;
	unsigned int soff = 0, doff = 0, done = 0, len;
	u8 next_iv[AES_BLOCK_SIZE];
	dma_addr_t sdma, ddma;
	int ret = 0;

	while (done < req->cryptlen) {
		len = min3(ssg->length - soff, dsg->length - doff,
			   (unsigned int)HLWD_CRYPTO_CHUNK);
		len = min(len, req->cryptlen - done);

		/* Decrypting in place clobbers the next IV, so take it first */
		if (decrypt)
			scatterwalk_map_and_copy(next_iv, req->src,
						 done + len - AES_BLOCK_SIZE,
						 AES_BLOCK_SIZE, 0);

		sdma = hlwd_aes_map(dev, ssg, soff, len,
				    inplace ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE);
		if (dma_mapping_error(dev, sdma)) {
			ret = -EIO;
			break;
		}
		ddma = sdma;
		if (!inplace) {
			ddma = hlwd_aes_map(dev, dsg, doff, len, DMA_FROM_DEVICE);
			if (dma_mapping_error(dev, ddma)) {
				dma_unmap_page(dev, sdma, len, DMA_TO_DEVICE);
				ret = -EIO;
				break;
			}
		}

		ret = hlwd_aes_exec(h, ctx, decrypt, iv, sdma, ddma, len);

		if (!inplace)
			dma_unmap_page(dev, ddma, len, DMA_FROM_DEVICE);
		dma_unmap_page(dev, sdma, len,
			       inplace ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE);
		if (ret)
			break;

		if (!decrypt)
			scatterwalk_map_and_copy(next_iv, req->dst,
						 done + len - AES_BLOCK_SIZE,
						 AES_BLOCK_SIZE, 0);
		memcpy(iv, next_iv, AES_BLOCK_SIZE);

		done += len;
		soff += len;
		doff += len;
		if (soff >= ssg->length) {
			ssg = sg_next(ssg);
			soff = 0;
		}
		if (doff >= dsg->length) {
			dsg = sg_next(dsg);
			doff = 0;
		}
	}
	return ret;
}

static int hlwd_aes_do_one(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = container_of(areq, struct skcipher_request, base);
	struct hlwd_aes_ctx *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct hlwd_aes_req *rctx = skcipher_request_ctx(req);
	struct hlwd_crypto *h = aes_device;
	u8 iv[AES_BLOCK_SIZE];
	int ret;

	memcpy(iv, req->iv, sizeof(iv));
	if (hlwd_aes_sg_direct(req->src, req->cryptlen) &&
	    hlwd_aes_sg_direct(req->dst, req->cryptlen))
		ret = hlwd_aes_do_direct(req, ctx, rctx->decrypt, iv);
	else
		ret = hlwd_aes_do_bounce(req, ctx, rctx->decrypt, iv);
	if (!ret)
		memcpy(req->iv, iv, sizeof(iv));

	hlwd_aes_clear(h);
	crypto_finalize_skcipher_request(engine, req, ret);
	return 0;
}

static int hlwd_aes_init_tfm(struct crypto_skcipher *tfm)
{
	struct hlwd_aes_ctx *ctx = crypto_skcipher_ctx(tfm);

	ctx->fallback = crypto_alloc_skcipher("cbc(aes)", 0,
					    CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->fallback))
		return PTR_ERR(ctx->fallback);

	crypto_skcipher_set_reqsize(tfm, sizeof(struct hlwd_aes_req) +
				   crypto_skcipher_reqsize(ctx->fallback));
	return 0;
}

static void hlwd_aes_exit_tfm(struct crypto_skcipher *tfm)
{
	struct hlwd_aes_ctx *ctx = crypto_skcipher_ctx(tfm);

	crypto_free_skcipher(ctx->fallback);
	memzero_explicit(ctx->key, sizeof(ctx->key));
}

static struct skcipher_engine_alg hlwd_aes_alg = {
	.base = {
		.base = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "cbc-aes-hollywood",
			.cra_priority = 300,
			.cra_flags = CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct hlwd_aes_ctx),
			.cra_module = THIS_MODULE,
		},
		.min_keysize = AES_MIN_KEY_SIZE,
		.max_keysize = AES_MAX_KEY_SIZE,
		.ivsize = AES_BLOCK_SIZE,
		.setkey = hlwd_aes_setkey,
		.encrypt = hlwd_aes_encrypt,
		.decrypt = hlwd_aes_decrypt,
		.init = hlwd_aes_init_tfm,
		.exit = hlwd_aes_exit_tfm,
	},
	.op.do_one_request = hlwd_aes_do_one,
};

static int hlwd_aes_probe(struct platform_device *pdev)
{
	struct hlwd_crypto *h;
	int ret;

	mutex_lock(&aes_device_lock);
	if (aes_device) {
		ret = -EBUSY;
		goto out;
	}

	h = hlwd_crypto_init(pdev, HLWD_CRYPTO_CHUNK * 2);
	if (IS_ERR(h)) {
		ret = PTR_ERR(h);
		goto out;
	}

	aes_device = h;
	ret = crypto_engine_register_skcipher(&hlwd_aes_alg);
	if (ret) {
		aes_device = NULL;
		hlwd_crypto_exit(h);
	}
out:
	mutex_unlock(&aes_device_lock);
	return ret;
}

static void hlwd_aes_remove(struct platform_device *pdev)
{
	crypto_engine_unregister_skcipher(&hlwd_aes_alg);
	hlwd_crypto_exit(platform_get_drvdata(pdev));
	aes_device = NULL;
}

static const struct of_device_id hlwd_aes_match[] = {
	{ .compatible = "nintendo,hollywood-aes" },
	{ .compatible = "nintendo,latte-aes" },
	{}
};
MODULE_DEVICE_TABLE(of, hlwd_aes_match);
static struct platform_driver hlwd_aes_driver = {
	.probe = hlwd_aes_probe,
	.remove = hlwd_aes_remove,
	.driver = {
		.name = "hollywood-aes",
		.of_match_table = hlwd_aes_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(hlwd_aes_driver);
MODULE_DESCRIPTION("ATI/Nintendo Hollywood/Latte AES-CBC accelerator");
MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("cbc(aes)");
