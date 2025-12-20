// SPDX-License-Identifier: GPL-2.0
/*
 * Platform support and de_Fuse serial console
 *
 * Nintendo Wii U bootwrapper support
 * Copyright (C) 2023 The linux-wiiu Team
 * Copyright (C) 2023 Max Thomas <mtinc2@gmail.com>
 */

#include "types.h"
#include "io.h"
#include "ops.h"

BSS_STACK(8192);

/* If this struct ever has to be changed in a non-ABI compatible way,
   change the magic.
   Past magics:
    - 0xCAFEFECA: initial version
*/
#define WIIU_LOADER_MAGIC 0xCAFEFECA
struct wiiu_loader_data {
	unsigned int magic;
	char cmdline[256];
	void *initrd;
	unsigned int initrd_sz;
};
const static struct wiiu_loader_data *arm_data = (void *)0x89200000;

// de_Fuse serial driver
#define SERIAL_DELAY (10)

#define LT_REG_BASE ((void *) 0x0D800000)
#define LT_GPIOE_OUT (LT_REG_BASE + 0x0C0)
#define LT_GPIOE_DIR (LT_REG_BASE + 0x0C4)

#define GP_DEBUG_SHIFT 16
#define GP_DEBUG_SERIAL_MASK 0xBF0000 // bit1 is input

static void gpio_debug_serial_send(u8 val)
{
	unsigned reg = in_be32(LT_GPIOE_OUT);
	reg &= ~GP_DEBUG_SERIAL_MASK;
	reg |= (val << GP_DEBUG_SHIFT);
	out_be32(LT_GPIOE_OUT, reg);
}

void serial_force_terminate(void)
{
	gpio_debug_serial_send(0x0F);
	udelay(SERIAL_DELAY);

	gpio_debug_serial_send(0x8F);
	udelay(SERIAL_DELAY);

	gpio_debug_serial_send(0x0F);
	gpio_debug_serial_send(0x00);
	udelay(SERIAL_DELAY);
}

void serial_send(u8 val)
{
	for (int j = 7; j >= 0; j--) {
		u8 bit = (val & (1 << j)) ? 1 : 0;
		gpio_debug_serial_send(bit);
		udelay(SERIAL_DELAY);
		gpio_debug_serial_send(0x80 | bit);
		udelay(SERIAL_DELAY);
	}

	serial_force_terminate();
}

static void wiiu_write_ipc(const char *buf, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		serial_send(buf[i]);
	}
}

static void wiiu_print(const char *str)
{
	int i;
	for (i = 0; str[i]; i++) {
		serial_send(str[i]);
	}
}

static void wiiu_copy_cmdline(char *cmdline, int cmdlineSz,
			      unsigned int timeout)
{
	/*  If the ARM left us a commandline, copy it in */
	if (arm_data->magic == WIIU_LOADER_MAGIC) {
		wiiu_print("found custom commandline!\n");
		strncpy(cmdline, arm_data->cmdline, min(256, cmdlineSz));
	}
}

void platform_init(u32 r3, u32 r4, u32 r5)
{
	u32 heapsize;
	void *initrdstart = (void *)0x02000000; //use all of mem1 when no initrd
	unsigned reg;

	reg = in_be32(LT_GPIOE_DIR);
	reg |= GP_DEBUG_SERIAL_MASK;
	out_be32(LT_GPIOE_DIR, reg);

	console_ops.write = wiiu_write_ipc;
	wiiu_print("Hello from the bootwrapper!\n");

	if (arm_data->magic == WIIU_LOADER_MAGIC) {
		if (arm_data->initrd_sz > 0) {
			loader_info.initrd_addr =
				(unsigned long)arm_data->initrd;
			loader_info.initrd_size =
				(unsigned long)arm_data->initrd_sz;

			if (arm_data->initrd < initrdstart)
				initrdstart = arm_data->initrd;
			wiiu_print("initrd ok\n");
		}
	}

	heapsize = (u32)initrdstart - (u32)_end;
	simple_alloc_init(_end, heapsize, 32, 64);

	wiiu_print("heap ok\n");

	fdt_init(_dtb_start);

	wiiu_print("dtb ok\n");

	console_ops.edit_cmdline = wiiu_copy_cmdline;
}