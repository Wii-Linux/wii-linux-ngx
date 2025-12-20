/*
 * HID driver for Nintendo Wii U GamePad, connected via console-internal DRH
 *
 * Copyright (C) 2021 Emmanuel Gil Peyrot <linkmauve@linkmauve.fr>
 * Copyright (C) 2019 Ash Logan <ash@heyquark.com>
 * Copyright (C) 2013 Mema Hacking
 *
 * Based on the excellent work at http://libdrc.org/docs/re/sc-input.html and
 * https://bitbucket.org/memahaxx/libdrc/src/master/src/input-receiver.cpp .
 * libdrc code is licensed under BSD 2-Clause.
 * Driver based on hid-udraw-ps3.c.
 *
 * TODO: maybe try and get their blessing before upstreaming.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include "hid-ids.h"

MODULE_AUTHOR("Ash Logan <ash@heyquark.com>");
MODULE_DESCRIPTION("Wii U GamePad driver [DRH]");
MODULE_LICENSE("GPL");

/*
 * The device is setup with multiple input devices:
 * - the touch area which works as a touchpad
 * - a joypad with a d-pad, and 7 buttons
 * - an accelerometer device
 */

#define DEVICE_NAME "Nintendo Wii U GamePad [DRH]"
/* resolution in pixels */
#define RES_X 854
#define RES_Y 480
/* display/touch size in mm */
#define WIDTH  138
#define HEIGHT 79
#define STICK_MIN 900
#define STICK_MAX 3200
#define VOLUME_MIN 0
#define VOLUME_MAX 255
#define NUM_STICK_AXES 4
#define NUM_TOUCH_POINTS 10
#define MAX_TOUCH_RES (1 << 12)
#define BATTERY_MIN 142
#define BATTERY_MAX 178
#define BATTERY_CAPACITY(val) ((val - BATTERY_MIN) * 100 / (BATTERY_MAX - BATTERY_MIN))
#define ACCEL_MIN -(1 << 15)
#define ACCEL_MAX ((1 << 15) - 1)
#define GYRO_MIN -(1 << 23)
#define GYRO_MAX ((1 << 23) - 1)

//#define PRESSURE_OFFSET 113
//#define MAX_PRESSURE (255 - PRESSURE_OFFSET)

struct drc {
	spinlock_t lock;

	struct input_dev *joy_input_dev;
	struct input_dev *touch_input_dev;
	struct input_dev *accel_input_dev;
	struct hid_device *hdev;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;

	u8 battery_energy;
	int battery_status;
};

enum ButtonMask {
	kBtnSync = 0x1,
	kBtnHome = 0x2,
	kBtnMinus = 0x4,
	kBtnPlus = 0x8,
	kBtnR = 0x10,
	kBtnL = 0x20,
	kBtnZR = 0x40,
	kBtnZL = 0x80,
	kBtnDown = 0x100,
	kBtnUp = 0x200,
	kBtnRight = 0x400,
	kBtnLeft = 0x800,
	kBtnY = 0x1000,
	kBtnX = 0x2000,
	kBtnB = 0x4000,
	kBtnA = 0x8000,

	kBtnTV = 0x200000,
	kBtnR3 = 0x400000,
	kBtnL3 = 0x800000,
} buttons;

static int drc_raw_event(struct hid_device *hdev, struct hid_report *report,
	 u8 *data, int len)
{
	struct drc *drc = hid_get_drvdata(hdev);
	int x, y, z, pressure, i, base;
	u32 buttons;
	unsigned long flags;

	if (len != 128)
		return 0;

	buttons = (data[80] << 16) | (data[2] << 8) | data[3];
	/* joypad */
	input_report_key(drc->joy_input_dev, BTN_DPAD_RIGHT, !!(buttons & kBtnRight));
	input_report_key(drc->joy_input_dev, BTN_DPAD_DOWN, !!(buttons & kBtnDown));
	input_report_key(drc->joy_input_dev, BTN_DPAD_LEFT, !!(buttons & kBtnLeft));
	input_report_key(drc->joy_input_dev, BTN_DPAD_UP, !!(buttons & kBtnUp));

	input_report_key(drc->joy_input_dev, BTN_EAST, !!(buttons & kBtnA));
	input_report_key(drc->joy_input_dev, BTN_SOUTH, !!(buttons & kBtnB));
	input_report_key(drc->joy_input_dev, BTN_NORTH, !!(buttons & kBtnX));
	input_report_key(drc->joy_input_dev, BTN_WEST, !!(buttons & kBtnY));

	input_report_key(drc->joy_input_dev, BTN_TL, !!(buttons & kBtnL));
	input_report_key(drc->joy_input_dev, BTN_TL2, !!(buttons & kBtnZL));
	input_report_key(drc->joy_input_dev, BTN_TR, !!(buttons & kBtnR));
	input_report_key(drc->joy_input_dev, BTN_TR2, !!(buttons & kBtnZR));

	input_report_key(drc->joy_input_dev, BTN_THUMBL, !!(buttons & kBtnL3));
	input_report_key(drc->joy_input_dev, BTN_THUMBR, !!(buttons & kBtnR3));

	input_report_key(drc->joy_input_dev, BTN_SELECT, !!(buttons & kBtnMinus));
	input_report_key(drc->joy_input_dev, BTN_START, !!(buttons & kBtnPlus));
	input_report_key(drc->joy_input_dev, BTN_MODE, !!(buttons & kBtnHome));

	input_report_abs(drc->joy_input_dev, ABS_VOLUME, data[14]);

	for (i = 0; i < NUM_STICK_AXES; i++) {
		s16 val = (data[7 + 2*i] << 8) | data[6 + 2*i];
		/* clamp */
		if (val < STICK_MIN) val = STICK_MIN;
		if (val > STICK_MAX) val = STICK_MAX;

		switch (i) {
			case 0: input_report_abs(drc->joy_input_dev, ABS_X, val); break;
			case 1: input_report_abs(drc->joy_input_dev, ABS_Y, val); break;
			case 2: input_report_abs(drc->joy_input_dev, ABS_RX, val); break;
			case 3: input_report_abs(drc->joy_input_dev, ABS_RY, val); break;
			default: break;
		}
	}

	input_sync(drc->joy_input_dev);

	/* Average touch points for ACCURACY */
	x = y = 0;
	for (i = 0; i < NUM_TOUCH_POINTS; i++) {
		base = 36 + 4 * i;

		x += ((data[base + 1] & 0xF) << 8) | data[base];
		y += ((data[base + 3] & 0xF) << 8) | data[base + 2];
	}
	x /= NUM_TOUCH_POINTS;
	y /= NUM_TOUCH_POINTS;

	/*This doesn't work at the moment*/
	pressure = 0;
	pressure |= ((data[37] >> 4) & 7) << 0;
	pressure |= ((data[39] >> 4) & 7) << 3;
	pressure |= ((data[41] >> 4) & 7) << 6;
	pressure |= ((data[43] >> 4) & 7) << 9;

	if (pressure != 0) {
		input_report_key(drc->touch_input_dev, BTN_TOUCH, 1);
		input_report_key(drc->touch_input_dev, BTN_TOOL_FINGER, 1);

		input_report_abs(drc->touch_input_dev, ABS_X, x);
		input_report_abs(drc->touch_input_dev, ABS_Y, MAX_TOUCH_RES - y);
	} else {
		input_report_key(drc->touch_input_dev, BTN_TOUCH, 0);
		input_report_key(drc->touch_input_dev, BTN_TOOL_FINGER, 0);
	}
	input_sync(drc->touch_input_dev);

	/* accelerometer */
	x = (data[16] << 8) | data[15];
	y = (data[18] << 8) | data[17];
	z = (data[20] << 8) | data[19];
	input_report_abs(drc->accel_input_dev, ABS_X, (int16_t)x);
	input_report_abs(drc->accel_input_dev, ABS_Y, (int16_t)y);
	input_report_abs(drc->accel_input_dev, ABS_Z, (int16_t)z);

	/* gyroscope */
	x = (data[23] << 24) | (data[22] << 16) | (data[21] << 8);
	y = (data[26] << 24) | (data[25] << 16) | (data[24] << 8);
	z = (data[29] << 24) | (data[28] << 16) | (data[27] << 8);
	input_report_abs(drc->accel_input_dev, ABS_RX, x >> 8);
	input_report_abs(drc->accel_input_dev, ABS_RY, y >> 8);
	input_report_abs(drc->accel_input_dev, ABS_RZ, z >> 8);
	input_sync(drc->accel_input_dev);

	/* magnetometer? */
	x = (data[31] << 8) | data[30];
	y = (data[33] << 8) | data[32];
	z = (data[35] << 8) | data[34];
	input_report_abs(drc->accel_input_dev, ABS_THROTTLE, (int16_t)x);
	input_report_abs(drc->accel_input_dev, ABS_RUDDER, (int16_t)y);
	input_report_abs(drc->accel_input_dev, ABS_WHEEL, (int16_t)z);

	/* battery */
	spin_lock_irqsave(&drc->lock, flags);
	drc->battery_energy = data[5];
	if (drc->battery_energy == BATTERY_MAX)
		drc->battery_status = POWER_SUPPLY_STATUS_FULL;
	else if ((data[4] & 0x40) != 0)
		drc->battery_status = POWER_SUPPLY_STATUS_CHARGING;
	else
		drc->battery_status = POWER_SUPPLY_STATUS_DISCHARGING;
	spin_unlock_irqrestore(&drc->lock, flags);

	/* let hidraw and hiddev handle the report */
	return 0;
}

static int drc_open(struct input_dev *dev)
{
	struct drc *drc = input_get_drvdata(dev);

	return hid_hw_open(drc->hdev);
}

static void drc_close(struct input_dev *dev)
{
	struct drc *drc = input_get_drvdata(dev);

	hid_hw_close(drc->hdev);
}

static struct input_dev *allocate_and_setup(struct hid_device *hdev,
		const char *name)
{
	struct input_dev *input_dev;

	input_dev = devm_input_allocate_device(&hdev->dev);
	if (!input_dev)
		return NULL;

	input_dev->name = name;
	input_dev->phys = hdev->phys;
	input_dev->dev.parent = &hdev->dev;
	input_dev->open = drc_open;
	input_dev->close = drc_close;
	input_dev->uniq = hdev->uniq;
	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor  = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = hdev->version;
	input_set_drvdata(input_dev, hid_get_drvdata(hdev));

	return input_dev;
}

static bool drc_setup_touch(struct drc *drc,
		struct hid_device *hdev)
{
	struct input_dev *input_dev;

	input_dev = allocate_and_setup(hdev, DEVICE_NAME " Touch");
	if (!input_dev)
		return false;

	input_dev->evbit[0] = BIT(EV_ABS) | BIT(EV_KEY);

	input_set_abs_params(input_dev, ABS_X, 100, MAX_TOUCH_RES - 100, 20, 0);
	input_abs_set_res(input_dev, ABS_X, RES_X / WIDTH);
	input_set_abs_params(input_dev, ABS_Y, 200, MAX_TOUCH_RES - 200, 20, 0);
	input_abs_set_res(input_dev, ABS_Y, RES_Y / HEIGHT);

	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);

	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	drc->touch_input_dev = input_dev;

	return true;
}

static bool drc_setup_accel(struct drc *drc,
		struct hid_device *hdev)
{
	struct input_dev *input_dev;

	input_dev = allocate_and_setup(hdev, DEVICE_NAME " Accelerometer");
	if (!input_dev)
		return false;

	input_dev->evbit[0] = BIT(EV_ABS);

	/* 1G accel is reported as about -7600, so clamp to 2G */
	input_set_abs_params(input_dev, ABS_X, ACCEL_MIN, ACCEL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, ACCEL_MIN, ACCEL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_Z, ACCEL_MIN, ACCEL_MAX, 0, 0);

	/* gyroscope */
	input_set_abs_params(input_dev, ABS_RX, GYRO_MIN, GYRO_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_RY, GYRO_MIN, GYRO_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_RZ, GYRO_MIN, GYRO_MAX, 0, 0);

	/* magnetometer? */
	/* XXX: figure out which ABS_* would make more sense to expose. */
	input_set_abs_params(input_dev, ABS_THROTTLE, ACCEL_MIN, ACCEL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_RUDDER, ACCEL_MIN, ACCEL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_WHEEL, ACCEL_MIN, ACCEL_MAX, 0, 0);

	set_bit(INPUT_PROP_ACCELEROMETER, input_dev->propbit);

	drc->accel_input_dev = input_dev;

	return true;
}

static enum power_supply_property drc_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_FULL,
};

static int drc_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct drc *drc = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;
	u8 battery_energy;
	int battery_status;

	spin_lock_irqsave(&drc->lock, flags);
	battery_energy = drc->battery_energy;
	battery_status = drc->battery_status;
	spin_unlock_irqrestore(&drc->lock, flags);

	switch (psp) {
		case POWER_SUPPLY_PROP_PRESENT:
			val->intval = 1;
			break;
		case POWER_SUPPLY_PROP_SCOPE:
			val->intval = POWER_SUPPLY_SCOPE_DEVICE;
			break;
		case POWER_SUPPLY_PROP_CAPACITY:
			val->intval = BATTERY_CAPACITY(battery_energy);
			break;
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = battery_status;
			break;
		case POWER_SUPPLY_PROP_ENERGY_NOW:
			val->intval = battery_energy;
			break;
		case POWER_SUPPLY_PROP_ENERGY_EMPTY:
			val->intval = BATTERY_MIN;
			break;
		case POWER_SUPPLY_PROP_ENERGY_FULL:
			val->intval = BATTERY_MAX;
			break;
		default:
			ret = -EINVAL;
			break;
	}
	return ret;
}

static bool drc_setup_joypad(struct drc *drc,
		struct hid_device *hdev)
{
	struct input_dev *input_dev;
	struct power_supply_config psy_cfg = { .drv_data = drc, };
	int ret;
	static uint8_t drc_num = 0;

	input_dev = allocate_and_setup(hdev, DEVICE_NAME " Joypad");
	if (!input_dev)
		return false;

	input_dev->evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);

	set_bit(BTN_DPAD_RIGHT, input_dev->keybit);
	set_bit(BTN_DPAD_DOWN, input_dev->keybit);
	set_bit(BTN_DPAD_LEFT, input_dev->keybit);
	set_bit(BTN_DPAD_UP, input_dev->keybit);
	set_bit(BTN_EAST, input_dev->keybit);
	set_bit(BTN_SOUTH, input_dev->keybit);
	set_bit(BTN_NORTH, input_dev->keybit);
	set_bit(BTN_WEST, input_dev->keybit);
	set_bit(BTN_TL, input_dev->keybit);
	set_bit(BTN_TL2, input_dev->keybit);
	set_bit(BTN_TR, input_dev->keybit);
	set_bit(BTN_TR2, input_dev->keybit);
	set_bit(BTN_THUMBL, input_dev->keybit);
	set_bit(BTN_THUMBR, input_dev->keybit);
	set_bit(BTN_SELECT, input_dev->keybit);
	set_bit(BTN_START, input_dev->keybit);
	set_bit(BTN_MODE, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_X, STICK_MIN, STICK_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, STICK_MIN, STICK_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_RX, STICK_MIN, STICK_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_RY, STICK_MIN, STICK_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_VOLUME, VOLUME_MIN, VOLUME_MAX, 0, 0);

	drc->joy_input_dev = input_dev;

	drc->battery_desc.properties = drc_battery_props;
	drc->battery_desc.num_properties = ARRAY_SIZE(drc_battery_props);
	drc->battery_desc.get_property = drc_battery_get_property;
	drc->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drc->battery_desc.use_for_apm = 0;

	/*
	 * TODO: Might be better to use the interface number as the drc_num,
	 * but I don’t know how to fetch it from the kernel…  In userland it is
	 * /sys/devices/platform/latte/d140000.usb/usb3/3-1/3-1:1.?/bInterfaceNumber
	 */
	drc->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "wiiu-drc-%i-battery", drc_num++);
	if (!drc->battery_desc.name)
		return -ENOMEM;

	drc->battery = devm_power_supply_register(&hdev->dev, &drc->battery_desc, &psy_cfg);
	if (IS_ERR(drc->battery)) {
		ret = PTR_ERR(drc->battery);
		hid_err(hdev, "Unable to register battery device\n");
		return ret;
	}

	power_supply_powers(drc->battery, &hdev->dev);

	return true;
}

static int drc_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct drc *drc;
	int ret;

	drc = devm_kzalloc(&hdev->dev, sizeof(struct drc), GFP_KERNEL);
	if (!drc)
		return -ENOMEM;

	drc->hdev = hdev;

	hid_set_drvdata(hdev, drc);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		return ret;
	}

	if (!drc_setup_joypad(drc, hdev) ||
	    !drc_setup_touch(drc, hdev) ||
	    !drc_setup_accel(drc, hdev)) {
		hid_err(hdev, "could not allocate interfaces\n");
		return -ENOMEM;
	}

	ret = input_register_device(drc->joy_input_dev) ||
		input_register_device(drc->touch_input_dev) ||
		input_register_device(drc->accel_input_dev);
	if (ret) {
		hid_err(hdev, "failed to register interfaces\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW | HID_CONNECT_DRIVER);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	return 0;
}

static const struct hid_device_id drc_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_WIIUDRH) },
	{ }
};
MODULE_DEVICE_TABLE(hid, drc_devices);

static struct hid_driver drc_driver = {
	.name = "hid-wiiu-drc",
	.id_table = drc_devices,
	.raw_event = drc_raw_event,
	.probe = drc_probe,
};
module_hid_driver(drc_driver);
