// SPDX-License-Identifier: GPL-2.0
/*
 * Nintendo GameCube/Wii/Wii U EXI SPI controller driver
 *
 * EXI (EXpansion Interface / EXternal Interface) is an SPI-like
 * bus found on Nintendo's PowerPC consoles: the GameCube, Wii, and Wii U.
 *
 * It is a multiplexed, multi-channel bus.  It offers 3 channels,
 * which can each support 3 devices on different Chip-Select (CS) lines:
 * - Channel 0:
 *   - CS 0: "Slot-A"
 *   - CS 1: Internal
 *   - CS 2: "Serial-Port 1"
 * - Channel 1:
 *   - CS 0: "Slot-B"
 *   - CS 1: Unused
 *   - CS 2: Unused
 * - Channel 2:
 *   - CS 0: "Serial-Port 2"
 *   - CS 1: Unused
 *   - CS 2: Unused
 *
 * It is used for several things, including:
 * - Official internal devices:
 *   - RTC/ROM/SRAM/UART chip (found on Channel 0 CS 1)
 * - Official external devices, found on Slot-A, Slot-B, and SP1:
 *   - [Slots]      GameCube Memory Card
 *   - [Slots]      GameCube SD Card Adapter (DOL-019)
 *   - [Slots]      GameCube Microphone (DOL-022)
 *   - [SP1]        GameCube Modem (DOL-012)
 *   - [SP1]        GameCube BroadBand Adapter (DOL-015)
 * - Unofficial external devices:
 *   - [Slots, SP2] SD Gecko or SD2SP2 SD Card Adapter (DOL-019 clone)
 *   - [Slots]      USB Gecko serial console (see its udbg driver in
 *                                            arch/powerpc/platforms/embedded6xx
 *                                            for more info)
 *   - And many more...
 *
 * EXI devices can be dynamically hotplugged / hot-removed, and as well have
 * a custom form of identication (you can plug anything into any slot/port),
 * which complicates its inclusion to the Linux SPI layer, which expects
 * static devices per port, defined in the device-tree.
 * To further add to the confusion, many unofficial devices, as well as a few
 * official ones (e.g. DOL-019), especially those which are off-the-shelf SPI
 * devices, actually _don't_ report any ID when probed, and need to be manually
 * checked with additional logic.
 *
 * As such, this driver handles hotplug/remove, as well as device detection
 * internally.
 *
 * Copyright (C) 2025 Michael "Techflash" Garofalo
 */

#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mmc/mmc.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

/*
 * Hardware registers
 */
struct exi_channel_regs {
	u32 csr;    /* Channel Status Register */
	u32 mar;    /* DMA Memory Address Register */
	u32 length; /* DMA Transfer Length */
	u32 cr;     /* Control Register */
	u32 data;   /* Immediate data */
};

struct exi_regs {
	struct exi_channel_regs channels[3]; /* EXI has 3 channels, each with the above registers */
};

/*
 * Hardware register values, masks, and shifts
 */
#define EXI_CSR_EXIINTMASK   BIT(0)
#define EXI_CSR_EXIINT       BIT(1)
#define EXI_CSR_TCINTMASK    BIT(2)
#define EXI_CSR_TCINT        BIT(3)
#define EXI_CSR_CLK_SHIFT    4
#define EXI_CSR_CLK          (7 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_64MHZ    (6 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_32MHZ    (5 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_16MHZ    (4 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_8MHZ     (3 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_4MHZ     (2 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_2MHZ     (1 << EXI_CSR_CLK_SHIFT)
#define   EXI_CSR_CLK_1MHZ     (0 << EXI_CSR_CLK_SHIFT)
#define EXI_CSR_CS_SHIFT     7
#define EXI_CSR_CS           (7 << EXI_CSR_CS_SHIFT)
#define EXI_CSR_EXTINTMASK   BIT(10)
#define EXI_CSR_EXTINT       BIT(11)
#define EXI_CSR_EXT          BIT(12)
#define EXI_CSR_ROMDIS       BIT(13)
#define EXI_CSR_PRESERVE     (EXI_CSR_EXIINTMASK | EXI_CSR_TCINTMASK | EXI_CSR_EXTINTMASK)

#define EXI_CR_TSTART        BIT(0)
#define EXI_CR_DMA           BIT(1)
#define EXI_CR_RW_SHIFT      2
#define EXI_CR_RW            (3 << EXI_CR_RW_SHIFT)
#define   EXI_CR_RW_RD         (0 << EXI_CR_RW_SHIFT)
#define   EXI_CR_RW_WR         (1 << EXI_CR_RW_SHIFT)
#define   EXI_CR_RW_RDWR       (2 << EXI_CR_RW_SHIFT)
#define EXI_CR_TLEN_SHIFT    4
#define EXI_CR_TLEN          (3 << EXI_CR_TLEN_SHIFT)

#define EXI_SD_CMD0_RETRIES  16
#define EXI_SD_IDLE_BYTES    10
#define EXI_SD_INIT_CRC      0x95
#define EXI_SD_R1_IDLE       BIT(0)
#define EXI_SD_R1_BUSY       BIT(7)

#define EXI_DMA_ALIGN        0x1f
#define EXI_DMA_MIN_LEN      (EXI_DMA_ALIGN + 1)

enum exi_cs_mode {
	EXI_CS_NORMAL,
	EXI_CS_SD,
};

struct exi_spi;

/*
 * Driver state
 */
struct exi_channel {
	struct mutex lock;
	struct exi_spi *exi;
	struct spi_controller *ctlr;
	struct exi_channel_regs __iomem *regs;
	spinlock_t io_lock;
	struct spi_device *devices[3];
	bool selected[3];
	bool clock_only[3];
	enum exi_cs_mode cs_mode[3];
	int num;
};

struct exi_spi {
	struct device *dev;
	struct exi_regs __iomem *regs; /* EXI channel registers */
	struct exi_channel channels[3];
	struct work_struct hotplug_work;
	unsigned long pending_hotplug;
	int irq;
};

/*
 * Driver helper functions
 */

/*
 * Return an EXI channel structure from a channel index
 */
static struct exi_channel *exi_get_channel(struct exi_spi *exi,
					   unsigned int channel)
{
	if (WARN_ON(channel > 2))
		return NULL;

	return &exi->channels[channel];
}

/*
 * Acquire the channel I/O lock
 */
static void exi_lock(struct exi_channel *channel)
{
	mutex_lock(&channel->lock);
}

/*
 * Release the channel I/O lock
 */
static void exi_unlock(struct exi_channel *channel)
{
	mutex_unlock(&channel->lock);
}

/*
 * Convert SPI Hz speed to EXI speed index
 */
static unsigned int exi_speed_spi_to_exi(unsigned int hz)
{
	unsigned int mhz = hz / 1000000;

	if (mhz > 32)
		return EXI_CSR_CLK_64MHZ;
	else if (mhz > 16)
		return EXI_CSR_CLK_32MHZ;
	else if (mhz > 8)
		return EXI_CSR_CLK_16MHZ;
	else if (mhz > 4)
		return EXI_CSR_CLK_8MHZ;
	else if (mhz > 2)
		return EXI_CSR_CLK_4MHZ;
	else if (mhz > 1)
		return EXI_CSR_CLK_2MHZ;

	return EXI_CSR_CLK_1MHZ;
}

/*
 * Convert EXI speed index to SPI Hz speed
 */
static unsigned int exi_speed_exi_to_spi(unsigned int idx)
{
	return (1 << (idx >> EXI_CSR_CLK_SHIFT)) * 1000000;
}

static bool exi_is_hotplug_slot(unsigned int channel, unsigned int cs)
{
	return cs == 0 && (channel == 0 || channel == 1);
}

static bool exi_ext_present(struct exi_channel *channel)
{
	return !!(in_be32(&channel->regs->csr) & EXI_CSR_EXT);
}

static bool exi_slot_present(struct exi_spi *exi,
			     unsigned int channel, unsigned int cs)
{
	struct exi_channel *ch = exi_get_channel(exi, channel);

	if (!ch)
		return false;

	if (channel == 0 && cs == 1)
		return true;

	if (!exi_is_hotplug_slot(channel, cs))
		return false;

	return exi_ext_present(ch);
}

static u32 exi_preserved_csr(struct exi_channel *channel)
{
	return in_be32(&channel->regs->csr) & EXI_CSR_PRESERVE;
}

static void exi_clear_ext(struct exi_channel *channel)
{
	unsigned long flags;

	spin_lock_irqsave(&channel->io_lock, flags);
	out_be32(&channel->regs->csr,
		 in_be32(&channel->regs->csr) | EXI_CSR_EXTINT);
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

static void exi_clear_exiint(struct exi_channel *channel)
{
	unsigned long flags;

	spin_lock_irqsave(&channel->io_lock, flags);
	out_be32(&channel->regs->csr,
		 in_be32(&channel->regs->csr) | EXI_CSR_EXIINT);
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

static void exi_set_exiint_mask(struct exi_channel *channel, bool enabled)
{
	unsigned long flags;
	u32 csr;

	spin_lock_irqsave(&channel->io_lock, flags);
	csr = in_be32(&channel->regs->csr) | EXI_CSR_EXIINT;
	if (enabled)
		csr |= EXI_CSR_EXIINTMASK;
	else
		csr &= ~EXI_CSR_EXIINTMASK;

	out_be32(&channel->regs->csr, csr);
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

static void exi_set_device_irq(struct exi_spi *exi, unsigned int channel,
			       const char *modalias, bool enabled)
{
	if (!strcmp(modalias, "gamecube-bba"))
		exi_set_exiint_mask(&exi->channels[2], enabled);
	else if (!strcmp(modalias, "gamecube-microphone"))
		exi_set_exiint_mask(&exi->channels[channel], enabled);
}

/*
 * EXI hardware functions
 */

/*
 * Selects the desired device (CS line) on the given
 * EXI channel, and sets the desired clock speed.
 */
static void exi_select(struct exi_channel *channel,
		       unsigned int cs,
		       unsigned int clk)
{
	u32 csr;
	struct exi_spi *exi;
	unsigned long flags;

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	if (WARN_ON(cs > 2) || WARN_ON(clk > EXI_CSR_CLK_32MHZ))
		return;

	dev_dbg(exi->dev, "Channel %d, selecting CS %d at clock %dMHz, CSR @ %p\n",
		channel->num, cs, (1 << (clk >> EXI_CSR_CLK_SHIFT)),
		&channel->regs->csr);

	spin_lock_irqsave(&channel->io_lock, flags);
	csr = exi_preserved_csr(channel);
	csr |= (1 << (EXI_CSR_CS_SHIFT + cs)); /* set the appropriate CS bit */
	csr |= clk;                            /* set the appropriate CLK bits */
	dev_dbg(exi->dev, "Writing CSR=0x%08x\n", csr);
	out_be32(&channel->regs->csr, csr);    /* write CSR back */
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

/*
 * Selects an EXI clock without asserting a CS line.
 */
static void exi_select_clock(struct exi_channel *channel, unsigned int clk)
{
	u32 csr;
	struct exi_spi *exi;
	unsigned long flags;

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	if (WARN_ON(clk > EXI_CSR_CLK_32MHZ))
		return;

	dev_dbg(exi->dev, "Channel %d, selecting clock %dMHz without CS\n",
		channel->num, (1 << (clk >> EXI_CSR_CLK_SHIFT)));

	spin_lock_irqsave(&channel->io_lock, flags);
	csr = exi_preserved_csr(channel);
	csr |= clk;
	out_be32(&channel->regs->csr, csr);
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

/*
 * Deselects any selected device (CS line) on the given
 * EXI channel.
 */
static void exi_deselect(struct exi_channel *channel)
{
	u32 csr;
	struct exi_spi *exi;
	unsigned long flags;

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	spin_lock_irqsave(&channel->io_lock, flags);
	csr = exi_preserved_csr(channel);
	dev_dbg(exi->dev, "Writing CSR=0x%08x\n", csr);
	out_be32(&channel->regs->csr, csr);    /* write CSR back */
	spin_unlock_irqrestore(&channel->io_lock, flags);
}

static void exi_select_for_device(struct exi_channel *channel,
				  unsigned int cs, unsigned int speed)
{
	exi_select(channel, cs, exi_speed_spi_to_exi(speed));
}

/*
 * Transfer modes
 */
#define MODE_READ  BIT(0)
#define MODE_WRITE BIT(1)

/*
 * Immediate transaction to channel.
 * Both the read and write will be of the same size if using both.
 * Assumes desired device is already selected.
 */
static int exi_xfer_imm(struct exi_channel *channel,
			unsigned int len,
			unsigned int mode,
			const void *in, void *out)
{
	u32 cr, data;
	struct exi_spi *exi;

	if (WARN_ON(!channel))
		return -EINVAL;

	exi = channel->exi;
	if (WARN_ON(len > 4) || WARN_ON(!len))
		return -EINVAL;

	/*
	 * Decide whether to poll or to use interrupts.
	 * If clock < 32MHz, it's worth it to use interrupts,
	 * since we'll be polling for a while.  However if clock
	 * is 32MHz, the cost of the interrupt is more than the
	 * cost of just spinning until the transfer is done.
	 *
	 * TODO: Do this
	 */

	/* read CR */
	cr = in_be32(&channel->regs->cr);

	/*
	 * transfer should not already be in progress or else
	 * we've done something very wrong
	 */
	if (WARN_ON(cr & EXI_CR_TSTART))
		while (in_be32(&channel->regs->cr) & EXI_CR_TSTART)
			;

	dev_dbg(exi->dev, "Channel %d, doing xfer with len=%d mode=%c%c\n",
			channel->num, len, (mode & MODE_READ) ? 'R' : '-',
			(mode & MODE_WRITE) ? 'W' : '-');

	/* get the desired data */
	if (mode & MODE_WRITE) {
		switch (len) {
		case 1:
			data = *(u8 *)in << 24;
			break;
		case 2:
			data = *(u16 *)in << 16;
			break;
		case 3:
			data = (u32)((u8 *)in)[0] << 24;
			data |= (u32)((u8 *)in)[1] << 16;
			data |= (u32)((u8 *)in)[2] << 8;
			break;
		case 4:
			data = *(u32 *)in;
			break;
		default:
			return -EINVAL;
		}
		dev_dbg(exi->dev, "Outgoing data from buffer, data=0x%08x\n", data);
	} else {
		data = 0;
		dev_dbg(exi->dev, "Outgoing data static, data=0x%08x\n", data);
	}

	/* give the EXI hardware our data */
	out_be32(&channel->regs->data, data);

	/* mode */
	cr = 0;
	if (mode == (MODE_READ | MODE_WRITE))
		cr |= EXI_CR_RW_RDWR;
	else if (mode == MODE_READ)
		cr |= EXI_CR_RW_RD;
	else if (mode == MODE_WRITE)
		cr |= EXI_CR_RW_WR;
	else
		return -EINVAL;

	cr |= ((len - 1) << EXI_CR_TLEN_SHIFT); /* length */
	cr |= EXI_CR_TSTART;              /* start the transfer */
	dev_dbg(exi->dev, "Writing CR=0x%08x\n", cr);
	out_be32(&channel->regs->cr, cr); /* do it */

	/* spin until transfer done */
	while (in_be32(&channel->regs->cr) & EXI_CR_TSTART)
		;

	/* transfer done, read our data, if any */
	if (mode & MODE_READ) {
		data = in_be32(&channel->regs->data);
		dev_dbg(exi->dev, "Incoming data=0x%08x\n", data);

		/* write it back */
		switch (len) {
		case 1:
			*(u8 *)out = (data & 0xff000000) >> 24;
			break;
		case 2:
			*(u16 *)out = (data & 0xffff0000) >> 16;
			break;
		case 3:
			((u8 *)out)[0] = (data & 0xff000000) >> 24;
			((u8 *)out)[1] = (data & 0x00ff0000) >> 16;
			((u8 *)out)[2] = (data & 0x0000ff00) >> 8;
			break;
		case 4:
			*(u32 *)out = data;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

/* I/O shortcuts */
#define exi_read_imm(channel, len, out)     exi_xfer_imm(channel, len, MODE_READ, NULL, out)
#define exi_write_imm(channel, len, in)     exi_xfer_imm(channel, len, MODE_WRITE, in, NULL)
#define exi_rdwr_imm(channel, len, in, out) exi_xfer_imm(channel, len, MODE_READ | MODE_WRITE, in, out)

static bool exi_dma_aligned(const void *buf)
{
	return !((unsigned long)buf & EXI_DMA_ALIGN);
}

static unsigned long exi_dma_align_next_addr(const void *buf)
{
	return ((unsigned long)buf + EXI_DMA_ALIGN) & ~((unsigned long)EXI_DMA_ALIGN);
}

static unsigned long exi_dma_align_prev_addr(const void *buf)
{
	return (unsigned long)buf & ~((unsigned long)EXI_DMA_ALIGN);
}

static int exi_xfer_imm_buf(struct exi_channel *channel, const u8 **tx,
			    u8 **rx, size_t len)
{
	int ret = 0;

	while (len) {
		unsigned int xfer_len = len >= 4 ? 4 : len;

		if (*rx && *tx)
			ret = exi_rdwr_imm(channel, xfer_len, *tx, *rx);
		else if (*rx)
			ret = exi_read_imm(channel, xfer_len, *rx);
		else if (*tx)
			ret = exi_write_imm(channel, xfer_len, *tx);
		else
			return -EINVAL;

		if (ret)
			return ret;

		if (*tx)
			*tx += xfer_len;
		if (*rx)
			*rx += xfer_len;
		len -= xfer_len;
	}

	return 0;
}

static int exi_xfer_dma_addr(struct exi_channel *channel, dma_addr_t dma_addr,
			     size_t len, enum dma_data_direction dir)
{
	struct exi_spi *exi = channel->exi;
	u32 cr;
	unsigned long deadline;
	unsigned long flags;
	int ret = 0;

	if (WARN_ON(dma_addr & EXI_DMA_ALIGN) || WARN_ON(len & EXI_DMA_ALIGN))
		return -EINVAL;

	out_be32(&channel->regs->data, ~0);
	out_be32(&channel->regs->mar, dma_addr);
	out_be32(&channel->regs->length, len);
	spin_lock_irqsave(&channel->io_lock, flags);
	out_be32(&channel->regs->csr,
		 in_be32(&channel->regs->csr) | EXI_CSR_TCINTMASK);
	spin_unlock_irqrestore(&channel->io_lock, flags);

	cr = EXI_CR_TSTART | EXI_CR_DMA;
	if (dir == DMA_FROM_DEVICE)
		cr |= EXI_CR_RW_RD;
	else
		cr |= EXI_CR_RW_WR;

	out_be32(&channel->regs->cr, cr);
	spin_lock_irqsave(&channel->io_lock, flags);
	out_be32(&channel->regs->csr,
		 in_be32(&channel->regs->csr) & ~EXI_CSR_TCINTMASK);
	spin_unlock_irqrestore(&channel->io_lock, flags);

	deadline = jiffies + 2 * HZ;
	while (in_be32(&channel->regs->cr) & EXI_CR_TSTART) {
		cpu_relax();
		if (time_after(jiffies, deadline)) {
			dev_err(exi->dev, "Channel %d DMA transfer timed out\n",
				channel->num);
			ret = -ETIMEDOUT;
			break;
		}
	}

	spin_lock_irqsave(&channel->io_lock, flags);
	out_be32(&channel->regs->csr,
		 in_be32(&channel->regs->csr) | EXI_CSR_TCINT);
	spin_unlock_irqrestore(&channel->io_lock, flags);

	return ret;
}

static int exi_xfer_dma(struct exi_channel *channel, struct device *dma_dev,
			const void *buf, size_t len,
			enum dma_data_direction dir)
{
	dma_addr_t dma_addr;
	int ret;

	if (WARN_ON(!exi_dma_aligned(buf)) || WARN_ON(len & EXI_DMA_ALIGN))
		return -EINVAL;

	dma_addr = dma_map_single(dma_dev, (void *)buf, len, dir);
	if (dma_mapping_error(dma_dev, dma_addr))
		return -EIO;

	if (dma_addr & EXI_DMA_ALIGN) {
		dma_unmap_single(dma_dev, dma_addr, len, dir);
		return -EADDRNOTAVAIL;
	}

	ret = exi_xfer_dma_addr(channel, dma_addr, len, dir);
	dma_unmap_single(dma_dev, dma_addr, len, dir);

	return ret;
}

static int exi_xfer_half_duplex(struct exi_channel *channel, const u8 **tx,
				u8 **rx, size_t len, struct device *dma_dev)
{
	const void *buf = *tx ?: *rx;
	enum dma_data_direction dir = *rx ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	size_t pre_len, dma_len, post_len;
	unsigned long dma_start, dma_end, buf_start, buf_end;
	bool used_dma = true;
	int ret;

	if (len < EXI_DMA_MIN_LEN)
		return exi_xfer_imm_buf(channel, tx, rx, len);

	buf_start = (unsigned long)buf;
	buf_end = buf_start + len;
	dma_start = exi_dma_align_next_addr(buf);
	dma_end = exi_dma_align_prev_addr((void *)buf_end);

	if (dma_end <= dma_start)
		return exi_xfer_imm_buf(channel, tx, rx, len);

	pre_len = dma_start - buf_start;
	dma_len = dma_end - dma_start;
	post_len = buf_end - dma_end;

	ret = exi_xfer_imm_buf(channel, tx, rx, pre_len);
	if (ret)
		return ret;

	ret = exi_xfer_dma(channel, dma_dev, *tx ?: *rx, dma_len, dir);
	if (ret == -EADDRNOTAVAIL) {
		used_dma = false;
		ret = exi_xfer_imm_buf(channel, tx, rx, dma_len);
	}
	if (ret)
		return ret;

	if (used_dma) {
		if (*tx)
			*tx += dma_len;
		if (*rx)
			*rx += dma_len;
	}

	return exi_xfer_imm_buf(channel, tx, rx, post_len);
}

static void exi_deselect_for_device(struct exi_channel *channel,
				    unsigned int cs, unsigned int speed)
{
	u8 tx = 0xff, rx;

	exi_deselect(channel);

	if (channel->cs_mode[cs] != EXI_CS_SD)
		return;

	exi_select_clock(channel, exi_speed_spi_to_exi(speed));
	exi_rdwr_imm(channel, 1, &tx, &rx);
	exi_deselect(channel);
}

/*
 * Read the ID of the given devicn device on the given channel
 */
static u32 exi_read_id(struct exi_spi *exi,
		       unsigned int channel,
		       unsigned int cs)
{
	u32 id;
	u16 cmd = 0x0000;
	struct exi_channel *ch = exi_get_channel(exi, channel);

	if (!ch)
		return 0;

	exi_lock(ch);                         /* grab (or wait for) lock on channel */
	exi_select(ch, cs, EXI_CSR_CLK_8MHZ); /* select this device */
	exi_write_imm(ch, 2, &cmd);           /* tell device to provide ID */
	exi_read_imm(ch, 4, &id);             /* read the ID reported by the device (if any) */
	exi_deselect(ch);                     /* deselect this device */
	exi_unlock(ch);                       /* release lock on the channel */

	return id;
}

/*
 * EXI Device ID mappings
 */
struct exi_id_entry {
	u32 id;
	char *name;
	char *modalias;
	u32 speed;
};

static struct exi_id_entry exi_id_table[] = {
	/* TODO: When do we see one or the other? */
	{ 0xffff1698, "GameCube Mask ROM/RTC/SRAM/UART", "gamecube-rtc" },
	{ 0xffff2843, "GameCube Mask ROM/RTC/SRAM/UART", "gamecube-rtc" },
	{ 0xfffff308, "Wii Mask ROM/RTC/SRAM/UART", "gamecube-rtc" },
	{ 0x00000004, "Memory Card 59", "gamecube-memory-card" },
	{ 0x00000008, "Memory Card 123", "gamecube-memory-card" },
	{ 0x00000010, "Memory Card 251", "gamecube-memory-card" },
	{ 0x00000020, "Memory Card 507", "gamecube-memory-card" },
	{ 0x00000040, "Memory Card 1019", "gamecube-memory-card" },
	{ 0x00000080, "Memory Card 2043", "gamecube-memory-card" },
	{ 0x01010000, "USB Adapter", "" },
	{ 0x01020000, "NPDP GDEV", "" },
	{ 0x02020000, "Modem", "" },
	{ 0x03010000, "Marlin?", "" },
	{ 0x04020200, "BroadBand Adapter (DOL-015)", "gamecube-bba", EXI_CSR_CLK_32MHZ },
	{ 0x04120000, "AD16", "" },
	{ 0x05070000, "IS Viewer", "" },
	{ 0x0a000000, "Microphone (DOL-022)", "gamecube-microphone", EXI_CSR_CLK_16MHZ },
	{ 0, NULL, NULL }
};

static void exi_set_device_mode(struct exi_spi *exi,
				unsigned int channel, unsigned int cs,
				const char *modalias)
{
	struct exi_channel *ch = exi_get_channel(exi, channel);

	if (!ch)
		return;

	if (!strcmp(modalias, "mmc-spi-slot"))
		ch->cs_mode[cs] = EXI_CS_SD;
	else
		ch->cs_mode[cs] = EXI_CS_NORMAL;
}

/*
 * Get an ID entry from an ID
 */
static struct exi_id_entry *exi_id_to_entry(u32 id)
{
	struct exi_id_entry *ent = exi_id_table;

	while (ent->name) {
		if (ent->id == id)
			return ent;
		ent++;
	}
	return NULL;
}

static int exi_sd_idle_clocks(struct exi_channel *ch, unsigned int clk)
{
	u8 tx = 0xff, rx;
	int i, ret = 0;

	exi_deselect(ch);
	exi_select_clock(ch, clk);
	for (i = 0; i < EXI_SD_IDLE_BYTES; i++) {
		ret = exi_rdwr_imm(ch, 1, &tx, &rx);
		if (ret)
			break;
	}
	exi_deselect(ch);

	return ret;
}

static int exi_sd_send_cmd0(struct exi_channel *ch,
			    unsigned int cs, unsigned int clk, u8 *r1)
{
	u8 cmd[6] = {
		0x40 | MMC_GO_IDLE_STATE,
		0, 0, 0, 0,
		EXI_SD_INIT_CRC,
	};
	u8 tx = 0xff;
	int i, ret = 0;

	exi_select(ch, cs, clk);
	for (i = 0; i < ARRAY_SIZE(cmd); i++) {
		ret = exi_write_imm(ch, 1, &cmd[i]);
		if (ret)
			goto out;
	}

	for (i = 0; i < EXI_SD_CMD0_RETRIES; i++) {
		ret = exi_rdwr_imm(ch, 1, &tx, r1);
		if (ret)
			goto out;
		if (!(*r1 & EXI_SD_R1_BUSY))
			break;
	}

	if (i == EXI_SD_CMD0_RETRIES)
		ret = -ETIMEDOUT;

out:
	exi_deselect(ch);
	return ret;
}

static bool exi_probe_sd(struct exi_spi *exi, unsigned int channel,
			 unsigned int cs)
{
	static const unsigned int init_clks[] = {
		EXI_CSR_CLK_1MHZ,
		EXI_CSR_CLK_2MHZ,
		EXI_CSR_CLK_4MHZ,
	};
	struct exi_channel *ch = exi_get_channel(exi, channel);
	u8 r1 = 0xff;
	int i;

	if (!ch)
		return false;

	exi_lock(ch);
	for (i = 0; i < ARRAY_SIZE(init_clks); i++) {
		if (exi_sd_idle_clocks(ch, init_clks[i]))
			continue;
		if (exi_sd_send_cmd0(ch, cs, init_clks[i], &r1))
			continue;
		if (r1 == EXI_SD_R1_IDLE)
			break;
	}
	exi_unlock(ch);

	if (r1 != EXI_SD_R1_IDLE) {
		dev_dbg(exi->dev, "[%d:%d]: SD probe failed, last R1=0x%02x\n",
			channel, cs, r1);
		return false;
	}

	dev_info(exi->dev, "[%d:%d]: SD card detected via SPI CMD0\n",
		 channel, cs);
	return true;
}

/*
 * Check for a few classes of devices that don't
 * report a standard EXI ID
 */
static bool exi_probe_noid(struct exi_spi *exi,
			   unsigned int channel,
			   unsigned int cs,
			   char **modalias,
			   u32 *speed)
{
	u16 resp = 0xffff, cmd = 0x9000;
	struct exi_channel *ch = exi_get_channel(exi, channel);

	if (!ch)
		return false;

	exi_lock(ch);                         /* grab (or wait for) lock on channel */
	exi_select(ch, cs, EXI_CSR_CLK_8MHZ); /* select this device */
	exi_rdwr_imm(ch, 2, &cmd, &resp);     /* send USB Gecko ID command */
	exi_deselect(ch);                     /* deselect this device */
	exi_unlock(ch);                       /* release lock on the channel */

	if (resp == 0x0470) {
		*modalias = "exi-usb-gecko";
		*speed = EXI_CSR_CLK_32MHZ;
		return true;
	}

	if (exi_probe_sd(exi, channel, cs)) {
		*modalias = "mmc-spi-slot";
		*speed = EXI_CSR_CLK_16MHZ;
		return true;
	}

	return false;
}


/*
 * Probe what device is on a given channel + CS, and
 * create a new SPI device for it.
 */
static int exi_probe(struct exi_spi *exi,
		     unsigned int channel,
		     unsigned int cs)
{
	char *name = "Unknown", *modalias = NULL;
	struct spi_device *device;
	struct spi_board_info info;
	u32 speed = EXI_CSR_CLK_8MHZ, id = exi_read_id(exi, channel, cs);
	struct exi_id_entry *ent = exi_id_to_entry(id);
	struct spi_controller *ctlr = exi->channels[channel].ctlr;
	int ret;

	if (exi->channels[channel].devices[cs])
		return 0;

	if (ent) {
		name = ent->name;
		modalias = ent->modalias;
		speed = ent->speed;
		dev_info(exi->dev, "[%d:%d]: Got ID: 0x%08x, device type: %s, modalias: %s\n", channel, cs, id, name, modalias);
	} else {
		dev_info(exi->dev, "[%d:%d]: Bogus ID: 0x%08x, trying exi_probe_noid\n", channel, cs, id);
	}

	/* clear our spi_board_info */
	memset(&info, 0, sizeof(struct spi_board_info));

	/* try to hardcode info where it makes sense, to avoid failures where it's possible to continue */
	if (channel == 0 && cs == 1) {
		/* must be RTC/ROM/SRAM/UART */
		modalias = "gamecube-rtc";
		speed = EXI_CSR_CLK_8MHZ;
	}

	if (!modalias && !exi_probe_noid(exi, channel, cs, &modalias, &speed)) {
		dev_info(exi->dev, "[%d:%d]: no supported device detected\n",
			 channel, cs);
		return 0;
	}

	dev_info(exi->dev, "[%d:%d]: new modalias: %s\n", channel, cs, modalias);

	/* set info */
	strscpy(info.modalias, modalias, SPI_NAME_SIZE);
	info.mode = SPI_MODE_0;
	info.chip_select = cs;
	info.max_speed_hz = exi_speed_exi_to_spi(speed);
	info.controller_data = ctlr;
	info.irq = exi->irq;
	exi_set_device_mode(exi, channel, cs, modalias);

	/* create it */
	device = spi_new_device(ctlr, &info);
	if (!device) {
		exi->channels[channel].cs_mode[cs] = EXI_CS_NORMAL;
		dev_err(exi->dev, "[%d:%d]: spi_new_device failed\n", channel, cs);
		return -ENOMEM;
	}
	ret = dma_coerce_mask_and_coherent(&device->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(exi->dev, "[%d:%d]: failed to set device DMA mask: %d\n",
			 channel, cs, ret);

	exi->channels[channel].devices[cs] = device;
	exi_set_device_irq(exi, channel, modalias, true);
	dev_info(exi->dev, "[%d:%d]: successfully added device\n", channel, cs);

	return 0;
}

static void exi_remove_device(struct exi_spi *exi,
			      unsigned int channel, unsigned int cs)
{
	struct spi_device *device;
	struct exi_channel *ch = exi_get_channel(exi, channel);

	if (!ch)
		return;

	exi_lock(ch);
	device = ch->devices[cs];
	if (device)
		exi_set_device_irq(exi, channel, device->modalias, false);
	ch->devices[cs] = NULL;
	ch->selected[cs] = false;
	ch->clock_only[cs] = false;
	ch->cs_mode[cs] = EXI_CS_NORMAL;
	exi_unlock(ch);

	if (device) {
		dev_info(exi->dev, "[%d:%d]: removed device\n", channel, cs);
		spi_unregister_device(device);
	}
}

static void exi_rescan_slot(struct exi_spi *exi,
			    unsigned int channel, unsigned int cs)
{
	bool present;

	if (exi_is_hotplug_slot(channel, cs)) {
		present = exi_slot_present(exi, channel, cs);

		if (present) {
			if (!exi->channels[channel].devices[cs])
				exi_probe(exi, channel, cs);
		} else if (exi->channels[channel].devices[cs])
			exi_remove_device(exi, channel, cs);
	} else {
		if (!exi->channels[channel].devices[cs])
			exi_probe(exi, channel, cs);
	}
}

static void exi_rescan_channel(struct exi_spi *exi, unsigned int channel)
{
	unsigned int cs;

	if (channel == 0) {
		for (cs = 0; cs < 3; cs++)
			exi_rescan_slot(exi, channel, cs);
	} else if (channel == 1) {
		exi_rescan_slot(exi, channel, 0);
	} else if (channel == 2) {
		exi_rescan_slot(exi, channel, 0);
	}
}

static void exi_hotplug_work(struct work_struct *work)
{
	struct exi_spi *exi = container_of(work, struct exi_spi, hotplug_work);
	unsigned int channel;

	for (channel = 0; channel < 3; channel++) {
		if (test_and_clear_bit(channel, &exi->pending_hotplug))
			exi_rescan_slot(exi, channel, 0);
	}
}

static irqreturn_t exi_irq(int irq, void *data)
{
	struct exi_spi *exi = data;
	unsigned int channel;
	bool handled = false;
	bool hotplug = false;

	for (channel = 0; channel < 3; channel++) {
		struct exi_channel *ch = &exi->channels[channel];
		u32 csr = in_be32(&ch->regs->csr);

		if (csr & EXI_CSR_EXIINT) {
			exi_clear_exiint(ch);
			handled = true;
		}

		if (csr & EXI_CSR_TCINT) {
			unsigned long flags;

			spin_lock_irqsave(&ch->io_lock, flags);
			out_be32(&ch->regs->csr,
				 in_be32(&ch->regs->csr) | EXI_CSR_TCINT);
			spin_unlock_irqrestore(&ch->io_lock, flags);
			handled = true;
		}

		if (!(csr & EXI_CSR_EXTINT))
			continue;

		exi_clear_ext(ch);
		set_bit(channel, &exi->pending_hotplug);
		hotplug = true;
		handled = true;
	}

	if (hotplug)
		schedule_work(&exi->hotplug_work);

	return handled ? IRQ_HANDLED : IRQ_NONE;
}


/*
 * SPI Layer functions
 */

/*
 * Perform one transfer
 */
static int exi_spi_transfer_one(struct spi_controller *ctlr,
				struct spi_device *spi,
				struct spi_transfer *xfer)
{
	struct exi_channel *channel = spi_controller_get_devdata(ctlr);
	struct exi_spi *exi = channel->exi;
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	size_t len = xfer->len;
	int ret = 0, cs;
	unsigned int speed = xfer->speed_hz ?: spi->max_speed_hz;

	cs = spi_get_chipselect(spi, 0);
	dev_dbg(exi->dev, "[%d:%d]: spi xfer, have_rx=%d have_tx=%d\n", channel->num, cs, !!rx, !!tx);

	if (len && !rx && !tx)
		return -EINVAL;

	exi_lock(channel);
	if (channel->selected[cs]) {
		if (channel->clock_only[cs])
			exi_select_clock(channel, exi_speed_spi_to_exi(speed));
		else
			exi_select_for_device(channel, cs, speed);
	}

	if (rx && tx)
		ret = exi_xfer_imm_buf(channel, &tx, &rx, len);
	else
		ret = exi_xfer_half_duplex(channel, &tx, &rx, len, &spi->dev);

	exi_unlock(channel);

	return ret;
}

/*
 * Set CS for a channel
 */
static void exi_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct exi_channel *channel = spi_controller_get_devdata(spi->controller);
	struct exi_spi *exi = channel->exi;
	int cs = spi_get_chipselect(spi, 0);
	unsigned int speed = spi->max_speed_hz;
	bool active = (spi->mode & SPI_CS_HIGH) ? enable : !enable;
	bool clock_only = active && channel->cs_mode[cs] == EXI_CS_SD &&
			  (spi->mode & SPI_CS_HIGH);

	dev_dbg(exi->dev, "[%d:%d]: spi set_cs, set CS %d = %d with speed %d\n", channel->num, cs, cs, enable, speed);

	exi_lock(channel);
	if (clock_only)
		exi_select_clock(channel, exi_speed_spi_to_exi(speed));
	else if (active)
		exi_select_for_device(channel, cs, speed);
	else
		exi_deselect_for_device(channel, cs, speed);
	channel->selected[cs] = active;
	channel->clock_only[cs] = clock_only;
	exi_unlock(channel);
}

static void exi_init_channel(struct exi_channel *channel)
{
	u32 csr = EXI_CSR_EXIINT | EXI_CSR_EXTINTMASK | EXI_CSR_EXTINT;

	if (channel->num == 0)
		csr |= EXI_CSR_ROMDIS;

	out_be32(&channel->regs->csr, csr);
	out_be32(&channel->regs->mar, 0);
	out_be32(&channel->regs->length, 0);
	out_be32(&channel->regs->cr, 0);
	out_be32(&channel->regs->data, 0);
}



/*
 * Probe
 */
static int exi_spi_probe(struct platform_device *pdev)
{
	struct exi_spi *exi;
	struct spi_controller *ctlr;
	struct resource *res;
	int i, ret;

	/* Driver state */
	exi = devm_kzalloc(&pdev->dev, sizeof(*exi), GFP_KERNEL);
	if (!exi)
		return -ENOMEM;

	exi->dev = &pdev->dev;
	INIT_WORK(&exi->hotplug_work, exi_hotplug_work);

	platform_set_drvdata(pdev, exi);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to set DMA mask\n");

	/* Memory-mapped IO region */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	exi->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(exi->regs))
		return PTR_ERR(exi->regs);

	/* EXI has 3 channels, so register 3 controllers */
	dev_info(&pdev->dev, "Registering EXI SPI controllers\n");
	for (i = 0; i < 3; i++) {
		ctlr = devm_spi_alloc_host(&pdev->dev, 0);
		if (!ctlr)
			return -ENOMEM;

		/* per-channel data */
		exi->channels[i].exi = exi;
		exi->channels[i].ctlr = ctlr;
		exi->channels[i].regs = &exi->regs->channels[i];
		exi->channels[i].num = i;
		mutex_init(&exi->channels[i].lock);
		spin_lock_init(&exi->channels[i].io_lock);

		/* controller info */
		ctlr->bus_num = i;
		ctlr->num_chipselect = 3; /* each channel offers 3 CS lines */
		ctlr->mode_bits = SPI_MODE_0 | SPI_MODE_1;
		ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
		ctlr->max_speed_hz = 32000000; /* EXI maxes out at 32MHz */
		ctlr->set_cs = exi_spi_set_cs;
		ctlr->transfer_one = exi_spi_transfer_one;
		ctlr->dev.of_node = pdev->dev.of_node;

		/* reset the hardware */
		exi_init_channel(&exi->channels[i]);

		/* set our per-controller devdata to the per-channel EXI state */
		spi_controller_set_devdata(ctlr, &exi->channels[i]);

		ret = devm_spi_register_controller(&pdev->dev, ctlr);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "Failed to register SPI controller %d\n", i);
	}

	exi->irq = platform_get_irq(pdev, 0);
	if (exi->irq < 0)
		return exi->irq;

	ret = devm_request_irq(&pdev->dev, exi->irq, exi_irq, IRQF_SHARED,
			       dev_name(&pdev->dev), exi);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to request EXI IRQ\n");

	/* Check all devices that might exist. */
	exi_probe(exi, 0, 1); /* Internal */
	for (i = 0; i < 3; i++)
		exi_rescan_channel(exi, i);

	return 0;
}

static void exi_spi_remove(struct platform_device *pdev)
{
	struct exi_spi *exi = platform_get_drvdata(pdev);
	unsigned int channel, cs;

	dev_info(&pdev->dev, "Removing EXI SPI host\n");
	disable_irq(exi->irq);
	cancel_work_sync(&exi->hotplug_work);

	for (channel = 0; channel < 3; channel++) {
		for (cs = 0; cs < 3; cs++)
			exi_remove_device(exi, channel, cs);
	}
}


static const struct of_device_id exi_spi_of_match[] = {
	{ .compatible = "nintendo,flipper-exi" },   /* GameCube */
	{ .compatible = "nintendo,hollywood-exi" }, /* Wii */
	{ .compatible = "nintendo,latte-exi" },     /* Wii U */
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, exi_spi_of_match);

static struct platform_driver exi_spi_driver = {
	.driver = {
		.name = "exi-spi",
		.of_match_table = exi_spi_of_match,
	},
	.probe  = exi_spi_probe,
	.remove = exi_spi_remove,
};
module_platform_driver(exi_spi_driver);

MODULE_DESCRIPTION("Nintendo GameCube/Wii/Wii U EXI SPI controller driver");
MODULE_AUTHOR("Michael \"Techflash\" Garoflalo <officialTechflashYT@gmail.com>");
MODULE_LICENSE("GPL");
