/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * include/uapi/drm/flipper_drm.h
 *
 * Copyright (C) 2026 Link Mauve
 * Author: Link Mauve <linkmauve@linkmauve.fr>
 */

#ifndef __FLIPPER_DRM_H__
#define __FLIPPER_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum drm_flipper_command {
	FIFO = 0,
	UPDATE_VERTEX,
};

struct drm_flipper_cmd {
	enum drm_flipper_command command;
	const void *pointer;
	u32 size;
	u32 reserved;
};

struct drm_flipper_run_cmds {
	const struct drm_flipper_cmd cmds[4];
	u32 cmds_len;
};

struct drm_flipper_write_tex_mem {
	const void *pointer;
	u32 size;
};

struct drm_flipper_map_efb {
	const void *pointer;
};

enum {
	DRM_FLIPPER_RUN_CMDS		= 0x00,
	DRM_FLIPPER_WRITE_TEX_MEM	= 0x01,
	DRM_FLIPPER_MAP_EFB		= 0x02,
	DRM_FLIPPER_NUM_IOCTLS		= 0x03,
};

/* Note: this is an enum so that it can be resolved by Rust bindgen. */
enum {
	DRM_IOCTL_FLIPPER_RUN_CMDS	= DRM_IOWR(DRM_COMMAND_BASE + DRM_FLIPPER_RUN_CMDS, struct drm_flipper_run_cmds),
	DRM_IOCTL_FLIPPER_WRITE_TEX_MEM	= DRM_IOWR(DRM_COMMAND_BASE + DRM_FLIPPER_WRITE_TEX_MEM, struct drm_flipper_write_tex_mem),
	DRM_IOCTL_FLIPPER_MAP_EFB	= DRM_IOR (DRM_COMMAND_BASE + DRM_FLIPPER_MAP_EFB, struct drm_flipper_map_efb),
};

#if defined(__cplusplus)
}
#endif

#endif /* __FLIPPER_DRM_H__ */
