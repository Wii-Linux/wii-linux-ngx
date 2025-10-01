/*
 * arch/powerpc/platforms/embedded6xx/gcnvi_udbg.c
 *
 * Nintendo GameCube/Wii framebuffer udbg output support.
 * Copyright (C) 2008-2009 The GameCube Linux Team
 * Copyright (C) 2008,2009 Albert Herranz
 *
 * Based on arch/ppc/platforms/gcn-con.c
 *
 * Nintendo GameCube early debug console
 * Copyright (C) 2004-2005 The GameCube Linux Team
 *
 * Based on console.c by tmbinc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#define pr_fmt(fmt)		"gcnvi_udbg: " fmt

#include <linux/io.h>
#include <linux/string.h>
#include <linux/console.h>
#include <linux/font.h>
#include <asm/prom.h>
#include <asm/udbg.h>
#include <mm/mmu_decl.h>

#include "gcnvi_udbg.h"

/*
 * Console settings.
 *
 */
#define SCREEN_WIDTH	640
#define SCREEN_HEIGHT	480
#define SCREEN_OVERSCAN	24

#define FONT_NAME "6x10"
#define FONT_XFACTOR 1
#define FONT_YFACTOR 1
#define FONT_XGAP   2
#define FONT_YGAP   0

/* YUYV values according to BT.601-7 Table 3:
 * 1-254 (0 and 255 are exclusive for synchronization)
 * Y has black=16, white=235
 * U/V/Cb/Cr has equal=128
 */
#define COLOR_WHITE 0xEB80EB80
#define COLOR_BLACK 0x10801080

static void _fill32(u32 color, void *start, int n)
{
	u32 *p;

	WARN_ON_ONCE(!IS_ALIGNED((size_t)start, sizeof(u32)));

	p = start;
	while (n-- > 0)
		*p++ = color;
}

struct console_data {
	unsigned char *framebuffer;
	int xres, yres, stride;

	const unsigned char *font;
	int font_xsize, font_ysize;

	int cursor_x, cursor_y;
	u32 foreground, background;

	int border_left, border_right, border_top, border_bottom;

	int scrolled_lines;
};

static struct console_data *default_console;

#if 0
static int console_set_color(int background, int foreground)
{
	default_console->foreground = foreground;
	default_console->background = background;
	return 0;
}
#endif

static void console_drawc(struct console_data *con, int x, int y,
			  unsigned char c)
{
	int ax, ay;
	u32 *ptr;
	u32 color2x[2];
	int bits;

	x >>= 1;
	ptr = (u32 *)(con->framebuffer + con->stride * y + x * 4);

	for (ay = 0; ay < con->font_ysize; ay++) {
#if FONT_XFACTOR == 2
		u32 color;
		for (ax = 0; ax < 8; ax++) {
			if ((con->font[c * con->font_ysize + ay] << ax) & 0x80)
				color = con->foreground;
			else
				color = con->background;
#if FONT_YFACTOR == 2
			/* pixel doubling: we write u32 */
			ptr[ay * 2 * con->stride / 4 + ax] = color;
			/* line doubling */
			ptr[(ay * 2 + 1) * con->stride / 4 + ax] = color;
#else
			ptr[ay * con->stride / 4 + ax] = color;
#endif
		}
#else
		for (ax = 0; ax < 4; ax++) {
			bits = (con->font[c * con->font_ysize + ay] << (ax * 2));
			if (bits & 0x80)
				color2x[0] = con->foreground;
			else
				color2x[0] = con->background;
			if (bits & 0x40)
				color2x[1] = con->foreground;
			else
				color2x[1] = con->background;
			ptr[ay * con->stride / 4 + ax] =
			    (color2x[0] & 0xFFFF00FF) |
			    (color2x[1] & 0x0000FF00);
		}
#endif
	}
}

static void console_putc(struct console_data *con, char c)
{
	int line_ysize, copy_ysize;

	line_ysize = con->font_ysize * FONT_YFACTOR + FONT_YGAP;
	switch (c) {
	case '\n':
		con->cursor_y += line_ysize;
		con->cursor_x = con->border_left;
		break;
	default:
		console_drawc(con, con->cursor_x, con->cursor_y, c);
		con->cursor_x += con->font_xsize * FONT_XFACTOR + FONT_XGAP;
		if ((con->cursor_x + (con->font_xsize * FONT_XFACTOR)) >
		    con->border_right) {
			con->cursor_y += line_ysize;
			con->cursor_x = con->border_left;
		}
	}
	if ((con->cursor_y + con->font_ysize * FONT_YFACTOR) >= con->border_bottom) {
		copy_ysize = con->border_bottom - con->border_top - line_ysize;
		memcpy(con->framebuffer + con->stride * con->border_top,
			con->framebuffer + con->stride * (con->border_top + line_ysize),
			con->stride * copy_ysize);
		_fill32(con->background,
			con->framebuffer + con->stride * (con->border_top + copy_ysize),
			con->stride * line_ysize / 4);
		con->cursor_y -= line_ysize;
		con->scrolled_lines += 1;
	}
}

static void gcnvi_udbg_console_init(struct console_data *con, void *framebuffer,
			 int xres, int yres, int stride)
{
	const struct font_desc *font;

	con->framebuffer = framebuffer;
	con->xres = xres;
	con->yres = yres;
	con->border_left = 0;
	con->border_top = SCREEN_OVERSCAN;
	con->border_right = con->xres;
	con->border_bottom = con->yres - SCREEN_OVERSCAN;
	con->stride = stride;
	con->cursor_y = con->border_top;
	con->cursor_x = con->border_left;

	font = find_font(FONT_NAME);
	if (!font)
		font = get_default_font(xres, yres, U32_MAX, U32_MAX);
	con->font = font->data;
	con->font_xsize = font->width;
	con->font_ysize = font->height;
	WARN_ON(con->border_bottom - con->border_top < con->font_ysize * FONT_YFACTOR + FONT_YGAP);

	con->foreground = COLOR_WHITE;
	con->background = COLOR_BLACK;

	con->scrolled_lines = 0;

	default_console = con;
}

/*
 * Video hardware setup.
 *
 */

/* Hardware registers */
#define VI_VTR                  0x00 /* u16 */
#define VI_VTR_ACV              (0x3ff<<4)
#define VI_DCR                  0x02 /* u16 */
#define VI_DCR_ENABLE           (0x1<<0)
#define VI_TFBL                 0x1c
#define VI_TFBR                 0x20
#define VI_BFBL                 0x24
#define VI_BFBR                 0x28
#define VI_DPV                  0x2c

#define _VI_LOWEST_BIT(bits)		((~((bits)-1))&(bits))
#define _VI_VALUE(x,bits)		(((x)&(bits))/_VI_LOWEST_BIT(bits)) /* raw bits to value */

/* NTSC settings (640x480) */
static const u32 vi_Mode640X480NtscYUV16[32] = {
	0x0F060001, 0x476901AD, 0x02EA5140, 0x00030018,
	0x00020019, 0x410C410C, 0x40ED40ED, 0x00435A4E,
	0x00000000, 0x00435A4E, 0x00000000, 0x00000000,
	0x110701AE, 0x10010001, 0x00010001, 0x00010001,
	0x00000000, 0x00000000, 0x28500100, 0x1AE771F0,
	0x0DB4A574, 0x00C1188E, 0xC4C0CBE2, 0xFCECDECF,
	0x13130F08, 0x00080C0F, 0x00FF0000, 0x00000000,
	0x02800000, 0x000000FF, 0x00FF00FF, 0x00FF00FF
};

/* Returns true if VI is enabled. */
static bool __init vi_is_enabled(void __iomem *io_base)
{
	bool enabled;

	enabled = !!(in_be16(io_base + VI_DCR) & VI_DCR_ENABLE);
	return enabled;
}

/* Returns the number of active lines that VI is expecting in xfb data. */
static u32 __init vi_active_lines(void __iomem *io_base)
{
	u32 lines;

	lines = (u32)_VI_VALUE(in_be16(io_base + VI_VTR), VI_VTR_ACV) << 1;
	/* expecting something like 240 or 480 or 576 */
	if (lines < 200 || lines > 600)
		pr_warn("unexpected active lines %lu!\n", (unsigned long)lines);
	return lines;
}

static void __init vi_setup_video(void __iomem *io_base, unsigned long xfb_start)
{
	const u32 *mode;
	int i;

	if (!vi_is_enabled(io_base)) {
		pr_info("setup NTSC 480i\n");
		mode = vi_Mode640X480NtscYUV16;
		for (i = 0; i < 32; i++)
			out_be32(io_base + i * sizeof(u32), mode[i]);
#ifdef CONFIG_WII
		pr_warn("AVE-RVL is not being setup!\n");
#endif
	}

	/* replace framebuffer address, interlaced mode */
	out_be32(io_base + VI_TFBL, 0x10000000 | (xfb_start >> 5));
	xfb_start += 2 * SCREEN_WIDTH;	/* line length */
	out_be32(io_base + VI_BFBL, 0x10000000 | (xfb_start >> 5));
}

/*
 * Retrieves and prepares the virtual address needed to access the hardware.
 */
static void __iomem *vi_setup_io_base(struct device_node *np)
{
	phys_addr_t paddr;
	const unsigned int *reg;
	void *io_base = NULL;

	reg = of_get_property(np, "reg", NULL);
	if (reg) {
		paddr = of_translate_address(np, reg);
		if (paddr)
			io_base = ioremap(paddr, reg[1]);
	}
	return io_base;
}

/*
 * udbg functions.
 *
 */

/* OF bindings */
static struct of_device_id gcnvi_udbg_ids[] __initdata = {
	{ .compatible = "nintendo,hollywood-vi", },
	{ .compatible = "nintendo,flipper-vi", },
};

static struct console_data gcnvi_udbg_console;

/*
 * Transmits a character.
 */
void gcnvi_udbg_putc(char ch)
{
	if (default_console)
		console_putc(default_console, ch);
}

/*
 * Initializes udbg support.
 *
 * NOTE
 * udbg is started before MMU and the device tree are available.
 * Initializing at that time requires parsing raw fdt and setting up an
 * expanded FIX_EARLY_DEBUG area. Instead of that, this udbg driver is being
 * initialized after MMU and the device tree become available.
 */
void __init gcnvi_udbg_init(void)
{
	unsigned long xfb_start = 0, xfb_size = 0;
	struct device_node *np = NULL;
	const unsigned long *prop;
	void *screen_base;
	void *io_base;
	u32 screen_height;

	for_each_matching_node(np, gcnvi_udbg_ids) {
		if (np)
			break;
	}
	if (!np)
		return;

	prop = of_get_property(np, "xfb-start", NULL);
	if (prop) {
		xfb_start = *prop;
		prop = of_get_property(np, "xfb-size", NULL);
		if (prop)
			xfb_size = *prop;
	}
	io_base = vi_setup_io_base(np);

	of_node_put(np);

	if (!prop || !io_base)
		return;

	screen_height = vi_is_enabled(io_base) ? vi_active_lines(io_base) : SCREEN_HEIGHT;
	if (xfb_size < 2 * SCREEN_WIDTH * screen_height)
		return;

	screen_base = ioremap_nocache(xfb_start, xfb_size);
	if (!screen_base)
		return;

	if (!IS_ALIGNED(xfb_start, 32) || !IS_ALIGNED(xfb_size, 32))
		pr_warn("xfb is not 32 byte aligned!\n");

	/* clear xfb and setup VI */
	_fill32(COLOR_BLACK, screen_base, xfb_size / sizeof(u32));
	vi_setup_video(io_base, xfb_start);

	gcnvi_udbg_console_init(&gcnvi_udbg_console, screen_base,
		     SCREEN_WIDTH, screen_height, 2 * SCREEN_WIDTH);

	udbg_putc = gcnvi_udbg_putc;
	pr_info("ready\n");
}
