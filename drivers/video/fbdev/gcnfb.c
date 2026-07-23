// SPDX-License-Identifier: GPL-2.0+
/*
 * Nintendo GameCube/Wii Video Interface (VI) frame buffer driver
 * Copyright (C) 2004-2009 The GameCube Linux Team
 * Copyright (C) 2004 Michael Steil <mist@c64.org>
 * Copyright (C) 2004,2005 Todd Jeffreys <todd@voidpointer.org>
 * Copyright (C) 2006,2007,2008,2009 Albert Herranz
 * Copyright (C) 2025-2026 Joe Mason <buddyjojo06@outlook.com>
 * Copyright (C) 2024-2026 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
 *
 * Based on vesafb (c) 1998 Gerd Knorr <kraxel@goldbach.in-berlin.de>
 *
 * GX code partially based on the following sources:
 *   - NetBSD WiiFB:
 *     Copyright (c) 2025 Jared McNeill <jmcneill@invisible.ca>
 *     All rights reserved.
 *
 *   - GXFB:
 *     Copyright (C) 2025-2026 Techflash
 *
 *   - libogc gx.c:
 *     Copyright (C) 2004 - 2025
 *     Michael Wiedenbauer (shagkur)
 *     Dave Murphy (WinterMute)
 */

#define DRV_MODULE_NAME   "gcn-vifb"
#define DRV_DESCRIPTION   "Nintendo GameCube/Wii Video Interface (VI) driver"
#define DRV_AUTHOR        "Michael Steil <mist@c64.org>, " \
			  "Todd Jeffreys <todd@voidpointer.org>, " \
			  "Albert Herranz, " \
			  "neagix, " \
			  "Techflash <officialTechflashYT@gmail.com>"

#define pr_fmt(fmt)     DRV_MODULE_NAME ": " fmt

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <asm/reg.h>
#ifdef CONFIG_WII_AVE_RVL
#include <linux/i2c.h>
#endif

/*
 * Driver changelog, since the git history has been screwed up over the years.
 * 1.0i - Albert Herranz (1/24/2009) - Initial release of the driver, based on vesafb
 *
 * 2.0i - Albert Herranz (3/01/2009 - 5/30/2009)
 *         - add video mode timings setup
 *         - Wii audio/video encoder support (RVL-AVE)
 *         - reset video hardware before initiating detection
 *         - add shutdown method
 *         - re-detect tv mode if cable type changes
 *         - initialize page addresses before using them
 *         - add nostalgic mode
 *
 * 2.1i - Albert Herranz (6/08/2009) - fix visible page calculation when panning
 *
 * 2.2  - neagix (8/15/2017) -
 *          - added RGB565 framebuffer patches, originally derived from
 *            https://fartersoft.com/blog/2011/06/22/hacking-up-an-rgb-framebuffer-driver-for-wii-linux/
 *          - tons of general cleanup and modernization
 *
 * 2.3t - Techflash (6/3/2024) - Corrected nostalgic mode height from 480 to 448, added this changelog
 *
 * 2.4t - Techflash (11/16/2025) - Massive cleanups, make nostalgic mode the only option, fixup resolutions
 *
 * 2.5t - Techflash (07/29/2026) - Use GX for all copying and conversion
 */
static char vifb_driver_version[] = "2.5t";

/*
 * Hardware registers.
 */

#define __declare_vi_reg_set_field(reg_size, reg, field_size, field, \
				   mask, shift) \
static inline reg_size vi_##reg##_set_##field(reg_size reg, field_size field) \
{									\
	reg &= ~(mask << shift);					\
	reg |= (field & mask) << shift;					\
	return reg;							\
}

#define __declare_vi_reg_clear_field(reg_size, reg, field, mask, shift) \
static inline reg_size vi_##reg##_clear_##field(reg_size reg)	\
{									\
	reg &= ~(mask << shift);					\
	return reg;							\
}

#define __declare_vi_reg_get_field(reg_size, reg, field_size, field, \
				   mask, shift) \
static inline field_size vi_##reg##_get_##field(reg_size reg)	\
{									\
	return (reg>>shift)&mask;					\
}

#define __declare_vi_reg_field(reg_size, reg, field_size, field, \
			       mask, shift) \
static inline reg_size vi_##reg##_##field(field_size field)	\
{									\
	return (field & mask) << shift;					\
}

#define __vi_reg_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_set_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_clear_field(reg_size, reg, field, mask, shift) 	  \
__declare_vi_reg_get_field(reg_size, reg, field_size, field, mask, shift) \
__declare_vi_reg_field(reg_size, reg, field_size, field, mask, shift)


#define VI_VTR			0x00 /* Vertical Timing, 16 bits */
__vi_reg_field(u16, vtr, u16, acv, 0x3ff, 4);		/* ACtive Video */
__vi_reg_field(u16, vtr, u8, equ, 0xf, 0);		/* EQUalization pulse */

#define VI_DCR			0x02 /* Display Configuration, 16 bits */
__vi_reg_field(u16, dcr, u8, fmt, 0x3, 8);		/* Format */
__vi_reg_field(u16, dcr, u8, le1, 0x3, 6);		/* Latch Enable 1 */
__vi_reg_field(u16, dcr, u8, le0, 0x3, 4);		/* Latch Enable 0 */
__vi_reg_field(u16, dcr, u8, dlr, 0x1, 3);		/* 3D mode */
__vi_reg_field(u16, dcr, u8, nin, 0x1, 2);		/* Non-Interlaced */
__vi_reg_field(u16, dcr, u8, rst, 0x1, 1);		/* Reset */
__vi_reg_field(u16, dcr, u8, enb, 0x1, 0);		/* Enable */

#define VI_HTR0			0x04 /* Horizontal Timing 0, 32 bits */
__vi_reg_field(u32, htr0, u8, hcs, 0x7f, 24);		/* Horz Color Start */
__vi_reg_field(u32, htr0, u8, hce, 0x7f, 16);		/* Horz Color End */
__vi_reg_field(u32, htr0, u16, hlw, 0x1ff, 0);		/* Half Line Width */

#define VI_HTR1			0x08 /* Horizontal Timing 1, 32 bits */
__vi_reg_field(u32, htr1, u16, hbs, 0x3ff, 17);		/* Horz Blank Start */
__vi_reg_field(u32, htr1, u16, hbe, 0x3ff, 7);		/* Horz Blank End */
__vi_reg_field(u32, htr1, u8, hsy, 0x7f, 0);		/* Horz Sync Width */

#define VI_VTO			0x0c /* Vertical Timing Odd, 32 bits */
__vi_reg_field(u32, vto, u16, psb, 0x3ff, 16);		/* Post Blanking */
__vi_reg_field(u32, vto, u16, prb, 0x3ff, 0);		/* Pre Blanking */

#define VI_VTE			0x10 /* Vertical Timing Even, 32 bits */
__vi_reg_field(u32, vte, u16, psb, 0x3ff, 16);		/* Post Blanking */
__vi_reg_field(u32, vte, u16, prb, 0x3ff, 0);		/* Pre Blanking */

#define VI_BBOI			0x14 /* Burst Blanking Odd Interval, 32 bits */
__vi_reg_field(u32, bboi, u16, be3, 0x7ff, 21);
__vi_reg_field(u32, bboi, u8, bs3, 0x1f, 16);
__vi_reg_field(u32, bboi, u16, be1, 0x7ff, 5);
__vi_reg_field(u32, bboi, u8, bs1, 0x1f, 0);

#define VI_BBEI			0x18 /* Burst Blanking Even Interval, 32 bits */
__vi_reg_field(u32, bbei, u16, be4, 0x7ff, 21);
__vi_reg_field(u32, bbei, u8, bs4, 0x1f, 16);
__vi_reg_field(u32, bbei, u16, be2, 0x7ff, 5);
__vi_reg_field(u32, bbei, u8, bs2, 0x1f, 0);

#define VI_TFBL			0x1c /* Top Field Base (L), 32 bits */
__vi_reg_field(u32, tfbl, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, tfbl, u8, xof, 0xf, 24);		/* X Offset */
__vi_reg_field(u32, tfbl, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_TFBR			0x20 /* Top Field Base (R), 32 bits */
__vi_reg_field(u32, tfbr, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, tfbr, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_BFBL			0x24 /* Bottom Field Base (L), 32 bits */
__vi_reg_field(u32, bfbl, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, bfbl, u8, xof, 0xf, 24);		/* X Offset */
__vi_reg_field(u32, bfbl, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_BFBR			0x28 /* Bottom Field Base (R), 32 bits */
__vi_reg_field(u32, bfbr, u8, pob, 0x1, 28);		/* Page Offset Bit */
__vi_reg_field(u32, bfbr, u32, fba, 0xffffff, 0);	/* Frame Buf Address */

#define VI_DI0			0x30 /* Display Interrupt 0, 32 bits */
#define VI_DI1			0x34 /* Display Interrupt 1, 32 bits */
#define VI_DI2			0x38 /* Display Interrupt 2, 32 bits */
#define VI_DI3			0x3C /* Display Interrupt 3, 32 bits */
__vi_reg_field(u32, dix, u8, irq, 0x1, 31);
__vi_reg_field(u32, dix, u8, enb, 0x1, 28);
__vi_reg_field(u32, dix, u16, vct, 0x3ff, 16);
__vi_reg_field(u32, dix, u16, hct, 0x3ff, 0);

#define VI_PCR			0x48 /* Picture Configuration, 16 bits */
__vi_reg_field(u16, pcr, u8, wpl, 0xff, 8);	/* reads per line in words */
__vi_reg_field(u16, pcr, u8, std, 0xff, 0);	/* stride per line in words */

#define VI_HSR			0x4a /* Horizontal Scaling, 16 bits */
__vi_reg_field(u16, hsr, u8, hs_en, 0x1, 12);
__vi_reg_field(u16, hsr, u16, stp, 0x1ff, 0);

#define VI_FCT0			0x4c /* Filter Coeficient Table 0, 32 bits */
#define VI_FCT1			0x50 /* Filter Coeficient Table 1, 32 bits */
#define VI_FCT2			0x54 /* Filter Coeficient Table 2, 32 bits */
#define VI_FCT3			0x58 /* Filter Coeficient Table 3, 32 bits */
#define VI_FCT4			0x5c /* Filter Coeficient Table 4, 32 bits */
#define VI_FCT5			0x60 /* Filter Coeficient Table 5, 32 bits */
#define VI_FCT6			0x64 /* Filter Coeficient Table 6, 32 bits */

#define VI_AA			0x68 /* Anti-aliasing, 32 bits */

#define VI_CLK			0x6c /* Video Clock, 16 bits */
__vi_reg_field(u16, clk, u8, _54mhz, 0x1, 0);

#define VI_SEL			0x6e /* DTV Status, 16 bits */
__vi_reg_field(u16, sel, u8, component, 0x1, 0);

#define VI_HBE			0x72 /* Horizontal Border End, 16 bits */
#define VI_HBS			0x74 /* Horizontal Border Start, 16 bits */

#define VI_UNK1			0x76 /* Unknown1, 16 bits */
#define VI_UNK2			0x78 /* Unknown2, 32 bits */
#define VI_UNK3			0x7c /* Unknown3, 32 bits */


enum {
	VI_SCAN_DONTCARE = 0,
	VI_SCAN_INTERLACED,
	VI_SCAN_PROGRESSIVE,
};
enum {
	VI_RATE_DONTCARE = 0,
	VI_RATE_50Hz,
	VI_RATE_60Hz,
};
enum {
	VI_TV_DONTCARE = 0,
	VI_TV_NTSC,
	VI_TV_PAL,
};


/*
 * Video modes and timings.
 *
 */

enum {
	VI_VM_NTSC_480i = 0,
	VI_VM_NTSC_480p,
	VI_VM_PAL_576i50,
	VI_VM_PAL_480i60,
	VI_VM_PAL_480p,
};

enum vi_video_format {
	VI_FMT_NTSC = 0,
	VI_FMT_PAL,
	VI_FMT_MPAL,
	VI_FMT_DEBUG,
};

enum vi_tv_mode_flags {
	__PAL_COLOR,	/* vs NTSC_COLOR */
	__PROGRESSIVE,	/* vs interlaced */
};

#define VI_VMF_PAL_COLOR	(1<<__PAL_COLOR)
#define VI_VMF_PROGRESSIVE	(1<<__PROGRESSIVE)


#define VI_VERT_ALIGN		0x1	/* in lines-1 */
#define VI_HORZ_ALIGN		0xf	/* in pixels-1 */
#define VI_HORZ_WORD_SIZE	32	/* bytes */

#define VI_XFB_WIDTH		640
#define TV_BYTES_PER_PIXEL	2 /* all supported TV modes are native YUYV */
#define VI_YUYV_BLACK		0x10801080

/*
 * Video mode timings.
 */
struct vi_mode_timings {
	/* VERTICAL SETTINGS */

	/*
	 * NTSC 480i
	 * 1 field = 262.5 lines (242.5 active, 20 blank)
	 * 1 frame = 2 fields = 2 x 262.5 = 525 lines (485 active, 40 blank)
	 *
	 * PAL 576i
	 * 1 field = 312.5 lines (287.5 active, 25 blank)
	 * 1 frame = 2 fields = 2 x 312.5 = 625 lines (575 active, 50 blank)
	 *
	 * NOTES:
	 * - the start of sync is considered the start of a line
	 * - the width of a half line is the width of a line divided by two
	 *
	 */

	/*
	 * Vertical position of the first active video line (0=top).
	 */
	unsigned int ypos;

	/*
	 * Horizontal position in pixels where the vertical blanking
	 * interval starts. Used for signaling the start of the vertical
	 * retrace.
	 */
	unsigned int htrap;

	/*
	 * Vertical position in field lines where the vertical blanking
	 * interval starts. Used for signaling the start of the vertical
	 * retrace.
	 */
	unsigned int vtrap;

	/*
	 * Active Video, specified in number of field lines.
	 */
	u16	acv;
	/*
	 * Equalization pulse, specified in number of half lines.
	 */
	u8	equ;

	/*
	 * Pre-blanking, specified in half lines.
	 */
	u16	prb_odd;
	u16	prb_even;

	/*
	 * Post-blanking, specified in half lines.
	 */
	u16	psb_odd;
	u16	psb_even;

	/*
	 * NOTE:
	 * Irrespective of what patent 6,609,977 says:
	 * - "bs*" seems to tell where the burst blanking for the current
	 *   field ends
	 * - "be*" seems to tell where the next burst blanking starts
	 */

	/*
	 * Patent says: "Start to burst blanking start in half lines".
	 */
	u8	bs1;
	u8	bs2;
	u8	bs3;
	u8	bs4;

	/*
	 * Patent says: "Start to burst blanking end in half lines".
	 */
	u16	be1;
	u16	be2;
	u16	be3;
	u16	be4;

	/* HORIZONTAL SETTINGS */

	/*
	 * A = Blank Start to Horizontal Sync Start, "Front Porch"
	 *     right_margin
	 * B = Horizontal Sync Width
	 *     hsync_len
	 * C = Horizontal Sync End to Blank End, "Back Porch"
	 *     left_margin
	 * D = Horizontal Line Width
	 *     hsync_len + left_margin + xres + right_margin
	 * E = Horizontal Visible Width
	 *     xres
	 *
	 *               :<-----------------D----------------->:
	 *           :   :     :     :<----------E-------->:   :
	 *           :<A>:<-B->:<-C->:                     :<A>:<-B->:<-C->:
	 *           :   :     :     :                     :   :     :     :
	 *        ___                 __________//_________                 __
	 *           |               |                     |               |
	 *           |               |                     |               |
	 * Blank     |___       _____|                     |___       _____|
	 *               |     |                               |     |
	 * Sync          |_____|                               |_____|
	 *
	 *
	 * f = Sync Start to Color Burst Start
	 * g = Color Burst Width
	 *
	 *  :       :             :                  :
	 *  :<--A-->:<-----B----->:<-------C-------->:
	 *  :       :             :                  :
	 *                                             _ Peak white level
	 *  |                            Color       |
	 *  |                            Burst       |
	 *  |_______               ______|||||||||___| _ Blanking level
	 *          | Sync        |      |||||||||
	 *          |_____________|                    _ Sync level
	 *
	 *  :       :                    :       :   :
	 *  :       :<---------f-------->:<--g-->:   :
	 *  :<--------------- A + B + C ------------>:
	 *  :                                        :
	 *
	 */

	/* Half (horizontal) line width, in pixel clocks (D/2)  */
	u16 hlw;

	/* Horizontal Sync Width, in pixel clocks (B) */
	u8 hsy;

	/* NOTE
	 * The color burst interval falls within the back porch,
	 * i.e. hcs must be greater than B and hce lower than B+C.
	 */

	/* Horizontal sync start to color burst start in pixel clocks (f) */
	u8 hcs;
	/* Horizontal sync start to color burst end in pixel clocks (f+g) */
	u8 hce;

	/*
	 * The following two settings depend on the effective horizontal
	 * line length, as they rely on A or C.
	 */

	/* Half line to horizontal blank start (D/2 - A)*/
	u16 hbs;

	/* Horizontal sync start to horizontal blank end (B+C)*/
	u16 hbe;

};

/*
 * TV mode.
 */
struct vi_tv_mode {
	char *name;
	__u32 flags;
	int width;		/* visible width in pixels */
	int height;		/* visible height in lines */
	int lines;		/* total lines */
};

/*
 * Video control data structure.
 */
struct vi_ctl {
	spinlock_t lock;

	void __iomem *io_base;
	void __iomem *wgpipe;
	void __iomem *pi_base;
	void __iomem *cp_base;
	void __iomem *pe_base;
	phys_addr_t gx_fifo_base;
	void *fifo;
	dma_addr_t fifo_dma;
	void *rgb_fb;
	dma_addr_t rgb_fb_dma;
	size_t rgb_fb_size;
	void *indirect_map;
	dma_addr_t indirect_map_dma;
	bool gx_faulted;
	struct completion pe_finished;
	unsigned int irq;
	unsigned int pe_irq;

	int in_vtrace;
	wait_queue_head_t vtrace_waitq;

	int visible_page;
	unsigned long page_address[2];
	unsigned long flip_pending;

	struct vi_tv_mode *mode;
	struct vi_mode_timings timings;
	int has_component_cable:1;	/* at last detection time */

	struct fb_info *info;
#ifdef CONFIG_WII_AVE_RVL
	struct i2c_client *i2c_client;
#endif
	struct device *dev;
};


/*
 * TV Mode Table
 */
static struct vi_tv_mode vi_tv_modes[] = {
	[VI_VM_NTSC_480i] = {
		.name = "NTSC 480i",

		.width = 640,
		.height = 448,
		.lines = 525,
	},
	[VI_VM_NTSC_480p] = {
		.name = "NTSC 480p",
		.flags = VI_VMF_PROGRESSIVE,
		.width = 640,
		.height = 448,
		.lines = 525,
	},
	[VI_VM_PAL_576i50] = {
		.name = "PAL 576i",
		.flags = VI_VMF_PAL_COLOR,
		.width = 640,
		.height = 574,
		.lines = 625,
	},
	[VI_VM_PAL_480i60] = {
		.name = "PAL 480i 60Hz",
		.flags = VI_VMF_PAL_COLOR,
		.width = 640,
		.height = 448,
		.lines = 525,
	},
	[VI_VM_PAL_480p] = {
		.name = "PAL 480p",
		.flags = VI_VMF_PROGRESSIVE|VI_VMF_PAL_COLOR,
		.width = 672,
		.height = 448,
		.lines = 525,
	},
};

/*
 * Filter Coeficient Table
 */
static const u32 vi_fct[] = {
	0x1AE771F0, 0x0DB4A574, 0x00C1188E, 0xC4C0CBE2,
	0xFCECDECF, 0x13130F08, 0x00080C0F,
};


/*
 * Default fix and var framebuffer data.
 */
static struct fb_fix_screeninfo vifb_fix = {
	.id = DRV_MODULE_NAME,
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,	/* lies, lies, lies, ... */
	.accel = FB_ACCEL_NONE,
	.capabilities = FB_CAP_FOURCC,
};

static struct fb_var_screeninfo vifb_var = {
	.activate = FB_ACTIVATE_NOW,
	.width = 640,
	.height = 448,
	.bits_per_pixel = 16,
	.vmode = FB_VMODE_INTERLACED,
};


/*
 * Setup parameters.
 */
static int want_ypan = 1;		/* 0..nothing, 1..ypan */

static int force_scan;
static int force_rate;
static int force_tv;

static u32 pseudo_palette[16];

/*
 * Framebuffer layout:
 *
 * Userspace and the fbdev helpers draw into a linear RGB565 DMA buffer.  At
 * vertical retrace GX samples that buffer as a texture, uses a 16x4 indirect
 * texture to undo GX's 4x4 RGB565 tiling in texture-coordinate space, renders
 * the result to EFB, then copies EFB to the YUYV XFB consumed by VI.
 */
static unsigned long gx_xfb_start;
static void *xfb_mem;
static unsigned int gx_xfb_size;

#define GX_INDIRECT_WIDTH		16
#define GX_INDIRECT_HEIGHT		4
#define GX_WIDE_EFB_WIDTH		672
#define GX_TALL_EFB_HEIGHT		574
#define GX_INDIRECT_MAP_SIZE		(GX_INDIRECT_WIDTH * \
					 GX_INDIRECT_HEIGHT * sizeof(u32))
#ifdef CONFIG_WII_AVE_RVL
static int vi_ave_setup(struct vi_ctl *ctl);
static int vi_ave_get_video_format(struct vi_ctl *ctl,
				   enum vi_video_format *fmt);
#endif

/*
 * Video mode timings calculation.
 *
 * Please, refer to the definition of "struct vi_mode_timings" for
 * a explanation of the different constants involved.
 *
 * References:
 * - http://www.pembers.freeserve.co.uk/World-TV-Standards
 */

static inline int vi_vmode_is_progressive(__u32 vmode)
{
	return (vmode & FB_VMODE_MASK) == FB_VMODE_NONINTERLACED;
}

static int vi_calc_horz_timings(struct vi_mode_timings *timings,
				 struct fb_var_screeninfo *var,
				 u16 width, u16 max_active_width,
				 u16 A, u8 B, u16 C, u16 D,
				 u8 f, u16 g)
{
	u16 extra_blanking, margin;

	if (width > max_active_width)
		return -EINVAL;

	/* adjusted horizontal settings */
	extra_blanking = max_active_width - width;
	margin = extra_blanking / 2;
	A += margin;
	C += extra_blanking - margin;

	timings->hlw = D / 2;
	timings->hsy = B;
	timings->hcs = f;
	timings->hce = f + g;
	timings->hbs = (D/2) - A;
	timings->hbe = B + C;

	/*
	 * Start of the blanking interval, between the first and second fields,
	 * begins after the last half line of the field.
	 */
	timings->htrap = (D / 2) + 1;

	var->left_margin = C;
	var->right_margin = A;
	var->hsync_len = B;

	return 0;
}

static int vi_ntsc_525_calc_horz_timings(struct vi_mode_timings *timings,
					  struct fb_var_screeninfo *var,
					  u16 width)
{
	u16 max_active_width;
	u16 A, C, D, g;
	u8 B, f;

	/* standard horizontal settings for 714 pixels */
	D = 858;		/* pixel clocks (H=63.556us, 13.5MHz clock) */
	max_active_width = 714;	/* (52.9us) 714.15 pixel clocks */
	B = 64;			/* ( 4.7us)  63.45 pixel clocks */
	f = 71;			/* ( 5.3us)  71.55 pixel clocks */
	g = 34;			/* ( 2.5us)  33.75 pixel clocks */
	A = 20;			/* ( 1.5us)  20.25 pixel clocks */
	C = 60;			/* ( 4.5us)  60.75 pixel clocks */

	return vi_calc_horz_timings(timings, var, width, max_active_width,
				    A, B, C, D, f, g);
}

static int vi_calc_vert_timings(struct vi_mode_timings *timings,
				struct fb_var_screeninfo *var,
				u16 height, u16 max_active_height,
				u16 P, u16 Q, u8 equ)
{
	u16 extra_blanking, margin, prb, psb;
	u8 interlace_bias;
	u8 shift;

	if (height > max_active_height)
		return -EINVAL;

	extra_blanking = max_active_height - height;	/* in frame lines */
	margin = extra_blanking / 2;			/* centered margins */
	prb = margin; 					/* in half lines */
	psb = extra_blanking - margin;			/* in half lines */

	/*
	 * Start of the blanking interval, between the first and second fields,
	 * begins after the last line of the field.
	 */
	if (vi_vmode_is_progressive(var->vmode)) {
		timings->acv = height;
		timings->vtrap = prb + height;
		interlace_bias = 0;
		shift = 1;
	} else {
		timings->acv = height / 2;
		timings->vtrap = (prb + height) / 2;
		interlace_bias = 1;
		shift = 0;
	}

	timings->equ = equ << shift;
	var->vsync_len = (3 * timings->equ) / 2; /* pre-eq + sync + post-eq */

	/*
	 * prb_* is specified as the number of half-lines since the end of
	 * the post-equalizing period.
	 * psb_* is specified as the number of half-lines from the end of
	 * the field.
	 */

	timings->ypos = margin;

	if (timings->ypos & 0x01) {
		/* odd field (1,3,5,...) */
		timings->prb_odd = (P + interlace_bias + prb) << shift;
		timings->psb_odd = (Q - interlace_bias + psb) << shift;
		timings->prb_even = (P + prb) << shift;
		timings->psb_even = (Q + psb) << shift;
	} else {
		/* even field (2,4,6,...) */
		timings->prb_even = (P + interlace_bias + prb) << shift;
		timings->psb_even = (Q - interlace_bias + psb) << shift;
		timings->prb_odd = (P + prb) << shift;
		timings->psb_odd = (Q + psb) << shift;
	}

	var->upper_margin = (Q + prb) / 2;
	var->lower_margin = (P + psb) / 2;

	return 0;
}

static int vi_ntsc_525_calc_vert_timings(struct vi_mode_timings *timings,
					 struct fb_var_screeninfo *var,
					 u16 height)
{
	u16 max_active_height;
	u16 P, Q;
	u8 equ;

	/* standard vertical settings for 484 active lines */
	max_active_height = 484;	/* 2 * 242.5 = 485 (*1) */

	/* blanking interval */
	/* from start of line 10, field 1 to end of line 20, field 1 */
	P = 2 * (20-10 + 1);
	Q = 1;	/* (*1) field line compensation for 484 vs 485 lines */

	equ = 2 * 3;	/* 3 lines of equalization */

	return vi_calc_vert_timings(timings, var, height, max_active_height,
				    P, Q, equ);
}

static int vi_pal_625_calc_timings(struct vi_mode_timings *timings,
				   struct fb_var_screeninfo *var,
				   unsigned int width, unsigned int height)
{
	u16 max_active_height, max_active_width;
	u16 A, C, D, g, P, Q;
	u8 B, f, equ;
	int error;

	/* standard horizontal settings for 702 pixels */
	D = 864;		/* pixel clocks (H=64us, 13.5MHz clock) */
	max_active_width = 702; /* (51.95us) 701.32 pixel clocks */
	B = 64;			/* ( 4.7us)   63.45 pixel clocks */
	f = 75;			/* ( 5.6us)   75.6  pixel clocks */
	g = 30;			/* ( 2.25us)  30.38 pixel clocks */
	A = 22;			/* ( 1.65us)  22.27 pixel clocks */
	C = 76;			/* ( 5.7us)   76.95 pixel clocks */

	error = vi_calc_horz_timings(timings, var, width, max_active_width,
				     A, B, C, D, f, g);
	if (error)
		return error;

	/* standard vertical settings for 574 active lines */
	max_active_height = 574;	/* 2 * 287.5 = 575 (*1) */

	/* blanking interval */
	/* from start of line 6, field 1 to mid of line 23, field 1 */
	P = (2 * (23-6 + 1)) - 1;
	Q = 1;	/* (*1) field line compensation for 574 vs 575 lines */

	equ = 2 * 2.5;		/* 2.5 lines of equalization */

	error = vi_calc_vert_timings(timings, var, height, max_active_height,
				     P, Q, equ);
	if (error)
		return error;

	/*
	 * Location of the 9 lines of burst blanking for each field
	 * (settings expressed in half lines).
	 */

	/* from start of line 1, field 1 to end of line 6, field 1 */
	timings->bs1 = 2 * (6-1 + 1);

	/* from start of line 1, field 1 to end of line 309, field 2 */
	timings->be1 = 2 * (309-1 + 1);

	/* from mid of line 313, field 2 to end of line 318, field 2 */
	timings->bs2 = (2 * (318-313 + 1)) - 1;

	/* from mid of line 313, field 2 to end of line 621, field 2 */
	timings->be2 = (2 * (612-617 + 1)) - 1;

	/* from start of line 1, field 3 to end of line 5, field 3 */
	timings->bs3 = 2 * (5-1 + 1);

	/* from start of line 1, field 3 to end of line 310, field 4 */
	timings->be3 = 2 * (310-1 + 1);

	/* from mid of line 313, field 4 to end of line 319, field 4 */
	timings->bs4 = (2 * (319-313 + 1)) - 1;

	/* from mid of line 313, field 4 to end of line 622, field 4 */
	timings->be4 = (2 * (622-313 + 1)) - 1;

	return 0;
}

static int vi_ntsc_525_calc_timings(struct vi_mode_timings *timings,
				    struct fb_var_screeninfo *var,
				    unsigned int width, unsigned int height)
{
	int error;

	error = vi_ntsc_525_calc_horz_timings(timings, var, width);
	if (error)
		return error;

	error = vi_ntsc_525_calc_vert_timings(timings, var, height);
	if (error)
		return error;

	/*
	 * Location of the 9 lines of burst blanking for each field
	 * (settings expressed in half lines).
	 */

	/* from start of line 4, field 1 to end of line 9, field 1 */
	timings->bs1 = 2 * (9-4 + 1);

	/* from start of line 4, field 1 to end of line 263, field 2 */
	timings->be1 = 2 * (263-4 + 1);

	/* from mid of line 266, field 2 to end of line 272, field 2 */
	timings->bs2 = (2 * (272-266 + 1)) - 1;

	/* from mid of line 266, field 2 to end of line 525, field 2 */
	timings->be2 = (2 * (525-266 + 1)) - 1;

	/* from start of line 4, field 3 to end of line 9, field 3 */
	timings->bs3 = 2 * (9-4 + 1);

	/* from start of line 4, field 3 to end of line 263, field 4 */
	timings->be3 = 2 * (263-4 + 1);

	/* from mid of line 266, field 4 to end of line 272, field 4 */
	timings->bs4 = (2 * (272-266 + 1)) - 1;

	/* from mid of line 266, field 4 to end of line 525, field 4 */
	timings->be4 = (2 * (525-266 + 1)) - 1;

	return 0;
}

static int vi_ntsc_525_prog_calc_timings(struct vi_mode_timings *timings,
					 struct fb_var_screeninfo *var,
					 unsigned int width,
					 unsigned int height)
{
	int error;

	error = vi_ntsc_525_calc_horz_timings(timings, var, width);
	if (error)
		return error;

	error = vi_ntsc_525_calc_vert_timings(timings, var, height);
	if (error)
		return error;

	/*
	 * Location of the 18 lines of burst blanking
	 * (settings expressed in half lines).
	 */

	/*
	 * |0 0 0 0 0 0|0 0 0 1 1 1|1 1 1 1 1 1|
	 * |1,2,3,4,5,6|7,8,9,0,1,2|3,4,5,6,7,8|
	 * :pre-equ    :sync       : post-equ  :
	 */

	/* from start of line 7 to end of line 18 */
	timings->bs1 = 2 * (18-7 + 1);
	timings->bs2 = timings->bs3 = timings->bs4 = timings->bs1;

	/* from start of line 7 to end of line 525 (last) */
	timings->be1 = 2 * (525-7 + 1);
	timings->be2 = timings->be3 = timings->be4 = timings->be1;

	return 0;
}

/*
 * Video hardware support.
 *
 */

static inline int vi_has_component_cable(struct vi_ctl *ctl)
{
	return vi_sel_get_component(in_be16(ctl->io_base + VI_SEL));
}

/*
 * Get video mode reported by hardware.
 * 0=NTSC, 1=PAL, 2=MPAL, 3=debug
 */
static inline enum vi_video_format vi_get_video_format(struct vi_ctl *ctl)
{
	return vi_dcr_get_fmt(in_be16(ctl->io_base + VI_DCR));
}

static inline int vi_video_format_is_ntsc(struct vi_ctl *ctl)
{
	return vi_get_video_format(ctl) == VI_FMT_NTSC;
}

static void vi_reset_video(struct vi_ctl *ctl)
{
	void __iomem *io_base = ctl->io_base;
	u16 dcr;

	dcr = in_be16(io_base + VI_DCR);
	out_be16(io_base + VI_DCR, vi_dcr_set_rst(dcr, 1));
	out_be16(io_base + VI_DCR, vi_dcr_clear_rst(dcr));
}

/*
 * Try to determine current TV video mode.
 */
static int vi_detect_tv_mode(struct vi_ctl *ctl)
{
	struct vi_tv_mode *modes = vi_tv_modes;
	void __iomem *io_base = ctl->io_base;
	char *source = "forced";
	enum vi_video_format fmt;
	int ntsc_idx, pal_idx;
	u16 dcr;
	int error;

	dcr = in_be16(io_base + VI_DCR);

	ctl->has_component_cable = vi_has_component_cable(ctl);

	if ((force_scan == VI_SCAN_PROGRESSIVE &&
					ctl->has_component_cable) ||
	    (force_scan != VI_SCAN_INTERLACED &&
					ctl->has_component_cable &&
					vi_dcr_get_nin(dcr))) {
		/* progressive modes */
		ntsc_idx = VI_VM_NTSC_480p;
		pal_idx = VI_VM_PAL_480p;
	} else {
		/* interlaced modes */
		ntsc_idx = VI_VM_NTSC_480i;
		if (force_rate == VI_RATE_50Hz ||
		    (force_rate != VI_RATE_60Hz &&
		     vi_dcr_get_fmt(dcr) == VI_FMT_PAL))
			pal_idx = VI_VM_PAL_576i50;
		else
			pal_idx = VI_VM_PAL_480i60;
	}

	if (force_tv == VI_TV_PAL ||
	    (force_tv != VI_TV_NTSC && pal_idx == VI_VM_PAL_576i50))
		fmt = VI_FMT_PAL;
	else if (force_tv == VI_TV_NTSC)
		fmt = VI_FMT_NTSC;
	else {
#ifdef CONFIG_WII_AVE_RVL
		/*
		 * Look at the audio/video encoder to detect true PAL vs NTSC.
		 */
		error = vi_ave_get_video_format(ctl, &fmt);
		if (error) {
			dev_warn(ctl->dev, "could not get video format from AVE: %d\n", error);
			/* initial guess before AVE is setup */
			error = 0;
			fmt = vi_get_video_format(ctl);
			source = "DCR";
		} else {
			source = "AVE";
		}
#else
		error = 0;
		fmt = vi_get_video_format(ctl);
		source = "DCR";
#endif
	}

	switch (fmt) {
	case VI_FMT_MPAL:
	case VI_FMT_DEBUG:
		/* we currently don't support MPAL or DEBUG, sorry */
		return -EINVAL;
	case VI_FMT_PAL:
		ctl->mode = modes + pal_idx;
		break;
	case VI_FMT_NTSC:
		ctl->mode = modes + ntsc_idx;
		break;
	default:
		return -EINVAL;
	}

	dev_info(ctl->dev, "format picked is: %d (source: %s)\n", fmt, source);
	dev_info(ctl->dev, "%s%s\n", ctl->mode->name, source);

	return 0;
}

/*
 * Initialize the video hardware for a given TV mode.
 */
static int vi_setup_tv_mode(struct vi_ctl *ctl, bool force_detect)
{
	void __iomem *io_base = ctl->io_base;
	struct vi_mode_timings *timings = &ctl->timings;
	struct fb_var_screeninfo *var = &ctl->info->var;
	struct vi_tv_mode *mode;
	int has_component_cable;
	u16 std, ppl;

	/* we need to re-detect the tv mode if the cable type changes */
	if (force_detect) {
		int error = vi_detect_tv_mode(ctl);
		if (error)
			return error;
	} else {
		has_component_cable = vi_has_component_cable(ctl);
		if ((ctl->has_component_cable && !has_component_cable) ||
			(!ctl->has_component_cable && has_component_cable)) {
			int error = vi_detect_tv_mode(ctl);
			if (error)
				return error;
		}
	}

	mode = ctl->mode;

	out_be16(io_base + VI_DCR,
		 vi_dcr_fmt((mode->lines == 625) ? VI_FMT_PAL : VI_FMT_NTSC) |
		 vi_dcr_nin((mode->flags & VI_VMF_PROGRESSIVE) ?  1 : 0) |
		 vi_dcr_enb(1));

	out_be16(io_base + VI_VTR,
		 vi_vtr_equ(timings->equ) | vi_vtr_acv(timings->acv));

	out_be32(io_base + VI_HTR0,
		 vi_htr0_hcs(timings->hcs) | vi_htr0_hce(timings->hce) |
		 vi_htr0_hlw(timings->hlw));

	out_be32(io_base + VI_HTR1,
		 vi_htr1_hbs(timings->hbs) | vi_htr1_hbe(timings->hbe) |
		 vi_htr1_hsy(timings->hsy));

	out_be32(io_base + VI_VTO,
		 vi_vto_prb(timings->prb_odd) | vi_vto_psb(timings->psb_odd));

	out_be32(io_base + VI_VTE,
		 vi_vte_prb(timings->prb_even) | vi_vte_psb(timings->psb_even));

	out_be32(io_base + VI_BBOI,
		 vi_bboi_bs1(timings->bs1) | vi_bboi_be1(timings->be1) |
		 vi_bboi_bs3(timings->bs3) | vi_bboi_be3(timings->be3));

	out_be32(io_base + VI_BBEI,
		 vi_bbei_bs2(timings->bs2) | vi_bbei_be2(timings->be2) |
		 vi_bbei_bs4(timings->bs4) | vi_bbei_be4(timings->be4));

	/* used only for 3D stuff */
	out_be32(io_base + VI_TFBR, 0);
	out_be32(io_base + VI_BFBR, 0);

	std = (var->xres_virtual * TV_BYTES_PER_PIXEL) / VI_HORZ_WORD_SIZE;
	if (!(mode->flags & VI_VMF_PROGRESSIVE))
		std *= 2;
	ppl = ALIGN((var->xoffset & VI_HORZ_ALIGN) + var->xres,
			VI_HORZ_ALIGN+1);
	out_be16(io_base + VI_PCR,
		 vi_pcr_std(std) |
		 vi_pcr_wpl((ppl * TV_BYTES_PER_PIXEL) / VI_HORZ_WORD_SIZE));

	/* disable horizontal scaler */
	out_be16(io_base + VI_HSR, vi_hsr_stp(256) | vi_hsr_hs_en(0));

	/* filter coefficient table, anti-aliasing */
	out_be32(io_base + VI_FCT0, vi_fct[0]);
	out_be32(io_base + VI_FCT1, vi_fct[1]);
	out_be32(io_base + VI_FCT2, vi_fct[2]);
	out_be32(io_base + VI_FCT3, vi_fct[3]);
	out_be32(io_base + VI_FCT4, vi_fct[4]);
	out_be32(io_base + VI_FCT5, vi_fct[5]);
	out_be32(io_base + VI_FCT6, vi_fct[6]);
	out_be32(io_base + VI_AA, 0x00ff0000);

	/* clock */
	out_be16(io_base + VI_CLK,
		 vi_clk__54mhz((mode->flags & VI_VMF_PROGRESSIVE) ? 1 : 0));

	/* borders for DEBUG mode encoder, not used in retail consoles */
	out_be16(io_base + VI_HBE, 0);
	out_be16(io_base + VI_HBS, 0);

	/* whatever */
	out_be16(io_base + VI_UNK1, 0x00ff);
	out_be32(io_base + VI_UNK2, 0x00ff00ff);
	out_be32(io_base + VI_UNK3, 0x00ff00ff);

	return 0;
}

/*
 * Set the address from where the video encoder will display data on screen.
 */
static void vi_set_framebuffer(struct vi_ctl *ctl, u32 addr)
{
	struct fb_info *info = ctl->info;
	void __iomem *io_base = ctl->io_base;
	u32 top, bot;
	u8 xof;
	int gx_ll = VI_XFB_WIDTH * TV_BYTES_PER_PIXEL;

	top = bot = addr;
	if (!vi_vmode_is_progressive(info->var.vmode)) {
		if (ctl->timings.ypos & 0x01)
			top += gx_ll;
		else
			bot += gx_ll;
	}
	xof = (top / 2) & VI_HORZ_ALIGN;

	out_be32(io_base + VI_TFBL,
		 vi_tfbl_pob(1) | vi_tfbl_xof(xof) | vi_tfbl_fba(top >> 5));
	out_be32(io_base + VI_BFBL, vi_bfbl_pob(1) | vi_bfbl_fba(bot >> 5));
}

/*
 * Swap the visible and back pages.
 */
static inline void vi_flip_page(struct vi_ctl *ctl)
{
	ctl->visible_page ^= 1;
	vi_set_framebuffer(ctl, ctl->page_address[ctl->visible_page]);

	ctl->flip_pending = 0;
}

static void vi_enable_interrupts(struct vi_ctl *ctl, int enable)
{
	void __iomem *io_base = ctl->io_base;

	if (enable) {
		/*
		 * We use DI0 and DI1 to signal the retrace interval.
		 */

		/* start of the vertical retrace */
		out_be32(io_base + VI_DI1,
			 vi_dix_irq(1) | vi_dix_enb(1) |
			 vi_dix_vct(ctl->timings.vtrap) |
			 vi_dix_hct(ctl->timings.htrap));

		/* end of the vertical retrace */
		out_be32(io_base + VI_DI0,
			 vi_dix_irq(1) | vi_dix_enb(1) |
			 vi_dix_vct(1) |
			 vi_dix_hct(1));
	} else {
		out_be32(io_base + VI_DI0, 0);
		out_be32(io_base + VI_DI1, 0);
	}
	/* these two are currently not used */
	out_be32(io_base + VI_DI2, 0);
	out_be32(io_base + VI_DI3, 0);
}

/*
 * GX support.  Broadway writes only the linear RGB565 texture in MEM1; GX
 * owns all EFB access.  During rendering and the EFB-to-XFB copy, the VI
 * interrupt handler retains the only CPU until PE reports completion.
 */
#define BP_REG(x)			((x) << 24)
#define GX_COORDS(x, y)			(((u32)(y) << 10) | (x))

#define GX_CMD_LOAD_CP			0x08
#define GX_CMD_LOAD_XF			0x10
#define GX_CMD_INVALIDATE_VTX		0x48
#define GX_CMD_LOAD_BP			0x61
#define GX_CMD_QUADS_VTXFMT0		0x80

#define BP_REG_GEN_MODE			BP_REG(0x00)
#define BP_REG_DISPLAY_COPY_FILTER(n)	BP_REG(0x01 + (n))
#define BP_REG_IND_MTX_A		BP_REG(0x06)
#define BP_REG_IND_MTX_B		BP_REG(0x07)
#define BP_REG_IND_MTX_C		BP_REG(0x08)
#define BP_REG_IND_IMASK		BP_REG(0x0f)
#define BP_REG_TEV_INDIRECT(stage)	BP_REG(0x10 + (stage))
#define BP_REG_SCISSOR_TL		BP_REG(0x20)
#define BP_REG_SCISSOR_BR		BP_REG(0x21)
#define BP_REG_LINE_POINT_WIDTH		BP_REG(0x22)
#define BP_REG_PERF0_TRI		BP_REG(0x23)
#define BP_REG_PERF0_QUAD		BP_REG(0x24)
#define BP_REG_IND_SCALE		BP_REG(0x25)
#define BP_REG_IND_ORDER		BP_REG(0x27)
#define BP_REG_TEV_ORDER		BP_REG(0x28)
#define BP_REG_SU_SSIZE(coord)		BP_REG(0x30 + 2 * (coord))
#define BP_REG_SU_TSIZE(coord)		BP_REG(0x31 + 2 * (coord))
#define BP_REG_ZMODE			BP_REG(0x40)
#define BP_REG_BLEND_MODE		BP_REG(0x41)
#define BP_REG_CONSTANT_ALPHA		BP_REG(0x42)
#define BP_REG_PE_CONTROL		BP_REG(0x43)
#define BP_REG_FIELD_MASK		BP_REG(0x44)
#define BP_REG_BUS_CLOCK0		BP_REG(0x46)
#define BP_REG_XFB_STRIDE		BP_REG(0x4d)
#define BP_REG_COPY_Y_SCALE		BP_REG(0x4e)
#define BP_REG_COPY_FILTER0		BP_REG(0x53)
#define BP_REG_COPY_FILTER1		BP_REG(0x54)
#define BP_REG_CLEAR_BBOX1		BP_REG(0x55)
#define BP_REG_CLEAR_BBOX2		BP_REG(0x56)
#define BP_REG_REVISION_BITS		BP_REG(0x58)
#define BP_REG_SCISSOR_OFFSET		BP_REG(0x59)
#define BP_REG_TEXTURE_INVALIDATE	BP_REG(0x66)
#define BP_REG_PERF1			BP_REG(0x67)
#define BP_REG_BUS_CLOCK1		BP_REG(0x69)
#define BP_REG_TEX_MODE0(map)		BP_REG(0x80 + (map))
#define BP_REG_TEX_MODE1(map)		BP_REG(0x84 + (map))
#define BP_REG_TEX_IMAGE0(map)		BP_REG(0x88 + (map))
#define BP_REG_TEX_IMAGE1(map)		BP_REG(0x8c + (map))
#define BP_REG_TEX_IMAGE2(map)		BP_REG(0x90 + (map))
#define BP_REG_TEX_IMAGE3(map)		BP_REG(0x94 + (map))
#define BP_REG_TEV_COLOR_ENV(stage)	BP_REG(0xc0 + 2 * (stage))
#define BP_REG_TEV_ALPHA_ENV(stage)	BP_REG(0xc1 + 2 * (stage))
#define BP_REG_ALPHA_COMPARE		BP_REG(0xf3)

#define BP_REG_PE_DONE			BP_REG(0x45)
#define BP_REG_EFB_COORDS_MIN		BP_REG(0x49)
#define BP_REG_EFB_COORDS_MAX		BP_REG(0x4a)
#define BP_REG_XFB_ADDR			BP_REG(0x4b)
#define BP_REG_COPY_CLEAR_AR		BP_REG(0x4f)
#define BP_REG_COPY_CLEAR_GB		BP_REG(0x50)
#define BP_REG_COPY_CLEAR_Z		BP_REG(0x51)
#define BP_REG_PE_COPY_EXECUTE		BP_REG(0x52)
#define  PE_COPY_EXECUTE_TO_XFB		BIT(14)
#define  PE_COPY_EXECUTE_YSCALE		BIT(10)
#define  PE_COPY_EXECUTE_CLAMP		(2 << 0)
#define  PE_DONE_TRIGGER		2

#define GX_GEN_NUM_TEXGENS(n)		((n) & 0xf)
#define GX_GEN_NUM_CHANS(n)		(((n) & 0x7) << 4)
#define GX_GEN_NUM_INDSTAGES(n)		(((n) & 0x7) << 16)

#define GX_CP_MATRIX_INDEX_A		0x30
#define GX_CP_PERF_MODE			0x20
#define GX_CP_VCD_LO			0x50
#define GX_CP_VCD_HI			0x60
#define GX_CP_VAT_A(vtxfmt)		(0x70 + (vtxfmt))
#define GX_CP_VAT_B(vtxfmt)		(0x80 + (vtxfmt))
#define GX_CP_VAT_C(vtxfmt)		(0x90 + (vtxfmt))

#define GX_XF_POS_MTX0			0x0000
#define GX_XF_TEX_MTX(index)		((index) << 2)
#define GX_XF_DUAL_TEX_MTX(index)	(0x0500 + ((index) << 2))
#define GX_XF_ERROR			0x1000
#define GX_XF_CLIP_DISABLE		0x1005
#define GX_XF_GP_METRIC			0x1006
#define GX_XF_INVTXSPEC			0x1008
#define GX_XF_NUMCOLORS			0x1009
#define GX_XF_AMBIENT0			0x100a
#define GX_XF_MATERIAL0			0x100c
#define GX_XF_COLOR0CNTRL		0x100e
#define GX_XF_ALPHA0CNTRL		0x1010
#define GX_XF_DUALTEX			0x1012
#define GX_XF_MATRIX_INDEX_A		0x1018
#define GX_XF_VIEWPORT			0x101a
#define GX_XF_PROJECTION		0x1020
#define GX_XF_NUMTEXGENS		0x103f
#define GX_XF_TEXGEN(index)		(0x1040 + (index))
#define GX_XF_DUAL_TEXGEN(index)	(0x1050 + (index))

#define GX_TEXMTX0			30
#define GX_TEXMTX1			33
#define GX_DTTIDENTITY			61
#define GX_TEXGEN_SRC_TEX0		(5 << 7)
#define GX_TEX_FORMAT_RGB565		4
#define GX_TEX_FORMAT_RGBA8		6
#define GX_TEX_DISABLE_EDGE_LOD		BIT(8)
#define GX_TEX_WRAP_REPEAT_S		BIT(0)
#define GX_TEX_WRAP_REPEAT_T		BIT(2)
#define GX_TEX_IMAGE_SIZE(fmt, width, height) \
	(((fmt) << 20) | (((height) - 1) << 10) | ((width) - 1))
#define GX_TEX_IMAGE_ADDR(addr)		((u32)(addr) >> 5)
#define GX_SU_RANGE_BIAS		BIT(16)

#define GX_TMEM_MAP0_EVEN		0x0d8000
#define GX_TMEM_MAP0_ODD		0x0dc000
#define GX_TMEM_MAP1_RGBA8_EVEN		0x0d8800
#define GX_TMEM_MAP1_RGBA8_ODD		0x0dc800

#define GX_TEV_ORDER_TEXCOORD0_MAP0	BIT(6)
#define GX_TEV_REPLACE_COLOR		0x08fff8
#define GX_TEV_REPLACE_ALPHA		0x08ffc0
#define GX_IND_ORDER_TEXCOORD1_MAP1	0x000009
#define GX_TEV_INDIRECT_RGB565		0x000270
#define GX_IND_MTX_A_640		0x400140
#define GX_IND_MTX_A_672		0x400150
#define GX_IND_MTX_B			0x800001
#define GX_IND_MTX_C			0x402000
#define GX_ALPHA_COMPARE_ALWAYS		0x3f0000
#define GX_INVALIDATE_TEX_ALL_0		0x001000
#define GX_INVALIDATE_TEX_ALL_1		0x001100

#define GX_VCD_POS_XY_DIRECT		BIT(9)
#define GX_VCD_TEX0_ST_DIRECT		1
#define GX_VAT_POS_XY_TEX0_ST_F32	0x41200008
#define GX_VAT_B_DEFAULT		0x80000000
#define GX_VAT_A_DEFAULT		0x40000000
#define GX_XF_ONE_TEXCOORD		BIT(4)
#define GX_CHANNEL_CONTROL_DISABLED	0x00000401

#define GX_FLOAT_ONE			0x3f800000
#define GX_FLOAT_NEG_ONE		0xbf800000
#define GX_FLOAT_QUARTER		0x3e800000
#define GX_FLOAT_NEG_FIVE		0xc0a00000
#define GX_FLOAT_447			0x43df8000
#define GX_FLOAT_573			0x440f4000
#define GX_FLOAT_639			0x441fc000
#define GX_FLOAT_671			0x4427c000

#define CP_SR				0x00
#define  CP_SR_IDLE_CMDS			0x0008
#define  CP_SR_IDLE_READ			0x0004
#define CP_CR				0x02
#define  CP_CR_READ_ENABLE		0x0001
#define  CP_CR_GP_LINK_ENABLE		0x0010
#define CP_CLEAR			0x04
#define  CP_CLEAR_OVERFLOW		0x0001
#define  CP_CLEAR_UNDERFLOW		0x0002
#define CP_FIFO_BASE_LO			0x20
#define CP_FIFO_BASE_HI			0x22
#define CP_FIFO_END_LO			0x24
#define CP_FIFO_END_HI			0x26
#define CP_FIFO_HIWAT_LO			0x28
#define CP_FIFO_HIWAT_HI			0x2a
#define CP_FIFO_LOWAT_LO			0x2c
#define CP_FIFO_LOWAT_HI			0x2e
#define CP_FIFO_RW_DIST_LO		0x30
#define CP_FIFO_RW_DIST_HI		0x32
#define CP_FIFO_WRITE_PTR_LO		0x34
#define CP_FIFO_WRITE_PTR_HI		0x36
#define CP_FIFO_READ_PTR_LO		0x38
#define CP_FIFO_READ_PTR_HI		0x3a
#define PE_ZCONF			0x00
#define  PE_ZCONF_UPD_ENABLE		0x0010
#define  PE_ZCONF_FUNC_ALWAYS		0x000e
#define  PE_ZCONF_COMP_ENABLE		0x0001
#define PE_ALPHA_CONF			0x02
#define  PE_ALPHA_CONF_OP_SET		0xf000
#define  PE_ALPHA_CONF_SRC_1		0x0100
#define  PE_ALPHA_CONF_UPD_A		0x0010
#define  PE_ALPHA_CONF_UPD_C		0x0008
#define PE_ALPHA_DEST			0x04
#define PE_ALPHA_MODE			0x06
#define  PE_ALPHA_MODE_ALWAYS		0x0700
#define PE_ALPHA_READ			0x08
#define  PE_ALPHA_READ_UNK		0x0004
#define  PE_ALPHA_READ_FF		0x0001
#define PE_ISR				0x0a
#define  PE_ISR_FINISH			0x0008
#define  PE_ISR_FINISH_ENABLE		0x0002
#define PI_INTSR			0x00
#define PI_FIFO_BASE_START		0x0c
#define PI_FIFO_BASE_END		0x10
#define PI_FIFO_WRITE_PTR		0x14

#define GX_FIFO_SIZE			4096
#define GX_FIFO_HIWAT(size)		((size) - 32)
#define GX_FIFO_LOWAT(size)		(((size) >> 1) & ~0x1f)
#define GX_COPY_TIMEOUT_US		5000
#define GX_POLL_INTERVAL_US		50

static void gx_ppcsync(void)
{
	unsigned long flags;
	u32 hid0;

	/*
	 * HID0_ABE is only temporary.  In process context a decrementer IRQ
	 * must not run with a foreign address-broadcast/coherency mode between
	 * these two HID0 writes.
	 */
	local_irq_save(flags);
	hid0 = mfspr(SPRN_HID0);
	mtspr(SPRN_HID0, hid0 | HID0_ABE);
	asm volatile("isync" : : : "memory");
	asm volatile("sync" : : : "memory");
	mtspr(SPRN_HID0, hid0);
	asm volatile("isync" : : : "memory");
	local_irq_restore(flags);
}

static void gx_set_wgpipe(phys_addr_t base)
{
	u32 value;

	if (base)
		mtspr(SPRN_WPAR_GEKKO, (u32)base);

	value = mfspr(SPRN_HID2_GEKKO);
	if (base)
		value |= HID2_WPE;
	else
		value &= ~HID2_WPE;
	mtspr(SPRN_HID2_GEKKO, value);
	asm volatile("isync" : : : "memory");
}

/*
 * Every GX command must be one uninterrupted write-gather stream.  The normal
 * PowerPC MMIO accessors issue ordering operations around individual stores,
 * which can split variable-width FIFO packets.  libogc uses plain volatile
 * native-endian stores for this aperture.
 */
static inline void gx_fifo_write8(struct vi_ctl *ctl, u8 value)
{
	asm volatile("stb %0,0(%1)" : : "r"(value), "b"(ctl->wgpipe) :
		     "memory");
}

static inline void gx_fifo_write16(struct vi_ctl *ctl, u16 value)
{
	asm volatile("sth %0,0(%1)" : : "r"(value), "b"(ctl->wgpipe) :
		     "memory");
}

static inline void gx_fifo_write32(struct vi_ctl *ctl, u32 value)
{
	asm volatile("stw %0,0(%1)" : : "r"(value), "b"(ctl->wgpipe) :
		     "memory");
}

static void gx_bp_set_reg(struct vi_ctl *ctl, u32 data)
{
	gx_fifo_write8(ctl, GX_CMD_LOAD_BP);
	gx_fifo_write32(ctl, data);
}

static void gx_cp_load(struct vi_ctl *ctl, u8 addr, u32 data)
{
	gx_fifo_write8(ctl, GX_CMD_LOAD_CP);
	gx_fifo_write8(ctl, addr);
	gx_fifo_write32(ctl, data);
}

static void gx_xf_load(struct vi_ctl *ctl, u16 addr, u32 data)
{
	gx_fifo_write8(ctl, GX_CMD_LOAD_XF);
	gx_fifo_write32(ctl, addr);
	gx_fifo_write32(ctl, data);
}

static void gx_xf_load_multi(struct vi_ctl *ctl, u16 addr, u16 count,
			     const u32 *data)
{
	u16 n;

	gx_fifo_write8(ctl, GX_CMD_LOAD_XF);
	gx_fifo_write32(ctl, ((u32)(count - 1) << 16) | addr);
	for (n = 0; n < count; n++)
		gx_fifo_write32(ctl, data[n]);
}

static void gx_init_indirect_map(struct vi_ctl *ctl)
{
	u32 linear[GX_INDIRECT_WIDTH * GX_INDIRECT_HEIGHT];
	u16 *out = ctl->indirect_map;
	unsigned int bx, by, x, y;

	for (y = 0; y < GX_INDIRECT_HEIGHT; y++) {
		for (x = 0; x < GX_INDIRECT_WIDTH; x++) {
			unsigned int k = x >> 2;
			unsigned int j = x & 3;
			u8 s = 2 * y + 128;
			u8 t = -4 * k + 3 * j + 128;
			u8 u = k - y + 128;

			/* RGBA bytes; GX consumes A/B/G as indirect S/T/U. */
			linear[y * GX_INDIRECT_WIDTH + x] =
				((u32)u << 16) | ((u32)t << 8) | s;
		}
	}

	/* Convert once to GX's 4x4, split AR/GB RGBA8 texture layout. */
	for (by = 0; by < GX_INDIRECT_HEIGHT; by += 4) {
		for (bx = 0; bx < GX_INDIRECT_WIDTH; bx += 4) {
			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					u32 p = linear[(by + y) *
						       GX_INDIRECT_WIDTH + bx + x];

					*out++ = cpu_to_be16(((u16)(u8)p << 8) |
							    (u8)(p >> 24));
				}
			}
			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					u32 p = linear[(by + y) *
						       GX_INDIRECT_WIDTH + bx + x];

					*out++ = cpu_to_be16((u16)(u8)(p >> 16)
							    << 8 |
							    (u8)(p >> 8));
				}
			}
		}
	}
}

static void gx_set_viewport(struct vi_ctl *ctl);
static void gx_set_scissor(struct vi_ctl *ctl, u32 width, u32 height);

static void gx_setup_indirect_renderer(struct vi_ctl *ctl)
{
	u32 width = ctl->mode->width;
	u32 height = ctl->mode->height;
	const u32 fb_tex_mtx[] = {
		GX_FLOAT_QUARTER, 0, 0, 0,	/* S = TEX0.s / 4 */
		0, GX_FLOAT_ONE, 0, 0,	/* T = TEX0.t */
	};
	const u32 ind_coord_mtx[] = {
		width == GX_WIDE_EFB_WIDTH ? 0x42280000 : 0x42200000, 0, 0, 0,
		0, height == GX_TALL_EFB_HEIGHT ? 0x430f8000 : 0x42e00000, 0, 0,
	};
	const u32 projection[] = {
		width == GX_WIDE_EFB_WIDTH ? 0x3b43569b : 0x3b4d1ed9,
		GX_FLOAT_NEG_ONE,
		height == GX_TALL_EFB_HEIGHT ? 0xbb64bf38 : 0xbb929cec,
		GX_FLOAT_ONE,
		0xbb5a740e,
		GX_FLOAT_NEG_ONE,
		1,	/* GX_ORTHOGRAPHIC */
	};
	const u32 position_mtx[] = {
		GX_FLOAT_ONE, 0, 0, 0,
		0, GX_FLOAT_ONE, 0, 0,
		0, 0, GX_FLOAT_ONE, GX_FLOAT_NEG_FIVE,
	};
	const u32 dual_tex_identity[] = {
		GX_FLOAT_ONE, 0, 0, 0,
		0, GX_FLOAT_ONE, 0, 0,
		0, 0, GX_FLOAT_ONE, 0,
	};
	const u32 gen_mode = GX_GEN_NUM_TEXGENS(2) | GX_GEN_NUM_CHANS(1) |
			     GX_GEN_NUM_INDSTAGES(1);
	const u32 matrix_index = (GX_TEXMTX0 << 6) | (GX_TEXMTX1 << 12);

	/*
	 * GX starts from undefined state on this boot chain.  Match GX_Init's
	 * default GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	 * otherwise PE may discard every rasterized texture fragment.
	 */
	gx_bp_set_reg(ctl, BP_REG_ALPHA_COMPARE |
		      GX_ALPHA_COMPARE_ALWAYS);

	/* Direct XY float position and direct ST float texture coordinate. */
	gx_cp_load(ctl, GX_CP_VCD_LO, GX_VCD_POS_XY_DIRECT);
	gx_cp_load(ctl, GX_CP_VCD_HI, GX_VCD_TEX0_ST_DIRECT);
	gx_cp_load(ctl, GX_CP_VAT_A(0), GX_VAT_POS_XY_TEX0_ST_F32);
	gx_cp_load(ctl, GX_CP_VAT_B(0), GX_VAT_B_DEFAULT);
	gx_cp_load(ctl, GX_CP_VAT_C(0), 0);
	gx_xf_load(ctl, GX_XF_INVTXSPEC, GX_XF_ONE_TEXCOORD);

	/* TEX0 drives two differently-scaled generated coordinates. */
	gx_cp_load(ctl, GX_CP_MATRIX_INDEX_A, matrix_index);
	gx_xf_load(ctl, GX_XF_MATRIX_INDEX_A, matrix_index);
	gx_xf_load_multi(ctl, GX_XF_POS_MTX0, ARRAY_SIZE(position_mtx),
			 position_mtx);
	gx_xf_load(ctl, GX_XF_NUMTEXGENS, 2);
	/*
	 * Match GX_SetNumChans(1) in the working libogc program.  TEX0 is
	 * replaced by TEV, but the stage is still ordered against COLOR0A0.
	 */
	gx_xf_load(ctl, GX_XF_NUMCOLORS, 1);
	gx_xf_load(ctl, GX_XF_AMBIENT0, 0);
	gx_xf_load(ctl, GX_XF_MATERIAL0, 0xffffffff);
	gx_xf_load(ctl, GX_XF_COLOR0CNTRL, GX_CHANNEL_CONTROL_DISABLED);
	gx_xf_load(ctl, GX_XF_ALPHA0CNTRL, GX_CHANNEL_CONTROL_DISABLED);
	gx_xf_load(ctl, GX_XF_TEXGEN(0), GX_TEXGEN_SRC_TEX0);
	gx_xf_load(ctl, GX_XF_DUAL_TEXGEN(0), GX_DTTIDENTITY);
	gx_xf_load(ctl, GX_XF_TEXGEN(1), GX_TEXGEN_SRC_TEX0);
	gx_xf_load(ctl, GX_XF_DUAL_TEXGEN(1), GX_DTTIDENTITY);
	/*
	 * GX_SetTexCoordGen() selects GX_DTTIDENTITY as the dual/post
	 * transform.  GX_Init() uploads that 3x4 identity matrix at XF 0x5f4;
	 * unlike the primary texture matrices, it is not hardware reset state.
	 * Leaving it undefined corrupts both generated texture coordinates.
	 */
	gx_xf_load_multi(ctl, GX_XF_DUAL_TEX_MTX(GX_DTTIDENTITY),
			 ARRAY_SIZE(dual_tex_identity),
			 dual_tex_identity);
	gx_xf_load_multi(ctl, GX_XF_TEX_MTX(GX_TEXMTX0),
			 ARRAY_SIZE(fb_tex_mtx),
			 fb_tex_mtx);
	gx_xf_load_multi(ctl, GX_XF_TEX_MTX(GX_TEXMTX1),
			 ARRAY_SIZE(ind_coord_mtx),
			 ind_coord_mtx);
	gx_xf_load_multi(ctl, GX_XF_PROJECTION, ARRAY_SIZE(projection),
			 projection);

	/* Linear RGB565 framebuffer on map 0. */
	gx_bp_set_reg(ctl, BP_REG_TEX_MODE0(0) | GX_TEX_DISABLE_EDGE_LOD);
	gx_bp_set_reg(ctl, BP_REG_TEX_MODE1(0));
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE0(0) |
		      GX_TEX_IMAGE_SIZE(GX_TEX_FORMAT_RGB565, width, height));
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE1(0) | GX_TMEM_MAP0_EVEN);
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE2(0) | GX_TMEM_MAP0_ODD);
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE3(0) |
		      GX_TEX_IMAGE_ADDR(ctl->rgb_fb_dma));

	/* Repeating 16x4 RGBA8 indirect map on map 1. */
	gx_bp_set_reg(ctl, BP_REG_TEX_MODE0(1) | GX_TEX_DISABLE_EDGE_LOD |
		      GX_TEX_WRAP_REPEAT_S | GX_TEX_WRAP_REPEAT_T);
	gx_bp_set_reg(ctl, BP_REG_TEX_MODE1(1));
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE0(1) |
		      GX_TEX_IMAGE_SIZE(GX_TEX_FORMAT_RGBA8,
					GX_INDIRECT_WIDTH, GX_INDIRECT_HEIGHT));
	/*
	 * Wii libogc assigns ordinary (non-CI/CMPR) map 1 to texture region
	 * 9: even TMEM at 0x10000 and odd TMEM at 0x90000.  RGBA8 consumes
	 * both planes, so using the generic map-1 region here is not equivalent.
	 */
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE1(1) |
		      GX_TMEM_MAP1_RGBA8_EVEN);
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE2(1) |
		      GX_TMEM_MAP1_RGBA8_ODD);
	gx_bp_set_reg(ctl, BP_REG_TEX_IMAGE3(1) |
		      GX_TEX_IMAGE_ADDR(ctl->indirect_map_dma));

	/* Texture extents/wrap for generated coordinate 0 and 1. */
	gx_bp_set_reg(ctl, BP_REG_SU_SSIZE(0) | (width - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_TSIZE(0) | (height - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_SSIZE(1) | GX_SU_RANGE_BIAS |
		      (GX_INDIRECT_WIDTH - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_TSIZE(1) | GX_SU_RANGE_BIAS |
		      (GX_INDIRECT_HEIGHT - 1));

	/* One-stage replace TEV using the indirect framebuffer correction. */
	gx_bp_set_reg(ctl, BP_REG_GEN_MODE | gen_mode);
	gx_bp_set_reg(ctl, BP_REG_TEV_ORDER |
		      GX_TEV_ORDER_TEXCOORD0_MAP0);
	/*
	 * GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE): A/B/C must explicitly use
	 * the ZERO selectors; their zero-valued encodings select CPREV/APREV.
	 */
	gx_bp_set_reg(ctl, BP_REG_TEV_COLOR_ENV(0) |
		      GX_TEV_REPLACE_COLOR);
	gx_bp_set_reg(ctl, BP_REG_TEV_ALPHA_ENV(0) |
		      GX_TEV_REPLACE_ALPHA);
	gx_bp_set_reg(ctl, BP_REG_IND_IMASK);
	gx_bp_set_reg(ctl, BP_REG_IND_ORDER |
		      GX_IND_ORDER_TEXCOORD1_MAP1);
	gx_bp_set_reg(ctl, BP_REG_IND_SCALE);
	gx_bp_set_reg(ctl, BP_REG_IND_MTX_A |
		      (width == GX_WIDE_EFB_WIDTH ? GX_IND_MTX_A_672 :
		       GX_IND_MTX_A_640));
	gx_bp_set_reg(ctl, BP_REG_IND_MTX_B | GX_IND_MTX_B);
	gx_bp_set_reg(ctl, BP_REG_IND_MTX_C | GX_IND_MTX_C);
	gx_bp_set_reg(ctl, BP_REG_TEV_INDIRECT(0) |
		      GX_TEV_INDIRECT_RGB565);

	/*
	 * Re-emit SU state after the final texture/coordinate association.
	 * This matches the point at which libogc's dirty-state flush derives
	 * these registers from GX_SetTevOrder()/GX_SetIndTexOrder().
	 */
	gx_bp_set_reg(ctl, BP_REG_SU_SSIZE(0) | (width - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_TSIZE(0) | (height - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_SSIZE(1) | GX_SU_RANGE_BIAS |
		      (GX_INDIRECT_WIDTH - 1));
	gx_bp_set_reg(ctl, BP_REG_SU_TSIZE(1) | GX_SU_RANGE_BIAS |
		      (GX_INDIRECT_HEIGHT - 1));
}

static void gx_draw_framebuffer(struct vi_ctl *ctl)
{
	u32 width = ctl->mode->width == GX_WIDE_EFB_WIDTH ?
		    GX_FLOAT_671 : GX_FLOAT_639;
	u32 height = ctl->mode->height == GX_TALL_EFB_HEIGHT ?
		     GX_FLOAT_573 : GX_FLOAT_447;

	/*
	 * Match GX_InvalidateTexAll().  The RGB565 framebuffer is CPU-written,
	 * so GX must not reuse texture-cache lines from the preceding frame.
	 */
	gx_bp_set_reg(ctl, BP_REG_IND_IMASK);
	gx_bp_set_reg(ctl, BP_REG_TEXTURE_INVALIDATE |
		      GX_INVALIDATE_TEX_ALL_0);
	gx_bp_set_reg(ctl, BP_REG_TEXTURE_INVALIDATE |
		      GX_INVALIDATE_TEX_ALL_1);
	gx_bp_set_reg(ctl, BP_REG_IND_IMASK);
	gx_fifo_write8(ctl, GX_CMD_INVALIDATE_VTX);

	/* XY/F32 position followed by ST/F32, exactly as declared by VCD/VAT0. */
	gx_fifo_write8(ctl, GX_CMD_QUADS_VTXFMT0);
	gx_fifo_write16(ctl, 4);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, width);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, GX_FLOAT_ONE);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, width);
	gx_fifo_write32(ctl, height);
	gx_fifo_write32(ctl, GX_FLOAT_ONE);
	gx_fifo_write32(ctl, GX_FLOAT_ONE);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, height);
	gx_fifo_write32(ctl, 0);
	gx_fifo_write32(ctl, GX_FLOAT_ONE);
}

static void gx_set_viewport(struct vi_ctl *ctl)
{
	u32 data[] = {
		ctl->mode->width == GX_WIDE_EFB_WIDTH ? 0x43a80000 : 0x43a00000,
		ctl->mode->height == GX_TALL_EFB_HEIGHT ? 0xc38f8000 : 0xc3600000,
		0x4b7fffff,
		ctl->mode->width == GX_WIDE_EFB_WIDTH ? 0x44298000 : 0x44258000,
		ctl->mode->height == GX_TALL_EFB_HEIGHT ? 0x441d4000 : 0x440d8000,
		0x4b7fffff,
	};

	gx_xf_load_multi(ctl, GX_XF_VIEWPORT, ARRAY_SIZE(data), data);
}

static void gx_set_scissor(struct vi_ctl *ctl, u32 width, u32 height)
{
	u32 xo = 0x156;
	u32 yo = 0x156;

	gx_bp_set_reg(ctl, BP_REG_SCISSOR_TL | yo | (xo << 12));
	gx_bp_set_reg(ctl, BP_REG_SCISSOR_BR |
		      (yo + height - 1) | ((xo + width - 1) << 12));
	gx_bp_set_reg(ctl, BP_REG_SCISSOR_OFFSET |
		      GX_COORDS(xo >> 1, yo >> 1));
}

static void gx_flush(struct vi_ctl *ctl)
{
	int n;

	/* A CP read orders preceding writes before the gather-pipe flush. */
	in_be16(ctl->cp_base + CP_CR);
	for (n = 0; n < 8; n++)
		gx_fifo_write32(ctl, 0);
	gx_ppcsync();
}

static int gx_clear_pe_finish(struct vi_ctl *ctl)
{
	u32 cause;
	u16 status;
	int error;

	out_be16(ctl->pe_base + PE_ISR,
		 PE_ISR_FINISH_ENABLE | PE_ISR_FINISH);
	gx_ppcsync();
	error = read_poll_timeout(in_be16, status,
				  !(status & PE_ISR_FINISH),
				  GX_POLL_INTERVAL_US, GX_COPY_TIMEOUT_US,
				  false, ctl->pe_base + PE_ISR);
	if (error)
		goto timeout;

	/* Do not mistake a stale cascaded PE cause for the next copy. */
	error = read_poll_timeout(in_be32, cause, !(cause & BIT(10)),
				  GX_POLL_INTERVAL_US, GX_COPY_TIMEOUT_US,
				  false, ctl->pi_base + PI_INTSR);
	if (!error)
		return 0;

timeout:
	dev_err(ctl->dev,
		"could not clear PE finish: PI=%08x PE=%04x\n",
		in_be32(ctl->pi_base + PI_INTSR),
		in_be16(ctl->pe_base + PE_ISR));
	return error;
}

static int gx_arm_pe_finish(struct vi_ctl *ctl)
{
	int error;

	error = gx_clear_pe_finish(ctl);
	if (!error) {
		/*
		 * The stale IRQ may have acknowledged its PI cause before it
		 * publishes completion.  Drain that handler before resetting
		 * the completion for the command about to be submitted.
		 */
		synchronize_irq(ctl->pe_irq);
		reinit_completion(&ctl->pe_finished);
	}
	return error;
}

static int gx_wait_pe_finish(struct vi_ctl *ctl, const char *phase)
{
	unsigned long timeout;
	int error = 0;

	/*
	 * Sleep until the dedicated PE-finish IRQ publishes completion.  Keep
	 * the wait bounded: if GX has wedged, continuing to append frames only
	 * overwrites the FIFO and obscures the first fault.
	 */
	timeout = usecs_to_jiffies(GX_COPY_TIMEOUT_US);
	timeout = wait_for_completion_timeout(&ctl->pe_finished, timeout);
	if (!timeout)
		error = -ETIMEDOUT;

	if (error)
		dev_crit(ctl->dev,
			"PE %s timeout: PI=%08x PE=%04x CP=%04x dist=%04x%04x write=%04x%04x read=%04x%04x\n",
			phase,
			in_be32(ctl->pi_base + PI_INTSR),
			in_be16(ctl->pe_base + PE_ISR),
			in_be16(ctl->cp_base + CP_SR),
			in_be16(ctl->cp_base + CP_FIFO_RW_DIST_HI),
			in_be16(ctl->cp_base + CP_FIFO_RW_DIST_LO),
			in_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_HI),
			in_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_LO),
			in_be16(ctl->cp_base + CP_FIFO_READ_PTR_HI),
			in_be16(ctl->cp_base + CP_FIFO_READ_PTR_LO));

	return error;
}

static int gx_wait_cp_idle(struct vi_ctl *ctl)
{
	u16 status;
	int error;

	error = read_poll_timeout(in_be16, status,
				  (status & (CP_SR_IDLE_CMDS |
					     CP_SR_IDLE_READ)) ==
				  (CP_SR_IDLE_CMDS | CP_SR_IDLE_READ),
				  GX_POLL_INTERVAL_US, GX_COPY_TIMEOUT_US, false,
				  ctl->cp_base + CP_SR);
	if (error) {
		dev_err(ctl->dev,
			"GX CP did not become idle: SR=%#x distance=%04x%04x write=%04x%04x read=%04x%04x\n",
			status,
			in_be16(ctl->cp_base + CP_FIFO_RW_DIST_HI),
			in_be16(ctl->cp_base + CP_FIFO_RW_DIST_LO),
			in_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_HI),
			in_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_LO),
			in_be16(ctl->cp_base + CP_FIFO_READ_PTR_HI),
			in_be16(ctl->cp_base + CP_FIFO_READ_PTR_LO));
		return error;
	}

	return 0;
}

static int gx_init(struct vi_ctl *ctl)
{
	u32 fifo_base, fifo_end, fifo_hiwat, fifo_lowat;
	u32 bus_clock = 243000000;
	u32 clock_div;
	u16 tmp;
	int error, n;

	dma_coerce_mask_and_coherent(ctl->dev, DMA_BIT_MASK(32));
	ctl->fifo = dma_alloc_noncoherent(ctl->dev, GX_FIFO_SIZE,
					  &ctl->fifo_dma, DMA_BIDIRECTIONAL,
					  GFP_KERNEL);
	if (!ctl->fifo)
		return -ENOMEM;

	fifo_base = (u32)ctl->fifo_dma;
	fifo_end = fifo_base + GX_FIFO_SIZE - 4;
	fifo_hiwat = GX_FIFO_HIWAT(GX_FIFO_SIZE);
	fifo_lowat = GX_FIFO_LOWAT(GX_FIFO_SIZE);
	gx_set_wgpipe(0);
	out_be16(ctl->cp_base + CP_CR, 0);
	out_be16(ctl->cp_base + CP_CLEAR,
		 CP_CLEAR_UNDERFLOW | CP_CLEAR_OVERFLOW);
	out_be16(ctl->cp_base + CP_FIFO_BASE_LO, fifo_base);
	out_be16(ctl->cp_base + CP_FIFO_BASE_HI, fifo_base >> 16);
	out_be16(ctl->cp_base + CP_FIFO_END_LO, fifo_end);
	out_be16(ctl->cp_base + CP_FIFO_END_HI, fifo_end >> 16);
	out_be16(ctl->cp_base + CP_FIFO_HIWAT_LO, fifo_hiwat);
	out_be16(ctl->cp_base + CP_FIFO_HIWAT_HI, fifo_hiwat >> 16);
	out_be16(ctl->cp_base + CP_FIFO_LOWAT_LO, fifo_lowat);
	out_be16(ctl->cp_base + CP_FIFO_LOWAT_HI, fifo_lowat >> 16);
	out_be16(ctl->cp_base + CP_FIFO_RW_DIST_LO, 0);
	out_be16(ctl->cp_base + CP_FIFO_RW_DIST_HI, 0);
	out_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_LO, fifo_base);
	out_be16(ctl->cp_base + CP_FIFO_WRITE_PTR_HI, fifo_base >> 16);
	out_be16(ctl->cp_base + CP_FIFO_READ_PTR_LO, fifo_base);
	out_be16(ctl->cp_base + CP_FIFO_READ_PTR_HI, fifo_base >> 16);
	gx_ppcsync();

	out_be32(ctl->pi_base + PI_FIFO_BASE_START, fifo_base);
	out_be32(ctl->pi_base + PI_FIFO_BASE_END, fifo_end);
	out_be32(ctl->pi_base + PI_FIFO_WRITE_PTR, fifo_base);
	gx_ppcsync();

	out_be16(ctl->cp_base + CP_CR, CP_CR_GP_LINK_ENABLE);
	tmp = in_be16(ctl->cp_base + CP_CR);
	out_be16(ctl->cp_base + CP_CR, tmp | CP_CR_READ_ENABLE);

	out_be16(ctl->pe_base + PE_ZCONF,
		 PE_ZCONF_UPD_ENABLE | PE_ZCONF_FUNC_ALWAYS |
		 PE_ZCONF_COMP_ENABLE);
	out_be16(ctl->pe_base + PE_ALPHA_CONF,
		 PE_ALPHA_CONF_OP_SET | PE_ALPHA_CONF_SRC_1 |
		 PE_ALPHA_CONF_UPD_A | PE_ALPHA_CONF_UPD_C);
	out_be16(ctl->pe_base + PE_ALPHA_DEST, 0);
	out_be16(ctl->pe_base + PE_ALPHA_MODE, PE_ALPHA_MODE_ALWAYS);
	out_be16(ctl->pe_base + PE_ALPHA_READ,
		 PE_ALPHA_READ_UNK | PE_ALPHA_READ_FF);

	gx_set_wgpipe(ctl->gx_fifo_base);
	for (n = 0; n < 8; n++) {
		gx_cp_load(ctl, GX_CP_VAT_A(n), GX_VAT_A_DEFAULT);
		gx_cp_load(ctl, GX_CP_VAT_B(n), GX_VAT_B_DEFAULT);
		gx_cp_load(ctl, GX_CP_VAT_C(n), 0);
	}
	gx_cp_load(ctl, GX_CP_PERF_MODE, 0);
	gx_xf_load(ctl, GX_XF_ERROR, 0x3f);
	gx_xf_load(ctl, GX_XF_CLIP_DISABLE, 0); /* clipping enabled */
	gx_xf_load(ctl, GX_XF_DUALTEX, 1);
	gx_xf_load(ctl, GX_XF_GP_METRIC, 0);

	gx_bp_set_reg(ctl, BP_REG_GEN_MODE | GX_GEN_NUM_TEXGENS(1));
	gx_bp_set_reg(ctl, BP_REG_DISPLAY_COPY_FILTER(0) | 0x666666);
	gx_bp_set_reg(ctl, BP_REG_DISPLAY_COPY_FILTER(1) | 0x666666);
	gx_bp_set_reg(ctl, BP_REG_DISPLAY_COPY_FILTER(2) | 0x666666);
	gx_bp_set_reg(ctl, BP_REG_DISPLAY_COPY_FILTER(3) | 0x666666);
	gx_bp_set_reg(ctl, BP_REG_LINE_POINT_WIDTH | 0x000606);
	gx_bp_set_reg(ctl, BP_REG_PERF0_TRI);
	gx_bp_set_reg(ctl, BP_REG_PERF0_QUAD);
	/*
	 * GX_Init() derives these two pipeline clock values from the external
	 * bus clock.  Do not inherit them from the boot environment: CP can
	 * consume commands while an incorrectly clocked raster pipeline fails
	 * to produce pixels.
	 */
	of_property_read_u32(ctl->dev->of_node->parent, "clock-frequency",
			     &bus_clock);
	clock_div = bus_clock / 500;
	gx_bp_set_reg(ctl, BP_REG_BUS_CLOCK1 |
		      (clock_div >> 11) | 0x0400);
	clock_div /= 4224;
	gx_bp_set_reg(ctl, BP_REG_BUS_CLOCK0 | clock_div | 0x0200);
	gx_bp_set_reg(ctl, BP_REG_CONSTANT_ALPHA);
	gx_bp_set_reg(ctl, BP_REG_FIELD_MASK | 3);
	gx_bp_set_reg(ctl, BP_REG_PE_CONTROL);
	gx_bp_set_reg(ctl, BP_REG_COPY_FILTER0 | 0x595000);
	gx_bp_set_reg(ctl, BP_REG_COPY_FILTER1 | 0x000015);
	gx_bp_set_reg(ctl, BP_REG_CLEAR_BBOX1 | 0x0003ff);
	gx_bp_set_reg(ctl, BP_REG_CLEAR_BBOX2 | 0x0003ff);
	gx_bp_set_reg(ctl, BP_REG_REVISION_BITS | 0x00000f);
	gx_bp_set_reg(ctl, BP_REG_PERF1);
	/* Opaque black copy-clear color and maximum Z. */
	gx_bp_set_reg(ctl, BP_REG_COPY_CLEAR_AR);
	gx_bp_set_reg(ctl, BP_REG_COPY_CLEAR_GB);
	gx_bp_set_reg(ctl, BP_REG_COPY_CLEAR_Z | 0xffffff);

	gx_set_viewport(ctl);
	gx_set_scissor(ctl, ctl->mode->width, ctl->mode->height);
	gx_bp_set_reg(ctl, BP_REG_ZMODE | 0x1f);
	if (ctl->mode->height == GX_TALL_EFB_HEIGHT)
		gx_bp_set_reg(ctl, BP_REG_COPY_Y_SCALE | 0x127);
	gx_bp_set_reg(ctl, BP_REG_BLEND_MODE | 0x4bc);
	gx_bp_set_reg(ctl, BP_REG_EFB_COORDS_MIN | GX_COORDS(0, 0));
	gx_bp_set_reg(ctl, BP_REG_EFB_COORDS_MAX |
		      GX_COORDS(ctl->mode->width - 1, ctl->mode->height - 1));
	gx_bp_set_reg(ctl, BP_REG_XFB_STRIDE | (ctl->mode->width >> 4));
	gx_bp_set_reg(ctl, BP_REG_XFB_ADDR |
		      GX_TEX_IMAGE_ADDR(gx_xfb_start));
	gx_setup_indirect_renderer(ctl);

	gx_flush(ctl);
	error = gx_wait_cp_idle(ctl);
	if (error)
		return error;
	return 0;
}

static void gx_copy_efb_to_xfb(struct vi_ctl *ctl)
{
	u32 copy_mask = ctl->mode->height == GX_TALL_EFB_HEIGHT ?
			PE_COPY_EXECUTE_YSCALE : 0;
	int error;

	if (READ_ONCE(ctl->gx_faulted))
		return;

	dma_sync_single_for_device(ctl->dev, ctl->rgb_fb_dma,
				   ctl->rgb_fb_size, DMA_TO_DEVICE);
	error = gx_arm_pe_finish(ctl);
	if (error)
		goto fault;
	gx_draw_framebuffer(ctl);
	gx_bp_set_reg(ctl, BP_REG_PE_DONE | PE_DONE_TRIGGER);
	gx_flush(ctl);
	error = gx_wait_pe_finish(ctl, "render");
	if (error)
		goto fault;
	error = gx_arm_pe_finish(ctl);
	if (error)
		goto fault;

	gx_bp_set_reg(ctl, BP_REG_PE_COPY_EXECUTE |
		      PE_COPY_EXECUTE_TO_XFB | PE_COPY_EXECUTE_CLAMP |
		      copy_mask);
	gx_bp_set_reg(ctl, BP_REG_PE_DONE | PE_DONE_TRIGGER);
	gx_flush(ctl);
	error = gx_wait_pe_finish(ctl, "copy");
	if (error)
		goto fault;
	dma_sync_single_for_cpu(ctl->dev, ctl->rgb_fb_dma,
				ctl->rgb_fb_size, DMA_TO_DEVICE);
	return;

fault:
	WRITE_ONCE(ctl->gx_faulted, true);
	dma_sync_single_for_cpu(ctl->dev, ctl->rgb_fb_dma,
				ctl->rgb_fb_size, DMA_TO_DEVICE);
	dev_crit(ctl->dev, "disabling GX submissions after completion failure\n");
}

static void vi_dispatch_vtrace(struct vi_ctl *ctl)
{
	unsigned long flags;

	spin_lock_irqsave(&ctl->lock, flags);
	if (ctl->flip_pending)
		vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	wake_up_interruptible(&ctl->vtrace_waitq);
}

static irqreturn_t vi_irq_handler(int irq, void *dev)
{
	struct fb_info *info = dev_get_drvdata((struct device *)dev);
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;
	u32 val;

	/* DI0 and DI1 are used to account for the vertical retrace */
	val = in_be32(io_base + VI_DI0);
	if (vi_dix_get_irq(val)) {
		ctl->in_vtrace = 0;

		out_be32(io_base + VI_DI0, vi_dix_clear_irq(val));
		return IRQ_HANDLED;
	}
	val = in_be32(io_base + VI_DI1);
	if (vi_dix_get_irq(val)) {
		ctl->in_vtrace = 1;

		out_be32(io_base + VI_DI1, vi_dix_clear_irq(val));
		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static irqreturn_t vi_irq_thread(int irq, void *dev)
{
	struct fb_info *info = dev_get_drvdata((struct device *)dev);
	struct vi_ctl *ctl = info->par;

	gx_copy_efb_to_xfb(ctl);
	vi_dispatch_vtrace(ctl);

	return IRQ_HANDLED;
}

static irqreturn_t pe_irq_handler(int irq, void *dev)
{
	struct fb_info *info = dev_get_drvdata((struct device *)dev);
	struct vi_ctl *ctl = info->par;

	/*
	 * IRQ 10 is the dedicated PE-finish interrupt.  PE_ISR_FINISH is not
	 * reliably observable as set by the time Broadway enters this handler;
	 * libogc and JoJo's driver therefore acknowledge the IRQ without using
	 * that bit as an additional qualification.
	 */
	out_be16(ctl->pe_base + PE_ISR,
		 PE_ISR_FINISH_ENABLE | PE_ISR_FINISH);
	complete(&ctl->pe_finished);
	return IRQ_HANDLED;
}

#ifdef CONFIG_WII_AVE_RVL

/*
 * Audio/Video Encoder hardware support.
 *
 */

/*
 * I/O accessors.
 */

static int vi_ave_outs(struct i2c_client *client, u8 reg,
		       void *data, size_t len)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msg[1];
	u8 buf[34];
	s32 result;
	int error = -EINVAL;

	if (len > sizeof(buf)-1)
		goto err_out;

	buf[0] = reg;
	memcpy(&buf[1], data, len);

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = len+1;
	msg[0].buf = buf;

	result = i2c_transfer(adap, msg, 1);
	if (result < 0)
		error = result;
	else if (result == 1) {
		/*
		 * The AVE needs a short interval to process a write before
		 * accepting the next transaction.
		 */
		udelay(2);
		error = 0;
	} else {
		error = -EIO;
	}

err_out:
	if (error)
		dev_err(&client->dev, "AVE-RVL: error (%d) writing to register %02Xh at client %02x\n",
			   error, reg, client->addr);
	return error;
}

static int vi_ave_out8(struct i2c_client *client, u8 reg, u8 data)
{
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_out16(struct i2c_client *client, u8 reg, u16 data)
{
	cpu_to_be16s(&data);
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_out32(struct i2c_client *client, u8 reg, u32 data)
{
	cpu_to_be32s(&data);
	return vi_ave_outs(client, reg, &data, sizeof(data));
}

static int vi_ave_ins(struct i2c_client *client, u8 reg,
		      void *data, size_t len)
{
	struct i2c_adapter *adap = client->adapter;
	struct i2c_msg msg[2];
	s32 result;
	int error;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = sizeof(reg);
	msg[0].buf = &reg;

	msg[1].addr = client->addr;
	msg[1].flags = (client->flags & I2C_M_TEN) | I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = data;

	result = i2c_transfer(adap, msg, 2);
	if (result < 0)
		error = result;
	else if (result == 2)
		error = 0;
	else
		error = -EIO;

	if (error)
		dev_err(&client->dev, "AVE-RVL: error (%d) reading from register %02Xh\n",
			   error, reg);

	return error;
}

static int vi_ave_in8(struct i2c_client *client, u8 reg, u8 *data)
{
	return vi_ave_ins(client, reg, data, sizeof(*data));
}


/*
 * Try to detect current video format.
 */
static int vi_ave_get_video_format(struct vi_ctl *ctl,
				   enum vi_video_format *fmt)
{
	int error;
	u8 val = 0xff;

	if (!ctl->i2c_client)
		return -ENODEV;

	error = vi_ave_in8(ctl->i2c_client, 0x01, &val);
	if (error)
		return -ENODEV;

	if ((val & 0x1f) == 2)
		*fmt = VI_FMT_PAL;
	else
		*fmt = VI_FMT_NTSC;

	return 0;
}


static u8 vi_ave_gamma[] = {
	0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00,
	0x10, 0x00, 0x10, 0x00, 0x10, 0x20, 0x40, 0x60,
	0x80, 0xa0, 0xeb, 0x10, 0x00, 0x20, 0x00, 0x40,
	0x00, 0x60, 0x00, 0x80, 0x00, 0xa0, 0x00, 0xeb,
	0x00
};

static struct vi_ctl *first_vi_ctl;
static struct i2c_client *first_vi_ave = NULL;

/*
 * Initialize the audio/video encoder.
 */
static int vi_ave_setup(struct vi_ctl *ctl)
{
	struct i2c_client *client;
	u8 macrovision[26];
	u8 component, format, pal60;
	int error;

#define ave_write(_call)			\
	do {					\
		error = (_call);		\
		if (error)			\
			return error;		\
	} while (0)

	client = ctl->i2c_client;
	if (!client && first_vi_ave)
		client = first_vi_ave;

	if (!client)
		return -ENODEV;

	memset(macrovision, 0, sizeof(macrovision));

	/*
	 * Magic initialization sequence borrowed from libogc.
	 */

	ave_write(vi_ave_out8(client, 0x6a, 1));
	ave_write(vi_ave_out8(client, 0x65, 1));

	/*
	 * NOTE
	 * We _can't use the fmt field in DCR to derive "format" here.
	 * DCR uses fmt=0 (NTSC) also for PAL 525 modes.
	 */

	format = 0;		/* default to NTSC */
	if ((ctl->mode->flags & VI_VMF_PAL_COLOR) != 0)
		format = 2;	/* PAL */
	component = (ctl->has_component_cable) ? 1<<5 : 0;
	ave_write(vi_ave_out8(client, 0x01, component | format));

	ave_write(vi_ave_out8(client, 0x00, 0));
	ave_write(vi_ave_out16(client, 0x71, 0x8e8e));
	ave_write(vi_ave_out8(client, 0x02, 7));
	ave_write(vi_ave_out16(client, 0x05, 0x0000));
	ave_write(vi_ave_out16(client, 0x08, 0x0000));
	ave_write(vi_ave_out32(client, 0x7a, 0x00000000));
	ave_write(vi_ave_outs(client, 0x40, macrovision,
			      sizeof(macrovision)));
	ave_write(vi_ave_out8(client, 0x0a, 0));
	ave_write(vi_ave_out8(client, 0x03, 1));
	ave_write(vi_ave_outs(client, 0x10, vi_ave_gamma,
			      sizeof(vi_ave_gamma)));
	ave_write(vi_ave_out8(client, 0x04, 1));

	ave_write(vi_ave_out32(client, 0x7a, 0x00000000));
	ave_write(vi_ave_out16(client, 0x08, 0x0000));

	ave_write(vi_ave_out8(client, 0x03, 1));

	/* clear bit 1 otherwise red and blue get swapped  */
	if (ctl->has_component_cable)
		ave_write(vi_ave_out8(client, 0x62, 0));

	/* PAL 480i/60 supposedly needs a "filter" */
	pal60 = !!(format == 2 && ctl->mode->lines == 525);
	ave_write(vi_ave_out8(client, 0x6e, pal60));

#undef ave_write
	return 0;
}

static int vi_attach_ave(struct vi_ctl *ctl, struct i2c_client *client)
{
	if (!ctl)
		return -ENODEV;
	if (!client)
		return -EINVAL;

	spin_lock(&ctl->lock);
	if (!ctl->i2c_client) {
		ctl->i2c_client = client;
		spin_unlock(&ctl->lock);
		dev_info(ctl->dev, "AVE-RVL support loaded\n");
		return 0;
	}
	spin_unlock(&ctl->lock);
	return -EBUSY;
}

static void vi_dettach_ave(struct vi_ctl *ctl)
{
	if (!ctl)
		return;

	spin_lock(&ctl->lock);
	if (ctl->i2c_client) {
		ctl->i2c_client = NULL;
		spin_unlock(&ctl->lock);
		dev_info(ctl->dev, "AVE-RVL support unloaded\n");
		return;
	}
	spin_unlock(&ctl->lock);
}

static int vi_ave_probe(struct i2c_client *client)
{
	int error;

	if (!first_vi_ctl)
		return -EPROBE_DEFER;

	if (first_vi_ave) {
		dev_dbg(&client->dev, "vi_ave_probe(): skipping further probes\n");
		return 0;
	}

	/* attach first a/v encoder to first framebuffer */
	error = vi_attach_ave(first_vi_ctl, client);
	if (error) {
		dev_err(&client->dev, "vi_ave_probe(): unable to attach AVE: error %d\n", error);
		return error;
	}

	first_vi_ave = client;
	error = vi_ave_setup(first_vi_ctl);
	if (error)
		goto err_detach;

	/* setup again the video mode using the a/v encoder */
	error = vi_setup_tv_mode(first_vi_ctl, true);
	if (error)
		goto err_detach;

	dev_info(&client->dev, "vi_ave_probe(): AVE attached successfully\n");
	return 0;

err_detach:
	first_vi_ave = NULL;
	vi_dettach_ave(first_vi_ctl);
	return error;
}

static void vi_ave_remove(struct i2c_client *client)
{
	if (first_vi_ave == client)
		first_vi_ave = NULL;

	if (first_vi_ctl && first_vi_ctl->i2c_client == client)
		first_vi_ctl->i2c_client = NULL;

	return;
}


static const struct of_device_id ave_of_match[] = {
	{ .compatible = "nintendo,wii-audio-video-encoder" },
	{  },
};

static struct i2c_driver vi_ave_driver = {
	.driver = {
		.name	= DRV_MODULE_NAME,
		.of_match_table = ave_of_match,
	},
	.probe		= vi_ave_probe,
	.remove		= vi_ave_remove,
};

#endif /* CONFIG_WII_AVE_RVL */


/*
 * Linux framebuffer support routines.
 *
 */

static int vifb_setcolreg(unsigned regno, unsigned red, unsigned green,
			   unsigned blue, unsigned transp, struct fb_info *info)
{
	/*
	 *  Set a single color register. The values supplied are
	 *  already rounded down to the hardware's capabilities
	 *  (according to the entries in the `var' structure). Return
	 *  != 0 for invalid regno.
	 */

	if (regno >= info->cmap.len)
		return 1;

	switch (info->var.bits_per_pixel) {
	case 16:
		if (info->var.red.offset == 10) {
			/* 1:5:5:5, not used currently */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800) >> 1) |
			    ((green & 0xf800) >> 6) | ((blue & 0xf800) >> 11);
		} else {
			/* 0:5:6:5 */
			((u32 *) (info->pseudo_palette))[regno] =
			    ((red & 0xf800)) |
			    ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
		}
		break;
	case 8:
	case 15:
	case 24:
	case 32:
		((u32 *)info->pseudo_palette)[regno] =
			((red >> 8) << 16) |
			((green >> 8) << 8) |
			(blue >> 8);
		break;
	}
	return 0;
}

static int vifb_check_var_timings(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct vi_tv_mode *mode = ctl->mode;
	struct vi_mode_timings timings;
	u32 yres = var->yres;
	int error = -EINVAL;

	if (vi_vmode_is_progressive(var->vmode)) {
		/* 480p */
		error = vi_ntsc_525_prog_calc_timings(&timings, var,
						      var->xres, var->yres);
	} else {
		if (mode->lines == 625)
			/* 576i */
			error = vi_pal_625_calc_timings(&timings, var,
							var->xres, yres);
		else
			/* 480i */
			error = vi_ntsc_525_calc_timings(&timings, var,
							 var->xres, var->yres);
	}
	if (error)
		goto err_out;

	ctl->timings = timings;
	var->pixclock = KHZ2PICOS(13.5 * 1000);
	var->sync = FB_SYNC_BROADCAST;

	error = 0;

err_out:
	return error;
}

static int vifb_format_is_fourcc(const struct fb_var_screeninfo *var)
{
	return var->grayscale > 1;
}


/*
 * Check var and eventually tweak it to something supported.
 * Do not modify par here.
 */
static int vifb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct vi_tv_mode *mode = ctl->mode;
	int error = -EINVAL;
	__u32 xres, yres, xres_virtual, yres_virtual;

	/* no custom viewports, sorry */
	if ((var->xoffset != 0) ||
		(var->yoffset != 0)) {
		dev_err(info->device, "Non-zero x/y offsets are not supported\n");
		return -EINVAL;
	}

	if (vifb_format_is_fourcc(var))
		return -EINVAL;

	var->nonstd = 0;
	if (var->bits_per_pixel == 16) {
		var->red.offset = 11;
		var->red.length = 5;
		var->green.offset = 5;
		var->green.length = 6;
		var->blue.offset = 0;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->transp.length = 0;
	} else {
		dev_err(info->device, "unsupported depth %u\n",
			var->bits_per_pixel);
		return -EINVAL;
	}

	var->grayscale = 0;

	yres = var->yres;
	if (yres & VI_VERT_ALIGN)
		yres = ALIGN(yres, VI_VERT_ALIGN+1);
	if (yres > mode->height) {
		dev_err(info->device, "yres %u out of bounds\n", yres);
		return -EINVAL;
	}
	if (yres < 16) {
		dev_err(info->device, "yres %u < 16 is too small\n", yres);
		return -EINVAL;
	}
	if (!yres)
		yres = mode->height;

	yres_virtual = var->yres_virtual;
	if (!yres_virtual || yres_virtual < yres)
		yres_virtual = yres;

	xres = var->xres;
	if (xres & VI_HORZ_ALIGN)
		xres = ALIGN(xres, VI_HORZ_ALIGN+1);
	if (xres > mode->width) {
		dev_err(info->device, "xres %u (%u) out of bounds (max %u)\n", var->xres, xres, mode->width);
		return -EINVAL;
	}
	if (!xres)
		xres = mode->width;

	xres_virtual = var->xres_virtual;
	if (xres_virtual & VI_HORZ_ALIGN)
		xres_virtual = ALIGN(xres_virtual, VI_HORZ_ALIGN+1);
	if (!xres_virtual || xres_virtual < xres)
		xres_virtual = xres;

	if (xres_virtual * yres_virtual * sizeof(u16) >
	    ctl->rgb_fb_size) {
		dev_err(info->device, "not enough memory for virtual resolution (%ux%ux%u)\n",
			   xres_virtual, yres_virtual, var->bits_per_pixel);
		return -EINVAL;
	}

	var->xres = xres;
	var->yres = yres;
	var->xres_virtual = xres_virtual;
	var->yres_virtual = yres_virtual;

	/* enable non-interlaced mode if supported */
	if (force_scan != VI_SCAN_INTERLACED && ctl->has_component_cable) {
		var->vmode = (mode->flags & VI_VMF_PROGRESSIVE) ?
					FB_VMODE_NONINTERLACED :
					FB_VMODE_INTERLACED;
	} else
		var->vmode = FB_VMODE_INTERLACED;

	error = vifb_check_var_timings(var, info);
	if (error)
		return error;

	return 0;
}

static void vifb_clear_all(struct fb_info *info)
{
	u32 *xfb;
	int i;

	memset(info->screen_buffer, 0, info->screen_size);

	i = gx_xfb_size >> 2;
	xfb = xfb_mem;
	while (i--)
		*(xfb++) = VI_YUYV_BLACK;
}

/*
 * Set the video mode according to info->var.
 */
static int vifb_set_par(struct fb_info *info)
{
	struct vi_ctl *ctl = info->par;
	struct fb_var_screeninfo *var = &info->var;
	unsigned long flags;
	int gx_ll;
#ifdef CONFIG_WII_AVE_RVL
	int error;
#endif

	if (vifb_format_is_fourcc(var) || var->bits_per_pixel != 16)
		return -EINVAL;

	info->fix.line_length = var->xres_virtual * sizeof(u16);
	info->fix.smem_len = info->fix.line_length * var->yres_virtual;
	info->screen_size = info->fix.smem_len;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	/* info->fix.smem_* refer to the virtual framebuffer, here however
	 * we want to store physical fb info, namely the
	 * addresses of the two pages used for flipping
	 */
	gx_ll = VI_XFB_WIDTH * TV_BYTES_PER_PIXEL;
	ctl->page_address[0] = gx_xfb_start;
	if (var->yres * gx_ll <= gx_xfb_size / 2)
		ctl->page_address[1] =
		    gx_xfb_start + var->yres * gx_ll;
	else /* this is weird but I don't understand it, so I don't touch it */
		ctl->page_address[1] = gx_xfb_start;

	/* set page 1 as the visible page and cancel pending flips */
	spin_lock_irqsave(&ctl->lock, flags);
	ctl->visible_page = 1;
	vi_flip_page(ctl);
	spin_unlock_irqrestore(&ctl->lock, flags);

	if (want_ypan) {
		info->fix.xpanstep = 2;
		info->fix.ypanstep = 1;
		info->flags |= FBINFO_HWACCEL_YPAN;
	} else {
		info->fix.xpanstep = 0;
		info->fix.ypanstep = 0;
	}

	vi_setup_tv_mode(ctl, false);
#ifdef CONFIG_WII_AVE_RVL
	if (ctl->i2c_client) {
		error = vi_ave_setup(ctl);
		if (error)
			return error;
	}
#endif

	return 0;
}

static void __maybe_unused vifb_fillrect(struct fb_info *info,
			  const struct fb_fillrect *rect)
{
	u32 __iomem *dst;
	u32 color;
	unsigned int x, y;

	if (!rect->width || !rect->height)
		return;

	color = rect->color;
	if (info->fix.visual == FB_VISUAL_TRUECOLOR)
		color = ((u32 *)info->pseudo_palette)[color];

	for (y = 0; y < rect->height; y++) {
		dst = (u32 __iomem *)
			((u8 __iomem *)info->screen_base +
			 (rect->dy + y) * info->fix.line_length) + rect->dx;
		for (x = 0; x < rect->width; x++) {
			if (rect->rop == ROP_XOR)
				out_be32(dst + x, in_be32(dst + x) ^ color);
			else
				out_be32(dst + x, color);
		}
	}
}

static void __maybe_unused vifb_copyarea(struct fb_info *info,
			  const struct fb_copyarea *area)
{
	u32 __iomem *src, *dst;
	int x, y, x_start, x_end, x_step;
	int y_start, y_end, y_step;

	if (!area->width || !area->height)
		return;

	if (area->dy > area->sy) {
		y_start = area->height - 1;
		y_end = -1;
		y_step = -1;
	} else {
		y_start = 0;
		y_end = area->height;
		y_step = 1;
	}
	if (area->dy == area->sy && area->dx > area->sx) {
		x_start = area->width - 1;
		x_end = -1;
		x_step = -1;
	} else {
		x_start = 0;
		x_end = area->width;
		x_step = 1;
	}

	for (y = y_start; y != y_end; y += y_step) {
		src = (u32 __iomem *)
			((u8 __iomem *)info->screen_base +
			 (area->sy + y) * info->fix.line_length) + area->sx;
		dst = (u32 __iomem *)
			((u8 __iomem *)info->screen_base +
			 (area->dy + y) * info->fix.line_length) + area->dx;
		for (x = x_start; x != x_end; x += x_step)
			out_be32(dst + x, in_be32(src + x));
	}
}

static void __maybe_unused vifb_imageblit(struct fb_info *info,
			   const struct fb_image *image)
{
	const u8 *src = image->data;
	u32 __iomem *dst;
	u32 fg, bg, color;
	unsigned int pitch, x, y;

	if (!image->width || !image->height)
		return;

	fg = image->fg_color;
	bg = image->bg_color;
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		fg = ((u32 *)info->pseudo_palette)[fg];
		bg = ((u32 *)info->pseudo_palette)[bg];
	}

	if (image->depth == 1) {
		pitch = DIV_ROUND_UP(image->width, 8);
		for (y = 0; y < image->height; y++) {
			dst = (u32 __iomem *)
				((u8 __iomem *)info->screen_base +
				 (image->dy + y) * info->fix.line_length) +
				image->dx;
			for (x = 0; x < image->width; x++) {
				color = src[y * pitch + x / 8] &
					BIT(7 - (x & 7)) ? fg : bg;
				out_be32(dst + x, color);
			}
		}
	} else if (image->depth == 32) {
		pitch = image->width * sizeof(color);
		for (y = 0; y < image->height; y++) {
			dst = (u32 __iomem *)
				((u8 __iomem *)info->screen_base +
				 (image->dy + y) * info->fix.line_length) +
				image->dx;
			for (x = 0; x < image->width; x++) {
				memcpy(&color, src + y * pitch +
				       x * sizeof(color), sizeof(color));
				out_be32(dst + x, color);
			}
		}
	}
}

static bool __maybe_unused
vifb_efb_word_valid(struct fb_info *info, unsigned long offset)
{
	unsigned long line = offset / info->fix.line_length;
	unsigned long column = offset % info->fix.line_length;

	return line < info->var.yres_virtual &&
	       column + sizeof(u32) <= info->var.xres_virtual * sizeof(u32);
}

static ssize_t __maybe_unused
vifb_read(struct fb_info *info, char __user *buf, size_t count,
			 loff_t *ppos)
{
	unsigned long limit = info->fix.line_length * info->var.yres_virtual;
	unsigned long pos = *ppos;
	size_t done = 0;

	if (pos >= limit)
		return 0;
	count = min_t(size_t, count, limit - pos);

	while (done < count) {
		unsigned long word_pos = pos & ~(sizeof(u32) - 1);
		size_t word_off = pos & (sizeof(u32) - 1);
		size_t len = min_t(size_t, sizeof(u32) - word_off,
				   count - done);
		u32 value = 0;

		if (vifb_efb_word_valid(info, word_pos))
			value = in_be32((u32 __iomem *)
				((u8 __iomem *)info->screen_base + word_pos));
		if (copy_to_user(buf + done, (u8 *)&value + word_off, len))
			return done ? done : -EFAULT;

		pos += len;
		done += len;
	}

	*ppos = pos;
	return done;
}

static ssize_t __maybe_unused
vifb_write(struct fb_info *info, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	unsigned long limit = info->fix.line_length * info->var.yres_virtual;
	unsigned long pos = *ppos;
	size_t done = 0;

	if (pos > limit)
		return -EFBIG;
	count = min_t(size_t, count, limit - pos);

	while (done < count) {
		unsigned long word_pos = pos & ~(sizeof(u32) - 1);
		size_t word_off = pos & (sizeof(u32) - 1);
		size_t len = min_t(size_t, sizeof(u32) - word_off,
				   count - done);
		u32 __iomem *dst = (u32 __iomem *)
			((u8 __iomem *)info->screen_base + word_pos);
		u32 value = 0;

		if (vifb_efb_word_valid(info, word_pos) &&
		    len != sizeof(value))
			value = in_be32(dst);

		if (copy_from_user((u8 *)&value + word_off, buf + done, len))
			return done ? done : -EFAULT;

		if (vifb_efb_word_valid(info, word_pos))
			out_be32(dst, value);

		pos += len;
		done += len;
	}

	*ppos = pos;
	return done;
}

static int vifb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct vi_ctl *ctl = info->par;

	return dma_mmap_pages(ctl->dev, vma, ctl->rgb_fb_size,
			      virt_to_page(ctl->rgb_fb));
}

struct fb_ops vifb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read,
	.fb_write = fb_sys_write,
	.fb_mmap = vifb_mmap,
	.fb_setcolreg = vifb_setcolreg,
	/*.fb_ioctl = vifb_ioctl,*/
	.fb_set_par = vifb_set_par,
	.fb_check_var = vifb_check_var,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit
};

#ifndef MODULE

static int vifb_setup(char *options)
{
	char *this_opt;

	if (!options || !*options)
		return 0;

	pr_info("options: %s\n", options);

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;

		if (!strcmp(this_opt, "redraw"))
			want_ypan = 0;
		else if (!strcmp(this_opt, "interlaced"))
			force_scan = VI_SCAN_INTERLACED;
		else if (!strcmp(this_opt, "progressive"))
			force_scan = VI_SCAN_PROGRESSIVE;
		else if (!strcmp(this_opt, "50Hz"))
			force_rate = VI_RATE_50Hz;
		else if (!strcmp(this_opt, "60Hz"))
			force_rate = VI_RATE_60Hz;
		else if (!strncmp(this_opt, "tv=", 3)) {
			if (!strncmp(this_opt + 3, "PAL", 3))
				force_tv = VI_TV_PAL;
			else if (!strncmp(this_opt + 3, "NTSC", 4))
				force_tv = VI_TV_NTSC;
		}
	}

	if (force_scan == VI_SCAN_PROGRESSIVE || force_tv == VI_TV_NTSC) {
		if (force_rate == VI_RATE_50Hz) {
			pr_info("ignoring forced 50Hz setting\n");
			force_rate = VI_RATE_DONTCARE;
		}
	}
	return 0;
}

#endif	/* MODULE */


/*
 * OF platform driver hooks.
 *
 */

static void __iomem *vifb_iomap_compatible(struct device *dev,
					   const char *compatible,
					   phys_addr_t *start)
{
	struct device_node *np;
	struct resource res;
	void __iomem *base;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return NULL;
	if (of_address_to_resource(np, 0, &res)) {
		of_node_put(np);
		return NULL;
	}
	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (start)
		*start = res.start;
	of_node_put(np);
	return base;
}

static unsigned int vifb_irq_compatible(const char *compatible,
					unsigned int index)
{
	struct device_node *np;
	unsigned int irq;

	np = of_find_compatible_node(NULL, NULL, compatible);
	if (!np)
		return 0;
	irq = irq_of_parse_and_map(np, index);
	of_node_put(np);
	return irq;
}

static int vifb_of_probe(struct platform_device *odev)
{
	struct device *dev = &odev->dev;
	struct fb_info *info;
	struct vi_ctl *ctl;
	u32 xfb_start, xfb_size, efb_start, efb_size;
	int error;

	if (of_property_read_u32(dev->of_node, "xfb-start", &xfb_start) ||
	    of_property_read_u32(dev->of_node, "xfb-size", &xfb_size) ||
	    of_property_read_u32(dev->of_node, "efb-start", &efb_start) ||
	    of_property_read_u32(dev->of_node, "efb-size", &efb_size)) {
		dev_err(dev, "missing EFB or XFB description\n");
		return -ENODEV;
	}

	info = framebuffer_alloc(sizeof(struct vi_ctl), dev);
	if (!info)
		return -ENOMEM;

	info->fbops = &vifb_ops;
	info->var = vifb_var;
	info->fix = vifb_fix;
	ctl = info->par;
	ctl->info = info;
	ctl->irq = irq_of_parse_and_map(dev->of_node, 0);
	ctl->pe_irq = vifb_irq_compatible("nintendo,flipper-gx-pe", 1);
	ctl->dev = dev;
	ctl->io_base = devm_of_iomap(dev, dev->of_node, 0, NULL);
	if (IS_ERR(ctl->io_base)) {
		error = PTR_ERR(ctl->io_base);
		goto err_release_info;
	}

	ctl->cp_base = vifb_iomap_compatible(dev,
					     "nintendo,flipper-gx-cp", NULL);
	ctl->pe_base = vifb_iomap_compatible(dev,
					     "nintendo,flipper-gx-pe", NULL);
	ctl->pi_base = vifb_iomap_compatible(dev,
					     "nintendo,flipper-pi", NULL);
	ctl->wgpipe = vifb_iomap_compatible(dev,
					    "nintendo,flipper-gx-fifo",
					    &ctl->gx_fifo_base);
	if (!ctl->cp_base || !ctl->pe_base || !ctl->pi_base || !ctl->wgpipe ||
	    !ctl->pe_irq) {
		dev_err(dev, "unable to map GX registers\n");
		error = -ENODEV;
		goto err_release_info;
	}

	xfb_mem = devm_ioremap(dev, xfb_start, xfb_size);
	if (!xfb_mem) {
		dev_err(dev, "unable to map XFB\n");
		error = -ENOMEM;
		goto err_release_info;
	}
	gx_xfb_start = xfb_start;
	gx_xfb_size = xfb_size;

	spin_lock_init(&ctl->lock);
	init_waitqueue_head(&ctl->vtrace_waitq);
	vi_reset_video(ctl);
	vi_detect_tv_mode(ctl);

	ctl->rgb_fb_size = ctl->mode->width * ctl->mode->height * sizeof(u16);
	ctl->rgb_fb = dma_alloc_noncoherent(dev, ctl->rgb_fb_size,
					    &ctl->rgb_fb_dma, DMA_TO_DEVICE,
					    GFP_KERNEL);
	if (!ctl->rgb_fb) {
		error = -ENOMEM;
		goto err_release_info;
	}
	ctl->indirect_map = dma_alloc_noncoherent(dev, GX_INDIRECT_MAP_SIZE,
						  &ctl->indirect_map_dma,
						  DMA_TO_DEVICE, GFP_KERNEL);
	if (!ctl->indirect_map) {
		error = -ENOMEM;
		goto err_free_rgb_fb;
	}
	gx_init_indirect_map(ctl);
	dma_sync_single_for_device(dev, ctl->indirect_map_dma,
				   GX_INDIRECT_MAP_SIZE, DMA_TO_DEVICE);
	info->fix.smem_start = ctl->rgb_fb_dma;
	info->fix.smem_len = ctl->rgb_fb_size;
	info->fix.line_length = ctl->mode->width * sizeof(u16);
	info->screen_buffer = ctl->rgb_fb;
	info->screen_size = ctl->rgb_fb_size;
	info->flags |= FBINFO_VIRTFB;

	error = gx_init(ctl);
	if (error) {
		if (ctl->fifo)
			goto err_free_fifo;
		goto err_free_indirect_map;
	}

#ifdef CONFIG_WII_AVE_RVL
	if (!first_vi_ctl)
		first_vi_ctl = ctl;
	if (first_vi_ave) {
		error = vi_attach_ave(ctl, first_vi_ave);
		if (error)
			dev_err(dev, "unable to attach AVE: error %d\n", error);
	}
#endif

	info->var.xres = ctl->mode->width;
	info->var.yres = ctl->mode->height;
	ctl->visible_page = 0;
	ctl->gx_faulted = false;
	init_completion(&ctl->pe_finished);
	info->pseudo_palette = pseudo_palette;

	error = fb_alloc_cmap(&info->cmap, 16, 0);
	if (error)
		goto err_free_fifo;
	error = vifb_check_var(&info->var, info);
	if (error)
		goto err_cmap;
	info->fix.smem_len = info->fix.line_length * info->var.yres_virtual;
	info->screen_size = info->fix.smem_len;

	vifb_clear_all(info);
	dev_set_drvdata(dev, info);
	vi_enable_interrupts(ctl, 0);
	error = request_threaded_irq(ctl->irq, vi_irq_handler, vi_irq_thread,
				     IRQF_ONESHOT, DRV_MODULE_NAME, dev);
	if (error)
		goto err_drvdata;
	error = request_irq(ctl->pe_irq, pe_irq_handler, IRQF_NO_THREAD,
			    DRV_MODULE_NAME "-pe", dev);
	if (error)
		goto err_vi_irq;
	error = register_framebuffer(info);
	if (error)
		goto err_pe_irq;

	pr_info("fb%d: %s frame buffer device (GX indirect RGB565)\n",
		info->node, info->fix.id);
	vi_enable_interrupts(ctl, 1);
	return 0;

err_pe_irq:
	free_irq(ctl->irq, dev);
	free_irq(ctl->pe_irq, dev);
	goto err_drvdata;
err_vi_irq:
	free_irq(ctl->irq, dev);
err_drvdata:
	dev_set_drvdata(dev, NULL);
err_cmap:
	fb_dealloc_cmap(&info->cmap);
err_free_fifo:
	dma_free_noncoherent(dev, GX_FIFO_SIZE, ctl->fifo, ctl->fifo_dma,
			     DMA_BIDIRECTIONAL);
err_free_indirect_map:
	dma_free_noncoherent(dev, GX_INDIRECT_MAP_SIZE, ctl->indirect_map,
			     ctl->indirect_map_dma, DMA_TO_DEVICE);
err_free_rgb_fb:
	dma_free_noncoherent(dev, ctl->rgb_fb_size, ctl->rgb_fb,
			     ctl->rgb_fb_dma, DMA_TO_DEVICE);
err_release_info:
	framebuffer_release(info);
	return error;
}

static void vifb_of_remove(struct platform_device *odev)
{
	struct vi_ctl *ctl;
	struct fb_info *info = dev_get_drvdata(&odev->dev);

	if (!info)
		return;

	ctl = info->par;

	vi_enable_interrupts(ctl, 0);
	free_irq(ctl->irq, &odev->dev);
	free_irq(ctl->pe_irq, &odev->dev);
	unregister_framebuffer(info);
	fb_dealloc_cmap(&info->cmap);
	dma_free_noncoherent(&odev->dev, GX_FIFO_SIZE, ctl->fifo,
			     ctl->fifo_dma, DMA_BIDIRECTIONAL);
	dma_free_noncoherent(&odev->dev, GX_INDIRECT_MAP_SIZE,
			     ctl->indirect_map, ctl->indirect_map_dma,
			     DMA_TO_DEVICE);
	dma_free_noncoherent(&odev->dev, ctl->rgb_fb_size, ctl->rgb_fb,
			     ctl->rgb_fb_dma, DMA_TO_DEVICE);

	dev_set_drvdata(&odev->dev, NULL);

#ifdef CONFIG_WII_AVE_RVL
	vi_dettach_ave(ctl);
	if (first_vi_ctl == ctl)
		first_vi_ctl = NULL;
#endif
	framebuffer_release(info);
}

static void vifb_of_shutdown(struct platform_device *odev)
{
	struct fb_info *info = dev_get_drvdata(&odev->dev);
	struct vi_ctl *ctl = info->par;
	void __iomem *io_base = ctl->io_base;

	vi_enable_interrupts(ctl, 0);
	synchronize_irq(ctl->irq);
	vi_reset_video(ctl);
	out_be16(io_base + VI_DCR, vi_dcr_enb(0));

	return;
}


static struct of_device_id vifb_of_match[] = {
	{ .compatible = "nintendo,flipper-vi", },
	{ .compatible = "nintendo,hollywood-vi", },
	{ },
};

MODULE_DEVICE_TABLE(of, vifb_of_match);

static struct platform_driver vifb_of_driver = {
	.driver = {
		.name = DRV_MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = vifb_of_match,
	},
	.probe = vifb_of_probe,
	.remove = vifb_of_remove,
	.shutdown = vifb_of_shutdown,
};

/*
 * Module interface hooks
 *
 */

static int __init vifb_init_module(void)
{
	int error;
#ifdef CONFIG_WII_AVE_RVL
	bool ave_registered = false;
#endif
	char *option = NULL;

	pr_info("%s - version %s\n", DRV_DESCRIPTION,
		   vifb_driver_version);

#ifndef MODULE
	if (fb_get_options(DRV_MODULE_NAME, &option))
		return -ENODEV;
	if (!option) {
		/* for backwards compatibility */
		if (fb_get_options("gcnfb", &option))
			return -ENODEV;
	}
	error = vifb_setup(option);
	if (error)
		return error;
#endif

#ifdef CONFIG_WII_AVE_RVL
	error = i2c_add_driver(&vi_ave_driver);
	if (error)
		pr_err("failed to register AVE (%d)\n", error);
	else
		ave_registered = true;
#endif

	error = platform_driver_register(&vifb_of_driver);
	if (error) {
		pr_err("failed to register driver (%d)\n", error);
#ifdef CONFIG_WII_AVE_RVL
		if (ave_registered)
			i2c_del_driver(&vi_ave_driver);
#endif
	}

	return error;
}

static void __exit vifb_exit_module(void)
{
	platform_driver_unregister(&vifb_of_driver);
#ifdef CONFIG_WII_AVE_RVL
	i2c_del_driver(&vi_ave_driver);
#endif
}

module_init(vifb_init_module);
module_exit(vifb_exit_module);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");
