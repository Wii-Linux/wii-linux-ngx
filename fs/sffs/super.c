// SPDX-License-Identifier: GPL-2.0-only
/*
 * BroadOn/Nintendo SFFS filesystem
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on code from NPLL:
 * Copyright (C) 2026 Techflash
 */

#include <crypto/skcipher.h>
#include <crypto/utils.h>
#include <linux/crypto.h>
#include <linux/bitmap.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/highmem.h>
#include <linux/hex.h>
#include <linux/scatterlist.h>
#include <linux/module.h>
#include <linux/mtd/super.h>
#include <linux/nintendo-nand-ecc.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/unaligned.h>

#include "format.h"

static const struct sffs_layout sffs_layouts[] = {
	{ "SFFS", 0x3f800, 16, 0x40, false },
	{ "SFS!", 0x3e000, 64, 0, true },
};

struct sffs_sb_info {
	struct sffs_metadata *meta;
	const struct sffs_layout *layout;
	u16 *parents;
	u8 key[16];
	struct crypto_skcipher *cipher;
	struct mutex io_lock;
	/* One cluster cache per mount, all I/O and crypto below io_lock */
	u8 *ciphertext;
	u8 *plaintext;
	u8 *pagebuf;
	unsigned long cached_ino;
	u32 cached_index;
};

struct sffs_inode_info {
	u16 *chain;
	u32 clusters;
	bool plaintext;
	struct inode vfs_inode;
};

struct sffs_context {
	u8 key[16];
	bool have_key;
};

static struct sffs_inode_info *SFFS_I(struct inode *inode)
{
	return container_of(inode, struct sffs_inode_info, vfs_inode);
}

#ifdef CONFIG_SFFS_BLOCK
static int sffs_block_read(struct super_block *sb, u64 pos, u8 *buf, size_t len)
{
	while (len) {
		struct buffer_head *bh;
		unsigned int offset = pos & (sb->s_blocksize - 1);
		size_t n = min_t(size_t, len, sb->s_blocksize - offset);

		bh = sb_bread(sb, pos >> sb->s_blocksize_bits);
		if (!bh)
			return -EIO;
		memcpy(buf, bh->b_data + offset, n);
		brelse(bh);
		pos += n;
		buf += n;
		len -= n;
	}
	return 0;
}
#endif

/* Caller serializes pagebuf use, including during initial superblock scan. */
static int sffs_read_page(struct super_block *sb, unsigned int page, u8 *out)
{
	struct sffs_sb_info *sbi = sb->s_fs_info;
	int ret;
	size_t done;
	u8 calculated[NINTENDO_NAND_ECC_SIZE];

	if (page >= SFFS_PAGES)
		return -EFSCORRUPTED;
#ifdef CONFIG_SFFS_MTD
	if (sb->s_mtd) {
		ret = mtd_read(sb->s_mtd, (loff_t)page * SFFS_PAGE,
			       SFFS_PAGE, &done, out);
		if (ret && !mtd_is_bitflip(ret))
			return ret;

		return done == SFFS_PAGE ? 0 : -EIO;
	}
#endif
#ifdef CONFIG_SFFS_BLOCK
	if (sb->s_bdev) {
		ret = sffs_block_read(sb, (u64)page * SFFS_RECORD,
				      sbi->pagebuf, SFFS_RECORD);
		if (ret)
			return ret;

		nintendo_nand_ecc_calculate(sbi->pagebuf, calculated);
		ret = nintendo_nand_ecc_correct(sbi->pagebuf,
					       sbi->pagebuf + SFFS_PAGE + 48, calculated);
		if (ret < 0)
			return ret;

		memcpy(out, sbi->pagebuf, SFFS_PAGE);
		return 0;
	}
#endif
	return -ENODEV;
}

static int sffs_find_super(struct super_block *sb)
{
	struct sffs_sb_info *sbi = sb->s_fs_info;
	struct sffs_metadata *candidate;
	u32 best = 0, start, generation;
	unsigned int l, slot, page;
	int ret = -EINVAL;
	const struct sffs_layout *layout;

	BUILD_BUG_ON(sizeof(*candidate) != SFFS_METADATA_SIZE);
	candidate = kvmalloc_obj(*candidate);
	if (!candidate)
		return -ENOMEM;

	for (l = 0; l < ARRAY_SIZE(sffs_layouts); l++) {
		layout = &sffs_layouts[l];

		for (slot = 0; slot < layout->generations; slot++) {
			start = layout->super_page + slot * (SFFS_METADATA_SIZE / SFFS_PAGE);

			ret = sffs_read_page(sb, start, (u8 *)candidate);
			if (ret || memcmp(candidate->magic, layout->magic, 4))
				continue;

			generation = be32_to_cpu(candidate->generation);
			if (sbi->layout && generation <= best)
				continue;

			for (page = 1; page < SFFS_METADATA_SIZE / SFFS_PAGE; page++) {
				ret = sffs_read_page(sb, start + page,
						     (u8 *)candidate + page * SFFS_PAGE);
				if (ret)
					break;
			}

			if (ret)
				continue;

			ret = sffs_validate(candidate, layout, sbi->parents);
			if (ret == -ENOMEM)
				goto out;
			if (ret)
				continue;

			memcpy(sbi->meta, candidate, sizeof(*candidate));
			sbi->layout = layout;
			best = generation;
		}
	}

	/* Rebuild parents, since rejected candidates may have overwritten them */
	ret = sbi->layout ? sffs_validate(sbi->meta, sbi->layout, sbi->parents) : -EINVAL;
out:
	kvfree(candidate);
	return ret;
}

static int sffs_load_cluster(struct inode *inode, u32 index)
{
	struct sffs_sb_info *sbi = inode->i_sb->s_fs_info;
	struct sffs_inode_info *info = SFFS_I(inode);
	struct skcipher_request *req;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist src, dst;
	u8 iv[16] = {};
	unsigned int i;
	int ret;

	if (index >= info->clusters)
		return -EFSCORRUPTED;
	if (sbi->cached_ino == inode->i_ino && sbi->cached_index == index)
		return 0;

	sbi->cached_ino = 0;
	for (i = 0; i < 8; i++) {
		ret = sffs_read_page(inode->i_sb, info->chain[index] * 8 + i,
				     sbi->ciphertext + i * SFFS_PAGE);
		if (ret)
			return ret;
	}
	if (info->plaintext)
		memcpy(sbi->plaintext, sbi->ciphertext, SFFS_CLUSTER_SIZE);
	else {
		req = skcipher_request_alloc(sbi->cipher, GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		sg_init_one(&src, sbi->ciphertext, SFFS_CLUSTER_SIZE);
		sg_init_one(&dst, sbi->plaintext, SFFS_CLUSTER_SIZE);
		skcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG |
					      CRYPTO_TFM_REQ_MAY_SLEEP, crypto_req_done, &wait);
		skcipher_request_set_crypt(req, &src, &dst, SFFS_CLUSTER_SIZE, iv);
		ret = crypto_wait_req(crypto_skcipher_decrypt(req), &wait);
		skcipher_request_free(req);
		if (ret)
			return ret;
	}

	sbi->cached_ino = inode->i_ino;
	sbi->cached_index = index;
	return 0;
}

static int sffs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct sffs_sb_info *sbi = inode->i_sb->s_fs_info;
	loff_t pos = folio_pos(folio);
	size_t filled = 0, len;
	u32 index;
	unsigned int offset;
	int ret = 0;

	mutex_lock(&sbi->io_lock);
	while (filled < folio_size(folio) && pos < inode->i_size) {
		index = pos / SFFS_CLUSTER_SIZE;
		offset = pos % SFFS_CLUSTER_SIZE;

		ret = sffs_load_cluster(inode, index);
		if (ret)
			break;

		len = min_t(size_t, SFFS_CLUSTER_SIZE - offset, folio_size(folio) - filled);
		len = min_t(loff_t, len, inode->i_size - pos);
		memcpy_to_folio(folio, filled, sbi->plaintext + offset, len);
		filled += len;
		pos += len;
	}
	mutex_unlock(&sbi->io_lock);

	if (filled < folio_size(folio))
		folio_zero_range(folio, filled, folio_size(folio) - filled);

	folio_end_read(folio, !ret);
	return ret;
}

static const struct address_space_operations sffs_aops = {
	.read_folio = sffs_read_folio,
};

static struct inode *sffs_iget(struct super_block *sb, unsigned int index);

static struct dentry *sffs_lookup(struct inode *dir, struct dentry *dentry,
				 unsigned int flags)
{
	struct sffs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct sffs_entry *e = &sbi->meta->entries[dir->i_ino - 1];
	struct inode *inode = NULL;
	u16 idx;

	if (dentry->d_name.len > 12)
		return ERR_PTR(-ENAMETOOLONG);

	for (idx = be16_to_cpu(e->sub); idx != SFFS_END;
	     idx = be16_to_cpu(sbi->meta->entries[idx].sib)) {
		e = &sbi->meta->entries[idx];
		if (sffs_namelen(e) == dentry->d_name.len &&
		    !memcmp(e->name, dentry->d_name.name, dentry->d_name.len)) {
			inode = sffs_iget(dir->i_sb, idx);
			break;
		}
	}
	return d_splice_alias(inode, dentry);
}

static int sffs_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct sffs_sb_info *sbi = dir->i_sb->s_fs_info;
	struct sffs_entry *e = &sbi->meta->entries[dir->i_ino - 1];
	u64 ordinal = 2;
	u16 idx;

	if (!dir_emit_dots(file, ctx))
		return 0;

	for (idx = be16_to_cpu(e->sub); idx != SFFS_END;
	     idx = be16_to_cpu(sbi->meta->entries[idx].sib), ordinal++) {
		e = &sbi->meta->entries[idx];
		if (ordinal < ctx->pos)
			continue;
		if (!dir_emit(ctx, e->name, sffs_namelen(e), idx + 1,
			      sffs_is_file(sbi->layout, e) ? DT_REG : DT_DIR))
			return 0;

		ctx->pos = ordinal + 1;
	}

	return 0;
}

static const struct inode_operations sffs_dir_iops = {
	.lookup = sffs_lookup,
};
static const struct file_operations sffs_dir_fops = {
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.iterate_shared = sffs_iterate,
};

static struct inode *sffs_iget(struct super_block *sb, unsigned int index)
{
	struct sffs_sb_info *sbi = sb->s_fs_info;
	struct sffs_entry *e = &sbi->meta->entries[index];
	struct sffs_inode_info *info;
	struct inode *inode;
	u16 cluster;
	u32 i;

	inode = iget_locked(sb, index + 1);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read_once(inode) & I_NEW))
		return inode;

	info = SFFS_I(inode);
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode_set_atime(inode, 0, 0);
	inode_set_mtime(inode, 0, 0);
	inode_set_ctime(inode, 0, 0);
	if (sffs_is_dir(sbi->layout, e)) {
		inode->i_mode = S_IFDIR | 0500;
		set_nlink(inode, 2);
		inode->i_op = &sffs_dir_iops;
		inode->i_fop = &sffs_dir_fops;
	} else {
		inode->i_mode = S_IFREG | 0400;
		inode->i_size = be32_to_cpu(e->size);
		inode->i_fop = &generic_ro_fops;
		inode->i_mapping->a_ops = &sffs_aops;

		info->clusters = DIV_ROUND_UP(inode->i_size, SFFS_CLUSTER_SIZE);
		if (info->clusters) {
			info->chain = kmalloc_array(info->clusters, sizeof(*info->chain), GFP_KERNEL);
			if (!info->chain) {
				iget_failed(inode);
				return ERR_PTR(-ENOMEM);
			}
		}

		cluster = be16_to_cpu(e->sub);
		for (i = 0; i < info->clusters; i++) {
			info->chain[i] = cluster;
			cluster = be16_to_cpu(sbi->meta->fat[cluster]);
		}

		/*
		 * The file scfm.img is used as a cache for the eMMC on the Wii U's SLC
		 * NAND, giving its name: SLC Cache For MLC. For performance reasons,
		 * IOSU stores this file in plaintext instead of encrypting it, since
		 * the contents it's caching are already encrypted. No metadata in the
		 * filesystem indicates this. For compatibility purposes, we reproduce
		 * the same behavior here.
		 */
		info->plaintext = sbi->layout->slc && sbi->parents[index] == 0 &&
			sffs_namelen(e) == 8 && !memcmp(e->name, "scfm.img", 8);
		inode->i_blocks = (u64)info->clusters * (SFFS_CLUSTER_SIZE / 512);
	}

	unlock_new_inode(inode);
	return inode;
}

static struct inode *sffs_alloc_inode(struct super_block *sb)
{
	struct sffs_inode_info *info = kzalloc_obj(*info);

	if (!info)
		return NULL;

	inode_init_once(&info->vfs_inode);
	return &info->vfs_inode;
}

static void sffs_free_inode(struct inode *inode)
{
	kfree(SFFS_I(inode));
}

static void sffs_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	kfree(SFFS_I(inode)->chain);
}

static void sffs_free_info(struct sffs_sb_info *sbi)
{
	if (!sbi)
		return;
	if (sbi->cipher)
		crypto_free_skcipher(sbi->cipher);

	kvfree(sbi->meta);
	kfree(sbi->parents);
	kfree_sensitive(sbi->plaintext);
	kfree_sensitive(sbi->ciphertext);
	kfree(sbi->pagebuf);
	kfree_sensitive(sbi);
}

static void sffs_put_super(struct super_block *sb)
{
	sffs_free_info(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static int sffs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct sffs_sb_info *sbi = dentry->d_sb->s_fs_info;
	unsigned int i;

	buf->f_type = SFFS_MAGIC;
	buf->f_bsize = SFFS_CLUSTER_SIZE;
	buf->f_blocks = sbi->layout->super_page / 8 - sbi->layout->first_cluster;
	buf->f_namelen = 12;
	for (i = 0; i < SFFS_ENTRIES; i++)
		if (sbi->parents[i] != SFFS_END)
			buf->f_files++;

	return 0;
}

static const struct super_operations sffs_sops = {
	.alloc_inode = sffs_alloc_inode,
	.free_inode = sffs_free_inode,
	.evict_inode = sffs_evict_inode,
	.put_super = sffs_put_super,
	.statfs = sffs_statfs,
};

static int sffs_get_otp_key(struct super_block *sb, u8 output[16])
{
#ifdef CONFIG_SFFS_MTD
	struct nvmem_cell *cell;
	void *key;
	size_t len;

	if (!sb->s_mtd)
		return -ENOKEY;
	cell = of_nvmem_cell_get(mtd_get_of_node(sb->s_mtd), "nand-key");
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	key = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(key))
		return PTR_ERR(key);
	if (len == 16)
		memcpy(output, key, len);
	kfree_sensitive(key);
	return len == 16 ? 0 : -EINVAL;
#else
	return -ENOKEY;
#endif
}

static int sffs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct sffs_context *ctx = fc->fs_private;
	struct sffs_sb_info *sbi;
	struct inode *root;
	int ret;
#ifdef CONFIG_SFFS_BLOCK
	u64 size;
#endif

	sbi = kzalloc_obj(*sbi);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sb->s_op = &sffs_sops;
	sb->s_magic = SFFS_MAGIC;
	sb->s_maxbytes = SZ_512M;
	sb->s_flags |= SB_RDONLY | SB_NOATIME;
	mutex_init(&sbi->io_lock);
#ifdef CONFIG_SFFS_MTD
	if (sb->s_mtd && (sb->s_mtd->size != SZ_512M ||
			  sb->s_mtd->writesize != SFFS_PAGE ||
			  sb->s_mtd->oobsize != 64))
		return -EINVAL;
#endif
#ifdef CONFIG_SFFS_BLOCK
	if (sb->s_bdev) {
		size = bdev_nr_bytes(sb->s_bdev);

		if (size != SFFS_IMAGE_SIZE && size != SFFS_IMAGE_SIZE + 1024)
			return -EINVAL;
		if (!sb_set_blocksize(sb, 1024))
			return -EINVAL;
	}
#endif
	sbi->meta = kvmalloc(sizeof(*sbi->meta), GFP_KERNEL);
	sbi->parents = kmalloc_array(SFFS_ENTRIES, sizeof(*sbi->parents), GFP_KERNEL);
	sbi->pagebuf = kmalloc(SFFS_RECORD, GFP_KERNEL);
	sbi->ciphertext = kmalloc(SFFS_CLUSTER_SIZE, GFP_KERNEL);
	sbi->plaintext = kmalloc(SFFS_CLUSTER_SIZE, GFP_KERNEL);
	if (!sbi->meta || !sbi->parents || !sbi->pagebuf ||
	    !sbi->ciphertext || !sbi->plaintext)
		return -ENOMEM;

	ret = sffs_find_super(sb);
	if (ret)
		return ret;

	if (ctx->have_key)
		memcpy(sbi->key, ctx->key, sizeof(sbi->key));
	else {
		ret = sffs_get_otp_key(sb, sbi->key);
		if (ret)
			return ret;
	}

	sbi->cipher = crypto_alloc_skcipher("cbc(aes)", 0, 0);
	if (IS_ERR(sbi->cipher)) {
		ret = PTR_ERR(sbi->cipher);
		sbi->cipher = NULL;
		return ret;
	}
	ret = crypto_skcipher_setkey(sbi->cipher, sbi->key, sizeof(sbi->key));
	if (ret)
		return ret;

	root = sffs_iget(sb, 0);
	if (IS_ERR(root))
		return PTR_ERR(root);

	sb->s_root = d_make_root(root);
	return sb->s_root ? 0 : -ENOMEM;
}

static const struct fs_parameter_spec sffs_parameters[] = {
	fsparam_string("key", 0),
	{}
};

static int sffs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct sffs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	int ret = fs_parse(fc, sffs_parameters, param, &result);

	if (ret < 0)
		return ret;
	if (strlen(param->string) != 32 || hex2bin(ctx->key, param->string, 16))
		return invalf(fc, "SFFS key must contain exactly 32 hexadecimal digits");

	ctx->have_key = true;
	return 0;
}

static int sffs_check_key(struct fs_context *fc)
{
	struct sffs_context *ctx = fc->fs_private;
	struct sffs_sb_info *sbi = fc->root->d_sb->s_fs_info;
	u8 key[16];
	int ret;

	if (ctx->have_key)
		return crypto_memneq(ctx->key, sbi->key, 16) ? -EBUSY : 0;

	ret = sffs_get_otp_key(fc->root->d_sb, key);
	if (!ret && crypto_memneq(key, sbi->key, sizeof(key)))
		ret = -EBUSY;

	memzero_explicit(key, sizeof(key));
	return ret;
}

static int sffs_get_tree(struct fs_context *fc)
{
	struct super_block *sb;
#ifdef CONFIG_SFFS_BLOCK
	struct sffs_context *ctx;
#endif
	int ret = -EOPNOTSUPP;

	fc->sb_flags |= SB_RDONLY | SB_NOATIME;
#ifdef CONFIG_SFFS_MTD
	if (fc->source && !strncmp(fc->source, "mtd", 3))
		ret = get_tree_mtd(fc, sffs_fill_super);
	else
#endif
#ifdef CONFIG_SFFS_BLOCK
	{
		ctx = fc->fs_private;

		if (!ctx->have_key)
			return -ENOKEY;
		ret = get_tree_bdev(fc, sffs_fill_super);
	}
#else
		ret = -EOPNOTSUPP;
#endif
	if (!ret) {
		ret = sffs_check_key(fc);
		if (ret) {
			sb = fc->root->d_sb;

			dput(fc->root);
			fc->root = NULL;
			deactivate_locked_super(sb);
		}
	}
	return ret;
}

static int sffs_reconfigure(struct fs_context *fc)
{
	if (!(fc->sb_flags & SB_RDONLY))
		return -EROFS;
	if (((struct sffs_context *)fc->fs_private)->have_key)
		return sffs_check_key(fc);

	return 0;
}

static void sffs_free_context(struct fs_context *fc)
{
	kfree_sensitive(fc->fs_private);
}

static const struct fs_context_operations sffs_context_ops = {
	.free = sffs_free_context,
	.parse_param = sffs_parse_param,
	.get_tree = sffs_get_tree,
	.reconfigure = sffs_reconfigure,
};

static int sffs_init_context(struct fs_context *fc)
{
	fc->fs_private = kzalloc_obj(struct sffs_context);
	if (!fc->fs_private)
		return -ENOMEM;

	fc->ops = &sffs_context_ops;
	return 0;
}

static void sffs_kill_sb(struct super_block *sb)
{
	/* Fill failures before s_root exists do not call put_super */
	bool mounted = !!sb->s_root;

	generic_shutdown_super(sb);
	if (!mounted)
		sffs_put_super(sb);
#ifdef CONFIG_SFFS_MTD
	if (sb->s_mtd) {
		put_mtd_device(sb->s_mtd);
		sb->s_mtd = NULL;
	}
#endif
#ifdef CONFIG_SFFS_BLOCK
	if (sb->s_bdev) {
		sync_blockdev(sb->s_bdev);
		bdev_fput(sb->s_bdev_file);
	}
#endif
}

static struct file_system_type sffs_type = {
	.owner = THIS_MODULE,
	.name = "sffs",
	.init_fs_context = sffs_init_context,
	.parameters = sffs_parameters,
	.kill_sb = sffs_kill_sb,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init sffs_init(void)
{
	return register_filesystem(&sffs_type);
}
static void __exit sffs_exit(void)
{
	unregister_filesystem(&sffs_type);
	rcu_barrier();
}
module_init(sffs_init);
module_exit(sffs_exit);
MODULE_DESCRIPTION("Read-only BroadOn/Nintendo SFFS filesystem and raw NAND images");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_ALIAS_FS("sffs");
