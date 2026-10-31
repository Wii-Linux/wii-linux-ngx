/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 32-bit Book3S support for suspend in which the CPU is completely powered
 * off (all registers and caches are lost) while at least RAM keeps its
 * contents.
 *
 * Something (firmware, hardware, etc) has to eventually resume the CPU at the
 * address returned by ppc_cpuoff_resume_phys().  Nothing else is assumed
 * about the state of the CPU.
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on the PowerMac sleep code (arch/powerpc/platforms/powermac/sleep.S):
 *    Copyright (C) 1999 Benjamin Herrenschmidt (benh@kernel.crashing.org)
 *    and Paul Mackerras (paulus@samba.org).
 */

#include <linux/linkage.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/page.h>
#include <asm/ppc_asm.h>
#include <asm/cputable.h>
#include <asm/cache.h>
#include <asm/thread_info.h>
#include <asm/asm-offsets.h>
#include <asm/mmu.h>
#include <asm/feature-fixups.h>

#define SL_SP		0x00
#define SL_MSR		0x04
#define SL_SDR1		0x08
#define SL_CR		0x0c
#define SL_LR		0x10
#define SL_R2		0x14
#define SL_TB		0x18	/* 8 bytes */
#define SL_HID0		0x20
#define SL_HID2		0x24
#define SL_HID4		0x28
#define SL_L2CR		0x2c
#define SL_SPRG0	0x30	/* 4 SPRGs */
#define SL_GQR0		0x40	/* 8 GQRs */
#define SL_DBAT0	0x60	/* 8 pairs of 8 bytes */
#define SL_IBAT0	0xa0	/* 8 pairs of 8 bytes */
#define SL_R12		0xe0	/* r12 to r31 */
#define SL_SIZE		(SL_R12 + 80)

#define SPRN_GQR0	912

	.section .text
	.align	5

/*
 * Disable the L2 cache.  The code must be in L1 cache for this, hence the alignment.
 * Clobbers r5
 */
SYM_FUNC_START_LOCAL(cpuoff_l2_off)
	mfspr	r5,SPRN_L2CR
	rlwinm	r5,r5,0,~L2CR_L2E
	b	21f
	.balign	32
20:	mtspr	SPRN_L2CR,r5
	sync
	isync
	b	23f
21:	b	22f
22:	sync
	isync
	b	20b
23:	blr
SYM_FUNC_END(cpuoff_l2_off)

/*
 * Flush the data caches (L1 and L2) to memory and turn them off, by
 * displacement.  Reads the first 4MB of the kernel linear mapping, so RAM
 * must be at least that large (and cacheable by the kernel mapping).
 * Needs the MMU on for the kernel mapping.
 * Clobbers r0, r3-r6, lr
 */
SYM_FUNC_START_LOCAL(cpuoff_flush_caches)
	/* L1 */
	lis	r4,KERNELBASE@h
	li	r3,0x2000
	mtctr	r3
1:	lwz	r0,0(r4)
	addi	r4,r4,L1_CACHE_BYTES
	bdnz	1b
	sync
BEGIN_FTR_SECTION
	/* L2: it holds castouts of the above, push everything through it */
	lis	r4,KERNELBASE@h
	lis	r3,2
	mtctr	r3
1:	lwz	r0,0(r4)
	addi	r4,r4,L1_CACHE_BYTES
	bdnz	1b
	sync
	isync
	lis	r4,KERNELBASE@h
	lis	r3,2
	mtctr	r3
1:	dcbf	0,r4
	addi	r4,r4,L1_CACHE_BYTES
	bdnz	1b
	sync
	isync

	/* disable L2 */
	mflr	r6
	bl	cpuoff_l2_off
	mtlr	r6
END_FTR_SECTION_IFSET(CPU_FTR_L2CR)
	/* flush what is left in L1, then turn the data cache off */
	lis	r4,KERNELBASE@h
	li	r3,0x2000
	mtctr	r3
1:	dcbf	0,r4
	addi	r4,r4,L1_CACHE_BYTES
	bdnz	1b
	sync
	mfspr	r3,SPRN_HID0
	rlwinm	r3,r3,0,~HID0_DCE
	sync
	isync
	mtspr	SPRN_HID0,r3
	sync
	isync
	blr
SYM_FUNC_END(cpuoff_flush_caches)

/*
 * Bring the caches into a known state: HID0 as saved with both L1 caches
 * flash invalidated and on, and L2 invalidated and restored.  Works with
 * the MMU on or off.
 * r11 = save area (as seen in the current mode)
 * Clobbers r0, r3-r5
 */
SYM_FUNC_START_LOCAL(cpuoff_caches_on)
	lwz	r3,SL_HID0(r11)
	rlwinm	r3,r3,0,11,7		/* no doze, nap, sleep */
	li	r5,0
	ori	r5,r5,HID0_ICE | HID0_DCE
	andc	r4,r3,r5
	sync
	isync
	mtspr	SPRN_HID0,r4		/* L1 off */
	sync
	isync
	ori	r4,r3,HID0_ICE | HID0_DCE | HID0_ICFI | HID0_DCI
	mtspr	SPRN_HID0,r4		/* flash invalidate */
	xori	r4,r4,HID0_ICFI | HID0_DCI
	mtspr	SPRN_HID0,r4
	sync
	isync
BEGIN_FTR_SECTION
	lwz	r5,SL_L2CR(r11)
	li	r3,0
	sync
	mtspr	SPRN_L2CR,r3		/* L2 off, whatever it was */
	sync
	isync
	rlwinm.	r0,r5,0,0,0		/* was it on? */
	beq	2f
	rlwinm	r4,r5,0,~L2CR_L2E
	oris	r4,r4,L2CR_L2I@h
	mtspr	SPRN_L2CR,r4		/* global invalidate */
	sync
	isync
1:	mfspr	r3,SPRN_L2CR
	rlwinm.	r0,r3,0,31,31		/* wait for L2IP to clear */
	bne	1b
	xoris	r4,r4,L2CR_L2I@h
	mtspr	SPRN_L2CR,r4
	sync
	mtspr	SPRN_L2CR,r5		/* back on */
	sync
	isync
2:
END_FTR_SECTION_IFSET(CPU_FTR_L2CR)
	blr
SYM_FUNC_END(cpuoff_caches_on)

/*
 * int ppc_cpuoff_suspend(int (*prepare)(void *arg), int (*final)(void *arg),
 *			  void *arg)
 *
 * Save the CPU state and call prepare(arg) (if not NULL), with interrupts off
 * and the caches still on.  If that succeeds, flush the data caches to memory
 * and turn them off, and call final(arg), which has to make the CPU lose power.
 *
 * On a successful power off the CPU is restarted at ppc_cpuoff_resume_phys(),
 * and this returns 0 to its caller as if nothing happened.
 *
 * If prepare() fails its error is returned.  If final() returns, the power off
 * did not happen, and its return value (-EIO if 0) is returned.  In both cases
 * everything is put back.
 */
_GLOBAL(ppc_cpuoff_suspend)
	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l

	mflr	r0
	stw	r0,SL_LR(r11)
	mfcr	r0
	stw	r0,SL_CR(r11)
	stw	r1,SL_SP(r11)
	stw	r2,SL_R2(r11)
	stmw	r12,SL_R12(r11)
	mr	r30,r3			/* prepare */
	mr	r29,r4			/* final */
	mr	r31,r5			/* arg */

	/* interrupts off for good */
	mfmsr	r4
	stw	r4,SL_MSR(r11)
	rlwinm	r0,r4,0,~MSR_EE
	sync
	mtmsr	r0
	isync
	mfsdr1	r4
	stw	r4,SL_SDR1(r11)

	/* stable timebase */
1:	mftbu	r4
	stw	r4,SL_TB(r11)
	mftb	r5
	stw	r5,SL_TB+4(r11)
	mftbu	r3
	cmpw	r3,r4
	bne	1b

	mfsprg	r4,0
	stw	r4,SL_SPRG0(r11)
	mfsprg	r4,1
	stw	r4,SL_SPRG0+4(r11)
	mfsprg	r4,2
	stw	r4,SL_SPRG0+8(r11)
	mfsprg	r4,3
	stw	r4,SL_SPRG0+12(r11)

	mfdbatu	r4,0
	stw	r4,SL_DBAT0(r11)
	mfdbatl	r4,0
	stw	r4,SL_DBAT0+4(r11)
	mfdbatu	r4,1
	stw	r4,SL_DBAT0+8(r11)
	mfdbatl	r4,1
	stw	r4,SL_DBAT0+12(r11)
	mfdbatu	r4,2
	stw	r4,SL_DBAT0+16(r11)
	mfdbatl	r4,2
	stw	r4,SL_DBAT0+20(r11)
	mfdbatu	r4,3
	stw	r4,SL_DBAT0+24(r11)
	mfdbatl	r4,3
	stw	r4,SL_DBAT0+28(r11)
	mfibatu	r4,0
	stw	r4,SL_IBAT0(r11)
	mfibatl	r4,0
	stw	r4,SL_IBAT0+4(r11)
	mfibatu	r4,1
	stw	r4,SL_IBAT0+8(r11)
	mfibatl	r4,1
	stw	r4,SL_IBAT0+12(r11)
	mfibatu	r4,2
	stw	r4,SL_IBAT0+16(r11)
	mfibatl	r4,2
	stw	r4,SL_IBAT0+20(r11)
	mfibatu	r4,3
	stw	r4,SL_IBAT0+24(r11)
	mfibatl	r4,3
	stw	r4,SL_IBAT0+28(r11)
BEGIN_MMU_FTR_SECTION
	mfspr	r4,SPRN_DBAT4U
	stw	r4,SL_DBAT0+32(r11)
	mfspr	r4,SPRN_DBAT4L
	stw	r4,SL_DBAT0+36(r11)
	mfspr	r4,SPRN_DBAT5U
	stw	r4,SL_DBAT0+40(r11)
	mfspr	r4,SPRN_DBAT5L
	stw	r4,SL_DBAT0+44(r11)
	mfspr	r4,SPRN_DBAT6U
	stw	r4,SL_DBAT0+48(r11)
	mfspr	r4,SPRN_DBAT6L
	stw	r4,SL_DBAT0+52(r11)
	mfspr	r4,SPRN_DBAT7U
	stw	r4,SL_DBAT0+56(r11)
	mfspr	r4,SPRN_DBAT7L
	stw	r4,SL_DBAT0+60(r11)
	mfspr	r4,SPRN_IBAT4U
	stw	r4,SL_IBAT0+32(r11)
	mfspr	r4,SPRN_IBAT4L
	stw	r4,SL_IBAT0+36(r11)
	mfspr	r4,SPRN_IBAT5U
	stw	r4,SL_IBAT0+40(r11)
	mfspr	r4,SPRN_IBAT5L
	stw	r4,SL_IBAT0+44(r11)
	mfspr	r4,SPRN_IBAT6U
	stw	r4,SL_IBAT0+48(r11)
	mfspr	r4,SPRN_IBAT6L
	stw	r4,SL_IBAT0+52(r11)
	mfspr	r4,SPRN_IBAT7U
	stw	r4,SL_IBAT0+56(r11)
	mfspr	r4,SPRN_IBAT7L
	stw	r4,SL_IBAT0+60(r11)
END_MMU_FTR_SECTION_IFSET(MMU_FTR_USE_HIGH_BATS)

	/* CPU configuration */
	mfspr	r4,SPRN_HID0
	stw	r4,SL_HID0(r11)
BEGIN_FTR_SECTION
	mfspr	r4,SPRN_L2CR
	stw	r4,SL_L2CR(r11)
END_FTR_SECTION_IFSET(CPU_FTR_L2CR)
BEGIN_FTR_SECTION
	/* Gekko / 750CL-style paired singles: HID2, HID4 (750CL only), GQRs */
	mfspr	r4,SPRN_HID2_GEKKO
	stw	r4,SL_HID2(r11)
	mfpvr	r4
	rlwinm	r4,r4,0,16,19
	cmpwi	r4,0x7000
	bne	5f
	mfspr	r4,SPRN_HID4_GEKKO
	stw	r4,SL_HID4(r11)
5:	mfspr	r4,SPRN_GQR0
	stw	r4,SL_GQR0(r11)
	mfspr	r4,SPRN_GQR0+1
	stw	r4,SL_GQR0+4(r11)
	mfspr	r4,SPRN_GQR0+2
	stw	r4,SL_GQR0+8(r11)
	mfspr	r4,SPRN_GQR0+3
	stw	r4,SL_GQR0+12(r11)
	mfspr	r4,SPRN_GQR0+4
	stw	r4,SL_GQR0+16(r11)
	mfspr	r4,SPRN_GQR0+5
	stw	r4,SL_GQR0+20(r11)
	mfspr	r4,SPRN_GQR0+6
	stw	r4,SL_GQR0+24(r11)
	mfspr	r4,SPRN_GQR0+7
	stw	r4,SL_GQR0+28(r11)
END_FTR_SECTION_IFSET(CPU_FTR_PAIRED_SINGLE)
	bl	__save_cpu_setup

	/* prepare, with a frame, and everything on */
	stwu	r1,-STACK_FRAME_MIN_SIZE(r1)
	li	r3,0
	cmplwi	r30,0
	beq	8f
	mtctr	r30
	mr	r3,r31
	bctrl
8:	mr.	r30,r3
	bne	7f			/* failed, caches untouched */

	/* point of no return for the data in the caches */
	bl	cpuoff_flush_caches

	/* caches off: stores go straight to RAM */
	mtctr	r29
	mr	r3,r31
	bctrl

	/* it returned, so no power loss: undo what we did to the caches */
	mr	r30,r3
	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l
	bl	cpuoff_caches_on
	cmpwi	r30,0
	bne	6f
	li	r30,-5			/* -EIO */
	b	6f
7:	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l
6:	lwz	r0,SL_CR(r11)
	mtcr	r0
	lwz	r2,SL_R2(r11)
	lwz	r1,SL_SP(r11)
	lwz	r0,SL_LR(r11)
	mtlr	r0
	mr	r3,r30
	lmw	r12,SL_R12(r11)
	blr
_ASM_NOKPROBE_SYMBOL(ppc_cpuoff_suspend)

/*
 * Physical address of ppc_cpuoff_resume.
 */
_GLOBAL(ppc_cpuoff_resume_phys)
	lis	r3,ppc_cpuoff_resume@ha
	addi	r3,r3,ppc_cpuoff_resume@l
	tophys(r3,r3)
	blr

/*
 * The resume entry point.  Called by whatever restarts the CPU, in real mode
 * at its physical address.
 */
_GLOBAL(ppc_cpuoff_resume)
	/* known MSR: MMU off, interrupts off, no FP, big endian */
	li	r0,0
	sync
	mtmsr	r0
	isync

	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l
	tophys(r11,r11)

	bl	cpuoff_caches_on

	/* kernel segment registers before touching memory above 256MB */
	bl	load_segment_registers
	sync
	isync

	bl	__restore_cpu_setup

	/* HID2 / HID4 / GQRs */
	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l
	tophys(r11,r11)
BEGIN_FTR_SECTION
	li	r4,0
	mtspr	SPRN_HID2_GEKKO,r4	/* PSE off while HID4 is changed */
	sync
	isync
	mfpvr	r4
	rlwinm	r4,r4,0,16,19
	cmpwi	r4,0x7000
	bne	5f
	lwz	r4,SL_HID4(r11)
	mtspr	SPRN_HID4_GEKKO,r4
	sync
5:	lwz	r4,SL_HID2(r11)
	mtspr	SPRN_HID2_GEKKO,r4
	sync
	isync
	lwz	r4,SL_GQR0(r11)
	mtspr	SPRN_GQR0,r4
	lwz	r4,SL_GQR0+4(r11)
	mtspr	SPRN_GQR0+1,r4
	lwz	r4,SL_GQR0+8(r11)
	mtspr	SPRN_GQR0+2,r4
	lwz	r4,SL_GQR0+12(r11)
	mtspr	SPRN_GQR0+3,r4
	lwz	r4,SL_GQR0+16(r11)
	mtspr	SPRN_GQR0+4,r4
	lwz	r4,SL_GQR0+20(r11)
	mtspr	SPRN_GQR0+5,r4
	lwz	r4,SL_GQR0+24(r11)
	mtspr	SPRN_GQR0+6,r4
	lwz	r4,SL_GQR0+28(r11)
	mtspr	SPRN_GQR0+7,r4
END_FTR_SECTION_IFSET(CPU_FTR_PAIRED_SINGLE)

	bl	reloc_offset
	bl	__init_fpu_registers

	lis	r11,cpuoff_save_area@ha
	addi	r11,r11,cpuoff_save_area@l
	tophys(r11,r11)

	/* SDR1, SPRGs, BATs */
	lwz	r4,SL_SDR1(r11)
	mtsdr1	r4
	lwz	r4,SL_SPRG0(r11)
	mtsprg	0,r4
	lwz	r4,SL_SPRG0+4(r11)
	mtsprg	1,r4
	lwz	r4,SL_SPRG0+8(r11)
	mtsprg	2,r4
	lwz	r4,SL_SPRG0+12(r11)
	mtsprg	3,r4

	lwz	r4,SL_DBAT0(r11)
	mtdbatu	0,r4
	lwz	r4,SL_DBAT0+4(r11)
	mtdbatl	0,r4
	lwz	r4,SL_DBAT0+8(r11)
	mtdbatu	1,r4
	lwz	r4,SL_DBAT0+12(r11)
	mtdbatl	1,r4
	lwz	r4,SL_DBAT0+16(r11)
	mtdbatu	2,r4
	lwz	r4,SL_DBAT0+20(r11)
	mtdbatl	2,r4
	lwz	r4,SL_DBAT0+24(r11)
	mtdbatu	3,r4
	lwz	r4,SL_DBAT0+28(r11)
	mtdbatl	3,r4
	lwz	r4,SL_IBAT0(r11)
	mtibatu	0,r4
	lwz	r4,SL_IBAT0+4(r11)
	mtibatl	0,r4
	lwz	r4,SL_IBAT0+8(r11)
	mtibatu	1,r4
	lwz	r4,SL_IBAT0+12(r11)
	mtibatl	1,r4
	lwz	r4,SL_IBAT0+16(r11)
	mtibatu	2,r4
	lwz	r4,SL_IBAT0+20(r11)
	mtibatl	2,r4
	lwz	r4,SL_IBAT0+24(r11)
	mtibatu	3,r4
	lwz	r4,SL_IBAT0+28(r11)
	mtibatl	3,r4
BEGIN_MMU_FTR_SECTION
	lwz	r4,SL_DBAT0+32(r11)
	mtspr	SPRN_DBAT4U,r4
	lwz	r4,SL_DBAT0+36(r11)
	mtspr	SPRN_DBAT4L,r4
	lwz	r4,SL_DBAT0+40(r11)
	mtspr	SPRN_DBAT5U,r4
	lwz	r4,SL_DBAT0+44(r11)
	mtspr	SPRN_DBAT5L,r4
	lwz	r4,SL_DBAT0+48(r11)
	mtspr	SPRN_DBAT6U,r4
	lwz	r4,SL_DBAT0+52(r11)
	mtspr	SPRN_DBAT6L,r4
	lwz	r4,SL_DBAT0+56(r11)
	mtspr	SPRN_DBAT7U,r4
	lwz	r4,SL_DBAT0+60(r11)
	mtspr	SPRN_DBAT7L,r4
	lwz	r4,SL_IBAT0+32(r11)
	mtspr	SPRN_IBAT4U,r4
	lwz	r4,SL_IBAT0+36(r11)
	mtspr	SPRN_IBAT4L,r4
	lwz	r4,SL_IBAT0+40(r11)
	mtspr	SPRN_IBAT5U,r4
	lwz	r4,SL_IBAT0+44(r11)
	mtspr	SPRN_IBAT5L,r4
	lwz	r4,SL_IBAT0+48(r11)
	mtspr	SPRN_IBAT6U,r4
	lwz	r4,SL_IBAT0+52(r11)
	mtspr	SPRN_IBAT6L,r4
	lwz	r4,SL_IBAT0+56(r11)
	mtspr	SPRN_IBAT7U,r4
	lwz	r4,SL_IBAT0+60(r11)
	mtspr	SPRN_IBAT7L,r4
END_MMU_FTR_SECTION_IFSET(MMU_FTR_USE_HIGH_BATS)

	/* flush all TLB entries */
	lis	r4,0x1000
1:	addic.	r4,r4,-0x1000
	tlbie	r4
	bgt	1b
	sync

	/* restore the timebase */
	li	r3,0
	mttbl	r3
	lwz	r3,SL_TB(r11)
	lwz	r4,SL_TB+4(r11)
	mttbu	r3
	mttbl	r4

	/* kick the decrementer so the timer code reprograms it */
	li	r0,1
	mtdec	r0

	/* callee saved registers */
	lwz	r0,SL_CR(r11)
	mtcr	r0
	lwz	r2,SL_R2(r11)
	lmw	r12,SL_R12(r11)

	/* return to the caller of ppc_cpuoff_suspend with the MMU back on */
	lwz	r3,SL_MSR(r11)
	lwz	r4,SL_LR(r11)
	lwz	r1,SL_SP(r11)
	mtsrr0	r4
	mtsrr1	r3
	li	r3,0
	sync
	isync
	rfi
_ASM_NOKPROBE_SYMBOL(ppc_cpuoff_resume)

	.section .bss
	.balign	L1_CACHE_BYTES
cpuoff_save_area:
	.space	SL_SIZE
	.balign	L1_CACHE_BYTES, 0
