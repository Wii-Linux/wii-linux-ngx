/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * arch/powerpc/platforms/embedded6xx/hlwd-ipc-mini.h
 *
 * Nintendo Wii "Hollywood" IPC support for fail0verflow's "mini" custom firmware.
 * Copyright (C) 2025-2026 Michael "Techflash" Garofalo <officialTechflashYT@gmail.com>
 *
 * Derived from BootMii ppcskel's 'ipc.c':
 * Copyright (C) 2008, 2009	Hector Martin "marcan" <marcan@marcansoft.com>
 * Copyright (C) 2009		Andre Heider "dhewg" <dhewg@wiibrew.org>
 * Copyright (C) 2009		John Kelley <wiidev@kelley.ca>
 */

#define pr_fmt(fmt)	"hlwd-ipc-mini: " fmt

#include <linux/delay.h>
#include <linux/types.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <asm/hlwd-ipc.h>
#include <asm/hlwd-ipc-mini.h>

/*
 * PPCCTRL IPC flags
 * There are more for ARMCTRL, but we don't touch that register
 */
#define X1  BIT(0)
#define Y2  BIT(1)
#define Y1  BIT(2)
#define X2  BIT(3)
#define IY1 BIT(4)
#define IY2 BIT(5)

struct infohdr {
	char magic[3];
	u8 version;
	void *mem2_boundary;
	volatile struct ipc_request_mini *ipc_in;
	u32 ipc_in_size;
	volatile struct ipc_request_mini *ipc_out;
	u32 ipc_out_size;
};

struct mini_state {
	struct infohdr *infohdr;                      /* MINI infohdr */
	u16 out_head;                                 /* Out head */
	u16 in_tail;                                  /* In tail */
	int in_size;                                  /* In queue size */
	int out_size;                                 /* Out queue size */
	volatile struct ipc_request_mini *in_queue;   /* In queue pointer */
	volatile struct ipc_request_mini *out_queue;  /* Out queue pointer */
	u32 cur_tag;                                  /* Current request number ("tag") */
};

int ipc_init_mini(struct hlwd_ipc *ipc)
{
	struct device_node *mini_np;
	u32 infohdr_ptr, *prop;
	void *infohdr_ptr_mapped;
	struct infohdr *infohdr;
	struct mini_state *state;
	struct ipc_request_mini req;
	int ret, len;
	u32 ppcmsg;

	mini_np = of_find_compatible_node(NULL, NULL, "fail0verflow,mini-ipc");
	if (!mini_np) {
		pr_err("no fail0verflow,mini-ipc node found");
		return -ENODEV;
	}

	prop = (u32 *)of_get_property(mini_np, "infohdr", &len);
	if (!prop || len != sizeof(u32)) {
		pr_err("infohdr property not found or invalid\n");
		return -ENODEV;
	}

	/* try to map the memory region that holds the infohdr pointer */
	infohdr_ptr_mapped = memremap(*prop, 4, MEMREMAP_WB);
	if (!infohdr_ptr_mapped) {
		pr_err("memremap failed for infohdr ptr\n");
		return -ENOMEM;
	}

	/* read the infohdr pointer from the region we just mapped */
	infohdr_ptr = *(u32 *)infohdr_ptr_mapped;

	/* unmap the location of the infohdr pointer */
	memunmap(infohdr_ptr_mapped);

	/* try to map the actual infohdr */
	infohdr = (struct infohdr *)memremap(infohdr_ptr, sizeof(struct infohdr), MEMREMAP_WB);
	if (!infohdr) {
		pr_err("memremap failed for infohdr\n");
		return -ENOMEM;
	}

	/* valid infohdr? */
	if (memcmp(infohdr->magic, "IPC", 3) != 0) {
		pr_err("invalid MINI IPC magic");
		ret = -EINVAL;
		goto out_invalid_infohdr;
	}

	if (infohdr->version != 1) {
		pr_err("unknown IPC version %d\n", infohdr->version);
		ret = -EINVAL;
		goto out_invalid_infohdr;
	}

	/* set up our internal state */
	state = kzalloc(sizeof(struct mini_state), GFP_KERNEL);
	if (!state) {
		pr_err("state allocation failed\n");
		ret = -ENOMEM;
		goto out_invalid_infohdr;
	}

	ipc->flavor_state = state;
	state->infohdr = infohdr;
	state->in_size = infohdr->ipc_in_size;
	state->out_size = infohdr->ipc_out_size;

	/* map the in queue */
	state->in_queue = ioremap((u32)infohdr->ipc_in, state->in_size);
	if (!state->in_queue) {
		pr_err("failed to map in_queue at %p\n", infohdr->ipc_in);
		ret = -ENOMEM;
		goto out_ioremap_in;
	}

	/* map the out queue */
	state->out_queue = ioremap((u32)infohdr->ipc_out, state->out_size);
	if (!state->out_queue) {
		pr_err("failed to map out_queue at %p\n", infohdr->ipc_out);
		ret = -ENOMEM;
		goto out_ioremap_out;
	}

	/* read in_tail and out_head from IPC hardware registers */
	ppcmsg = in_be32(&ipc->regs->ppcmsg);
	state->in_tail = ppcmsg & 0xffff;
	state->out_head = ppcmsg >> 16;

	state->cur_tag = 1;

	pr_info("initial in tail: %d, out head: %d\n", state->in_tail, state->out_head);

	pr_info("running trivial tests:\n");
	ret = ipc_exchange_mini(&req, IPC_MINI_CODE_PING, 3, 1, 0);
	pr_info(" * fast ping: %d\n", ret);
	ret = ipc_exchange_mini(&req, IPC_MINI_CODE_SLWPING, 3, 1, 0);
	pr_info(" * slow ping: %d\n", ret);
	ret = ipc_exchange_mini(&req, IPC_MINI_CODE_GETVERS, 3, 1, 0);
	pr_info(" * getvers: %d (version: 0x%08x)\n", ret, req.args[0]);
	return 0;


out_ioremap_out:
	iounmap((void *)state->in_queue);
out_ioremap_in:
	memunmap(infohdr);
	kfree(state);
out_invalid_infohdr:
	return ret;
}

static u16 peek_outtail(struct hlwd_ipc *ipc)
{
	u32 val = in_be32(&ipc->regs->armmsg);
	return (u16)(val & 0xffff);
}

static u16 peek_inhead(struct hlwd_ipc *ipc)
{
	u32 val = in_be32(&ipc->regs->armmsg);
	return (u16)(val >> 16);
}

static void poke_intail(struct hlwd_ipc *ipc)
{
	u32 val;
	struct mini_state *state;

	state = (struct mini_state *)ipc->flavor_state;

	val = in_be32(&ipc->regs->ppcmsg);
	val &= 0xffff0000;
	val |= state->in_tail;
	out_be32(&ipc->regs->ppcmsg, val);
}

static void poke_outhead(struct hlwd_ipc *ipc)
{
	u32 val;
	struct mini_state *state;

	state = (struct mini_state *)ipc->flavor_state;

	val = in_be32(&ipc->regs->ppcmsg);
	val &= 0x0000ffff;
	val |= (state->out_head << 16);
	out_be32(&ipc->regs->ppcmsg, val);
}

static int inqueue_full(struct hlwd_ipc *ipc)
{
	struct mini_state *state = (struct mini_state *)ipc->flavor_state;

	return peek_inhead(ipc) == ((state->in_tail + 1) & (state->in_size - 1));
}

int ipc_vpost_mini(u32 code, u32 tag, int num_args, va_list args)
{
	struct hlwd_ipc *ipc;
	struct mini_state *state;
	int i = 0, arg = 0;

	ipc = ipc_get_state();
	if (!ipc)
		return -EINVAL;
	if (ipc->flavor != IPC_FLAVOR_MINI)
		return -EINVAL;

	state = (struct mini_state *)ipc->flavor_state;

	if (inqueue_full(ipc)) {
		pr_warn("in queue full, this might be bad...\n");
		while (inqueue_full(ipc)) {
			msleep(5);
			i++;
			if (i > 200 && i % 200 == 0) {
				pr_err("Starlet is probably stuck!  Still waiting on inhead %d != %d\n",
						peek_inhead(ipc), ((state->in_tail + 1) & (state->in_size - 1)));
			}
			/* if Starlet can't process 1 IPC message in 10 entire seconds, it's definetly hosed */
			if (i > 10000) {
				pr_crit("abandoning all hope for submitting this request, "
					"Starlet is locked up; please reboot the system to restore functionality.\n");
				return -ETIMEDOUT;
			}
		}
	}
	/* prepare our message */
	state->in_queue[state->in_tail].code = code;
	state->in_queue[state->in_tail].tag = tag;
	while (num_args--)
		state->in_queue[state->in_tail].args[arg++] = va_arg(args, u32);

	state->in_tail = (state->in_tail + 1) & (state->in_size - 1);

	/* send it off */
	poke_intail(ipc);
	out_be32(&ipc->regs->ppcctrl, X1);

	/* success, Starlet is processing it */
	return 0;
}

int ipc_post_mini(u32 code, u32 tag, int num_args, ...)
{
	va_list args;
	int ret;

	if (num_args > 0)
		va_start(args, num_args);

	ret = ipc_vpost_mini(code, tag, num_args, args);

	if (num_args > 0)
		va_end(args);

	return ret;
}

/* TODO: IRQs? */
int ipc_receive_mini(struct ipc_request_mini *req, int max_attempts)
{
	struct hlwd_ipc *ipc;
	struct mini_state *state;
	int i = 0;

	ipc = ipc_get_state();
	if (!ipc)
		return -EINVAL;
	if (ipc->flavor != IPC_FLAVOR_MINI)
		return -EINVAL;

	state = (struct mini_state *)ipc->flavor_state;

	while (peek_outtail(ipc) == state->out_head) {
		msleep(5);
		i++;
		if (i > 200 && i % 200 == 0) {
			pr_warn("Starlet might be stuck!  Still waiting on outtail %d == %d\n",
				peek_outtail(ipc), state->out_head);
		}
		if (i > 10000) {
			pr_crit("abandoning all hope for receiving this request, "
				"Starlet is locked up; please reboot the system to restore functionality.\n");
			memset(req, 0, sizeof(struct ipc_request_mini));
			return -ETIMEDOUT;
		}
		if (i > max_attempts && max_attempts > 1) {
			memset(req, 0, sizeof(struct ipc_request_mini));
			return -ETIMEDOUT;
		}
	}

	/* we got a message! */
	memcpy(req, (void *)&state->out_queue[state->out_head], sizeof(struct ipc_request_mini));

	/* update our state and hardware state accordingly */
	state->out_head = (state->out_head + 1) & (state->out_size - 1);
	poke_outhead(ipc);

	/* success, read your message */
	return 0;
}

int ipc_receive_tagged_mini(struct ipc_request_mini *req, u32 code, u32 tag, int max_recv_attempts, int max_attempts)
{
	int error = 0;

	error = ipc_receive_mini(req, max_recv_attempts);
	if (error)
		return error;
	while (req->code != code || req->tag != tag) {
		pr_warn("Got response with wrong info!  Expecting: code=%d, tag=%d; Received: code=%d, tag=%d\n", code, tag, req->code, req->tag);
		error = -ETIMEDOUT;

		max_attempts--;
		if (max_attempts <= 0)
			break;
		error = ipc_receive_mini(req, max_recv_attempts);
		if (error)
			break;
	}

	return error;
}


int ipc_exchange_mini(struct ipc_request_mini *req, u32 code, int max_recv_attempts, int max_attempts, int num_args, ...)
{
	struct hlwd_ipc *ipc;
	struct mini_state *state;
	va_list args;
	int error = 0;

	ipc = ipc_get_state();
	if (!ipc)
		return -EINVAL;
	if (ipc->flavor != IPC_FLAVOR_MINI)
		return -EINVAL;

	state = (struct mini_state *)ipc->flavor_state;
	
	if (num_args > 0)
		va_start(args, num_args);

	/* send the message */
	error = ipc_vpost_mini(code, state->cur_tag, num_args, args);
	if (error)
		goto out;

	/* get the reply */
	error = ipc_receive_tagged_mini(req, code, state->cur_tag, max_recv_attempts, max_attempts);
	if (error)
		goto out;

	/* increment our tag */
	state->cur_tag++;

	return 0;

out:
	if (num_args > 0)
		va_end(args);

	return error;
}

