// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii U udbg support (to Starbuck coprocessor, via chipset IPC)
 *
 * Copyright (C) 2022 The linux-wiiu Team
 *
 * Based on arch/powerpc/platforms/embedded6xx/udbgecko_udbg.c
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008-2009 Albert Herranz
 */

#include <asm/udbg.h>

#include "latte-ipc.h"
#include "udbg_latteipc.h"

#define WIIU_LOADER_CMD_PRINT 0x01000000

/*
 * Transmits a character.
 * Sends over IPC to linux-loader for printing.
 */
static void latteipc_udbg_putc(char c)
{
	latteipc_starbuck_msg(WIIU_LOADER_CMD_PRINT | (c << 16));
}

/*
 * Latte IPC udbg support initialization.
 */
void __init latteipc_udbg_init(void)
{
	latteipc_init();

	udbg_putc = latteipc_udbg_putc;
	udbg_printf("latteipc_udbg: ready\n");
}

#ifdef CONFIG_PPC_EARLY_DEBUG_LATTEIPC

void __init udbg_init_latteipc(void)
{
	latteipc_init_early();

	/* Assume a firmware is present, add hooks */
	udbg_putc = latteipc_udbg_putc;
}

#endif /* CONFIG_PPC_EARLY_DEBUG_LATTEIPC */
