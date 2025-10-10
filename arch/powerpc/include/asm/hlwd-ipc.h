/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nintendo Wii "Hollywood" IPC support.
 * Copyright (C) 2025 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
 */

#ifndef __HLWD_IPC_H
#define __HLWD_IPC_H
#include <linux/types.h>

struct ipc_regs {
	u32 ppcmsg;  /* General purpose data, Broadway -> Starlet */
	u32 ppcctrl; /* Flags, Broadway -> Starlet */
	u32 armmsg;  /* General purpose data, Starlet -> Broadway */
	u32 armctrl; /* Flags, Starlet -> Broadway */
};

enum ipc_flavor {
	IPC_FLAVOR_MINI,   /* fail0verflow (prev. Team Twiizers) "mini" custom firmware */
	IPC_FLAVOR_IOS,    /* Official Nintendo/BroadOn IOS firmware */
	IPC_FLAVOR_UNKNOWN /* Unknown, or perhaps Starlet crashed */
};

struct hlwd_ipc {
	struct ipc_regs __iomem* regs; /* ioremap'd hardware registers */
	enum ipc_flavor flavor;        /* detected IPC flavor */
	void *flavor_state;            /* flavor-specific state information */
};

/*
 * Used by the MINI and IOS implementations
 * to get the IPC state
 */
struct hlwd_ipc *ipc_get_state(void);

/*
 * Get the current IPC flavor
 */
enum ipc_flavor ipc_get_flavor(void);

#endif
