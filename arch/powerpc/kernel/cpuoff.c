// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Suspend support for a fully powered off CPU.
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */

#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/irqflags.h>
#include <asm/cpuoff.h>
#include <asm/cputable.h>
#include <asm/mmu_context.h>
#include <asm/switch_to.h>

int ppc_cpuoff_enter(int (*prepare)(void *arg), int (*final)(void *arg),
		     void *arg)
{
	int ret;

	if (WARN_ON(!final))
		return -EINVAL;
	if (WARN_ON(!irqs_disabled()))
		return -EINVAL;

	/* 74xx cache flushing and extra state aren't handled */
	if (WARN_ON(cpu_has_feature(CPU_FTR_SPEC7450)))
		return -ENODEV;

	/*
	 * Everything in the thread's registers that is lazily kept in the CPU
	 * (FP, AltiVec, ...) has to go to memory, the CPU state is lost.
	 */
	flush_all_to_thread(current);

	ret = ppc_cpuoff_suspend(prepare, final, arg);

	/* segment registers and the hash table are for the current context */
	switch_mmu_context(current->active_mm, current->active_mm, NULL);

	return ret;
}
EXPORT_SYMBOL_GPL(ppc_cpuoff_enter);
