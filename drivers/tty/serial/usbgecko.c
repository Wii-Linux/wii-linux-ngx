// SPDX-License-Identifier: GPL-2.0+
/*
 * drivers/serial/usbgecko.c
 *
 * Console and TTY driver for the USB Gecko adapter.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 * Copyright (C) 2024-2026 Michael "Techflash" Garofalo
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <uapi/linux/sched/types.h>

#define DRV_MODULE_NAME "usbgecko"
#define DRV_DESCRIPTION "Console and TTY driver for the USB Gecko adapter"
#define DRV_AUTHOR      "Albert Herranz, Michael \"Techflash\" Garofalo"

static char ug_driver_version[] = "0.4";

struct ug_adapter {
	struct spi_device *spi_device;
	struct task_struct *poller;
	struct mutex mutex;
	int refcnt;
};

static struct ug_adapter ug_adapters[2];


/*
 *
 * Hardware interface.
 */

/*
 *
 */
static void ug_spi_io_transaction(struct spi_device *spi_device, u16 i, u16 *o)
{
	struct spi_transfer spi_xfer = {
		.tx_buf = &i,
		.rx_buf = o,
		.len = 2
	};

	spi_sync_transfer(spi_device, &spi_xfer, 1);
}

#if 0
/*
 *
 */
static void ug_io_transaction(struct ug_adapter *adapter, u16 i, u16 *o)
{
	struct spi_device *spi_device = adapter->spi_device;

	if (spi_device)
		ug_spi_io_transaction(spi_device, i, o);
}
#endif

/*
 *
 */
static int ug_check_adapter(struct spi_device *spi_device)
{
	u16 data;
	int tries = 10;

	while (tries--) {
		ug_spi_io_transaction(spi_device, 0x9000, &data);

		if (data == 0x0470)
			return 1;

		msleep(50); // give it some time to wake up
	}

	dev_err(&spi_device->dev, "check failed, got 0x%04x from 0x9000\n", data);
	return 0;

}


/*
 *
 */
#ifdef CONFIG_CONSOLE_POLL
static int ug_tx_ready(struct ug_adapter *adapter)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xC000, &data);
	return data & 0x0400;
}

/*
 *
 */
static int ug_rx_ready(struct ug_adapter *adapter)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xD000, &data);
	return data & 0x0400;
}
#endif

#if 0
/*
 *
 */
static int ug_putc(struct ug_adapter *adapter, char c)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xB000| (c << 4), &data);
	return data & 0x0400;
}

/*
 *
 */
static int ug_getc(struct ug_adapter *adapter, char *c)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xA000, &data);
	if ((data & 0x0800)) {
		*c = data & 0xff;
		return 1;
	}
	return 0;
}
#endif 
/*
 *
 */
static int ug_safe_putc(struct ug_adapter *adapter, char c)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xC000, &data);
	if ((data & 0x0400))
		ug_spi_io_transaction(spi_device, 0xB000|(c<<4), &data);
	return data & 0x0400;
}

/*
 *
 */
static int ug_safe_getc(struct ug_adapter *adapter, char *c)
{
	struct spi_device *spi_device = adapter->spi_device;
	u16 data;

	if (!spi_device)
		return 0;

	ug_spi_io_transaction(spi_device, 0xD000, &data);
	if ((data & 0x0400)) {
		ug_spi_io_transaction(spi_device, 0xA000, &data);
		if ((data & 0x0800)) {
			*c = data & 0xff;
			return 1;
		}
	}
	return 0;
}

/*
 *
 * Linux console interface.
 */

/*
 *
 */
static void ug_console_write(struct console *co, const char *buf,
			      unsigned int count)
{
	struct ug_adapter *adapter = co->data;
	char *b = (char *)buf;

	while (count--) {
		if (*b == '\n')
			ug_safe_putc(adapter, '\r');
		ug_safe_putc(adapter, *b++);
	}
}

/*
 *
 */
static int ug_console_read(struct console *co, char *buf,
			    unsigned int count)
{
	struct ug_adapter *adapter = co->data;
	int i;
	char c;

	i = count;
	while (i--) {
		ug_safe_getc(adapter, &c);
		*buf++ = c;
	}
	return count;
}

static struct tty_driver *ug_tty_driver;

static struct tty_driver *ug_console_device(struct console *co, int *index)
{
	*index = co->index;
	return ug_tty_driver;
}


static struct console ug_consoles[] = {
	{
		.name   = DRV_MODULE_NAME,
		.write  = ug_console_write,
		.read   = ug_console_read,
		.device = ug_console_device,
		.flags  = CON_PRINTBUFFER | CON_ENABLED,
		.index  = 0,
		.data	= &ug_adapters[0],
	},
	{
		.name   = DRV_MODULE_NAME,
		.write  = ug_console_write,
		.read   = ug_console_read,
		.device = ug_console_device,
		.flags  = CON_PRINTBUFFER | CON_ENABLED,
		.index  = 1,
		.data	= &ug_adapters[1],
	},
};


/*
 *
 * Linux tty driver.
 */

static int ug_tty_poller(void *tty_)
{
	struct sched_param param = { .sched_priority = 1 };
	struct tty_struct *tty = tty_;
	struct ug_adapter *adapter;
	int count, chunk;
	const int max_outstanding = 32;
	char ch;

	sched_setscheduler(current, SCHED_FIFO, &param);
	set_current_state(TASK_RUNNING);

	chunk = 0;
	while (!kthread_should_stop()) {
		count = 0;
		adapter = tty->driver_data;
		if (adapter)
			count = ug_safe_getc(adapter, &ch);
		set_current_state(TASK_INTERRUPTIBLE);
		if (count) {
			tty_insert_flip_char(ug_tty_driver->ports[tty->index], ch, TTY_NORMAL);
			if (chunk++ > max_outstanding) {
				tty_flip_buffer_push(ug_tty_driver->ports[tty->index]);
				chunk = 0;
			}
		} else {
			if (chunk) {
				tty_flip_buffer_push(ug_tty_driver->ports[tty->index]);
				chunk = 0;
			}
			schedule_timeout(1);
		}
		set_current_state(TASK_RUNNING);
	}

	return 0;
}

static int ug_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct ug_adapter *adapter;
	int index;
	int retval = 0;

	index = tty->index;
	adapter = &ug_adapters[index];

	mutex_lock(&adapter->mutex);

	if (!adapter->spi_device) {
		mutex_unlock(&adapter->mutex);
		return -ENODEV;
	}

	if (!adapter->refcnt) {
		adapter->poller = kthread_run(ug_tty_poller, tty, "kugtty");
		if (IS_ERR(adapter->poller)) {
			dev_err(&adapter->spi_device->dev, "error creating poller thread\n");
			mutex_unlock(&adapter->mutex);
			return -ENOMEM;
		}
	}

	adapter->refcnt++;
	tty->driver_data = adapter;

	mutex_unlock(&adapter->mutex);

	return retval;
}

static void ug_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct ug_adapter *adapter;
	int index;

	index = tty->index;
	adapter = &ug_adapters[index];

	mutex_lock(&adapter->mutex);

	adapter->refcnt--;
	if (!adapter->refcnt) {
		if (!IS_ERR(adapter->poller))
			kthread_stop(adapter->poller);
		adapter->poller = ERR_PTR(-EINVAL);
		tty->driver_data = NULL;
	}

	mutex_unlock(&adapter->mutex);
}

static int ug_tty_write(struct tty_struct *tty,
			 const unsigned char *buf, size_t count)
{
	struct ug_adapter *adapter = tty->driver_data;
	char *b = (char *)buf;
	int index;
	int i;

	if (!adapter)
		return -ENODEV;

	index = tty->index;
	adapter = &ug_adapters[index];
	for (i = 0; i < count; i++)
		ug_safe_putc(adapter, *b++);
	return count;
}

static unsigned int ug_tty_write_room(struct tty_struct *tty)
{
	return 0x123; /* whatever */
}

static unsigned int ug_tty_chars_in_buffer(struct tty_struct *tty)
{
	return 0; /* unbuffered */
}

#ifdef CONFIG_CONSOLE_POLL
static int ug_poll_init(struct tty_driver *driver, int line, char *options)
{
	return 0;
}

static int ug_poll_get_char(struct tty_driver *driver, int line)
{
	struct tty_struct *tty = driver->ttys[line];
	struct ug_adapter *adapter = tty->driver_data;
	char ch;

	while (!ug_rx_ready(adapter))
		cpu_relax();
	ug_safe_getc(adapter, &ch);
	return ch;
}

static void ug_poll_put_char(struct tty_driver *driver, int line, char c)
{
	struct tty_struct *tty = driver->ttys[line];
	struct ug_adapter *adapter = tty->driver_data;

	while (!ug_tx_ready(adapter))
		cpu_relax();
	ug_safe_putc(adapter, c);
}
#endif

static const struct tty_operations ug_tty_ops = {
	.open = ug_tty_open,
	.close = ug_tty_close,
	.write = ug_tty_write,
	.write_room = ug_tty_write_room,
	.chars_in_buffer = ug_tty_chars_in_buffer,
#ifdef CONFIG_CONSOLE_POLL
	.poll_init = ug_poll_init,
	.poll_get_char = ug_poll_get_char,
	.poll_put_char = ug_poll_put_char,
#endif
};


static int ug_tty_init(void)
{
	struct tty_driver *driver;
	int retval;

	driver = tty_alloc_driver(2, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(driver))
		return PTR_ERR(driver);
	driver->name = DRV_MODULE_NAME "con";
	driver->major = TTY_MAJOR;
	driver->minor_start = 64;
	driver->type = TTY_DRIVER_TYPE_SYSCONS;
	driver->init_termios = tty_std_termios;
	tty_set_operations(driver, &ug_tty_ops);

	retval = tty_register_driver(driver);
	if (retval) {
		tty_driver_kref_put(driver);
		return retval;
	}
	ug_tty_driver = driver;
	return 0;
}

static void ug_tty_exit(void)
{
	struct tty_driver *driver = ug_tty_driver;

	ug_tty_driver = NULL;
	if (driver) {
		tty_unregister_driver(driver);
		tty_driver_kref_put(driver);
	}
}



/*
 *
 * EXI layer interface.
 */

/*
 *
 */
static int ug_probe(struct spi_device *spi_device)
{
	struct console *console;
	struct ug_adapter *adapter;
	unsigned int slot;
	struct tty_port *port;

	dev_info(&spi_device->dev, "probing for channel %d, device %d\n",
	spi_device->controller->bus_num, spi_get_chipselect(spi_device, 0));

	/* don't try to drive a device which already has a real identifier */
#if 0
	if (exi_device->eid.id != EXI_ID_NONE) {
		dev_err(&exi_device->dev, "device ID is not NONE (0x%x), skipping\n",
		        exi_device->eid.id);
		return -ENODEV;
	}
#endif

	if (!ug_check_adapter(spi_device)) {
		dev_err(&spi_device->dev, "check_adapter() failed\n");
		return -ENODEV;
	}

	slot = spi_device->controller->bus_num;
	console = &ug_consoles[slot];
	adapter = console->data;

	if (!ug_tty_driver->ports[slot]) {
		dev_info(&spi_device->dev, "initializing console on slot %c\n", 'A'+slot);
		port = kmalloc(sizeof(*port), GFP_KERNEL);

		if (!port)
			return -ENOMEM;

		tty_port_init(port);
		ug_tty_driver->ports[slot] = port;
		tty_port_register_device(port, ug_tty_driver, slot, NULL);
	}


	dev_info(&spi_device->dev, "USB Gecko detected in memcard slot-%c\n",
		   'A'+slot);

	adapter->poller = ERR_PTR(-EINVAL);
	mutex_init(&adapter->mutex);
	adapter->refcnt = 0;

	adapter->spi_device = spi_dev_get(spi_device);
	spi_set_drvdata(spi_device, adapter);
	register_console(console);


	return 0;
}

/*
 * Makes unavailable the USB Gecko adapter identified by the EXI device
 * `spi_device'.
 */
static void ug_remove(struct spi_device *spi_device)
{
	struct console *console;
	struct ug_adapter *adapter;
	unsigned int slot;

	dev_info(&spi_device->dev, "removing device on channel %d, device %d\n",
	spi_device->controller->bus_num, spi_get_chipselect(spi_device, 0));

	slot = spi_device->controller->bus_num;
	console = &ug_consoles[slot];
	adapter = console->data;

	if (adapter->refcnt)
		dev_err(&spi_device->dev, "adapter removed while in use!\n");

	unregister_console(console);

	if (ug_tty_driver->ports[slot]) {
		tty_unregister_device(ug_tty_driver, slot);
		tty_port_destroy(ug_tty_driver->ports[slot]);
		kfree(ug_tty_driver->ports[slot]);
		ug_tty_driver->ports[slot] = NULL;
	}


	spi_set_drvdata(spi_device, NULL);
	adapter->spi_device = NULL;
	spi_dev_put(spi_device);


	mutex_destroy(&adapter->mutex);

	dev_info(&spi_device->dev, "USB Gecko removed from memcard slot-%c\n",
		   'A'+slot);
}

static const struct spi_device_id ug_id_table[] = {
	{ "exi-usb-gecko", },
	{ }
};
MODULE_DEVICE_TABLE(spi, ug_id_table);


static struct spi_driver ug_spi_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
	},
	.id_table = ug_id_table,
	.probe = ug_probe,
	.remove = ug_remove,
};


/*
 *
 * Module interface.
 */

static int __init ug_init_module(void)
{
	int retval;

	pr_info("%s - version %s\n", DRV_DESCRIPTION,
		   ug_driver_version);

	retval = ug_tty_init();
	if (retval)
		return retval;

	retval = spi_register_driver(&ug_spi_driver);
	if (retval)
		ug_tty_exit();

	return retval;
}

static void __exit ug_exit_module(void)
{
	spi_unregister_driver(&ug_spi_driver);
	ug_tty_exit();
}

module_init(ug_init_module);
module_exit(ug_exit_module);

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_LICENSE("GPL");

