/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Suspend support for a fully powered off CPU.
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 */
#ifndef _ASM_POWERPC_CPUOFF_H
#define _ASM_POWERPC_CPUOFF_H

/**
 * ppc_cpuoff_enter - save the CPU state, power off, and restore it on resume
 * @prepare: Optional.  Called with interrupts off and caches on, to do
 *           whatever is needed ahead of the transition.  May not sleep.
 *           Returns 0 to go on, an error to abort.
 * @final:   Required.  Called with interrupts off, after the data caches have
 *           been flushed to RAM and turned off.  Should make the CPU lose
 *           power, such that it is later restarted at ppc_cpuoff_resume_phys(),
 *           and never return in that case.  If it does return, the transition
 *           did not happen (give it a timeout) and it returns an error.
 *           Since the caches are off, stores to RAM from here go straight to
 *           it (some problems have problems with this).  Data that @final
 *           modifies in RAM must not be needed after the resume.
 * @arg:     Passed to both @prepare and @final.
 *
 * Must be called with interrupts disabled, on the only online CPU.
 *
 * Returns 0 after a resume, or the error from @prepare or @final if the
 * transition did not happen, with the CPU state and caches as they were.
 */
int ppc_cpuoff_enter(int (*prepare)(void *arg), int (*final)(void *arg),
		     void *arg);

/**
 * ppc_cpuoff_resume_phys - physical address to restart the CPU at
 *
 * All that is required of whatever restarts the CPU is to start execution
 * here, in big-endian mode, with a vaguely sane CPU state.  The MMU, caches,
 * etc, can be in any state.  RAM must have kept its contents.
 */
unsigned long ppc_cpuoff_resume_phys(void);

/* The low-level suspend procedure; don't use this, use ppc_cpuoff_enter(). */
int ppc_cpuoff_suspend(int (*prepare)(void *arg), int (*final)(void *arg),
		       void *arg);

#endif /* _ASM_POWERPC_CPUOFF_H */
