/* SPDX-License-Identifier: MIT */
#ifndef USB_HOST_HLWD_URB
#define USB_HOST_HLWD_URB

#include "linux/types.h"
struct usb_hcd;
struct urb;

int hlwd_map_urb_for_dma(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags);
void hlwd_unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb);

#endif
