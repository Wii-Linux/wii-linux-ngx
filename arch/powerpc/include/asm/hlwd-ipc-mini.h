/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nintendo Wii "Hollywood" IPC support for the "mini" custom firmware.
 * Copyright (C) 2025-2026 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
 *
 * Derived from BootMii ppcskel's 'ipc.h':
 * Copyright (C) 2008, 2009	Haxx Enterprises <bushing@gmail.com>
 * Copyright (C) 2008, 2009	Hector Martin "marcan" <marcan@marcansoft.com>
 * Copyright (C) 2009		Andre Heider "dhewg" <dhewg@wiibrew.org>
 * Copyright (C) 2009		John Kelley <wiidev@kelley.ca>
 * Copyright (C) 2008, 2009	Sven Peter <svenpeter@gmail.com>
 */

#ifndef __HLWD_IPC_MINI_H
#define __HLWD_IPC_MINI_H
#include <linux/types.h>
#include <linux/stdarg.h>
#include "hlwd-ipc.h"

/*
 * IPC codes
 */
#define IPC_MINI_CODE_SLWPING       0x00000000
#define IPC_MINI_CODE_PING          0x01000000
#define IPC_MINI_CODE_GETVERS       0x00000002

#define IPC_MINI_CODE_PPC_BOOT_MEM  0x00060000
#define IPC_MINI_CODE_PPC_BOOT_FILE 0x00060001

/* only supported in downstream versions of MINI */
#define IPC_MINI_CODE_PM_SUPP       0x00800000
#define IPC_MINI_CODE_PM_SUSPEND    0x00800001

#define IPC_MINI_PM_MAGIC           0x574c6e78 /* ASCII 'WLnx' */

/* IPC_MINI_CODE_PM_SUSPEND wake events, and the reply's reason */
#define IPC_MINI_PM_WAKE_POWER      BIT(0)  /* power button */
#define IPC_MINI_PM_WAKE_TIMEOUT    BIT(1)  /* timeout elapsed */
/* IPC_MINI_CODE_PM_SUSPEND options (high 16 bits of args[2]) */
#define IPC_MINI_PM_OPT_LED_ON      BIT(0)
#define IPC_MINI_PM_OPT_LED_BLINK   BIT(1)
#define IPC_MINI_PM_OPT_KEEP_FAN    BIT(2)  /* MINI's default, like Nintendo's, is the fan off */
#define IPC_MINI_PM_OPT_CLR_NORESET BIT(3)


/*
 * IPC request structure
 */
struct ipc_request_mini {
	union {
		struct {
			u8 flags;
			u8 device;
			u16 req;
		};
		u32 code;
	};
	u32 tag;
	u32 args[6];
};


/*
 * Initialize MINI IPC interface, only meant to be called by
 * the IPC core.
 */
int ipc_init_mini(struct hlwd_ipc *ipc);

/* Release all MINI-specific mappings and state. */
void ipc_cleanup_mini(struct hlwd_ipc *ipc);

/*
 * Post a message w/ va_args.  See ipc_post_mini.
 */
int ipc_vpost_mini(u32 code, u32 tag, int num_args, va_list args);

/*
 * Post a message to Starlet.
 *
 * Returns -ETIMEDOUT if Starlet is stuck for too long.
 * Returns -EINVAL if not using MINI, or IPC not initialized.
 * Returns 0 on success.
 */
int ipc_post_mini(u32 code, u32 tag, int num_args, ...);

/*
 * Like ipc_post_mini(), but Starlet is not told about the message.  That is
 * done later by writing the value that ipc_kick_reg_mini() gives to the
 * register that it returns (NULL if not using MINI).  For the PM request
 * that needs to be sent after the caches have been turned off.
 */
int ipc_post_mini_nokick(u32 code, u32 tag, int num_args, ...);
u32 __iomem *ipc_kick_reg_mini(u32 *val);

/*
 * Receive a message from Starlet, filling in 'req' with
 * the message that has been received.
 *
 * Returns -ETIMEDOUT if Starlet is stuck for too long.
 * Returns -EINVAL if not using MINI, or IPC not initialized.
 * Returns 0 on success.
 */
int ipc_receive_mini(struct ipc_request_mini *req, int max_attempts);

/*
 * Receive a message from Starlet, whose code and tag match
 * the provided parameters, filling in 'req' with the
 * message that has been received.
 *
 * 'max_recv_attempts' is the 'max_attempts' of the ipc_receive_mini call.
 * 'max_attempts' is how many times we wait for the message, if we ended up
 * receiving some other message.
 *
 * Returns -ETIMEDOUT if Starlet is stuck for too long.
 * Returns -EINVAL if not using MINI, or IPC not initialized.
 * Returns 0 on success.
 */
int ipc_receive_tagged_mini(struct ipc_request_mini *req, u32 code, u32 tag, int max_recv_attempts, int max_attempts);

/*
 * Exchange a message with Starlet, filling in 'req' with
 * the message that has been received.
 *
 * Returns -ETIMEDOUT if Starlet is stuck for too long,
 *   for either receiving the message, or replying.
 * Returns -EINVAL if not using MINI, or IPC not initialized.
 * Returns 0 on success for both send and receive.
 */
int ipc_exchange_mini(struct ipc_request_mini *req, u32 code, int max_recv_attempts, int max_attempts, int num_args, ...);

/*
 * Get the tag that the next ipc_exchange_mini() would use, for callers that
 * need to split the exchange into a post and a later receive.  Call
 * ipc_put_tag_mini() when the receive is done.
 */
u32 ipc_get_tag_mini(void);
void ipc_put_tag_mini(void);

#endif
