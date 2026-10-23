// SPDX-License-Identifier: GPL-2.0-only
/*
 * ATI/Nintendo Hollywood/Latte SHA-1 Engine
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on code from NPLL:
 * Copyright (C) 2026 Techflash
 */

#include <crypto/internal/hash.h>
#include <crypto/sha1.h>
#include <crypto/scatterwalk.h>
#include <linux/of.h>
#include "hollywood.h"

#define SHA_CTRL 0x00
#define SHA_CTRL_EXEC BIT(31)
#define SHA_CTRL_IRQ  BIT(30)
#define SHA_SRC  0x04
#define SHA_H0   0x08
#define SHA_H1   0x0c
#define SHA_H2   0x10
#define SHA_H3   0x14
#define SHA_H4   0x18

static struct hlwd_crypto *sha_device;
static DEFINE_MUTEX(sha_device_lock);

struct hlwd_sha_req {
	struct sha1_state state;
	unsigned int final;
	int error;
};

static int hlwd_sha_init(struct ahash_request *req)
{
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	memset(rctx, 0, sizeof(*rctx));
	rctx->state.state[0] = SHA1_H0;
	rctx->state.state[1] = SHA1_H1;
	rctx->state.state[2] = SHA1_H2;
	rctx->state.state[3] = SHA1_H3;
	rctx->state.state[4] = SHA1_H4;
	return 0;
}

static int hlwd_sha_enqueue(struct ahash_request *req, bool final)
{
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	if (rctx->error)
		return rctx->error;
	rctx->final = final;
	return crypto_transfer_hash_request_to_engine(sha_device->engine, req);
}

static int hlwd_sha_update(struct ahash_request *req)
{
	return req->nbytes ? hlwd_sha_enqueue(req, false) : 0;
}

static int hlwd_sha_final(struct ahash_request *req)
{
	/* final() ignores req->src/nbytes, which may describe the last update */
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	rctx->final = true;
	if (rctx->error)
		return rctx->error;

	return crypto_transfer_hash_request_to_engine(sha_device->engine, req);
}

static int hlwd_sha_finup(struct ahash_request *req);
static int hlwd_sha_digest(struct ahash_request *req)
{
	hlwd_sha_init(req);
	return hlwd_sha_finup(req);
}

static int hlwd_sha_export(struct ahash_request *req, void *out)
{
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	if (rctx->error)
		return rctx->error;

	memcpy(out, &rctx->state, sizeof(rctx->state));
	return 0;
}

static int hlwd_sha_import(struct ahash_request *req, const void *in)
{
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	memset(rctx, 0, sizeof(*rctx));
	memcpy(&rctx->state, in, sizeof(rctx->state));
	if (rctx->state.count > U64_MAX / 8) {
		rctx->error = -EOVERFLOW;
		return rctx->error;
	}

	return 0;
}

static int hlwd_sha_blocks(struct hlwd_crypto *h, struct sha1_state *s,
			   unsigned int len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 5; i++)
		hlwd_crypto_write(h, SHA_H0 + i * 4, s->state[i]);

	hlwd_crypto_write(h, SHA_SRC, lower_32_bits(h->dma));
	ret = hlwd_crypto_run(h, SHA_CTRL_EXEC | SHA_CTRL_IRQ | (len / SHA1_BLOCK_SIZE - 1));
	if (!ret)
		for (i = 0; i < 5; i++)
			s->state[i] = in_be32(h->regs + SHA_H0 + i * 4);

	return ret;
}

/* FIXME: wtf */
/* Bit 1 is private request state: include current src during finalization. */
#define SHA_FINUP 2

static int hlwd_sha_finup(struct ahash_request *req)
{
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);

	if (rctx->error)
		return rctx->error;

	rctx->final = SHA_FINUP;
	return crypto_transfer_hash_request_to_engine(sha_device->engine, req);
}

static int hlwd_sha_do_one(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req = container_of(areq, struct ahash_request, base);
	struct hlwd_sha_req *rctx = ahash_request_ctx(req);
	struct sha1_state *s = &rctx->state;
	struct hlwd_crypto *h = sha_device;
	unsigned int nbytes = rctx->final == 1 ? 0 : req->nbytes;
	unsigned int off = 0, buffered = s->count % SHA1_BLOCK_SIZE, len, i;
	int ret = 0;

	if (s->count > U64_MAX / 8 || nbytes > U64_MAX / 8 - s->count) {
		ret = -EOVERFLOW;
		goto out;
	}

	if (buffered && nbytes) {
		len = min(nbytes, SHA1_BLOCK_SIZE - buffered);
		scatterwalk_map_and_copy(s->buffer + buffered, req->src, 0, len, 0);
		s->count += len;
		off += len;
		buffered += len;

		if (buffered == SHA1_BLOCK_SIZE) {
			memcpy(h->buf, s->buffer, SHA1_BLOCK_SIZE);
			ret = hlwd_sha_blocks(h, s, SHA1_BLOCK_SIZE);
			if (ret)
				goto out;
			buffered = 0;
		}
	}

	while (nbytes - off >= SHA1_BLOCK_SIZE) {
		len = min_t(unsigned int, round_down(nbytes - off, SHA1_BLOCK_SIZE),
			    HLWD_CRYPTO_CHUNK);
		scatterwalk_map_and_copy(h->buf, req->src, off, len, 0);
		ret = hlwd_sha_blocks(h, s, len);
		if (ret)
			goto out;

		off += len;
		s->count += len;
	}

	if (nbytes > off) {
		len = nbytes - off;
		scatterwalk_map_and_copy(s->buffer + buffered, req->src, off, len, 0);
		s->count += len;
	}

	if (rctx->final) {
		buffered = s->count % SHA1_BLOCK_SIZE;
		len = buffered < 56 ? 64 : 128;
		memset(h->buf, 0, len);
		memcpy(h->buf, s->buffer, buffered);
		h->buf[buffered] = 0x80;
		*(u64 *)(h->buf + len - 8) = s->count * 8;

		ret = hlwd_sha_blocks(h, s, len);
		if (!ret) {
			for (i = 0; i < 5; i++)
				((u32 *)req->result)[i] = s->state[i];

			memzero_explicit(s, sizeof(*s));
		}
	}
out:
	rctx->error = ret;
	memzero_explicit(h->buf, min_t(u64, (u64)nbytes + 128, HLWD_CRYPTO_CHUNK));
	crypto_finalize_hash_request(engine, req, ret);
	return 0;
}

static int hlwd_sha_tfm_init(struct crypto_tfm *tfm)
{
	crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm), sizeof(struct hlwd_sha_req));
	return 0;
}

static struct ahash_engine_alg hlwd_sha_alg = {
	.base = {
		.init = hlwd_sha_init,
		.update = hlwd_sha_update,
		.final = hlwd_sha_final,
		.finup = hlwd_sha_finup,
		.digest = hlwd_sha_digest,
		.export = hlwd_sha_export,
		.import = hlwd_sha_import,
		.halg = {
			.digestsize = SHA1_DIGEST_SIZE,
			.statesize = sizeof(struct sha1_state),
			.base = {
				.cra_name = "sha1",
				.cra_driver_name = "sha1-hollywood",
				.cra_priority = 300,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA1_BLOCK_SIZE,
				.cra_module = THIS_MODULE,
				.cra_init = hlwd_sha_tfm_init,
			},
		},
	},
	.op.do_one_request = hlwd_sha_do_one,
};

static int hlwd_sha_probe(struct platform_device *pdev)
{
	struct hlwd_crypto *h;
	int ret;

	mutex_lock(&sha_device_lock);
	if (sha_device) {
		ret = -EBUSY;
		goto out;
	}

	h = hlwd_crypto_init(pdev, HLWD_CRYPTO_CHUNK);
	if (IS_ERR(h)) {
		ret = PTR_ERR(h);
		goto out;
	}

	sha_device = h;

	ret = crypto_engine_register_ahash(&hlwd_sha_alg);
	if (ret) {
		sha_device = NULL;
		hlwd_crypto_exit(h);
	}
out:
	mutex_unlock(&sha_device_lock);
	return ret;
}

static void hlwd_sha_remove(struct platform_device *pdev)
{
	crypto_engine_unregister_ahash(&hlwd_sha_alg);
	hlwd_crypto_exit(platform_get_drvdata(pdev));
	sha_device = NULL;
}

static const struct of_device_id hlwd_sha_match[] = {
	{ .compatible = "nintendo,hollywood-sha1" },
	{ .compatible = "nintendo,latte-sha1" },
	{}
};
MODULE_DEVICE_TABLE(of, hlwd_sha_match);
static struct platform_driver hlwd_sha_driver = {
	.probe = hlwd_sha_probe,
	.remove = hlwd_sha_remove,
	.driver = {
		.name = "hollywood-sha1",
		.of_match_table = hlwd_sha_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(hlwd_sha_driver);
MODULE_DESCRIPTION("ATI/Nintendo Hollywood/Latte SHA-1 engine");
MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("sha1");
