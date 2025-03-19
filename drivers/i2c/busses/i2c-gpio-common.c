/*
 * Core logic for the bitbanging I2C bus driver using the GPIO API
 *
 * Copyright (C) 2007 Atmel Corporation
 * Copyright (C) 2025 Techflash
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "i2c-gpio-common.h"

#include <linux/i2c-algo-bit.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>

struct i2c_gpio_desc {
	struct gpio_desc *sda;
	struct gpio_desc *scl;
	struct i2c_algo_bit_data bit_data;
};

/* Toggle SDA by changing the direction of the pin */
static void i2c_gpio_setsda_dir(void *data, int state)
{
	struct i2c_gpio_desc *desc = data;

	if (state)
		gpiod_direction_input(desc->sda);
	else
		gpiod_direction_output(desc->sda, 0);
}

/*
 * Toggle SDA by changing the output value of the pin. This is only
 * valid for pins configured as open drain (i.e. setting the value
 * high effectively turns off the output driver.)
 */
static void i2c_gpio_setsda_val(void *data, int state)
{
	struct i2c_gpio_desc *desc = data;

	gpiod_set_value(desc->sda, state);
}

/*
 * Toggle SCL by changing the output value of the pin. This is used
 * for pins that are configured as open drain and for output-only
 * pins. The latter case will break the i2c protocol, but it will
 * often work in practice.
 */
static void i2c_gpio_setscl_val(void *data, int state)
{
	struct i2c_gpio_desc *desc = data;

	gpiod_set_value(desc->scl, state);
}

static int i2c_gpio_getsda(void *data)
{
	struct i2c_gpio_desc *desc = data;

	return gpiod_get_value(desc->sda);
}

static int i2c_gpio_getscl(void *data)
{
	struct i2c_gpio_desc *desc = data;

	return gpiod_get_value(desc->scl);
}

int i2c_gpio_adapter_probe(struct i2c_adapter *adap, struct device *dev)
{
	struct i2c_gpio_desc *desc;
	int error;

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->sda = devm_gpiod_get(dev, "sda", GPIOD_ASIS);
	if (IS_ERR(desc->sda))
		return PTR_ERR(desc->sda);

	desc->scl = devm_gpiod_get(dev, "scl", GPIOD_ASIS);
	if (IS_ERR(desc->scl))
		return PTR_ERR(desc->scl);

	desc->bit_data.setsda = i2c_gpio_setsda_val;
	desc->bit_data.setscl = i2c_gpio_setscl_val;
	desc->bit_data.getsda = i2c_gpio_getsda;
	desc->bit_data.getscl = i2c_gpio_getscl;
	desc->bit_data.udelay = 5;
	desc->bit_data.timeout = HZ / 10;
	desc->bit_data.data = desc;

	adap->algo_data = &desc->bit_data;
	adap->dev.parent = dev;
	snprintf(adap->name, sizeof(adap->name), "i2c-gpio");

	return i2c_bit_add_numbered_bus(adap);
}
EXPORT_SYMBOL(i2c_gpio_adapter_probe);

int i2c_gpio_adapter_remove(struct i2c_adapter *adap)
{
	i2c_del_adapter(adap);
	return 0;
}
EXPORT_SYMBOL(i2c_gpio_adapter_remove);

MODULE_AUTHOR("Haavard Skinnemoen <hskinnemoen@atmel.com>");
MODULE_DESCRIPTION("Platform-independent bitbanging I2C driver common logic");
MODULE_LICENSE("GPL");
