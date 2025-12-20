// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii U board-specific support
 *
 * Copyright (C) 2023 The linux-wiiu Team
 */
#define DRV_MODULE_NAME "wiiu"
#define pr_fmt(fmt) DRV_MODULE_NAME ": " fmt

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm.h>

#include <asm/machdep.h>
#include <asm/time.h>
#include <asm/udbg.h>

#include "espresso-pic.h"
#include "latte-pic.h"
#include "latte-ipc.h"

#define WIIU_LOADER_CMD_POWEROFF 0xCAFE0001
#define WIIU_LOADER_CMD_REBOOT   0xCAFE0002

static void __noreturn wiiu_halt(void)
{
	for (;;)
		cpu_relax();
}

static void __noreturn wiiu_power_off(void)
{
	latteipc_starbuck_msg(WIIU_LOADER_CMD_POWEROFF);
	wiiu_halt();
}

static void __noreturn wiiu_restart(char *cmd)
{
	latteipc_starbuck_msg(WIIU_LOADER_CMD_REBOOT);
	wiiu_halt();
}

static int __init wiiu_probe(void)
{
	if (!of_machine_is_compatible("nintendo,wiiu"))
		return 0;

	pm_power_off = wiiu_power_off;

	return 1;
}

static void __init wiiu_init_irq(void)
{
	espresso_pic_init();
	latte_pic_init();
}

static int __init wiiu_device_probe(void)
{
	if (!machine_is(wiiu))
		return 0;

	of_platform_default_populate(NULL, NULL, NULL);
	return 0;
}
device_initcall(wiiu_device_probe);

define_machine(wiiu) {
	.name = "wiiu",
	.probe = wiiu_probe,
	.halt = wiiu_halt,
	.progress = udbg_progress,
	.calibrate_decr = generic_calibrate_decr,
	.init_IRQ = wiiu_init_irq,
	.get_irq = espresso_pic_get_irq,
	.restart = wiiu_restart,
};
