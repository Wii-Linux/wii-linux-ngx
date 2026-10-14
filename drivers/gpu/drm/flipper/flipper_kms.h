/* SPDX-License-Identifier: GPL-2.0 or MIT */
/*
 * Entry points of the minimal KMS pipeline (flipper_kms.c), called by the Rust
 * side of the Flipper DRM driver.
 */

#ifndef _FLIPPER_KMS_H
#define _FLIPPER_KMS_H

struct drm_device;
struct vi_ctl;

/* Set up the display pipeline.  Call before the DRM device is registered. */
int flipper_kms_init(struct drm_device *drm, struct vi_ctl *ctl);

/* Start the in-kernel clients (fbdev emulation).  Call after registering. */
void flipper_kms_register(struct drm_device *drm);

/* The TV mode that gcnfb detected has changed. */
void flipper_kms_mode_changed(struct drm_device *drm);

/* Turn everything off.  Call before gcnfb_destroy(). */
void flipper_kms_shutdown(struct drm_device *drm);

#endif /* _FLIPPER_KMS_H */
