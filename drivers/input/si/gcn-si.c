// SPDX-License-Identifier: GPL-2.0+
/*
 * Nintendo GameCube/Wii Serial Interface (SI) driver.
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Steven Looman
 * Copyright (C) 2005,2008,2009 Albert Herranz
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */

/* #define SI_DEBUG */

#include <linux/completion.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

/*
 * This keymap is for a datel adapter + normal US keyboard.
 */
#include "gcn-keymap.h"

/*
 * Defining HACK_FORCE_KEYBOARD_PORT allows one to specify a port that
 * will be identified as a keyboard port in case the port gets incorrectly
 * identified.
 */
#define HACK_FORCE_KEYBOARD_PORT


#define DRV_MODULE_NAME  "gcn-si"
#define DRV_DESCRIPTION  "Nintendo GameCube/Wii Serial Interface (SI) driver"
#define DRV_AUTHOR       "Steven Looman <steven@krx.nl>, " \
			 "Albert Herranz"

static const char si_driver_version[] = "1.1t";

#define SI_MAX_PORTS		4		/* the four controller ports */
#define SI_REFRESH_TIME		(HZ/100)	/* input polling interval */
#define SI_HOTPLUG_TIME		HZ		/* device detection interval */
#define SI_TRANSFER_TIMEOUT	(HZ/10)		/* timeout for transfers */
#define SI_PROBE_TRIES		1

/*
 * Hardware registers
 */
#define SI_PORT_SPACING	12

#define SICOUTBUF(i)	(0x00 + (i)*SI_PORT_SPACING)
#define SICINBUFH(i)	(0x04 + (i)*SI_PORT_SPACING)
#define SICINBUFL(i)	(0x08 + (i)*SI_PORT_SPACING)
#define SIPOLL		0x30
#define SICOMCSR	0x34
#define SISR		0x38
#define SIEXILK		0x3c
#define SIBUF(i)	(0x80 + (i) * sizeof(u32))
#define SI_BUF_SIZE	0x80

/* SICOMCSR bits */
#define SI_COMCSR_TSTART		BIT(0)
#define SI_COMCSR_CHAN_SHIFT		1
#define SI_COMCSR_INLEN_SHIFT		8
#define SI_COMCSR_OUTLEN_SHIFT		16
#define SI_COMCSR_RDSTINT		BIT(28)
#define SI_COMCSR_COMERR		BIT(29)
#define SI_COMCSR_TCINTMSK		BIT(30)
#define SI_COMCSR_TCINT			BIT(31)
#define SI_COMCSR_W1C_MASK		(SI_COMCSR_TCINT | SI_COMCSR_RDSTINT)

/* SIPOLL bits */
#define SI_POLL_VBCPY_SHIFT		0
#define SI_POLL_EN_SHIFT		4
#define SI_POLL_Y_SHIFT			8
#define SI_POLL_X_SHIFT			16

/* SISR bits */
#define SI_SR_WR			BIT(31)
#define SI_SR_NOREP(n)			BIT(3 + ((3 - (n)) * 8))
#define SI_SR_COLL(n)			BIT(2 + ((3 - (n)) * 8))
#define SI_SR_OVRUN(n)			BIT(1 + ((3 - (n)) * 8))
#define SI_SR_UNRUN(n)			BIT((3 - (n)) * 8)
#define SI_SR_ERR_MASK(n)		(SI_SR_NOREP(n) | SI_SR_COLL(n) | \
					 SI_SR_OVRUN(n) | SI_SR_UNRUN(n))
#define SI_SR_ALL_ERR_MASK		(SI_SR_ERR_MASK(0) | SI_SR_ERR_MASK(1) | \
					 SI_SR_ERR_MASK(2) | SI_SR_ERR_MASK(3))

/* JoyBus commands */
#define JOYBUS_CMD_STATUS		0x00
#define JOYBUS_CMD_DIRECT_GCN		0x40
#define JOYBUS_CMD_DIRECT_GCN_KB	0x54

#define SI_MKOUTBUF(cmd, output0, output1) \
	(((u32)(cmd) << 16) | ((u32)(output0) << 8) | (u32)(output1))
#define SI_MKPOLL(vbcpy, en, y, x) \
	((((u32)(vbcpy) & 0x0f) << SI_POLL_VBCPY_SHIFT) | \
	 (((u32)(en) & 0x0f) << SI_POLL_EN_SHIFT) | \
	 (((u32)(y) & 0xff) << SI_POLL_Y_SHIFT) | \
	 (((u32)(x) & 0x3ff) << SI_POLL_X_SHIFT))

enum si_device_id {
	SI_DEVICE_ID_N64_MIC		= 0x0001,
	SI_DEVICE_ID_N64_KBD		= 0x0002,
	SI_DEVICE_ID_GBA		= 0x0004,
	SI_DEVICE_ID_N64_MOUSE		= 0x0200,
	SI_DEVICE_ID_N64_CONTROLLER	= 0x0500,
	SI_DEVICE_ID_GBA_2		= 0x0800,
	SI_DEVICE_ID_GCN_KBD		= 0x0820,
	SI_DEVICE_ID_STANDARD		= 0x0900,
	SI_DEVICE_ID_WAVEBIRD_1		= 0xa800,
	SI_DEVICE_ID_WAVEBIRD_2		= 0xe960,
	SI_DEVICE_ID_WAVEBIRD_3		= 0xe9a0,
	SI_DEVICE_ID_WAVEBIRD_4		= 0xebb0,
};

/* GameCube controller direct report */
#define GCN_PAD_LSTICK_Y_SHIFT		0
#define GCN_PAD_LSTICK_Y		(0xffu << GCN_PAD_LSTICK_Y_SHIFT)
#define GCN_PAD_LSTICK_X_SHIFT		8
#define GCN_PAD_LSTICK_X		(0xffu << GCN_PAD_LSTICK_X_SHIFT)
#define GCN_PAD_LEFT			BIT(16)
#define GCN_PAD_RIGHT			BIT(17)
#define GCN_PAD_DOWN			BIT(18)
#define GCN_PAD_UP			BIT(19)
#define GCN_PAD_Z			BIT(20)
#define GCN_PAD_RT			BIT(21)
#define GCN_PAD_LT			BIT(22)
#define GCN_PAD_A			BIT(24)
#define GCN_PAD_B			BIT(25)
#define GCN_PAD_X			BIT(26)
#define GCN_PAD_Y			BIT(27)
#define GCN_PAD_START			BIT(28)
#define GCN_PAD_RTRIG_SHIFT		0
#define GCN_PAD_RTRIG			(0xffu << GCN_PAD_RTRIG_SHIFT)
#define GCN_PAD_LTRIG_SHIFT		8
#define GCN_PAD_LTRIG			(0xffu << GCN_PAD_LTRIG_SHIFT)
#define GCN_PAD_CSTICK_Y_SHIFT		16
#define GCN_PAD_CSTICK_Y		(0xffu << GCN_PAD_CSTICK_Y_SHIFT)
#define GCN_PAD_CSTICK_X_SHIFT		24
#define GCN_PAD_CSTICK_X		(0xffu << GCN_PAD_CSTICK_X_SHIFT)


struct si_keyboard_status {
	unsigned char old[3];
};

enum si_control_type {
	CTL_NONE,
	CTL_PAD,
	CTL_KEYBOARD,
	CTL_GBA,
	CTL_N64_PAD,
	CTL_N64_KEYBOARD,
	CTL_N64_MOUSE,
	CTL_N64_MIC,
	CTL_UNKNOWN
};

struct si_drvdata;

struct si_port {
	unsigned int index;
	struct si_drvdata *drvdata;

	u32 id; /* SI id */

	enum si_control_type type;
	bool registered;
	bool opened;

	struct input_dev *idev;
	struct timer_list timer;
	char name[32];

	union {
		struct si_keyboard_status keyboard;
	};

};

struct si_drvdata {
	unsigned long flags;
#define SI_QUIESCE	(1<<0)

	struct si_port ports[SI_MAX_PORTS];
	struct delayed_work hotplug_work;

	void __iomem *io_base;
	int irq;
	struct completion transfer_done;
	bool ignore_collisions;

	struct device *dev;
};


#ifdef HACK_FORCE_KEYBOARD_PORT

static int si_force_keyboard_port = -1;

#ifdef MODULE
module_param_named(force_keyboard_port, si_force_keyboard_port, int, 0644);
MODULE_PARM_DESC(force_keyboard_port, "port n becomes a keyboard port if"
		 " automatic identification fails");
#else
static int __init si_force_keyboard_port_setup(char *line)
{
	if (sscanf(line, "%d", &si_force_keyboard_port) != 1)
		si_force_keyboard_port = -1;
	return 1;
}
__setup("force_keyboard_port=", si_force_keyboard_port_setup);
#endif /* MODULE */

#endif /* HACK_FORCE_KEYBOARD_PORT */


/*
 * Hardware.
 *
 */

static void si_drain_all_inbufs(void __iomem *io_base);

static void si_reset_all(void __iomem *io_base)
{
	int i;

	/* clear all SI registers */

	for (i = 0; i < SI_MAX_PORTS; ++i)
		out_be32(io_base + SICOUTBUF(i), 0);
	out_be32(io_base + SIPOLL, 0);
	out_be32(io_base + SICOMCSR, SI_COMCSR_W1C_MASK);
	out_be32(io_base + SISR, SI_SR_ALL_ERR_MASK);

	for (i = 0; i < SI_BUF_SIZE / sizeof(u32); ++i)
		out_be32(io_base + SIBUF(i), 0);

	si_drain_all_inbufs(io_base);
	out_be32(io_base + SIEXILK, 0);
}

static void si_set_rumbling(void __iomem *io_base, unsigned int index,
			    int rumble)
{
	out_be32(io_base + SICOUTBUF(index),
		 SI_MKOUTBUF(JOYBUS_CMD_DIRECT_GCN, 0, 0) |
		 (rumble ? 1 : 0));
	out_be32(io_base + SISR, SI_SR_WR);
}

static void si_drain_inbuf(void __iomem *io_base, unsigned int index)
{
	u32 resp;

	resp = in_be32(io_base + SICINBUFH(index));
	resp = in_be32(io_base + SICINBUFL(index));
	(void)resp;
}

static void si_drain_all_inbufs(void __iomem *io_base)
{
	unsigned int i;

	for (i = 0; i < SI_MAX_PORTS; ++i)
		si_drain_inbuf(io_base, i);
}

static void si_clear_iobuf(void __iomem *io_base)
{
	unsigned int i;

	for (i = 0; i < SI_BUF_SIZE / sizeof(u32); ++i)
		out_be32(io_base + SIBUF(i), 0);
}

static int si_flush_poll_buffers(struct si_drvdata *drvdata)
{
	void __iomem *io_base = drvdata->io_base;
	unsigned long deadline = jiffies + SI_TRANSFER_TIMEOUT;

	out_be32(io_base + SISR, SI_SR_WR);
	while (in_be32(io_base + SISR) & SI_SR_WR) {
		if (time_after(jiffies, deadline)) {
			dev_err(drvdata->dev, "SI buffer flush timed out\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	return 0;
}

enum si_comerr_result {
	SI_COMERR_NONE,
	SI_COMERR_NOREP,
	SI_COMERR_ERROR,
};

static enum si_comerr_result si_handle_comerr(struct si_drvdata *drvdata,
					      unsigned int index)
{
	void __iomem *io_base = drvdata->io_base;
	u32 sr;

	if (!(in_be32(io_base + SICOMCSR) & SI_COMCSR_COMERR))
		return SI_COMERR_NONE;

	sr = in_be32(io_base + SISR);
	out_be32(io_base + SISR, SI_SR_ERR_MASK(index));

	/* Latte reports a collision on every transfer, even successful ones. */
	if (drvdata->ignore_collisions) {
		sr &= ~SI_SR_COLL(index);
		if (!(sr & SI_SR_ERR_MASK(index)))
			return SI_COMERR_NONE;
	}

	if (sr & (SI_SR_COLL(index) | SI_SR_OVRUN(index) |
		  SI_SR_UNRUN(index))) {
		dev_warn(drvdata->dev,
			 "port %u transfer error: no response=%u collision=%u overrun=%u underrun=%u\n",
			 index + 1, !!(sr & SI_SR_NOREP(index)),
			 !!(sr & SI_SR_COLL(index)),
			 !!(sr & SI_SR_OVRUN(index)),
			 !!(sr & SI_SR_UNRUN(index)));
		return SI_COMERR_ERROR;
	}

	return SI_COMERR_NOREP;
}

static irqreturn_t si_irq(int irq, void *data)
{
	struct si_drvdata *drvdata = data;
	u32 comcsr;

	comcsr = in_be32(drvdata->io_base + SICOMCSR);
	if (!(comcsr & SI_COMCSR_TCINT))
		return IRQ_NONE;

	out_be32(drvdata->io_base + SICOMCSR, SI_COMCSR_TCINT);
	complete(&drvdata->transfer_done);

	return IRQ_HANDLED;
}

static int si_transfer(struct si_drvdata *drvdata, unsigned int index,
		       u32 out, unsigned int out_len, unsigned int in_len,
		       u32 *resp)
{
	void __iomem *io_base = drvdata->io_base;
	enum si_comerr_result comerr;
	u32 comcsr;

	reinit_completion(&drvdata->transfer_done);
	out_be32(io_base + SICOMCSR, SI_COMCSR_W1C_MASK);
	si_drain_all_inbufs(io_base);
	si_clear_iobuf(io_base);

	out_be32(io_base + SIBUF(0), out);
	comcsr = (out_len << SI_COMCSR_OUTLEN_SHIFT) |
		 (in_len << SI_COMCSR_INLEN_SHIFT) |
		 (index << SI_COMCSR_CHAN_SHIFT) |
		 SI_COMCSR_TCINTMSK | SI_COMCSR_TSTART;
	out_be32(io_base + SICOMCSR, comcsr);

	if (!wait_for_completion_timeout(&drvdata->transfer_done,
					 SI_TRANSFER_TIMEOUT)) {
		/*
		 * Mask and acknowledge the interrupt before synchronizing so a
		 * late completion cannot satisfy the next transfer's wait.
		 */
		out_be32(io_base + SICOMCSR, SI_COMCSR_W1C_MASK);
		synchronize_irq(drvdata->irq);
		dev_err(drvdata->dev, "port %u serial transfer timed out\n",
			index + 1);
		return -ETIMEDOUT;
	}

	comerr = si_handle_comerr(drvdata, index);
	if (comerr == SI_COMERR_NOREP)
		return -ENODEV;
	if (comerr == SI_COMERR_ERROR)
		return -EIO;

	if (resp)
		*resp = in_be32(io_base + SIBUF(0));
	return 0;
}

static void si_setup_polling(struct si_drvdata *drvdata)
{
	void __iomem *io_base = drvdata->io_base;
	unsigned int poll_bits = 0;
	int i;

	out_be32(io_base + SIPOLL, 0);

	for (i = 0; i < SI_MAX_PORTS; ++i) {
		switch (drvdata->ports[i].type) {
		case CTL_PAD:
			out_be32(io_base + SICOUTBUF(i),
				 SI_MKOUTBUF(JOYBUS_CMD_DIRECT_GCN, 0x03, 0));
			break;
		case CTL_KEYBOARD:
			out_be32(io_base + SICOUTBUF(i),
				 SI_MKOUTBUF(JOYBUS_CMD_DIRECT_GCN_KB, 0, 0));
			break;
		default:
			continue;
		}
		poll_bits |= 1 << (SI_POLL_EN_SHIFT + (3 - i));
	}

	out_be32(io_base + SIPOLL, SI_MKPOLL(0, poll_bits >> SI_POLL_EN_SHIFT,
					     1, 7));
	si_flush_poll_buffers(drvdata);
	out_be32(io_base + SICOMCSR, SI_COMCSR_W1C_MASK);
}

static void si_timer(struct timer_list *t)
{
	struct si_port *port = timer_container_of(port, t, timer);

	unsigned int index = port->index;
	void __iomem *io_base = port->drvdata->io_base;
	unsigned long raw[2];
	unsigned char key[3];
	unsigned char oldkey;
	int i;

	if (!port->registered || !port->idev)
		goto out;

	raw[0] = in_be32(io_base + SICINBUFH(index));
	raw[1] = in_be32(io_base + SICINBUFL(index));

	switch (port->type) {
	case CTL_PAD:
		/* buttons */
		input_report_key(port->idev, BTN_A, raw[0] & GCN_PAD_A);
		input_report_key(port->idev, BTN_B, raw[0] & GCN_PAD_B);
		input_report_key(port->idev, BTN_X, raw[0] & GCN_PAD_X);
		input_report_key(port->idev, BTN_Y, raw[0] & GCN_PAD_Y);
		input_report_key(port->idev, BTN_Z, raw[0] & GCN_PAD_Z);
		input_report_key(port->idev, BTN_TL,
				 raw[0] & GCN_PAD_LT);
		input_report_key(port->idev, BTN_TR,
				 raw[0] & GCN_PAD_RT);
		input_report_key(port->idev, BTN_START,
				 raw[0] & GCN_PAD_START);
		input_report_key(port->idev, BTN_0, raw[0] & GCN_PAD_UP);
		input_report_key(port->idev, BTN_1, raw[0] & GCN_PAD_RIGHT);
		input_report_key(port->idev, BTN_2, raw[0] & GCN_PAD_DOWN);
		input_report_key(port->idev, BTN_3, raw[0] & GCN_PAD_LEFT);

		/* axis */
		/* a stick */
		input_report_abs(port->idev, ABS_X,
				 (raw[0] & GCN_PAD_LSTICK_X) >>
				 GCN_PAD_LSTICK_X_SHIFT);
		input_report_abs(port->idev, ABS_Y,
				 0xFF - ((raw[0] & GCN_PAD_LSTICK_Y) >>
					 GCN_PAD_LSTICK_Y_SHIFT));

		/* b pad */
		if (raw[0] & GCN_PAD_RIGHT)
			input_report_abs(port->idev, ABS_HAT0X, 1);
		else if (raw[0] & GCN_PAD_LEFT)
			input_report_abs(port->idev, ABS_HAT0X, -1);
		else
			input_report_abs(port->idev, ABS_HAT0X, 0);

		if (raw[0] & GCN_PAD_DOWN)
			input_report_abs(port->idev, ABS_HAT0Y, 1);
		else if (raw[0] & GCN_PAD_UP)
			input_report_abs(port->idev, ABS_HAT0Y, -1);
		else
			input_report_abs(port->idev, ABS_HAT0Y, 0);

		/* c stick */
		input_report_abs(port->idev, ABS_RX,
				 (raw[1] & GCN_PAD_CSTICK_X) >>
				 GCN_PAD_CSTICK_X_SHIFT);
		input_report_abs(port->idev, ABS_RY,
				 (raw[1] & GCN_PAD_CSTICK_Y) >>
				 GCN_PAD_CSTICK_Y_SHIFT);

		/* triggers */
		input_report_abs(port->idev, ABS_BRAKE,
				 (raw[1] & GCN_PAD_LTRIG) >>
				 GCN_PAD_LTRIG_SHIFT);
		input_report_abs(port->idev, ABS_GAS,
				 (raw[1] & GCN_PAD_RTRIG) >>
				 GCN_PAD_RTRIG_SHIFT);

		break;

	case CTL_KEYBOARD:
		/*
		raw nibbles:
		  [4]<C>[0][0][0][0][0][0] <1H><1L><2H><2L><3H><3L><X><C>
		where:
		  [n] = fixed to n
		  <nH> <nL> = high / low nibble of n-th key pressed
			(0 if not pressed)
		  <X> = <1H> xor <2H> xor <3H>
		  <C> = counter: 0, 0, 1, 1, 2, 2, ..., F, F, 0, 0, ...
		*/
		key[0] = (raw[1] >> 24) & 0xFF;
		key[1] = (raw[1] >> 16) & 0xFF;
		key[2] = (raw[1] >>  8) & 0xFF;

		/* check if anything was released */
		for (i = 0; i < 3; ++i) {
			oldkey = port->keyboard.old[i];
			if (oldkey != key[0] &&
			    oldkey != key[1] && oldkey != key[2])
				input_report_key(port->idev,
						 gamecube_keymap[oldkey], 0);
		}

		/* report keys */
		for (i = 0; i < 3; ++i) {
			if (key[i])
				input_report_key(port->idev,
						 gamecube_keymap[key[i]], 1);
			port->keyboard.old[i] = key[i];
		}
		break;

	default:
		break;
	}

	input_sync(port->idev);

out:
	if (port->registered && port->opened && port->idev &&
	    !(port->drvdata->flags & SI_QUIESCE))
		mod_timer(&port->timer, jiffies + SI_REFRESH_TIME);
}

/*
 * Input driver hooks.
 *
 */

static int si_open(struct input_dev *idev)
{
	struct si_port *port = input_get_drvdata(idev);

	port->opened = true;
	mod_timer(&port->timer, jiffies + SI_REFRESH_TIME);

	return 0;
}

static void si_close(struct input_dev *idev)
{
	struct si_port *port = input_get_drvdata(idev);

	port->opened = false;
	timer_delete_sync(&port->timer);
}

static int si_event(struct input_dev *idev, unsigned int type,
		    unsigned int code, int value)
{
	struct si_port *port = input_get_drvdata(idev);
	unsigned int index = port->index;
	void __iomem *io_base = port->drvdata->io_base;

	if (type == EV_FF) {
		if (code == FF_RUMBLE)
			si_set_rumbling(io_base, index, value);
	}

	return value;
}

static int si_setup_pad(struct input_dev *idev)
{
	int retval;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_ABS, idev->evbit);

	set_bit(BTN_A, idev->keybit);
	set_bit(BTN_B, idev->keybit);
	set_bit(BTN_X, idev->keybit);
	set_bit(BTN_Y, idev->keybit);
	set_bit(BTN_Z, idev->keybit);
	set_bit(BTN_TL, idev->keybit);
	set_bit(BTN_TR, idev->keybit);
	set_bit(BTN_START, idev->keybit);
	set_bit(BTN_0, idev->keybit);
	set_bit(BTN_1, idev->keybit);
	set_bit(BTN_2, idev->keybit);
	set_bit(BTN_3, idev->keybit);

	/* a stick */
	set_bit(ABS_X, idev->absbit);

	input_alloc_absinfo(idev);
	idev->absinfo[ABS_X].minimum = 0;
	idev->absinfo[ABS_X].maximum = 255;
	idev->absinfo[ABS_X].fuzz = 8;
	idev->absinfo[ABS_X].flat = 8;

	set_bit(ABS_Y, idev->absbit);
	idev->absinfo[ABS_Y].minimum = 0;
	idev->absinfo[ABS_Y].maximum = 255;
	idev->absinfo[ABS_Y].fuzz = 8;
	idev->absinfo[ABS_Y].flat = 8;

	/* b pad */
	input_set_abs_params(idev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(idev, ABS_HAT0Y, -1, 1, 0, 0);

	/* c stick */
	set_bit(ABS_RX, idev->absbit);
	idev->absinfo[ABS_RX].minimum = 0;
	idev->absinfo[ABS_RX].maximum = 255;
	idev->absinfo[ABS_RX].fuzz = 8;
	idev->absinfo[ABS_RX].flat = 8;

	set_bit(ABS_RY, idev->absbit);
	idev->absinfo[ABS_RY].minimum = 0;
	idev->absinfo[ABS_RY].maximum = 255;
	idev->absinfo[ABS_RY].fuzz = 8;
	idev->absinfo[ABS_RY].flat = 8;

	/* triggers */
	set_bit(ABS_GAS, idev->absbit);
	idev->absinfo[ABS_GAS].minimum = -255;
	idev->absinfo[ABS_GAS].maximum = 255;
	idev->absinfo[ABS_GAS].fuzz = 16;
	idev->absinfo[ABS_GAS].flat = 16;

	set_bit(ABS_BRAKE, idev->absbit);
	idev->absinfo[ABS_BRAKE].minimum = -255;
	idev->absinfo[ABS_BRAKE].maximum = 255;
	idev->absinfo[ABS_BRAKE].fuzz = 16;
	idev->absinfo[ABS_BRAKE].flat = 16;

	/* rumbling */
	set_bit(EV_FF, idev->evbit);
	set_bit(FF_RUMBLE, idev->ffbit);
	retval = input_ff_create(idev, 1);
	if (retval)
		return retval;
	idev->event = si_event;
	return 0;
}

static void si_setup_keyboard(struct input_dev *idev)
{
	int i;

	set_bit(EV_KEY, idev->evbit);
	set_bit(EV_REP, idev->evbit);

	for (i = 0; i < 255; ++i)
		set_bit(gamecube_keymap[i], idev->keybit);
}

static const char *si_type_name(enum si_control_type type)
{
	switch (type) {
	case CTL_NONE:
		return "not present";
	case CTL_PAD:
		return "GameCube controller";
	case CTL_KEYBOARD:
		return "GameCube ASCII keyboard";
	case CTL_GBA:
		return "Game Boy Advance";
	case CTL_N64_PAD:
		return "N64 controller";
	case CTL_N64_KEYBOARD:
		return "N64 keyboard";
	case CTL_N64_MOUSE:
		return "N64 mouse";
	case CTL_N64_MIC:
		return "N64 microphone";
	default:
		return "unknown";
	}
}

static bool si_type_supported(enum si_control_type type)
{
	return type == CTL_PAD || type == CTL_KEYBOARD;
}

static enum si_control_type si_decode_device_id(u32 id)
{
	switch (id) {
	case SI_DEVICE_ID_STANDARD:
	case SI_DEVICE_ID_WAVEBIRD_1:
	case SI_DEVICE_ID_WAVEBIRD_2:
	case SI_DEVICE_ID_WAVEBIRD_3:
	case SI_DEVICE_ID_WAVEBIRD_4:
		return CTL_PAD;
	case SI_DEVICE_ID_GCN_KBD:
		return CTL_KEYBOARD;
	case SI_DEVICE_ID_GBA:
	case SI_DEVICE_ID_GBA_2:
		return CTL_GBA;
	case SI_DEVICE_ID_N64_CONTROLLER:
		return CTL_N64_PAD;
	case SI_DEVICE_ID_N64_KBD:
		return CTL_N64_KEYBOARD;
	case SI_DEVICE_ID_N64_MOUSE:
		return CTL_N64_MOUSE;
	case SI_DEVICE_ID_N64_MIC:
		return CTL_N64_MIC;
	case 0:
	case 0xffff:
		return CTL_NONE;
	default:
		return CTL_UNKNOWN;
	}
}

static int si_probe_port_id(struct si_port *port, u32 *id)
{
	unsigned int tries = SI_PROBE_TRIES;
	u32 resp = 0;
	int error;

	while (tries--) {
		error = si_transfer(port->drvdata, port->index,
				    SI_MKOUTBUF(JOYBUS_CMD_STATUS, 0, 0),
				    1, 3, &resp);
		if (!error) {
			*id = resp >> 16;
			return 0;
		}

		if (error == -ETIMEDOUT)
			return error;
	}

	*id = 0xffff;
	return -ENODEV;
}

static int si_register_port(struct si_port *port)
{
	struct input_dev *idev;
	int retval = 0;

	idev = input_allocate_device();
	if (!idev) {
		dev_err(port->drvdata->dev, "failed to allocate input_dev\n");
		return -ENOMEM;
	}

	idev->open = si_open;
	idev->close = si_close;
	idev->name = port->name;

	switch (port->type) {
	case CTL_PAD:
		retval = si_setup_pad(idev);
		break;
	case CTL_KEYBOARD:
		si_setup_keyboard(idev);
		break;
	default:
		break;
	}

	if (retval) {
		input_free_device(idev);
		return retval;
	}

	input_set_drvdata(idev, port);
	port->idev = idev;
	retval = input_register_device(idev);
	if (retval) {
		dev_err(port->drvdata->dev,
			"input device registration failed (%d) for port %u\n",
			retval, port->index + 1);
		input_free_device(idev);
		port->idev = NULL;
		return retval;
	}

	port->registered = true;
	return 0;
}

static void si_unregister_port(struct si_port *port)
{
	struct input_dev *idev = port->idev;

	if (!idev)
		return;

	port->registered = false;
	port->opened = false;
	timer_delete_sync(&port->timer);
	port->idev = NULL;
	input_unregister_device(idev);
	memset(&port->keyboard, 0, sizeof(port->keyboard));
}

static void si_update_port(struct si_port *port)
{
	enum si_control_type old_type = port->type;
	enum si_control_type new_type;
	u32 id;
	int error;

	error = si_probe_port_id(port, &id);
	if (error == -ETIMEDOUT)
		return;

	if (error == -ENODEV)
		new_type = CTL_NONE;
	else
		new_type = si_decode_device_id(id);

#ifdef HACK_FORCE_KEYBOARD_PORT
	if (new_type == CTL_UNKNOWN && port->index + 1 == si_force_keyboard_port) {
		dev_warn(port->drvdata->dev, "port %u forced to keyboard mode\n",
			 port->index + 1);
		id = SI_DEVICE_ID_GCN_KBD;
		new_type = CTL_KEYBOARD;
	}
#endif /* HACK_FORCE_KEYBOARD_PORT */

	if (old_type == new_type && port->id == id) {
		if (!port->registered && si_type_supported(new_type))
			si_register_port(port);
		return;
	}

	if (port->registered)
		si_unregister_port(port);

	port->id = id;
	port->type = new_type;
	snprintf(port->name, sizeof(port->name), "%s", si_type_name(new_type));

	if (new_type == CTL_UNKNOWN)
		dev_info(port->drvdata->dev, "port %u: unknown device 0x%04x\n",
			 port->index + 1, id);
	else if (new_type == CTL_NONE)
		dev_info(port->drvdata->dev, "port %u: disconnected\n",
			 port->index + 1);
	else
		dev_info(port->drvdata->dev, "port %u: %s (0x%04x)%s\n",
			 port->index + 1, si_type_name(new_type), id,
			 si_type_supported(new_type) ? "" : " unsupported");

	if (si_type_supported(new_type))
		si_register_port(port);
}

static void si_scan_ports(struct si_drvdata *drvdata)
{
	int i;

	out_be32(drvdata->io_base + SIPOLL, 0);
	if (si_flush_poll_buffers(drvdata))
		return;
	si_drain_all_inbufs(drvdata->io_base);

	for (i = 0; i < SI_MAX_PORTS; ++i)
		si_update_port(&drvdata->ports[i]);

	si_setup_polling(drvdata);
}

static void si_hotplug_work(struct work_struct *work)
{
	struct si_drvdata *drvdata =
		container_of(to_delayed_work(work), struct si_drvdata,
			     hotplug_work);

	if (drvdata->flags & SI_QUIESCE)
		return;

	si_scan_ports(drvdata);

	if (!(drvdata->flags & SI_QUIESCE))
		schedule_delayed_work(&drvdata->hotplug_work, SI_HOTPLUG_TIME);
}

/*
 * Setup routines.
 *
 */

static int si_of_probe(struct platform_device *odev)
{
	void __iomem *io_base;
	struct device *dev;
	struct si_drvdata *drvdata;
	struct si_port *port;
	int error;
	int index;

	dev = &odev->dev;

	io_base = of_iomap(dev->of_node, 0);
	if (!io_base) {
		dev_err(dev, "no io memory range found\n");
		return -ENODEV;
	}

	drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata) {
		dev_err(dev, "failed to allocate si_drvdata\n");
		dev_set_drvdata(dev, NULL);
		kfree(drvdata);
		return -ENOMEM;
	}
	dev_set_drvdata(dev, drvdata);
	drvdata->dev = dev;
	drvdata->io_base = io_base;
	drvdata->ignore_collisions =
		of_device_is_compatible(dev->of_node, "nintendo,latte-si");
	init_completion(&drvdata->transfer_done);

	INIT_DELAYED_WORK(&drvdata->hotplug_work, si_hotplug_work);
	si_reset_all(drvdata->io_base);

	drvdata->irq = platform_get_irq(odev, 0);
	if (drvdata->irq < 0) {
		error = drvdata->irq;
		goto err_free_drvdata;
	}

	error = request_irq(drvdata->irq, si_irq, 0, dev_name(dev), drvdata);
	if (error) {
		dev_err(dev, "failed to request IRQ %d: %d\n",
			drvdata->irq, error);
		goto err_free_drvdata;
	}

	for (index = 0; index < SI_MAX_PORTS; ++index) {
		port = &drvdata->ports[index];

		memset(port, 0, sizeof(*port));
		port->index = index;
		port->drvdata = drvdata;
		port->type = CTL_NONE;
		timer_setup(&port->timer, si_timer, 0);
		snprintf(port->name, sizeof(port->name), "%s",
			 si_type_name(CTL_NONE));
	}

	si_scan_ports(drvdata);
	schedule_delayed_work(&drvdata->hotplug_work, SI_HOTPLUG_TIME);

	return 0;

err_free_drvdata:
	dev_set_drvdata(dev, NULL);
	kfree(drvdata);
	iounmap(io_base);
	return error;
}

static void si_of_remove(struct platform_device *odev)
{
	struct si_drvdata *drvdata = dev_get_drvdata(&odev->dev);
	struct si_port *port;
	int index;

	if (drvdata) {
		drvdata->flags |= SI_QUIESCE;
		cancel_delayed_work_sync(&drvdata->hotplug_work);

		for (index = 0; index < SI_MAX_PORTS; ++index) {
			port = &drvdata->ports[index];
			si_unregister_port(port);
		}

		if (drvdata->io_base) {
			si_reset_all(drvdata->io_base);
			free_irq(drvdata->irq, drvdata);
			iounmap(drvdata->io_base);
			drvdata->io_base = NULL;
		}

		dev_set_drvdata(&odev->dev, NULL);
		kfree(drvdata);
	}
}

static void si_of_shutdown(struct platform_device *odev)
{
	struct si_drvdata *drvdata = dev_get_drvdata(&odev->dev);
	int i;

	if (drvdata) {
		drvdata->flags |= SI_QUIESCE;
		cancel_delayed_work_sync(&drvdata->hotplug_work);
		for (i = 0; i < SI_MAX_PORTS; ++i)
			timer_delete_sync(&drvdata->ports[i].timer);
		si_reset_all(drvdata->io_base);
	}
}

static const struct of_device_id si_of_match[] = {
	{ .compatible = "nintendo,flipper-si" },
	{ .compatible = "nintendo,hollywood-si" },
	{ .compatible = "nintendo,latte-si" },
	{ },
};


MODULE_DEVICE_TABLE(of, si_of_match);

static struct platform_driver si_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = si_of_match,
	},
	.probe = si_of_probe,
	.remove = si_of_remove,
	.shutdown = si_of_shutdown,
};


/*
 * Module interface hooks.
 *
 */

static int __init si_init_module(void)
{
	pr_info("%s - version %s\n", DRV_DESCRIPTION,
		   si_driver_version);

	return platform_driver_register(&si_of_driver);
}

static void __exit si_exit_module(void)
{
	platform_driver_unregister(&si_of_driver);
}

module_init(si_init_module);
module_exit(si_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
