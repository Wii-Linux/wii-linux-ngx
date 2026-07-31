// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * arch/powerpc/boot/gamecube.c
 *
 * Nintendo GameCube bootwrapper support
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 */

#include <stddef.h>
#include "stdio.h"
#include "types.h"
#include "io.h"
#include "ops.h"

#include "ugecon.h"

BSS_STACK(8192);

#define MEM1_TOP		(24 * 1024 * 1024)

void platform_init(unsigned long r3, unsigned long r4, unsigned long r5)
{
	static const struct fdt_mapped_range mapped_ram[] = {
		{ 0, MEM1_TOP },
	};
	u32 heapsize = 16*1024*1024 - (u32)_end;

	simple_alloc_init(_end, heapsize, 32, 64);
	fdt_init_from_loader(r3, r4, r5, mapped_ram, ARRAY_SIZE(mapped_ram));

	if (ug_probe())
		console_ops.write = ug_console_write;
}
