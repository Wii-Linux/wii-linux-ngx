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
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/slab.h>
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
	if (channel == 0 && (cs == 0 || cs == 2))
		return true;
	if (channel == 1 && cs == 0)
		return true;
	if (channel == 2 && cs == 0)
		return true;

	return false;
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
	out_be32(&channel->regs->csr, exi_preserved_csr(channel) | EXI_CSR_EXTINT);
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

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	if (WARN_ON(cs > 2) || WARN_ON(clk > EXI_CSR_CLK_32MHZ))
		return;

	dev_dbg(exi->dev, "Channel %d, selecting CS %d at clock %dMHz, CSR @ %p\n",
		channel->num, cs, (1 << (clk >> EXI_CSR_CLK_SHIFT)),
		&channel->regs->csr);

	csr = exi_preserved_csr(channel);
	csr |= (1 << (EXI_CSR_CS_SHIFT + cs)); /* set the appropriate CS bit */
	csr |= clk;                            /* set the appropriate CLK bits */
	dev_dbg(exi->dev, "Writing CSR=0x%08x\n", csr);
	out_be32(&channel->regs->csr, csr);    /* write CSR back */
}

/*
 * Selects an EXI clock without asserting a CS line.
 */
static void exi_select_clock(struct exi_channel *channel, unsigned int clk)
{
	u32 csr;
	struct exi_spi *exi;

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	if (WARN_ON(clk > EXI_CSR_CLK_32MHZ))
		return;

	dev_dbg(exi->dev, "Channel %d, selecting clock %dMHz without CS\n",
		channel->num, (1 << (clk >> EXI_CSR_CLK_SHIFT)));

	csr = exi_preserved_csr(channel);
	csr |= clk;
	out_be32(&channel->regs->csr, csr);
}

/*
 * Deselects any selected device (CS line) on the given
 * EXI channel.
 */
static void exi_deselect(struct exi_channel *channel)
{
	u32 csr;
	struct exi_spi *exi;

	if (WARN_ON(!channel))
		return;

	exi = channel->exi;
	csr = exi_preserved_csr(channel);
	dev_dbg(exi->dev, "Writing CSR=0x%08x\n", csr);
	out_be32(&channel->regs->csr, csr);    /* write CSR back */
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
	{ 0x04020200, "BroadBand Adapter (DOL-015)", "gamecube-bba" },
	{ 0x04120000, "AD16", "" },
	{ 0x05070000, "IS Viewer", "" },
	{ 0x0a000000, "Microphone (DOL-022)", "gamecube-microphone" },
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

/*
 * Probe what device is on a given channel + CS, and
 * create a new SPI device for it.
 */
static int exi_probe(struct exi_spi *exi,
		      unsigned int channel,
		      unsigned int cs)
{
	char *name, *modalias;
	struct spi_device *device;
	struct spi_board_info info;
	u32 speed, id = exi_read_id(exi, channel, cs);
	struct exi_id_entry *ent = exi_id_to_entry(id);
	struct spi_controller *ctlr = exi->channels[channel].ctlr;

	if (exi->channels[channel].devices[cs])
		return 0;

	if (ent) {
		name = ent->name;
		modalias = ent->modalias;
		speed = ent->speed;
	} else {
		name = "Unknown";
		modalias = "none";
		speed = EXI_CSR_CLK_8MHZ;
	}

	dev_info(exi->dev, "[%d:%d]: Got ID: 0x%08x, device type: %s, modalias: %s\n", channel, cs, id, name, modalias);

	/* clear our spi_board_info */
	memset(&info, 0, sizeof(struct spi_board_info));

	/* try to hardcode info where it makes sense, to avoid failures where it's possible to continue */
	if (channel == 0 && cs == 1) {
		/* must be RTC/ROM/SRAM/UART */
		modalias = "gamecube-rtc";
		speed = EXI_CSR_CLK_8MHZ;
	}

	/* FIXME: Really should autodetect, this is just blatant guessing */
	if (!ent && channel == 0 && cs == 0) {
		/* assume SDGecko in Slot-A */
		modalias = "mmc-spi-slot";
		speed = EXI_CSR_CLK_16MHZ;
	}

	if (!ent && channel == 2 && cs == 0) {
		/* assume SD2SP2 */
		modalias = "mmc-spi-slot";
		speed = EXI_CSR_CLK_16MHZ;
	}

	if (!ent && channel == 1 && cs == 0) {
		/* assume USB Gecko in Slot-B */
		modalias = "exi-usb-gecko";
		speed = EXI_CSR_CLK_32MHZ;
	}

	dev_info(exi->dev, "[%d:%d]: new modalias: %s\n", channel, cs, modalias);

	/* set info */
	strscpy(info.modalias, modalias, SPI_NAME_SIZE);
	info.mode = SPI_MODE_0;
	info.chip_select = cs;
	info.max_speed_hz = exi_speed_exi_to_spi(speed);
	info.controller_data = ctlr;
	exi_set_device_mode(exi, channel, cs, modalias);

	/* create it */
	device = spi_new_device(ctlr, &info);
	if (!device) {
		exi->channels[channel].cs_mode[cs] = EXI_CS_NORMAL;
		dev_err(exi->dev, "[%d:%d]: spi_new_device failed\n", channel, cs);
		return -ENOMEM;
	}

	exi->channels[channel].devices[cs] = device;
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
	bool present = exi_slot_present(exi, channel, cs);

	if (present) {
		if (!exi->channels[channel].devices[cs])
			exi_probe(exi, channel, cs);
	} else if (exi->channels[channel].devices[cs]) {
		exi_remove_device(exi, channel, cs);
	}
}

static void exi_rescan_channel(struct exi_spi *exi, unsigned int channel)
{
	unsigned int cs;

	for (cs = 0; cs < 3; cs++) {
		if (exi_is_hotplug_slot(channel, cs))
			exi_rescan_slot(exi, channel, cs);
	}
}

static void exi_hotplug_work(struct work_struct *work)
{
	struct exi_spi *exi = container_of(work, struct exi_spi, hotplug_work);
	unsigned int channel;

	for (channel = 0; channel < 3; channel++) {
		if (test_and_clear_bit(channel, &exi->pending_hotplug))
			exi_rescan_channel(exi, channel);
	}
}

static irqreturn_t exi_irq(int irq, void *data)
{
	struct exi_spi *exi = data;
	unsigned int channel;
	bool handled = false;

	for (channel = 0; channel < 3; channel++) {
		struct exi_channel *ch = &exi->channels[channel];
		u32 csr = in_be32(&ch->regs->csr);

		if (!(csr & EXI_CSR_EXTINT))
			continue;

		exi_clear_ext(ch);
		set_bit(channel, &exi->pending_hotplug);
		handled = true;
	}

	if (handled)
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

	while (len) {
		unsigned int xferLen;

		/* TODO: DMA for half-duplex transfers >= 32B */
		if (len >= 4) {
			xferLen = 4;
			len -= 4;
		} else { /* short / end of transfer */
			xferLen = len;
			len = 0;
		}

		if (rx && tx)
			ret = exi_rdwr_imm(channel, xferLen, tx, rx);
		else if (rx)
			ret = exi_read_imm(channel, xferLen, rx);
		else if (tx)
			ret = exi_write_imm(channel, xferLen, tx);

		if (ret)
			break;

		if (tx)
			tx += xferLen;
		if (rx)
			rx += xferLen;
	}

	if (!ret)
		spi_finalize_current_transfer(ctlr);
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
	u32 csr = EXI_CSR_EXTINTMASK | EXI_CSR_EXTINT;

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

	ret = devm_request_irq(&pdev->dev, exi->irq, exi_irq, 0,
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
