// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii U udbg support (to de_Fuse modchip, via debug GPIO)
 *
 * Copyright (C) 2023 The linux-wiiu Team
 *
 * Based on arch/powerpc/platforms/embedded6xx/udbgecko_udbg.c
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008-2009 Albert Herranz
 */

#include <mm/mmu_decl.h>
#include <linux/io.h>
#include <asm/udbg.h>
#include <asm/fixmap.h>

#define GPIO_DEBUG_SHIFT 16
#define GPIO_DEBUG_SERIAL_MASK 0xBF0000 // bit1 is input

#define LT_MMIO_BASE ((phys_addr_t)0x0d800000)
#define LT_GPIOE_OUT (0x0C0)
#define LT_GPIOE_DIR (0x0C4)

static void __iomem *latte_gpio_base;

static void wait(void)
{
	int t = 10000;
	while (t--)
		cpu_relax();
}

static void defuse_udbg_send_gpio(u8 val)
{
	void __iomem *gpioe_out_reg =
		(void __iomem *)(latte_gpio_base + LT_GPIOE_OUT);

	unsigned reg = in_be32(gpioe_out_reg);
	reg &= ~GPIO_DEBUG_SERIAL_MASK;
	reg |= (val << GPIO_DEBUG_SHIFT);
	out_be32(gpioe_out_reg, reg);
}

static void defuse_udbg_force_terminate(void)
{
	defuse_udbg_send_gpio(0x0F);
	wait();

	defuse_udbg_send_gpio(0x8F);
	wait();

	defuse_udbg_send_gpio(0x0F);
	defuse_udbg_send_gpio(0x00);
	wait();
}

void defuse_udbg_putc(char c)
{
	for (int j = 7; j >= 0; j--) {
		u8 bit = (c & (1 << j)) ? 1 : 0;
		defuse_udbg_send_gpio(bit);
		wait();
		defuse_udbg_send_gpio(0x80 | bit);
		wait();
	}

	defuse_udbg_force_terminate();
}

/* non-early init omitted - would need to play nice with the GPIO driver */

#ifdef CONFIG_PPC_EARLY_DEBUG_DEFUSE

void __init udbg_init_defuse(void)
{
	/*
	 * At this point we have a BAT already setup that enables I/O
	 * to the IPC hardware.
	 *
	 * The BAT uses a virtual address range reserved at the fixmap.
	 * This must match the virtual address configured in
	 * head_32.S:setup_latteipc_bat().
	 */
	latte_gpio_base = (void __iomem *)__fix_to_virt(FIX_EARLY_DEBUG_BASE);

	/* TODO check for prescence of de_Fuse chip */
	udbg_putc = defuse_udbg_putc;

	/*
	 * Prepare again the same BAT for MMU_init.
	 * This allows udbg I/O to continue working after the MMU is
	 * turned on for real.
	 * It is safe to continue using the same virtual address as it is
	 * a reserved fixmap area.
	 */
	setbat(1, (unsigned long)latte_gpio_base, LT_MMIO_BASE, 128 * 1024,
	       PAGE_KERNEL_NCG);
}

#endif /* CONFIG_PPC_EARLY_DEBUG_DEFUSE */
