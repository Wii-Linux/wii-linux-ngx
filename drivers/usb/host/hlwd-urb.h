/* SPDX-License-Identifier: MIT */
#ifndef USB_HOST_HLWD_URB
#define USB_HOST_HLWD_URB

#include "linux/types.h"
struct usb_hcd;
struct urb;
struct device;

/*
 * Initialise the shared cached-MEM2 bounce pool from the HCD's second
 * "memory-region" phandle. Call once per controller at probe (process
 * context); first caller wins, the rest are no-ops.
 */
int hlwd_bounce_pool_init(struct device *dev);

int hlwd_map_urb_for_dma(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags);
void hlwd_unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb);

#endif
