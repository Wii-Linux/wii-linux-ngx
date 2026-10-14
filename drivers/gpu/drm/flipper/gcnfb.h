/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Interface between the Flipper DRM driver and the legacy GameCube/Wii
 * GX/VI bring-up code (gcnfb.c).
 *
 * gcnfb.c provides GX initialization, VI mode/timing setup (including the
 * Wii AVE), the VI/PE interrupts and the GX-based RGB565 -> YUYV conversion.
 * The DRM driver provides the device, serializes all GX access between its
 * clients (userspace and the framebuffer conversion) and provides the
 * display pipeline on top.
 */

#ifndef _FLIPPER_GCNFB_H
#define _FLIPPER_GCNFB_H

#include <linux/io.h>
#include <linux/types.h>

struct device;
struct vi_ctl;

/*
 * A unit of GX work.  @state_lost is true when somebody other than the caller's
 * client used GX since the caller's previous job, i.e. GX state must be set
 * up again before relying on it.  Returns 0 or a negative errno.
 */
typedef int (*gcnfb_gx_job_fn)(void *data, bool state_lost);

struct gcnfb_ops {
	/*
	 * Run @job with exclusive access to GX, on behalf of the framebuffer
	 * conversion client.  May sleep.  Returns the job's return value.
	 */
	int (*gx_submit)(void *priv, gcnfb_gx_job_fn job, void *data);

	/* The detected TV mode may have changed (e.g. the AVE attached). */
	void (*mode_changed)(void *priv);
};

/* Register blocks, mapped and owned by the DRM driver. */
struct gcnfb_regs {
	void __iomem *cp;
	void __iomem *pe;
	void __iomem *vi;
	void __iomem *pi;
	void __iomem *wgpipe;
};

struct gcnfb_mode {
	const char *name;
	unsigned int width;	/* visible width in pixels */
	unsigned int height;	/* visible height in lines */
	unsigned int lines;	/* total lines (525 or 625) */
	bool progressive;
	bool pal;
};

/*
 * Bring up GX and VI (but leave scanout off) and start servicing the VI and
 * PE interrupts.  @dev is the platform device, which needs the "pififo" memory
 * resource and the "vi" and "pe-finish" interrupts.  Returns an ERR_PTR() on
 * failure.
 */
struct vi_ctl *gcnfb_create(struct device *dev,
			    const struct gcnfb_regs *regs,
			    const struct gcnfb_ops *ops, void *priv);
void gcnfb_destroy(struct vi_ctl *ctl);

/* Stop scanout and interrupts at system shutdown. */
void gcnfb_shutdown(struct vi_ctl *ctl);

/* The currently detected TV mode, which is also the only mode scanned out. */
void gcnfb_get_mode(struct vi_ctl *ctl, struct gcnfb_mode *out);

/* (Re)program VI, GX and the AVE for the current TV mode and start scanout. */
int gcnfb_modeset(struct vi_ctl *ctl);
void gcnfb_scanout_disable(struct vi_ctl *ctl);

/* Copy part of an RGB565 image of the mode's size into the scanout source. */
void gcnfb_write_fb(struct vi_ctl *ctl, const void *src,
		    unsigned int src_pitch, unsigned int x1, unsigned int y1,
		    unsigned int x2, unsigned int y2);

#endif /* _FLIPPER_GCNFB_H */
