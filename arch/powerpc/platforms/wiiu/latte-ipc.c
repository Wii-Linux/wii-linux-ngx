// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii U LatteIPC support code
 *
 * Copyright (C) 2024 The linux-wiiu Team
 */

#include <mm/mmu_decl.h>

#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <asm/prom.h>
#include <asm/fixmap.h>
#include <asm/udbg.h>

#include "latte-ipc.h"

typedef struct __attribute__((packed)) {
	__be32 ppcmsg;		/* Any-purpose u32 mailbox (""ppc side"") */
	__be32 ppcctrl;		/* Control+flags (ppc side) */
	__be32 iopmsg;		/* Any-purpose u32 mailbox (""ppc side"") */
	__be32 iopctrl;		/* Control+flags (iop side, but we can still access it) */
} lt_ipc_t;

#define LT_MMIO_BASE ((phys_addr_t)0x0d800000)
#define LT_IPC_PPCMSG 0x00
#define LT_IPC_PPCCTRL 0x04

#define LT_IPC_PPCCTRL_X1  0x01
#define LT_IPC_PPCCTRL_Y1  0x04
#define LT_IPC_PPCCTRL_IY1 0x10
#define LT_IPC_IOPCTRL_Y1  0x01

// todo: convert wood to lt_ipc_t
static void __iomem *latteipc_io_wood_base;
static DEFINE_PER_CPU(lt_ipc_t *, latteipc_io_latte);

/*
 * Sends a message to the Starbuck. This uses the legacy/Wood IPC, since we're keeping the per-core IPCs for ourselves.
 */
void latteipc_starbuck_msg(u32 msg) {
	void __iomem *ppcmsg_reg = latteipc_io_wood_base + LT_IPC_PPCMSG;
	void __iomem *ppcctrl_reg = latteipc_io_wood_base + LT_IPC_PPCCTRL;

	out_be32(ppcmsg_reg, msg);
	out_be32(ppcctrl_reg, LT_IPC_PPCCTRL_X1);

	while (in_be32(ppcctrl_reg) & LT_IPC_PPCCTRL_X1)
		barrier();
}

/*
 * Causes an interrupt on another CPU. We'll use the IPC Y1 flag for this and just pretend to be the Starbuck
 * to trigger it.
 */
void latteipc_ipi_cpu(int cpu) {
	lt_ipc_t *target_ipc = per_cpu(latteipc_io_latte, cpu);
	printk("%s: ipi to %d\n", __func__, cpu);
	setbits32(&target_ipc->iopctrl, LT_IPC_IOPCTRL_Y1);
}

static irqreturn_t latteipc_ipi_intr(int irq, void *d) {
	lt_ipc_t *ipc = this_cpu_ptr(latteipc_io_latte);
	// ack the irq
	setbits32(&ipc->ppcctrl, LT_IPC_PPCCTRL_Y1);

#ifdef CONFIG_SMP
	smp_ipi_demux();
#endif
	return IRQ_HANDLED;
}

void __init latteipc_setup_ipis(void) {
	unsigned cpu;
	int irq, err;
	struct device_node *np;
	unsigned long res;

	np = of_find_compatible_node(NULL, NULL, "nintendo,latte-ipc");
	if (!np) {
		udbg_printf("%s: IPC node not found\n", __func__);
		return;
	}

	for_each_present_cpu (cpu) {
		lt_ipc_t **ipc = per_cpu_ptr(&latteipc_io_latte, cpu);

		pr_info("starting IPC on %08x (cpu %d)\n", (unsigned)*ipc, cpu);
		// Ack and enable interrupts for Y1
		setbits32(&(*ipc)->ppcctrl, LT_IPC_PPCCTRL_IY1 | LT_IPC_PPCCTRL_Y1);
		res = ioread32(&(*ipc)->ppcctrl);

		irq = irq_of_parse_and_map(np, cpu + 1); // +1 to skip over compat-mode IRQ
		if (!irq) {
			pr_err("%s: Failed to map IRQ!\n", __func__);
			goto out;
		}

		pr_info("requesting IRQ #%d (%08lx)\n", irq, res);

		err = request_irq(irq, latteipc_ipi_intr, IRQF_PERCPU | IRQF_NO_THREAD | IRQF_NO_SUSPEND, "IPI", NULL);
		WARN(err < 0, "unable to request IPI %d for core %u\n", irq, cpu);
	}

	pr_info("<- %s\n", __func__);

out:
	of_node_put(np);
}

/*
 * Maps and initialises the IPC controller hardware.
 */
static void __init latteipc_setup_ipc_dev(struct device_node *np)
{
	void __iomem *wood_io_base, *latte_io_base = NULL;
	phys_addr_t wood_paddr, latte_paddr;
	const unsigned int *reg;
	int reg_len;
	unsigned cpu;

	reg = of_get_property(np, "reg", &reg_len);
	if (!reg || reg_len != sizeof(reg[0]) * 4) {
		pr_err("%s: incorrect reg on latte-ipc node!\n", __func__);
		goto out;
	}

	wood_paddr = of_translate_address(np, &reg[0]);
	latte_paddr = of_translate_address(np, &reg[2]);
	if (!wood_paddr || !latte_paddr) {
		pr_err("%s: couldn't map registers!\n", __func__);
		goto out;
	}

	wood_io_base = ioremap(wood_paddr, reg[1]);
	latte_io_base = ioremap(latte_paddr, reg[3]);
	if (!wood_io_base || !latte_io_base) {
		pr_err("%s: couldn't map registers!\n", __func__);
		goto out;
	}

	// only commit once we have good ones
	latteipc_io_wood_base = wood_io_base;

	for_each_present_cpu(cpu) {
		lt_ipc_t **ipc = per_cpu_ptr(&latteipc_io_latte, cpu);

		//Compute pic address
		*ipc = latte_io_base + (sizeof(lt_ipc_t) * cpu);
		pr_info("latte ipc for cpu %u at %08X (%08x)\n", cpu, (unsigned)*ipc, latte_paddr + (sizeof(lt_ipc_t) * cpu));
	}

out:
	return;
}

/*
 * Latte IPC support initialization.
 */
void __init latteipc_init(void)
{
	struct device_node *np;

	if (latteipc_io_wood_base)
		udbg_printf("%s: early -> final\n", __func__);

	np = of_find_compatible_node(NULL, NULL, "nintendo,latte-ipc");
	if (!np) {
		udbg_printf("%s: IPC node not found\n", __func__);
		goto out;
	}

	latteipc_setup_ipc_dev(np);

	of_node_put(np);
out:
	return;
}

#ifdef CONFIG_PPC_EARLY_DEBUG_LATTEIPC

/*
 * Initialise latteipc early, to support udbg bootconsole on Starbuck.
 */
void __init latteipc_init_early(void)
{
	/*
	 * At this point we have a BAT already setup that enables I/O
	 * to the IPC hardware.
	 *
	 * The BAT uses a virtual address range reserved at the fixmap.
	 * This must match the virtual address configured in
	 * head_32.S:setup_latteipc_bat().
	 */
	latteipc_io_wood_base = (void __iomem *)__fix_to_virt(FIX_EARLY_DEBUG_BASE);

	/*
	 * Prepare again the same BAT for MMU_init.
	 * This allows udbg I/O to continue working after the MMU is
	 * turned on for real.
	 * It is safe to continue using the same virtual address as it is
	 * a reserved fixmap area.
	 */
	setbat(1, (unsigned long)latteipc_io_wood_base, LT_MMIO_BASE, 128 * 1024,
	       PAGE_KERNEL_NCG);
}

#endif /* CONFIG_PPC_EARLY_DEBUG_LATTEIPC */
