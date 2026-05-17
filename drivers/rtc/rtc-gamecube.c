// SPDX-License-Identifier: GPL-2.0
/*
 * Nintendo GameCube, Wii and Wii U RTC driver
 *
 * This driver is for the MX23L4005, more specifically its real-time clock and
 * SRAM storage.  The value returned by the RTC counter must be added with the
 * offset stored in a bias register in SRAM (on the GameCube and Wii) or in
 * /config/rtc.xml (on the Wii U).  The latter being very impractical to access
 * from Linux, this driver assumes the bootloader has read it and stored it in
 * SRAM like for the other two consoles.
 *
 * This device sits on a bus named EXI (which is similar to SPI), channel 0,
 * device 1.  This driver is written to assume that EXI is exposed as an SPI
 * host.
 *
 * References:
 * - https://wiiubrew.org/wiki/Hardware/RTC
 * - https://wiibrew.org/wiki/MX23L4005
 *
 * Copyright (C) 2018 rw-r-r-0644
 * Copyright (C) 2021 Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on rtc-gcn.c
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2005,2008,2009 Albert Herranz
 * Based on gamecube_time.c from Torben Nielsen.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/spi/spi.h>

/* RTC registers */
#define RTC_COUNTER	0x200000
#define RTC_SRAM	0x200001
#define RTC_SRAM_BIAS	0x200004
#define RTC_SNAPSHOT	0x204000
#define RTC_ONTMR	0x210000
#define RTC_OFFTMR	0x210001
#define RTC_TEST0	0x210004
#define RTC_TEST1	0x210005
#define RTC_TEST2	0x210006
#define RTC_TEST3	0x210007
#define RTC_CONTROL0	0x21000c
#define RTC_CONTROL1	0x21000d

/* RTC flags */
#define RTC_CONTROL0_UNSTABLE_POWER	0x00000800
#define RTC_CONTROL0_LOW_BATTERY	0x00000200

struct priv {
	struct regmap *regmap;
	struct spi_device *spi;
	u32 rtc_bias;
};

static int rtc_reg_read(void *context, u32 reg, u32 *data)
{
	int err;
	__be32 cmd = cpu_to_be32(reg << 8);
	struct priv *d = (struct priv *)context;
	struct spi_transfer xfers[2] = {
		{ .tx_buf = &cmd, .rx_buf = NULL, .len = 4 },
		{ .tx_buf = NULL, .rx_buf = data, .len = 4 }
	};

	/* Write register offset and read data */
	err = spi_sync_transfer(d->spi, xfers, 2);
	if (err)
		return err;

	return 0;
}

static int rtc_reg_write(void *context, u32 reg, u32 data)
{
	int err;
	__be32 cmd = cpu_to_be32(reg << 8);
	struct priv *d = (struct priv *)context;
	struct spi_transfer xfers[2] = {
		{ .tx_buf = &cmd, .rx_buf = NULL, .len = 4 },
		{ .tx_buf = &data, .rx_buf = NULL, .len = 4 }
	};

	/* Write register offset and write data */
	err = spi_sync_transfer(d->spi, xfers, 2);
	if (err)
		return err;

	return 0;
}

static const struct regmap_bus rtc_regmap = {
	/* TODO: is that true?  Not that it matters here, but still. */
	.fast_io = true,
	.reg_read = rtc_reg_read,
	.reg_write = rtc_reg_write,
};

static int gamecube_rtc_read_time(struct device *dev, struct rtc_time *t)
{
	struct priv *d = dev_get_drvdata(dev);
	int ret;
	u32 counter;
	time64_t timestamp;

	ret = regmap_read(d->regmap, RTC_COUNTER, &counter);
	if (ret)
		return ret;

	/* Add the counter and the bias to obtain the timestamp */
	timestamp = (time64_t)d->rtc_bias + counter;
	rtc_time64_to_tm(timestamp, t);

	return 0;
}

static int gamecube_rtc_set_time(struct device *dev, struct rtc_time *t)
{
	struct priv *d = dev_get_drvdata(dev);
	time64_t timestamp;

	/* Subtract the timestamp and the bias to obtain the counter value */
	timestamp = rtc_tm_to_time64(t);
	return regmap_write(d->regmap, RTC_COUNTER, timestamp - d->rtc_bias);
}

static int gamecube_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	struct priv *d = dev_get_drvdata(dev);
	int value;
	int control0;
	int ret;

	switch (cmd) {
	case RTC_VL_READ:
		ret = regmap_read(d->regmap, RTC_CONTROL0, &control0);
		if (ret)
			return ret;

		value = 0;
		if (control0 & RTC_CONTROL0_UNSTABLE_POWER)
			value |= RTC_VL_DATA_INVALID;
		if (control0 & RTC_CONTROL0_LOW_BATTERY)
			value |= RTC_VL_BACKUP_LOW;
		return put_user(value, (unsigned int __user *)arg);

	default:
		return -ENOIOCTLCMD;
	}
}

static const struct rtc_class_ops gamecube_rtc_ops = {
	.read_time	= gamecube_rtc_read_time,
	.set_time	= gamecube_rtc_set_time,
	.ioctl		= gamecube_rtc_ioctl,
};

static int gamecube_rtc_read_offset_from_sram(struct priv *d)
{
	struct device_node *np;
	int ret;
	struct resource res;
	void __iomem *hw_srnprot;
	u32 old;

	np = of_find_compatible_node(NULL, NULL, "nintendo,latte-srnprot");
	if (!np)
		np = of_find_compatible_node(NULL, NULL,
					     "nintendo,hollywood-srnprot");
	if (!np) {
		pr_info("HW_SRNPROT not found, assuming a GameCube\n");
		return regmap_read(d->regmap, RTC_SRAM_BIAS, &d->rtc_bias);
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		pr_err("no io memory range found\n");
		return -1;
	}

	hw_srnprot = ioremap(res.start, resource_size(&res));
	if (!hw_srnprot) {
		pr_err("failed to ioremap hw_srnprot\n");
		return -ENOMEM;
	}
	old = ioread32be(hw_srnprot);

	/* TODO: figure out why we use this magic constant.  I obtained it by
	 * reading the leftover value after boot, after IOSU already ran.
	 *
	 * On my Wii U, setting this register to 1 prevents the console from
	 * rebooting properly, so wiiubrew.org must be missing something.
	 *
	 * See https://wiiubrew.org/wiki/Hardware/Latte_registers
	 */
	if (old != 0x7bf)
		iowrite32be(0x7bf, hw_srnprot);

	/* Get the offset from RTC SRAM.
	 *
	 * Its default location on the GameCube and on the Wii is in the SRAM,
	 * while on the Wii U the bootloader needs to fill it with the contents
	 * of /config/rtc.xml on the SLC (the eMMC).  We don’t do that from
	 * Linux since it requires implementing a proprietary filesystem and do
	 * file decryption, instead we require the bootloader to fill the same
	 * SRAM address as on previous consoles.
	 */
	ret = regmap_read(d->regmap, RTC_SRAM_BIAS, &d->rtc_bias);

	/* Reset SRAM access to how it was before, our job here is done. */
	if (old != 0x7bf)
		iowrite32be(old, hw_srnprot);

	iounmap(hw_srnprot);

	if (ret)
		pr_err("failed to get the RTC bias\n");

	return ret;
}

static const struct regmap_range rtc_rd_ranges[] = {
	regmap_reg_range(0x200000, 0x200010),
	regmap_reg_range(0x204000, 0x204000),
	regmap_reg_range(0x210000, 0x210001),
	regmap_reg_range(0x210004, 0x210007),
	regmap_reg_range(0x21000c, 0x21000d),
};

static const struct regmap_access_table rtc_rd_regs = {
	.yes_ranges =	rtc_rd_ranges,
	.n_yes_ranges =	ARRAY_SIZE(rtc_rd_ranges),
};

static const struct regmap_range rtc_wr_ranges[] = {
	regmap_reg_range(0x200000, 0x200010),
	regmap_reg_range(0x204000, 0x204000),
	regmap_reg_range(0x210000, 0x210001),
	regmap_reg_range(0x21000d, 0x21000d),
};

static const struct regmap_access_table rtc_wr_regs = {
	.yes_ranges =	rtc_wr_ranges,
	.n_yes_ranges =	ARRAY_SIZE(rtc_wr_ranges),
};

static const struct regmap_config gamecube_rtc_regmap_config = {
	.reg_bits = 24,
	.val_bits = 32,
	.rd_table = &rtc_rd_regs,
	.wr_table = &rtc_wr_regs,
	.max_register = 0x21000d,
	.name = "gamecube-rtc",
};

static int gamecube_rtc_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct rtc_device *rtc;
	struct priv *d;
	int ret;

	d = devm_kzalloc(dev, sizeof(struct priv), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->spi = spi;
	d->regmap = devm_regmap_init(dev, &rtc_regmap, d,
				     &gamecube_rtc_regmap_config);
	if (IS_ERR(d->regmap))
		return PTR_ERR(d->regmap);

	ret = gamecube_rtc_read_offset_from_sram(d);
	if (ret)
		return ret;
	dev_dbg(dev, "SRAM bias: 0x%x", d->rtc_bias);

	dev_set_drvdata(dev, d);

	rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	/* We can represent further than that, but it depends on the stored
	 * bias and we can’t modify it persistently on all supported consoles,
	 * so here we pretend to be limited to 2106.
	 */
	rtc->range_min = 0;
	rtc->range_max = U32_MAX;
	rtc->ops = &gamecube_rtc_ops;

	devm_rtc_register_device(rtc);

	return 0;
}

static const struct spi_device_id gamecube_rtc_id_table[] = {
	{ "gamecube-rtc", },
	{ }
};
MODULE_DEVICE_TABLE(spi, gamecube_rtc_id_table);

static struct spi_driver gamecube_rtc_driver = {
	.probe		= gamecube_rtc_probe,
	.driver		= {
		.name	= "rtc-gamecube",
	},
	.id_table	= gamecube_rtc_id_table
};
module_spi_driver(gamecube_rtc_driver);

MODULE_AUTHOR("Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>");
MODULE_DESCRIPTION("Nintendo GameCube, Wii and Wii U RTC driver");
MODULE_LICENSE("GPL");
