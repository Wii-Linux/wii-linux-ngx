// SPDX-License-Identifier: MIT
/*
 * The Nintendo Hollywood SoC has 3 usb HCDs:
 * - EHCI with 4 ports (2 external, 2 internal)
 * - first companion OHCI with 2 ports (external)
 * - second companion OHCI with 2 ports (internal)
 *   - first port is connected to a usb 1.1 bluetooth daughtercard
 *   - second port is either not connected or fake
 *
 * With experimentation it was found that:
 * - using non 32-byte aligned TDs causes "hangs" (gets stuck in that TD)
 * - using non 32-byte aligned buffers causes reads/writes in the wrong place
 * - writing non 32-bit values corrupts dma memory (uncached MEM1/MEM2)
 * - TODO from the ARM cpu non 32-bit writes corrupt uncached MEM1, but MEM2
 *   works fine. Can the usb hardware corrupt buffer data in MEM1?
 *
 * For usb to work properly with linux internals:
 * 1) usb dma addresses must be 32-byte aligned
 * 2) hardware communication structures must use uncached 32-bit accesses
 * 3) hcd buffers and hcd-private variables must use kernel memory (cached)
 *
 * Since some internal parts of linux don't respect the minimum dma alignment,
 * the functions `hlwd_(un)map_urb_for_dma` need to be used.
 *
 * If everything is aligned or pio is used (root hub?), it will call the
 * standard `usb_hcd_(un)map_urb_for_dma` functions.
 *
 * If any unaligned data is found, it will create temporary buffers to bounce
 * the data and mark the urb with `URB_ALIGNED_TEMP_BUFFER` for unmapping.
 *
 * TODO is it possible to create a dma_map_ops with equivalent behaviour?
 */

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "hlwd-urb.h"

#define HLWD_URB_MINALIGN 32

/** Print debug messages. */
static const bool HLWD_URB_DEBUG = false;
/** Print the stack so unaligned data can be traced back to the allocation. */
static const bool HLWD_URB_DEBUG_STACK = false;

#define DEBUG_EXPR(expr) if (HLWD_URB_DEBUG) expr
#define DEBUG_STACK() if (HLWD_URB_DEBUG_STACK) dump_stack()

/*
 * Back data of a temporary buffer.
 *
 * This data is placed at the end of the temporary buffer.
 * It contains the original values that will be restored.
 */
struct hlwd_tmpbuf {
	size_t size;
	unsigned char *buffer;
	dma_addr_t dma;
	int num_sgs;
	struct scatterlist *sg;
};

static size_t hlwd_tmpbuf_size(size_t size, size_t align)
{
	BUG_ON(!align || (align & (align - 1))); /* must be power of 2 */
	return ALIGN(size + sizeof(struct hlwd_tmpbuf), max(align, __alignof__(struct hlwd_tmpbuf)));
}

static struct hlwd_tmpbuf *hlwd_tmpbuf_struct(void *buffer, size_t alloc_size)
{
	BUG_ON(!buffer);
	return (struct hlwd_tmpbuf *)(buffer + alloc_size - sizeof(struct hlwd_tmpbuf));
}

static void hlwd_print_sgs(unsigned char *prefix, struct scatterlist *sg, int num_sgs, size_t print_size)
{
	size_t size;
	int i;
	if (!sg)
		return;
	num_sgs = max(1, num_sgs);
	size = 0;
	for_each_sg(sg, sg, num_sgs, i) {
		if (size < print_size) {
			printk(">   sg[%d]\n", i);
			print_hex_dump("", prefix, DUMP_PREFIX_ADDRESS, 16, 1, sg_virt(sg), min(sg->length, print_size - size), true);
			size += sg->length;
		}
	}
	print_size = min(print_size, size);
	printk(">   %zu/%zu\n", print_size, size);
}

static void hlwd_print_buffer(unsigned char *prefix, unsigned char *buffer, size_t size, size_t print_size)
{
	if (!buffer)
		return;
	print_size = min(print_size, size);
	print_hex_dump("", prefix, DUMP_PREFIX_ADDRESS, 16, 1, buffer, print_size, true);
	printk(">   %zu/%zu\n", print_size, size);
}

static bool hlwd_tmpbuf_map(
	struct hlwd_tmpbuf *self,
	struct device *dev,
	gfp_t mem_flags,
	enum dma_data_direction dir,
	size_t align)
{
	size_t sg_size;
	size_t size;
	int num_sgs;
	struct scatterlist *sg;
	int i;
	size_t alloc_size;
	unsigned char *tmpbuf;

	/* make sure linux is configured with compatible alignments */
	BUILD_BUG_ON(ARCH_DMA_MINALIGN < HLWD_URB_MINALIGN);
	BUILD_BUG_ON(ARCH_KMALLOC_MINALIGN < HLWD_URB_MINALIGN);
	BUILD_BUG_ON(ARCH_SLAB_MINALIGN < HLWD_URB_MINALIGN);

	DEBUG_EXPR(printk(">  hlwd_tmpbuf_map\n"));
	BUG_ON(!self);
	BUG_ON(!dev);
	if (self->num_sgs) {
		BUG_ON(!self->sg);
		sg_size = 0;
		num_sgs = max(1, self->num_sgs);
		for_each_sg(self->sg, sg, num_sgs, i) {
			sg_size += sg->length;
		}
		DEBUG_EXPR(printk(">   sg_size=%zu\n", sg_size));
		size = min(self->size, sg_size);
	} else {
		size = self->size;
	}
	DEBUG_EXPR(printk(">   size=%zu\n", size));
	alloc_size = hlwd_tmpbuf_size(size, align);
	DEBUG_EXPR(printk(">   alloc_size=%zu\n", alloc_size));
	tmpbuf = kzalloc(alloc_size, mem_flags);
	if (!tmpbuf) {
		DEBUG_EXPR(printk(">   alloc failed\n"));
		goto err_alloc;
	}
	if (!IS_ALIGNED((size_t)tmpbuf, align)) {
		DEBUG_EXPR(printk(">   not aligned tmpbuf=%px\n", tmpbuf));
		goto err_aligned;
	}
	/* copy original request */
	*hlwd_tmpbuf_struct(tmpbuf, alloc_size) = *self;
	if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
		/* copy data */
		if (self->num_sgs) {
			BUG_ON(!self->sg);
			DEBUG_EXPR(printk(">   copy from sgs\n"));
			sg_size = sg_copy_to_buffer(self->sg, self->num_sgs, tmpbuf, size);
			DEBUG_EXPR(printk(">   sg_size=%zu\n", sg_size));
			BUG_ON(size != sg_size);
		} else if (self->sg) {
			DEBUG_EXPR(printk(">   copy from sg\n"));
			memcpy(tmpbuf, sg_virt(self->sg), size);
		} else if (self->buffer) {
			DEBUG_EXPR(printk(">   copy from buffer\n"));
			memcpy(tmpbuf, self->buffer, size);
		}
	}
	DEBUG_EXPR(print_hex_dump("", ">   tmpbuf: ", DUMP_PREFIX_ADDRESS, 16, 1, tmpbuf, alloc_size, true));
	self->size = size;
	self->buffer = tmpbuf;
	self->num_sgs = 0;
	self->sg = NULL;
	self->dma = dma_map_single(dev, tmpbuf, size, dir);
	if (dma_mapping_error(dev, self->dma)) {
		DEBUG_EXPR(printk(">   map failed\n"));
		goto err_map;
	}
	return true;
err_map:
	*self = *hlwd_tmpbuf_struct(tmpbuf, alloc_size);
err_aligned:
	kfree(tmpbuf);
err_alloc:
	return false;
}

static void hlwd_tmpbuf_unmap(
	struct hlwd_tmpbuf *self,
	struct device *dev,
	enum dma_data_direction dir,
	size_t align)
{
	size_t alloc_size;
	unsigned char *tmpbuf;
	struct hlwd_tmpbuf *original;
	int num_sgs;
	size_t sg_size;

	DEBUG_EXPR(printk(">  hlwd_tmpbuf_unmap\n"));
	BUG_ON(!self);
	BUG_ON(!dev);
	dma_unmap_single(dev, self->dma, self->size, dir);
	alloc_size = hlwd_tmpbuf_size(self->size, align);
	tmpbuf = self->buffer;
	DEBUG_EXPR(print_hex_dump("", ">   tmpbuf: ", DUMP_PREFIX_ADDRESS, 16, 1, tmpbuf, alloc_size, true));
	original = hlwd_tmpbuf_struct(self->buffer, alloc_size);
	if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
		/* copy data */
		if (original->num_sgs) {
			BUG_ON(!original->sg);
			DEBUG_EXPR(printk(">   copy to sgs\n"));
			sg_size = sg_copy_from_buffer(original->sg, num_sgs, self->buffer, self->size);
			BUG_ON(sg_size != self->size);
		} else if (original->sg) {
			DEBUG_EXPR(printk(">   copy to sg\n"));
			memcpy(sg_virt(original->sg), self->buffer, self->size);
		} else if (original->buffer) {
			DEBUG_EXPR(printk(">   copy to buffer\n"));
			memcpy(original->buffer, self->buffer, self->size);
		}
	}
	*self = *original;
	kfree(tmpbuf);
}

static bool hlwd_urb_is_aligned(struct urb *urb)
{
	struct scatterlist *sg;
	bool is_aligned = true;
	int i;

	DEBUG_EXPR(printk(">  hlwd_urb_is_aligned urb=%px\n", urb));
	BUG_ON(!urb);
	is_aligned = is_aligned && IS_ALIGNED((size_t)urb->setup_packet, HLWD_URB_MINALIGN);
	DEBUG_EXPR(printk(">   is_aligned=%d urb->setup_packet=%px\n", is_aligned, urb->setup_packet));
	if (urb->num_sgs) {
		/* sgs */
		BUG_ON(!urb->sg);
		for_each_sg(urb->sg, sg, urb->num_sgs, i) {
			is_aligned = is_aligned && IS_ALIGNED((size_t)sg_virt(sg), HLWD_URB_MINALIGN);
			DEBUG_EXPR(printk(">   is_aligned=%d sg_virt(urb->sg[%d])=%px\n", is_aligned, i, sg_virt(sg)));
		}
	} else if (urb->sg) {
		/* sg */
		is_aligned = is_aligned && IS_ALIGNED((size_t)sg_virt(urb->sg), HLWD_URB_MINALIGN);
		DEBUG_EXPR(printk(">   is_aligned=%d sg_virt(urb->sg)=%px\n", is_aligned, sg_virt(urb->sg)));
	} else if (urb->transfer_buffer) {
		/* buffer */
		is_aligned = is_aligned && IS_ALIGNED((size_t)urb->transfer_buffer, HLWD_URB_MINALIGN);
		DEBUG_EXPR(printk(">   is_aligned=%d urb->transfer_buffer=%px\n", is_aligned, urb->transfer_buffer));
	}
	DEBUG_EXPR(printk(">   is_aligned=%d to %d bytes\n", is_aligned, HLWD_URB_MINALIGN));
	return is_aligned;
}

/** Function for `struct hc_driver.map_urb_for_dma`.
 *
 * 1) If the data is 32-byte aligned, it tries to delegate to the default
 *    implementation `usb_hcd_map_urb_for_dma`.
 *
 * 2) If step 1 fails it bounces and maps all data with aligned temporary
 *    buffers, marking the urb with flag `URB_ALIGNED_TEMP_BUFFER`.
 */
int hlwd_map_urb_for_dma(
	struct usb_hcd *hcd,
	struct urb *urb,
	gfp_t mem_flags
) {
	struct device *dev;
	struct hlwd_tmpbuf tmp;
	enum dma_data_direction dir;
	int ret;

	BUILD_BUG_ON(!IS_ENABLED(CONFIG_HAS_DMA));
	BUG_ON(!hcd->self.uses_dma); /* needs dma in internal structures */

	dev = hcd->self.sysdev;
	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	DEBUG_EXPR({
		printk("> hlwd_map_urb_for_dma: %s hcd=%px(%s) urb=%px(%s) mem_flags=%pGg\n", dev_name(dev), hcd, hcd->irq_descr, urb, dev_name(&urb->dev->dev), &mem_flags);
		if (urb->setup_packet) {
			hlwd_print_buffer(">   setup: ", urb->setup_packet, sizeof(struct usb_ctrlrequest), 0xffffffff);
		}
		if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
			if (urb->num_sgs) {
				BUG_ON(!urb->sg);
				hlwd_print_sgs(">   sgs: ", urb->sg, urb->num_sgs, 0xffffffff);
			} if (urb->sg) {
				hlwd_print_buffer(">   sg: ", sg_virt(urb->sg), urb->transfer_buffer_length, 0xffffffff);
			} else if (urb->transfer_buffer) {
				hlwd_print_buffer(">   buffer: ", urb->transfer_buffer, urb->transfer_buffer_length, 0xffffffff);
			}
		}
		printk(">   hcd->self.uses_pio_for_control=%d\n", hcd->self.uses_pio_for_control);
		printk(">  urb->setup_packet=%px\n", urb->setup_packet);
		printk(">  urb->setup_dma=%pad\n", &urb->setup_dma);
		printk(">  urb->transfer_buffer_length=%u\n", urb->transfer_buffer_length);
		printk(">  urb->num_sgs=%d\n", urb->num_sgs);
		printk(">  urb->sg=%px\n", urb->sg);
		printk(">  urb->transfer_buffer=%px\n", urb->transfer_buffer);
		printk(">  urb->transfer_dma=%pad\n", &urb->transfer_dma);
		printk(">  urb->transfer_flags=0x%x\n", urb->transfer_flags);
		DEBUG_STACK(); /* see where the data came from to fix alignment */
	});
	if (hcd->self.uses_pio_for_control || hlwd_urb_is_aligned(urb)) {
		DEBUG_EXPR(printk(">   default\n"));
		ret = usb_hcd_map_urb_for_dma(hcd, urb, mem_flags);
		if (ret == 0)
			return 0; /* done */
		DEBUG_EXPR(printk(">   ret=%d\n", ret));
	}
	if (urb->setup_packet) {
		DEBUG_EXPR(printk(">   tmp setup\n"));
		tmp = (struct hlwd_tmpbuf) {
			.size = sizeof(struct usb_ctrlrequest),
			.buffer = urb->setup_packet,
			.dma = urb->setup_dma,
		};
		if (!hlwd_tmpbuf_map(&tmp, dev, mem_flags, DMA_TO_DEVICE, HLWD_URB_MINALIGN)) {
			DEBUG_EXPR(printk(">  failed setup\n"));
			return -ENOMEM;
		}
		urb->setup_packet = tmp.buffer;
		urb->setup_dma = tmp.dma;
	}
	urb->transfer_flags &= ~URB_NO_TRANSFER_DMA_MAP; /* always map */
	if (urb->num_sgs || urb->sg || urb->transfer_buffer) {
		DEBUG_EXPR(printk(">   tmp sgs/sg/transfer"));
		tmp = (struct hlwd_tmpbuf) {
			.size = urb->transfer_buffer_length,
			.buffer = urb->transfer_buffer,
			.dma = urb->transfer_dma,
			.num_sgs = urb->num_sgs,
			.sg = urb->sg,
		};
		if (!hlwd_tmpbuf_map(&tmp, dev, mem_flags, dir, HLWD_URB_MINALIGN)) {
			DEBUG_EXPR(printk(">  failed transfer\n"));
			if (urb->setup_packet) {
				tmp = (struct hlwd_tmpbuf) {
					.size = sizeof(struct usb_ctrlrequest),
					.buffer = urb->setup_packet,
					.dma = urb->setup_dma,
				};
				hlwd_tmpbuf_unmap(&tmp, dev, DMA_TO_DEVICE, HLWD_URB_MINALIGN);
				urb->setup_packet = tmp.buffer;
				urb->setup_dma = tmp.dma;
			}
			return -ENOMEM;
		}
		urb->transfer_buffer_length = tmp.size;
		urb->transfer_buffer = tmp.buffer;
		urb->transfer_dma = tmp.dma;
		urb->num_sgs = 0;
		urb->sg = NULL;
	}
	urb->transfer_flags |= URB_ALIGNED_TEMP_BUFFER;
	DEBUG_EXPR({
		printk("> after:\n");
		printk(">  urb->setup_packet=%p\n", urb->setup_packet);
		printk(">  urb->setup_dma=%pad\n", &urb->setup_dma);
		printk(">  urb->transfer_buffer_length=%u\n", urb->transfer_buffer_length);
		printk(">  urb->num_sgs=%d\n", urb->num_sgs);
		printk(">  urb->sg=%px\n", urb->sg);
		printk(">  urb->transfer_buffer=%p\n", urb->transfer_buffer);
		printk(">  urb->transfer_dma=%pad\n", &urb->transfer_dma);
		printk(">  urb->transfer_flags=0x%x\n", urb->transfer_flags);
	})
	return 0;
}

/** Function for `struct hc_driver.unmap_urb_for_dma`.
 *
 * 1) If the temporary buffers were not used, it delegates to the default
 *    implementation `usb_hcd_unmap_urb_for_dma`.
 *
 * 2) Otherwise it unmaps and unbounces the data in aligned temporary buffers,
 *    The urb must have flag `URB_ALIGNED_TEMP_BUFFER`.
 */
void hlwd_unmap_urb_for_dma(struct usb_hcd *hcd, struct urb *urb)
{
	struct device *dev;
	enum dma_data_direction dir;
	struct hlwd_tmpbuf tmp;

	dev = hcd->self.sysdev;
	dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	DEBUG_EXPR(printk("> hlwd_unmap_urb_for_dma: %s hcd=%p(%s) urb=%p(%s)\n", dev_name(dev), hcd, hcd->irq_descr, urb, dev_name(&urb->dev->dev)));
	if (!(urb->transfer_flags & URB_ALIGNED_TEMP_BUFFER)) {
		DEBUG_EXPR(printk(">  default\n"));
		usb_hcd_unmap_urb_for_dma(hcd, urb);
		goto done;
	}
	DEBUG_EXPR({
		printk("> tmp\n");
		printk(">  urb->setup_packet=%p\n", urb->setup_packet);
		printk(">  urb->setup_dma=%pad\n", &urb->setup_dma);
		printk(">  urb->transfer_buffer_length=%u\n", urb->transfer_buffer_length);
		printk(">  urb->num_sgs=%d\n", urb->num_sgs);
		printk(">  urb->sg=%px\n", urb->sg);
		printk(">  urb->transfer_buffer=%p\n", urb->transfer_buffer);
		printk(">  urb->transfer_dma=%pad\n", &urb->transfer_dma);
		printk(">  urb->transfer_flags=0x%x\n", urb->transfer_flags);
	});
	if (urb->setup_packet) {
		tmp = (struct hlwd_tmpbuf) {
			.size = sizeof(struct usb_ctrlrequest),
			.buffer = urb->setup_packet,
			.dma = urb->setup_dma,
		};
		hlwd_tmpbuf_unmap(&tmp, dev, DMA_TO_DEVICE, HLWD_URB_MINALIGN);
		urb->setup_packet = tmp.buffer;
		urb->setup_dma = tmp.dma;
	}
	if (urb->transfer_buffer) {
		tmp = (struct hlwd_tmpbuf) {
			.size = urb->transfer_buffer_length,
			.buffer = urb->transfer_buffer,
			.dma = urb->transfer_dma,
		};
		hlwd_tmpbuf_unmap(&tmp, dev, dir, HLWD_URB_MINALIGN);
		urb->transfer_buffer_length = tmp.size;
		urb->transfer_buffer = tmp.buffer;
		urb->transfer_dma = tmp.dma;
		urb->num_sgs = tmp.num_sgs;
		urb->sg = tmp.sg;
	}
	urb->transfer_flags &= ~URB_ALIGNED_TEMP_BUFFER;
	DEBUG_EXPR({
		printk("> after:\n");
		printk(">  urb->setup_packet=%p\n", urb->setup_packet);
		printk(">  urb->setup_dma=%pad\n", &urb->setup_dma);
		printk(">  urb->transfer_buffer_length=%u\n", urb->transfer_buffer_length);
		printk(">  urb->num_sgs=%d\n", urb->num_sgs);
		printk(">  urb->sg=%px\n", urb->sg);
		printk(">  urb->transfer_buffer=%p\n", urb->transfer_buffer);
		printk(">  urb->transfer_dma=%pad\n", &urb->transfer_dma);
		printk(">  urb->transfer_flags=0x%x\n", urb->transfer_flags);
	});
done:
	DEBUG_EXPR({
		printk(">  urb->actual_length=%zu\n", urb->actual_length);
		if (dir == DMA_FROM_DEVICE) {
			if (urb->num_sgs) {
				BUG_ON(!urb->sg);
				hlwd_print_sgs(">   sgs: ", urb->sg, urb->num_sgs, urb->actual_length);
			} else if (urb->sg) {
				hlwd_print_buffer(">   sg: ", sg_virt(urb->sg), urb->transfer_buffer_length, urb->actual_length);
			} else if (urb->transfer_buffer) {
				hlwd_print_buffer(">   buffer: ", urb->transfer_buffer, urb->transfer_buffer_length, urb->actual_length);
			}
		}
	});
}
