// SPDX-License-Identifier: GPL-2.0-only

#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "r600fb_reg.h"

#define DRIVER_NAME "r600fb"
#define DRIVER_DESC "DRM framebuffer driver for R600 cards"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0

/*
 * The main event
 */
struct r600fb_device {
	struct drm_device dev;

	struct drm_display_mode mode;
	const struct drm_format_info *format;
	unsigned int pitch;

	void __iomem *regs;

	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static struct r600fb_device *r600fb_device_of_dev(struct drm_device *dev)
{
	return container_of(dev, struct r600fb_device, dev);
}

/*
 * Driver-y bits
 */
static const uint32_t r600fb_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGRX8888
};
static void r600fb_format_set(struct r600fb_device *rdev, const struct drm_format_info *format)
{
	u32 swap;
	u32 control;
	switch (format->format) {
		/* "true" LE XRGB8888, not the fake "we asked for XR24 but meant BX24" stuff */
		case DRM_FORMAT_XRGB8888:
			control = DGRPH_DEPTH_32BPP | DGRPH_FORMAT_32BPP_ARGB8888 | DGRPH_ARRAY_LINEAR_ALIGNED;
			swap = DGRPH_CROSSBAR_RGBA(R, G, B, A);
			break;
		case DRM_FORMAT_BGRX8888:
			control = DGRPH_DEPTH_32BPP | DGRPH_FORMAT_32BPP_ARGB8888 | DGRPH_ARRAY_LINEAR_ALIGNED;
			swap = DGRPH_ENDIAN_SWAP_32 | DGRPH_CROSSBAR_RGBA(R, G, B, A);
			break;
		default:
			return;
	}

	drm_dbg(&rdev->dev, "changing to %c%c%c%c - cntl %08x swap %08x",
		format->format >>  0 & 0xFF,
		format->format >>  8 & 0xFF,
		format->format >> 16 & 0xFF,
		format->format >> 24 & 0xFF,

		control, swap);
	writereg(D1 + DGRPH_SWAP_CNTL, swap);
	writereg(D1 + DGRPH_CONTROL, control);
	rdev->format = format;
}

/*
 * Modesetting
 */

static struct drm_mode_config_funcs r600fb_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Plane
 */

static void
r600fb_primary_plane_helper_atomic_update(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct r600fb_device *rdev = r600fb_device_of_dev(plane->dev);
	struct drm_plane_state *plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state = drm_atomic_get_old_plane_state(state, plane);

	if (!plane_state->fb)
		return;

	drm_fb_dma_sync_non_coherent(&rdev->dev, old_plane_state, plane_state);

	if (plane_state->fb->format != rdev->format)
		r600fb_format_set(rdev, plane_state->fb->format);

	writereg(D1 + DGRPH_PRIMARY_SURFACE_ADDRESS, drm_fb_dma_get_gem_addr(plane_state->fb, plane_state, 0));
}


static int r600fb_primary_plane_helper_atomic_check(struct drm_plane *plane,
						    struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static const struct drm_plane_helper_funcs r600fb_primary_plane_helper_funcs = {
	.atomic_check = r600fb_primary_plane_helper_atomic_check,
	.atomic_update = r600fb_primary_plane_helper_atomic_update,
};

static const struct drm_plane_funcs r600fb_primary_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,

};

static enum drm_mode_status
r600fb_crtc_helper_mode_valid(struct drm_crtc *crtc,
			      const struct drm_display_mode *mode)
{
	struct r600fb_device *sdev = r600fb_device_of_dev(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &sdev->mode);
}

/*
 * The CRTC is always enabled. Screen updates are performed by
 * the primary plane's atomic_update function. Disabling clears
 * the screen in the primary plane's atomic_disable function.
 */
static const struct drm_crtc_helper_funcs r600fb_crtc_helper_funcs = {
	.mode_valid = r600fb_crtc_helper_mode_valid,
	.atomic_check = drm_crtc_helper_atomic_check,
};

static const struct drm_crtc_funcs r600fb_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static int r600fb_connector_helper_get_modes(struct drm_connector *connector)
{
	struct r600fb_device *sdev = r600fb_device_of_dev(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &sdev->mode);
}

static const struct drm_connector_helper_funcs r600fb_connector_helper_funcs = {
	.get_modes = r600fb_connector_helper_get_modes,
};

static const struct drm_connector_funcs r600fb_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static struct r600fb_device *r600fb_device_create(struct drm_driver *drv,
						  struct platform_device *pdev)
{
	struct r600fb_device *sdev;
	struct drm_device *dev;
	struct resource *regs;
	struct drm_plane *primary_plane;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	int ret;

	static const struct drm_display_mode mode = { DRM_MODE_INIT(
		60, 1280, 720, DRM_MODE_RES_MM(1280, 96ul),
		DRM_MODE_RES_MM(720, 96ul)) };

	sdev = devm_drm_dev_alloc(&pdev->dev, drv, struct r600fb_device, dev);
	if (IS_ERR(sdev))
		return ERR_CAST(sdev);
	dev = &sdev->dev;
	platform_set_drvdata(pdev, sdev);

	// hardware init
	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sdev->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(sdev->regs))
		return ERR_PTR(-ENODEV);
	// todo regmap

	// modesetting
	ret = drmm_mode_config_init(dev);
	if (ret)
		return ERR_PTR(ret);

	// todo these are guesses. check the radeon ref
	dev->mode_config.min_width = 32;
	dev->mode_config.max_width = 4096;
	dev->mode_config.min_height = 32;
	dev->mode_config.max_height = 4096;
	dev->mode_config.preferred_depth = 32;
	dev->mode_config.funcs = &r600fb_mode_config_funcs;
	// This is a hard requirement or else drmModeAddFB2 returns EOPNOTSUPP. It's supposed to
	// cause userspace to fall back on drmModeAddFB but most new-style software doesn't
	// include the fallback code.
	dev->mode_config.quirk_addfb_prefer_host_byte_order = true;

	sdev->mode = mode;

	// primary plane
	primary_plane = &sdev->primary_plane;
	ret = drm_universal_plane_init(dev, primary_plane, 0,
				       &r600fb_primary_plane_funcs, r600fb_formats,
				       ARRAY_SIZE(r600fb_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, "tv");
	if (ret)
		return ERR_PTR(ret);
	drm_plane_helper_add(primary_plane, &r600fb_primary_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(primary_plane);

	/* CRTC */

	crtc = &sdev->crtc;
	ret = drm_crtc_init_with_planes(dev, crtc, primary_plane, NULL,
					&r600fb_crtc_funcs, NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(crtc, &r600fb_crtc_helper_funcs);

	/* Encoder */

	encoder = &sdev->encoder;
	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_simple_encoder_init(dev, encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ERR_PTR(ret);

	/* Connector */

	connector = &sdev->connector;
	ret = drm_connector_init(dev, connector, &r600fb_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ERR_PTR(ret);
	drm_connector_helper_add(connector, &r600fb_connector_helper_funcs);
	drm_connector_set_panel_orientation_with_quirk(
		connector, DRM_MODE_PANEL_ORIENTATION_UNKNOWN, 1280, 720);

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret)
		return ERR_PTR(ret);

	drm_mode_config_reset(dev);

	return sdev;
}

static struct drm_gem_object *r600fb_create_object(struct drm_device *dev, size_t size)
{
	struct drm_gem_dma_object *obj;

	if (size == 0)
		return ERR_PTR(-EINVAL);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->map_noncoherent = true;

	return &obj->base;
}

/*
 * DRM driver
 */

DEFINE_DRM_GEM_DMA_FOPS(r600fb_fops);

static struct drm_driver r600fb_driver = {
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_TTM_DRIVER_OPS,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &r600fb_fops,

	.gem_create_object = r600fb_create_object,
};

/*
 * Platform driver
 */

static int r600fb_probe(struct platform_device *pdev)
{
	struct r600fb_device *rdev;
	struct drm_device *dev;
	int ret;

	rdev = r600fb_device_create(&r600fb_driver, pdev);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	dev = &rdev->dev;

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_client_setup(dev, NULL);

	return 0;
}

static void r600fb_remove(struct platform_device *pdev)
{
	struct r600fb_device *rdev = platform_get_drvdata(pdev);
	struct drm_device *dev = &rdev->dev;

	drm_dev_unplug(dev);
}

static const struct of_device_id r600fb_of_match_table[] = {
	{
		.compatible = "nintendo,latte-gpu7",
	},
	{},
};
MODULE_DEVICE_TABLE(of, r600fb_of_match_table);

static struct platform_driver r600fb_platform_driver = {
	.driver = {
		.name = "r600fb",
		.of_match_table = r600fb_of_match_table,
	},
	.probe = r600fb_probe,
	.remove = r600fb_remove,
};

module_platform_driver(r600fb_platform_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL v2");
