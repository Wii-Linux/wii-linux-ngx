// SPDX-License-Identifier: GPL-2.0 or MIT
/*
 * Minimal KMS for the Flipper/Hollywood video interface.
 *
 * There is exactly one mode: the TV mode gcnfb.c detected.  Scanout is
 * RGB565 only; gcnfb.c converts it to YUYV with GX at every retrace.  The Rust
 * side of the driver owns the DRM device and calls into this file to set up
 * the pipeline, so that DRM's fbdev emulation can provide a console.
 */

#include <linux/container_of.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_print.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "flipper_kms.h"
#include "gcnfb.h"

struct flipper_kms {
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct vi_ctl *ctl;
};

static struct flipper_kms *to_kms(struct drm_device *drm)
{
	return drm->dev_private;
}

static struct flipper_kms *pipe_to_kms(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct flipper_kms, pipe);
}

/* Connector */

static int flipper_connector_get_modes(struct drm_connector *connector)
{
	struct flipper_kms *kms = to_kms(connector->dev);
	struct drm_display_mode *mode;
	struct gcnfb_mode m;

	if (!kms->ctl)
		return 0;
	gcnfb_get_mode(kms->ctl, &m);

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	/* Nominal BT.601 timings; the real ones are programmed by gcnfb. */
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->clock = m.progressive ? 27000 : 13500;
	mode->hdisplay = m.width;
	mode->hsync_start = m.width + 16;
	mode->hsync_end = mode->hsync_start + 62;
	mode->htotal = m.lines == 625 ? 864 : 858;
	mode->vdisplay = m.height;
	mode->vsync_start = m.height + 4;
	mode->vsync_end = mode->vsync_start + 3;
	mode->vtotal = m.lines;
	if (!m.progressive)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs flipper_connector_helper_funcs = {
	.get_modes = flipper_connector_get_modes,
};

static const struct drm_connector_funcs flipper_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* Display pipe */

static enum drm_mode_status
flipper_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			const struct drm_display_mode *mode)
{
	struct flipper_kms *kms = pipe_to_kms(pipe);
	struct gcnfb_mode m;

	if (!kms->ctl)
		return MODE_ERROR;
	gcnfb_get_mode(kms->ctl, &m);
	if (mode->hdisplay != m.width || mode->vdisplay != m.height)
		return MODE_BAD;
	if (!!(mode->flags & DRM_MODE_FLAG_INTERLACE) == m.progressive)
		return MODE_BAD;
	return MODE_OK;
}

static void flipper_pipe_send_event(struct drm_crtc *crtc)
{
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (!event)
		return;
	crtc->state->event = NULL;

	spin_lock_irq(&crtc->dev->event_lock);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void flipper_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct flipper_kms *kms = pipe_to_kms(pipe);
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	int error;

	if (!kms->ctl)
		return;

	error = gcnfb_modeset(kms->ctl);
	if (error)
		drm_err(pipe->crtc.dev, "failed to set video mode: %d\n", error);

	/* Scanout restarts from whatever the conversion buffer holds. */
	if (fb)
		gcnfb_write_fb(kms->ctl, shadow->data[0].vaddr, fb->pitches[0],
			       0, 0, fb->width, fb->height);
}

static void flipper_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct flipper_kms *kms = pipe_to_kms(pipe);

	if (kms->ctl)
		gcnfb_scanout_disable(kms->ctl);
}

static void flipper_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_state)
{
	struct flipper_kms *kms = pipe_to_kms(pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow = to_drm_shadow_plane_state(state);
	struct drm_framebuffer *fb = state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;

	if (fb && kms->ctl) {
		drm_atomic_helper_damage_iter_init(&iter, old_state, state);
		drm_atomic_for_each_plane_damage(&iter, &clip)
			gcnfb_write_fb(kms->ctl, shadow->data[0].vaddr,
				       fb->pitches[0], clip.x1, clip.y1,
				       clip.x2, clip.y2);
	}

	flipper_pipe_send_event(&pipe->crtc);
}

static const struct drm_simple_display_pipe_funcs flipper_pipe_funcs = {
	.mode_valid = flipper_pipe_mode_valid,
	.enable = flipper_pipe_enable,
	.disable = flipper_pipe_disable,
	.update = flipper_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static const uint32_t flipper_formats[] = {
	DRM_FORMAT_RGB565,
};

static const struct drm_mode_config_funcs flipper_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Entry points for the Rust driver */

int flipper_kms_init(struct drm_device *drm, struct vi_ctl *ctl)
{
	struct flipper_kms *kms;
	int error;

	error = drmm_mode_config_init(drm);
	if (error)
		return error;

	kms = drmm_kzalloc(drm, sizeof(*kms), GFP_KERNEL);
	if (!kms)
		return -ENOMEM;
	kms->ctl = ctl;
	drm->dev_private = kms;

	drm->mode_config.min_width = 16;
	drm->mode_config.min_height = 16;
	drm->mode_config.max_width = 720;
	drm->mode_config.max_height = 576;
	drm->mode_config.preferred_depth = 16;
	drm->mode_config.funcs = &flipper_mode_config_funcs;

	error = drm_connector_init(drm, &kms->connector,
				   &flipper_connector_funcs,
				   DRM_MODE_CONNECTOR_Composite);
	if (error)
		return error;
	drm_connector_helper_add(&kms->connector,
				 &flipper_connector_helper_funcs);

	error = drm_simple_display_pipe_init(drm, &kms->pipe,
					     &flipper_pipe_funcs,
					     flipper_formats,
					     ARRAY_SIZE(flipper_formats),
					     NULL, &kms->connector);
	if (error)
		return error;
	drm_plane_enable_fb_damage_clips(&kms->pipe.plane);

	drm_mode_config_reset(drm);
	return 0;
}

/* Call after the device is registered: starts the fbdev console. */
void flipper_kms_register(struct drm_device *drm)
{
	drm_client_setup(drm, NULL);
}

/* The detected TV mode changed, let the clients pick up the new one. */
void flipper_kms_mode_changed(struct drm_device *drm)
{
	drm_kms_helper_hotplug_event(drm);
}

/* Disable all outputs.  Call before the gcnfb context goes away. */
void flipper_kms_shutdown(struct drm_device *drm)
{
	struct flipper_kms *kms = to_kms(drm);

	drm_atomic_helper_shutdown(drm);
	kms->ctl = NULL;
}
