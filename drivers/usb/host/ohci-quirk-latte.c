// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/usb/host/ohci-quirk-latte.c
 *
 * Quirk for the
 *
 * Copyright (C) 2024 Logan C. <loganisamazing@outlook.com>
 * Copyright (C) 2018 Ash Logan <ash@heyquark.com>
 * Copyright (C) 2017 rw-r-r-0644
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * Based on ohci-ppc-of.c and ohci-platform.c
 *
 * This file is licenced under the GPL.
 */


static DEFINE_SPINLOCK(control_quirk_lock);

void ohci_latte_control_quirk(struct ohci_hcd *ohci) {
	static struct ed *ed; /* empty ED */
	struct td *td; /* dummy TD */
	__hc32 head;
	__hc32 current;
	unsigned long flags;

	/*
	 * One time only.
	 * Allocate and keep a special empty ED with just a dummy TD.
	 */
	if (!ed) {
		ed = ed_alloc(ohci, GFP_ATOMIC);
		if (!ed)
			return;

		td = td_alloc(ohci, GFP_ATOMIC);
		if (!td) {
			ed_free(ohci, ed);
			ed = NULL;
			return;
		}

		ed->hwNextED = 0;
		ed->hwTailP = ed->hwHeadP = cpu_to_hc32(ohci, td->td_dma & ED_MASK);
		ed->hwINFO |= cpu_to_hc32(ohci, ED_OUT);
		wmb();
	}

	spin_lock_irqsave(&control_quirk_lock, flags);

	/*
	 * The OHCI USB host controllers on the Nintendo Wii
	 * video game console stop working when new TDs are
	 * added to a scheduled control ED after a transfer has
	 * has taken place on it.
	 *
	 * Before scheduling any new control TD, we make the
	 * controller happy by always loading a special control ED
	 * with a single dummy TD and letting the controller attempt
	 * the transfer.
	 * The controller won't do anything with it, as the special
	 * ED has no TDs, but it will keep the controller from failing
	 * on the next transfer.
	 */
	head = ohci_readl(ohci, &ohci->regs->ed_controlhead);
	if (head) {
		/*
		 * Load the special empty ED and tell the controller to
		 * process the control list.
		 */
		ohci_writel(ohci, ed->dma, &ohci->regs->ed_controlhead);
		ohci_writel(ohci, ohci->hc_control | OHCI_CTRL_CLE,
			    &ohci->regs->control);
		ohci_writel(ohci, OHCI_CLF, &ohci->regs->cmdstatus);

		/* spin until the controller is done with the control list  */
		current = ohci_readl(ohci, &ohci->regs->ed_controlcurrent);
		for (u64 end = ktime_get_ns() + 10000; ktime_get_ns() < end && !current;) {
			cpu_relax();
			current = ohci_readl(ohci,
					     &ohci->regs->ed_controlcurrent);
		}

		/* restore the old control head and control settings */
		ohci_writel(ohci, ohci->hc_control, &ohci->regs->control);
		ohci_writel(ohci, head, &ohci->regs->ed_controlhead);
	}

	spin_unlock_irqrestore(&control_quirk_lock, flags);
}
