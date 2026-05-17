// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nintendo Flipper/Hollywood Drive Interface block driver.
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define HLWD_DI_NAME			"hlwd-di"

#define DI_SR				0x00
#define DI_CVR				0x04
#define DI_CMDBUF0			0x08
#define DI_CMDBUF1			0x0c
#define DI_CMDBUF2			0x10
#define DI_MAR				0x14
#define DI_LENGTH			0x18
#define DI_CR				0x1c
#define DI_IMMBUF			0x20

#define DI_CMD_INQUIRY			0x12000000
#define DI_CMD_READ_DISC_ID		0xa8000040
#define DI_CMD_READ_NORMAL		0xa8000000
#define DI_CMD_READ_DVDR		0xd0000000
#define DI_CMD_READ_PHYSINFO		0xad000000
#define DI_CMD_GET_STATUS		0xe0000000

#define DI_SR_DEINTMASK		BIT(1)
#define DI_SR_DEINT			BIT(2)
#define DI_SR_TCINTMASK		BIT(3)
#define DI_SR_TCINT			BIT(4)
#define DI_SR_BRKINTMASK		BIT(5)
#define DI_SR_BRKINT			BIT(6)
#define DI_SR_INTMASKS			(DI_SR_DEINTMASK | DI_SR_TCINTMASK | \
					 DI_SR_BRKINTMASK)
#define DI_SR_INTS			(DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT)

#define DI_CVR_CVR			BIT(0)
#define DI_CVR_CVRINTMASK		BIT(1)
#define DI_CVR_CVRINT			BIT(2)

#define DI_CR_TSTART			BIT(0)
#define DI_CR_DMA			BIT(1)

#define DI_STATUS_LID_OPEN		0x01
#define DI_STATUS_DISC_CHANGED		0x02
#define DI_STATUS_NO_DISC		0x03

#define DI_ERROR_COVER_OPENED		0x023a00
#define DI_ERROR_NORMAL_READ_ON_DVDR	0x053000
#define DI_ERROR_MEDIA_CHANGED		0x062800

#define DI_STATUS_GET_STATUS(status)	((u8)(((status) & 0xff000000) >> 24))
#define DI_STATUS_GET_ERR(status)	((status) & 0x00ffffff)

#define DVD_BLOCK_SIZE			2048
#define ISO9660_PVD_SECTOR		16
#define ISO9660_VOLUME_SPACE_SIZE	80
#define DI_DMA_BUF_SIZE		(32 * 1024)
#define DI_TIMEOUT_STATUS_US		(5 * USEC_PER_SEC)
#define DI_TIMEOUT_MEDIA_US		(30 * USEC_PER_SEC)
#define DI_TIMEOUT_READ_US		(30 * USEC_PER_SEC)
#define DI_MEDIA_SETTLE_MS		1500
#define DI_MEDIA_POLL_MS		200
#define DI_MEDIA_RETRY_MS		250
#define WII_SL_DISC_SIZE		4699979776ULL

#define HW_CTRL_COMPATIBLE		"nintendo,hollywood-control"
#define HW_CTRL_RESETS			0x94
#define HW_RESETS_RSTB_DIRSTB		BIT(10)

struct di_inquiry_response {
	__be32 unk;
	__be32 date;
	__be32 pad[6];
};

struct di_disc_id {
	u8 bytes[32];
};

struct di_phys_format_info {
	u8 disc_category_and_version;
	u8 disc_size_and_rate;
	u8 disc_structure;
	u8 recording_density;
	__be32 first_data_psn;
	__be32 last_data_psn;
	u8 pad[2036];
} __packed;

enum di_media_state {
	DI_MEDIA_EMPTY = 0,
	DI_MEDIA_ORIGINAL,
	DI_MEDIA_DVDR_REJECTED,
	DI_MEDIA_UNKNOWN,
};

struct hlwd_di {
	struct device *dev;
	void __iomem *regs;
	void __iomem *ctrl;
	struct gpio_desc *di_spin_gpio;

	struct mutex lock; /* serializes DI commands and media state */
	struct delayed_work media_work;

	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	bool disk_added;
	bool media_present;
	bool media_validated;
	bool using_dvdr_read;
	bool media_changed;
	u32 last_cvr;
	u32 drive_date;
	u64 media_size;

	void *dma_buf;
	dma_addr_t dma_addr;
};

static int hlwd_di_major;
static unsigned long long dvdr_size_bytes;

module_param_named(dvdr_size_bytes, dvdr_size_bytes, ullong, 0644);
MODULE_PARM_DESC(dvdr_size_bytes,
		 "Override DVD-R media size in bytes; 0=auto, capped by physical size");

static inline u32 di_read(struct hlwd_di *di, u32 reg)
{
	return ioread32be(di->regs + reg);
}

static inline void di_write(struct hlwd_di *di, u32 reg, u32 val)
{
	iowrite32be(val, di->regs + reg);
}

static void di_ack_sr(struct hlwd_di *di, u32 ints)
{
	u32 sr = di_read(di, DI_SR);

	di_write(di, DI_SR, (sr & DI_SR_INTMASKS) | (ints & DI_SR_INTS));
}

static int di_wait_idle(struct hlwd_di *di, unsigned long timeout_us)
{
	ktime_t timeout = ktime_add_us(ktime_get(), timeout_us);

	while (di_read(di, DI_CR) & DI_CR_TSTART) {
		if (ktime_after(ktime_get(), timeout))
			return -ETIMEDOUT;
		usleep_range(1000, 2000);
	}

	return 0;
}

static int di_do_cmd(struct hlwd_di *di, u32 cmdbuf0, u32 cmdbuf1, u32 cmdbuf2,
		     size_t data_len, unsigned long timeout_us)
{
	u32 sr, residual;
	int ret;

	if (data_len > DI_DMA_BUF_SIZE)
		return -EINVAL;

	ret = di_wait_idle(di, timeout_us);
	if (ret)
		return ret;

	di_ack_sr(di, DI_SR_INTS);

	di_write(di, DI_CMDBUF0, cmdbuf0);
	di_write(di, DI_CMDBUF1, cmdbuf1);
	di_write(di, DI_CMDBUF2, cmdbuf2);
	di_write(di, DI_MAR, data_len ? lower_32_bits(di->dma_addr) : 0);
	di_write(di, DI_LENGTH, data_len);
	di_write(di, DI_CR, DI_CR_TSTART | (data_len ? DI_CR_DMA : 0));

	ret = di_wait_idle(di, timeout_us);
	if (ret) {
		dev_err(di->dev, "command %08x %08x %08x timed out\n",
			cmdbuf0, cmdbuf1, cmdbuf2);
		return ret;
	}

	sr = di_read(di, DI_SR) & DI_SR_INTS;
	di_ack_sr(di, sr);

	if (sr & DI_SR_BRKINT)
		return -EINTR;
	if (sr & DI_SR_DEINT)
		return -EIO;
	if (data_len) {
		residual = di_read(di, DI_LENGTH);
		if (residual) {
			dev_dbg(di->dev, "command %08x DMA residual %u/%zu\n",
				cmdbuf0, residual, data_len);
			return -EIO;
		}
	}

	return 0;
}

static int di_get_status_raw(struct hlwd_di *di, u32 *status)
{
	int ret;

	di_write(di, DI_IMMBUF, 0);
	ret = di_do_cmd(di, DI_CMD_GET_STATUS, 0, 0, 0, DI_TIMEOUT_STATUS_US);
	if (ret)
		return ret;

	*status = di_read(di, DI_IMMBUF);
	return 0;
}

static bool di_status_is_no_disc(u32 status)
{
	return DI_STATUS_GET_STATUS(status) == DI_STATUS_NO_DISC ||
	       DI_STATUS_GET_STATUS(status) == DI_STATUS_LID_OPEN ||
	       DI_STATUS_GET_ERR(status) == DI_ERROR_COVER_OPENED;
}

static bool di_status_is_media_changed(u32 status)
{
	return DI_STATUS_GET_STATUS(status) == DI_STATUS_DISC_CHANGED ||
	       DI_STATUS_GET_ERR(status) == DI_ERROR_MEDIA_CHANGED;
}

static const char *di_wii_drive_rev(u32 date)
{
	switch (date) {
	case 0x20060526:
		return "DMS or D2A";
	case 0x20060907:
		return "D2B";
	case 0x20070213:
		return "D2C or D2E";
	case 0x20080714:
		return "D3 or D3-2";
	case 0x20081218:
		return "D4v1";
	case 0x20091121:
	case 0x20101207:
		return "D4v2";
	case 0x20120629:
		return "Wii Mini Disc Drive?";
	case 0x20110628:
		return "Wii U Drive 1";
	case 0x20120712:
		return "Wii U Drive 2";
	default:
		return "unknown";
	}
}

static bool di_buf_interesting(const u8 *buf, size_t len)
{
	bool all_zero = true, all_ff = true;
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i])
			all_zero = false;
		if (buf[i] != 0xff)
			all_ff = false;
		if (!all_zero && !all_ff)
			return true;
	}

	return false;
}

static int di_read_raw_locked(struct hlwd_di *di, size_t len, u64 off)
{
	u32 cmd, cmdbuf1, cmdbuf2;

	if ((off & (DVD_BLOCK_SIZE - 1)) || (len & (DVD_BLOCK_SIZE - 1)))
		return -EINVAL;
	if (len > DI_DMA_BUF_SIZE)
		return -EINVAL;

	if (!di->using_dvdr_read) {
		if (off >> 2 > U32_MAX)
			return -EINVAL;
		cmd = DI_CMD_READ_NORMAL;
		cmdbuf1 = (u32)(off >> 2);
		cmdbuf2 = (u32)len;
	} else {
		cmd = DI_CMD_READ_DVDR;
		cmdbuf1 = (u32)(off / DVD_BLOCK_SIZE);
		cmdbuf2 = (u32)(len / DVD_BLOCK_SIZE);
	}

	return di_do_cmd(di, cmd, cmdbuf1, cmdbuf2, len, DI_TIMEOUT_READ_US);
}

static int di_validate_media_read_locked(struct hlwd_di *di)
{
	bool sector0_ok = false;
	int ret;

	memset(di->dma_buf, 0, DVD_BLOCK_SIZE);
	ret = di_read_raw_locked(di, DVD_BLOCK_SIZE, 0);
	if (!ret && di_buf_interesting(di->dma_buf, DVD_BLOCK_SIZE))
		sector0_ok = true;

	memset(di->dma_buf, 0, DVD_BLOCK_SIZE);
	ret = di_read_raw_locked(di, DVD_BLOCK_SIZE, 16 * DVD_BLOCK_SIZE);
	if (!ret && di_buf_interesting(di->dma_buf, DVD_BLOCK_SIZE)) {
		di->media_validated = true;
		return 0;
	}
	if (sector0_ok) {
		di->media_validated = true;
		return 0;
	}

	di->media_validated = false;
	return ret ?: -EIO;
}

static enum di_media_state di_classify_probe(int read_id_ret, u32 status,
					     const struct di_disc_id *disc_id)
{
	if (!read_id_ret) {
		pr_info("disc id %02x%02x%02x%02x%02x%02x\n",
			disc_id->bytes[0], disc_id->bytes[1], disc_id->bytes[2],
			disc_id->bytes[3], disc_id->bytes[4], disc_id->bytes[5]);
		return DI_MEDIA_ORIGINAL;
	}

	if (DI_STATUS_GET_ERR(status) == DI_ERROR_NORMAL_READ_ON_DVDR)
		return DI_MEDIA_DVDR_REJECTED;
	if (di_status_is_no_disc(status))
		return DI_MEDIA_EMPTY;
	if (di_status_is_media_changed(status))
		return DI_MEDIA_UNKNOWN;

	return DI_MEDIA_UNKNOWN;
}

static int di_read_disc_size_locked(struct hlwd_di *di, u64 *size)
{
	struct di_phys_format_info *info = di->dma_buf;
	u32 first, last;
	int ret;

	memset(info, 0, sizeof(*info));
	ret = di_do_cmd(di, DI_CMD_READ_PHYSINFO, 0, 0, sizeof(*info),
			DI_TIMEOUT_MEDIA_US);
	if (ret) {
		*size = WII_SL_DISC_SIZE;
		dev_info(di->dev, "using fallback Wii single-layer size: %llu bytes\n",
			 *size);
		return 0;
	}

	first = be32_to_cpu(info->first_data_psn);
	last = be32_to_cpu(info->last_data_psn);
	if (last < first || (!di->using_dvdr_read && !first && !last)) {
		*size = WII_SL_DISC_SIZE;
		dev_info(di->dev, "using fallback Wii single-layer size: %llu bytes\n",
			 *size);
		return 0;
	}

	*size = (u64)(last - first + 1) * DVD_BLOCK_SIZE;
	dev_info(di->dev, "disc format first PSN=%08x last PSN=%08x size=%llu\n",
		 first, last, *size);
	return 0;
}

static u64 di_cap_disc_size(struct hlwd_di *di, u64 size, u64 max_size,
			    const char *source)
{
	size = round_down(size, DVD_BLOCK_SIZE);
	if (size > max_size) {
		dev_warn(di->dev, "%s size %llu exceeds physical size %llu, capping\n",
			 source, size, max_size);
		size = max_size;
	}

	return size;
}

static int di_read_iso9660_size_locked(struct hlwd_di *di, u64 max_size,
				       u64 *size)
{
	const u8 *pvd = di->dma_buf;
	u32 le_blocks, be_blocks;
	int ret;

	memset(di->dma_buf, 0, DVD_BLOCK_SIZE);
	ret = di_read_raw_locked(di, DVD_BLOCK_SIZE,
				 ISO9660_PVD_SECTOR * DVD_BLOCK_SIZE);
	if (ret)
		return ret;

	if (pvd[0] != 1 || memcmp(&pvd[1], "CD001", 5) || pvd[6] != 1)
		return -EINVAL;

	le_blocks = get_unaligned_le32(&pvd[ISO9660_VOLUME_SPACE_SIZE]);
	be_blocks = get_unaligned_be32(&pvd[ISO9660_VOLUME_SPACE_SIZE + 4]);
	if (!le_blocks || le_blocks != be_blocks)
		return -EINVAL;

	*size = di_cap_disc_size(di, (u64)le_blocks * DVD_BLOCK_SIZE,
				 max_size, "ISO9660");
	dev_info(di->dev, "DVD-R ISO9660 volume space: %u blocks, size=%llu\n",
		 le_blocks, *size);
	return 0;
}

static int di_read_dvdr_size_locked(struct hlwd_di *di, u64 *size)
{
	u64 phys_size;
	int ret;

	ret = di_read_disc_size_locked(di, &phys_size);
	if (ret)
		return ret;
	if (phys_size < DVD_BLOCK_SIZE)
		return -EIO;

	if (dvdr_size_bytes) {
		*size = di_cap_disc_size(di, dvdr_size_bytes, phys_size,
					 "DVD-R override");
		dev_info(di->dev, "using DVD-R override size: %llu bytes\n",
			 *size);
		return 0;
	}

	ret = di_read_iso9660_size_locked(di, phys_size, size);
	if (!ret)
		return 0;

	*size = phys_size;
	dev_info(di->dev, "DVD-R ISO9660 sizing failed (%d), using physical size: %llu bytes\n",
		 ret, *size);
	return 0;
}

static void di_set_capacity_locked(struct hlwd_di *di, u64 bytes)
{
	sector_t sectors = bytes >> SECTOR_SHIFT;

	di->media_size = bytes;
	if (di->disk_added)
		set_capacity_and_notify(di->disk, sectors);
	else
		set_capacity(di->disk, sectors);
}

static void di_clear_media_locked(struct hlwd_di *di)
{
	di->media_present = false;
	di->media_validated = false;
	di->using_dvdr_read = false;
	di_set_capacity_locked(di, 0);
}

static int di_register_media_locked(struct hlwd_di *di)
{
	u64 size;
	int ret;

	if (!di->media_validated)
		return -ENOMEDIUM;

	if (di->using_dvdr_read)
		ret = di_read_dvdr_size_locked(di, &size);
	else
		ret = di_read_disc_size_locked(di, &size);
	if (ret)
		return ret;
	if (!size)
		return -EIO;

	di->media_present = true;
	di_set_capacity_locked(di, size);
	return 0;
}

static int di_probe_media_locked(struct hlwd_di *di)
{
	struct di_disc_id *disc_id = di->dma_buf;
	enum di_media_state media;
	unsigned long settle_until;
	u32 status = 0;
	int ret;

	settle_until = jiffies + msecs_to_jiffies(DI_MEDIA_SETTLE_MS);

again:
	memset(disc_id, 0, sizeof(*disc_id));
	di->media_validated = false;

	ret = di_do_cmd(di, DI_CMD_READ_DISC_ID, 0, sizeof(*disc_id),
			sizeof(*disc_id), DI_TIMEOUT_MEDIA_US);
	di_get_status_raw(di, &status);
	media = di_classify_probe(ret, status, disc_id);

	dev_info(di->dev, "drive status %08x, read id ret=%d, media=%d\n",
		 status, ret, media);

	if (media == DI_MEDIA_DVDR_REJECTED) {
		dev_info(di->dev, "DVD-R read commands required\n");
		di->using_dvdr_read = true;
		ret = di_validate_media_read_locked(di);
		if (ret)
			goto err_clear;
		return di_register_media_locked(di);
	}

	if (media == DI_MEDIA_ORIGINAL) {
		di->using_dvdr_read = false;
		ret = di_validate_media_read_locked(di);
		if (ret)
			goto err_clear;
		return di_register_media_locked(di);
	}

	if (media == DI_MEDIA_EMPTY) {
		dev_info(di->dev, "no disc present\n");
		ret = -ENOMEDIUM;
		goto err_clear;
	}

	if (di_status_is_media_changed(status) && time_before(jiffies, settle_until)) {
		msleep(DI_MEDIA_RETRY_MS);
		goto again;
	}

	ret = ret ?: -EAGAIN;

err_clear:
	di_clear_media_locked(di);
	return ret;
}

static int di_wait_for_status_locked(struct hlwd_di *di, const char *ctx,
				     u32 *status)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(30000);
	int ret;

	do {
		ret = di_get_status_raw(di, status);
		if (!ret)
			return 0;
		usleep_range(10000, 11000);
	} while (time_before(jiffies, timeout));

	dev_err(di->dev, "%s: drive did not become ready, ret=%d CR=%08x SR=%08x\n",
		ctx, ret, di_read(di, DI_CR), di_read(di, DI_SR));
	return ret ?: -ETIMEDOUT;
}

static int di_reset_locked(struct hlwd_di *di)
{
	unsigned long timeout;
	u32 val;

	if (!di->ctrl)
		return -ENODEV;

	if (di->di_spin_gpio)
		gpiod_set_value_cansleep(di->di_spin_gpio, 0);

	val = ioread32be(di->ctrl + HW_CTRL_RESETS);
	iowrite32be(val & ~HW_RESETS_RSTB_DIRSTB, di->ctrl + HW_CTRL_RESETS);
	usleep_range(1000, 2000);
	val = ioread32be(di->ctrl + HW_CTRL_RESETS);
	iowrite32be(val | HW_RESETS_RSTB_DIRSTB, di->ctrl + HW_CTRL_RESETS);

	timeout = jiffies + msecs_to_jiffies(1000);
	while (!(di_read(di, DI_SR) & DI_SR_TCINTMASK)) {
		di_write(di, DI_SR,
			 (di_read(di, DI_SR) & ~DI_SR_INTS) | DI_SR_TCINTMASK);
		if (time_after(jiffies, timeout)) {
			dev_err(di->dev, "DI registers did not accept TCINTMASK, SR=%08x\n",
				di_read(di, DI_SR));
			return -ETIMEDOUT;
		}
		usleep_range(1000, 2000);
	}

	di_write(di, DI_SR, di_read(di, DI_SR) & ~(DI_SR_INTS | DI_SR_INTMASKS));
	di_write(di, DI_CVR, di_read(di, DI_CVR) & ~(DI_CVR_CVRINTMASK |
						     DI_CVR_CVRINT));
	di_write(di, DI_SR, (di_read(di, DI_SR) & DI_SR_INTMASKS) | DI_SR_INTS);
	di_write(di, DI_CVR, di_read(di, DI_CVR) | DI_CVR_CVRINT);
	return 0;
}

static int di_init_drive_locked(struct hlwd_di *di)
{
	struct di_inquiry_response *resp = di->dma_buf;
	u32 raw_status;
	int ret;

	ret = di_reset_locked(di);
	if (ret)
		return ret;

	ret = di_wait_for_status_locked(di, "init reset", &raw_status);
	if (ret)
		return ret;

	memset(resp, 0, sizeof(*resp));
	ret = di_do_cmd(di, DI_CMD_INQUIRY, 0, sizeof(*resp), sizeof(*resp),
			DI_TIMEOUT_STATUS_US);
	if (!ret) {
		di->drive_date = be32_to_cpu(resp->date);
		dev_info(di->dev, "drive date %08x (%s)\n", di->drive_date,
			 di_wii_drive_rev(di->drive_date));
	} else {
		dev_warn(di->dev, "drive inquiry failed: %d\n", ret);
	}

	di->last_cvr = di_read(di, DI_CVR);
	return 0;
}

static void di_copy_to_bvec(const struct bio_vec *bvec, unsigned int off,
			    const void *src, unsigned int len)
{
	void *dst = kmap_local_page(bvec->bv_page);

	memcpy((u8 *)dst + bvec->bv_offset + off, src, len);
	kunmap_local(dst);
}

static blk_status_t di_read_request_locked(struct hlwd_di *di,
					   struct request *rq)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	unsigned int bytes = blk_rq_bytes(rq);
	unsigned int bytes_left = bytes;
	unsigned int bounce_off = 0;
	unsigned int bounce_len = 0;
	u64 read_pos = pos;
	blk_status_t status = BLK_STS_OK;

	if (!di->media_present) {
		status = BLK_STS_IOERR;
		goto out;
	}
	if ((pos & (DVD_BLOCK_SIZE - 1)) || (bytes & (DVD_BLOCK_SIZE - 1))) {
		status = BLK_STS_IOERR;
		goto out;
	}
	if (pos + bytes > di->media_size) {
		status = BLK_STS_IOERR;
		goto out;
	}

	rq_for_each_segment(bvec, rq, iter) {
		unsigned int done = 0;

		while (done < bvec.bv_len) {
			unsigned int copy_len;

			if (bounce_off == bounce_len) {
				unsigned int chunk = min(bytes_left, DI_DMA_BUF_SIZE);
				int ret;

				chunk = round_down(chunk, DVD_BLOCK_SIZE);
				if (!chunk) {
					status = BLK_STS_IOERR;
					goto out;
				}

				ret = di_read_raw_locked(di, chunk, read_pos);
				if (ret == -ETIMEDOUT) {
					status = BLK_STS_TIMEOUT;
					goto out;
				}
				if (ret) {
					status = BLK_STS_IOERR;
					goto out;
				}

				read_pos += chunk;
				bytes_left -= chunk;
				bounce_off = 0;
				bounce_len = chunk;
			}

			copy_len = min(bvec.bv_len - done, bounce_len - bounce_off);
			di_copy_to_bvec(&bvec, done, (u8 *)di->dma_buf + bounce_off,
					copy_len);
			bounce_off += copy_len;
			done += copy_len;
		}
	}

out:
	if (status != BLK_STS_OK)
		dev_warn_ratelimited(di->dev,
				     "read failed: pos=%llu bytes=%u read_pos=%llu left=%u status=%u\n",
				     pos, bytes, read_pos, bytes_left, status);
	return status;
}

static blk_status_t hlwd_di_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct hlwd_di *di = hctx->queue->queuedata;
	blk_status_t status;

	blk_mq_start_request(rq);

	if (blk_rq_is_passthrough(rq) || req_op(rq) != REQ_OP_READ) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	mutex_lock(&di->lock);
	status = di_read_request_locked(di, rq);
	mutex_unlock(&di->lock);

	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops hlwd_di_mq_ops = {
	.queue_rq = hlwd_di_queue_rq,
};

static int hlwd_di_open(struct gendisk *disk, blk_mode_t mode)
{
	struct hlwd_di *di = disk->private_data;
	int ret = 0;

	if (mode & BLK_OPEN_WRITE)
		return -EROFS;

	mutex_lock(&di->lock);
	if (!di->media_present && !(di_read(di, DI_CVR) & DI_CVR_CVR))
		ret = di_probe_media_locked(di);
	if (ret == -ENOMEDIUM)
		ret = -ENOMEDIUM;
	else if (ret == -EAGAIN)
		ret = -ENOMEDIUM;
	mutex_unlock(&di->lock);

	return ret;
}

static unsigned int hlwd_di_check_events(struct gendisk *disk,
					 unsigned int clearing)
{
	struct hlwd_di *di = disk->private_data;
	unsigned int events = 0;

	mutex_lock(&di->lock);
	if (di->media_changed) {
		events = DISK_EVENT_MEDIA_CHANGE;
		if (clearing)
			di->media_changed = false;
	}
	mutex_unlock(&di->lock);

	return events;
}

static const struct block_device_operations hlwd_di_fops = {
	.owner		= THIS_MODULE,
	.open		= hlwd_di_open,
	.check_events	= hlwd_di_check_events,
};

static void hlwd_di_media_work(struct work_struct *work)
{
	struct hlwd_di *di = container_of(to_delayed_work(work), struct hlwd_di,
					 media_work);
	bool was_open, is_open;
	u32 cvr;

	mutex_lock(&di->lock);

	cvr = di_read(di, DI_CVR);
	was_open = !!(di->last_cvr & DI_CVR_CVR);
	is_open = !!(cvr & DI_CVR_CVR);
	di->last_cvr = cvr;

	if (was_open != is_open) {
		dev_info(di->dev, "cover %s\n", is_open ? "open" : "closed");
		di->media_changed = true;
		if (is_open) {
			di_clear_media_locked(di);
		} else {
			if (!di_reset_locked(di))
				di_probe_media_locked(di);
		}
		disk_force_media_change(di->disk);
	}

	mutex_unlock(&di->lock);
	schedule_delayed_work(&di->media_work, msecs_to_jiffies(DI_MEDIA_POLL_MS));
}

static void __iomem *hlwd_di_iomap_compatible(const char *compatible)
{
	struct device_node *np;
	void __iomem *base;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return NULL;

	base = of_iomap(np, 0);
	of_node_put(np);
	return base;
}

static int hlwd_di_probe(struct platform_device *pdev)
{
	struct queue_limits lim = {
		.logical_block_size	= DVD_BLOCK_SIZE,
		.physical_block_size	= DVD_BLOCK_SIZE,
		.io_min			= DVD_BLOCK_SIZE,
		.dma_alignment		= 31,
		.max_hw_sectors		= DI_DMA_BUF_SIZE >> SECTOR_SHIFT,
		.max_segments		= 1,
		.max_segment_size	= DI_DMA_BUF_SIZE,
		.features		= BLK_FEAT_ROTATIONAL,
	};
	struct device *dev = &pdev->dev;
	struct hlwd_di *di;
	int ret;

	di = devm_kzalloc(dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	di->dev = dev;
	mutex_init(&di->lock);
	platform_set_drvdata(pdev, di);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	di->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(di->regs))
		return PTR_ERR(di->regs);

	di->ctrl = hlwd_di_iomap_compatible(HW_CTRL_COMPATIBLE);
	if (!di->ctrl)
		return dev_err_probe(dev, -ENODEV,
				     "missing %s node for reset control\n",
				     HW_CTRL_COMPATIBLE);

	di->di_spin_gpio = devm_gpiod_get_optional(dev, "di-spin",
						   GPIOD_OUT_LOW);
	if (IS_ERR(di->di_spin_gpio)) {
		ret = PTR_ERR(di->di_spin_gpio);
		goto err_iounmap_ctrl;
	}

	di->dma_buf = dmam_alloc_coherent(dev, DI_DMA_BUF_SIZE, &di->dma_addr,
					  GFP_KERNEL);
	if (!di->dma_buf) {
		ret = -ENOMEM;
		goto err_iounmap_ctrl;
	}

	ret = blk_mq_alloc_sq_tag_set(&di->tag_set, &hlwd_di_mq_ops, 1,
				      BLK_MQ_F_BLOCKING);
	if (ret)
		goto err_iounmap_ctrl;

	di->disk = blk_mq_alloc_disk(&di->tag_set, &lim, di);
	if (IS_ERR(di->disk)) {
		ret = PTR_ERR(di->disk);
		goto err_free_tag_set;
	}

	di->disk->major = hlwd_di_major;
	di->disk->first_minor = 0;
	di->disk->minors = 1;
	di->disk->fops = &hlwd_di_fops;
	di->disk->private_data = di;
	di->disk->flags |= GENHD_FL_REMOVABLE | GENHD_FL_NO_PART;
	di->disk->events = DISK_EVENT_MEDIA_CHANGE;
	strscpy(di->disk->disk_name, "hlwddi", DISK_NAME_LEN);
	set_disk_ro(di->disk, 1);
	set_capacity(di->disk, 0);

	mutex_lock(&di->lock);
	ret = di_init_drive_locked(di);
	if (!ret && !(di->last_cvr & DI_CVR_CVR))
		di_probe_media_locked(di);
	mutex_unlock(&di->lock);
	if (ret)
		goto err_put_disk;

	ret = add_disk(di->disk);
	if (ret)
		goto err_put_disk;
	di->disk_added = true;

	INIT_DELAYED_WORK(&di->media_work, hlwd_di_media_work);
	schedule_delayed_work(&di->media_work,
			      msecs_to_jiffies(DI_MEDIA_POLL_MS));

	dev_info(dev, "Hollywood DI block device registered\n");
	return 0;

err_put_disk:
	put_disk(di->disk);
err_free_tag_set:
	blk_mq_free_tag_set(&di->tag_set);
err_iounmap_ctrl:
	iounmap(di->ctrl);
	return ret;
}

static void hlwd_di_remove(struct platform_device *pdev)
{
	struct hlwd_di *di = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&di->media_work);
	if (di->disk_added)
		del_gendisk(di->disk);
	put_disk(di->disk);
	blk_mq_free_tag_set(&di->tag_set);
	iounmap(di->ctrl);
}

static const struct of_device_id hlwd_di_of_match[] = {
	{ .compatible = "nintendo,hollywood-di" },
	{ }
};
MODULE_DEVICE_TABLE(of, hlwd_di_of_match);

static struct platform_driver hlwd_di_driver = {
	.probe = hlwd_di_probe,
	.remove = hlwd_di_remove,
	.driver = {
		.name = HLWD_DI_NAME,
		.of_match_table = hlwd_di_of_match,
	},
};

static int __init hlwd_di_init(void)
{
	int ret;

	hlwd_di_major = register_blkdev(0, HLWD_DI_NAME);
	if (hlwd_di_major < 0)
		return hlwd_di_major;

	ret = platform_driver_register(&hlwd_di_driver);
	if (ret)
		unregister_blkdev(hlwd_di_major, HLWD_DI_NAME);

	return ret;
}

static void __exit hlwd_di_exit(void)
{
	platform_driver_unregister(&hlwd_di_driver);
	unregister_blkdev(hlwd_di_major, HLWD_DI_NAME);
}

module_init(hlwd_di_init);
module_exit(hlwd_di_exit);

MODULE_AUTHOR("Michael \"Techflash\" Garofalo");
MODULE_DESCRIPTION("Nintendo Hollywood Drive Interface block driver");
MODULE_LICENSE("GPL");
