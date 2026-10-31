// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nintendo Wii power management using the "MINI" Starlet firmware
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * To suspend, Broadway asks Starlet over IPC to put the system into its idle
 * state.  Starlet holds Broadway (and most of the rest of the system) in
 * reset/clock-gated/power-gated, keeps the memory alive, waits for a wake
 * event, and then restarts Broadway at a physical address that is part of the
 * request.  That address is the PowerPC cpuoff resume code, which restores
 * everything that Linux expects, and execution continues as if the suspend
 * call just returned.
 */

#define pr_fmt(fmt)	"wii-pm: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/suspend.h>
#include <linux/timekeeping.h>

#include <asm/cpuoff.h>
#include <asm/hlwd-ipc.h>
#include <asm/hlwd-ipc-mini.h>
#include <asm/machdep.h>
#include <asm/time.h>

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX	"wii_pm."

/*
 * The fan GPIO is switched off by MINI during suspend unless asked otherwise.
 * Left in suspend for long without the fan, the console can be damaged by
 * heat (same as what hapened with the official IOS standby code), so ask MINI
 * to keep the fan on unless the user explicitly says not to.
 */
static bool fan_on_suspend = true;
module_param(fan_on_suspend, bool, 0644);
MODULE_PARM_DESC(fan_on_suspend, "Keep the fan running while suspended (default: Y)");

/* how long to wait for Starlet to take us down once it has the request */
#define PM_ENTER_WAIT_MS	5000

/*
 * MAJOR bumps are ABI-breaking for some existing functionality, MINOR bumps
 * only introduce new functionality, fix bugs, or have otherwise non-breaking
 * changes to existing functionality.  We can still use the firmware on a
 * yet-unsupported minor revision, but not on a yet-unsupported major revision.
 */
#define PM_MAX_SUPPORTED_VER_MAJOR 1
#define PM_MAX_SUPPORTED_VER_MINOR 0

#define HW_AIPPROT 0x0d800070
/*
 * If 0, DI, SI, EXI, and AI appear in their legacy Flipper positions, but
 * Broadway is able to boot from EXI.  As such, MINI clears this bit during
 * boot, including resume.
 *
 * If 1, these blocks appear in their modern Hollywood positions, but Broadway
 * is unable to boot from EXI.  We don't care about the ability to boot from
 * EXI after we've already booted, so we set this after resume.  MINI will
 * clear it once again if we go to suspend again.
 */
#define AIPPROT_ENAHBIOPI BIT(0)

static bool pm_supported;
static u16 cur_ver_major, cur_ver_minor;

struct wii_pm_req {
	u32 tag;
	u32 wake_events;
	u32 options;
	u32 __iomem *kick;	/* doorbell */
	u32 kick_val;
	u32 wait_tb;		/* how long to wait for it to work, in timebase ticks */
};

static int wii_pm_query(void)
{
	struct ipc_request_mini req;
	int ret;

	if (ipc_get_flavor() != IPC_FLAVOR_MINI)
		return -ENODEV;

	ret = ipc_exchange_mini(&req, IPC_MINI_CODE_PM_SUPP, 3, 1, 0);
	if (ret) {
		/* older MINI builds don't know the request, and never answer it */
		pr_err("this MINI version has no power management support, disabling\n");
		return -ENODEV;
	}

	if (req.args[0] != IPC_MINI_PM_MAGIC) {
		pr_err("unexpected power management firmware magic %08x, possibly an unofficial MINI build with something else in that device slot, disabling\n", req.args[0]);
		return -ENODEV;
	}

	/* unpack version */
	cur_ver_major = (u16)(req.args[1] >> 16);
	cur_ver_minor = (u16)(req.args[1] & 0xffff);
	if (!cur_ver_major || cur_ver_major > PM_MAX_SUPPORTED_VER_MAJOR) {
		pr_err("unsupported power management firmware version %u.%u (expected <= %u.%u), disabling\n",
			cur_ver_major, cur_ver_minor,
			PM_MAX_SUPPORTED_VER_MAJOR, PM_MAX_SUPPORTED_VER_MINOR);
		return -ENODEV;
	}
	else if (cur_ver_major == PM_MAX_SUPPORTED_VER_MAJOR && cur_ver_minor > PM_MAX_SUPPORTED_VER_MINOR)
		pr_warn("unsupported power management firmware version %u.%u (expected <= %u.%u), some features of this firmware may not be usable by this kernel version, continuing\n",
			cur_ver_major, cur_ver_minor,
			PM_MAX_SUPPORTED_VER_MAJOR, PM_MAX_SUPPORTED_VER_MINOR);


	return 0;
}

#ifdef CONFIG_SUSPEND
/*
 * Runs with caches on and interrupts off.  Queues the request, but does not
 * tell Starlet about it yet; that is the doorbell write wii_pm_final() does
 * once the caches are off.
 */
static int wii_pm_suspend_prepare(void *arg)
{
	struct wii_pm_req *r = arg;

	return ipc_post_mini_nokick(IPC_MINI_CODE_PM_SUSPEND, r->tag, 4,
				    r->wake_events, 3, r->options << 16,
				    (u32)ppc_cpuoff_resume_phys());
}

/*
 * Runs with the data caches off and interrupts disabled, only a word MMIO
 * store and timebase reads, since sub-word stores to MEM1 with caches off
 * corrupt the written word and neighboring words on the GameCube and Wii.
 * Doesn't return if Starlet successfully handles the request (likely).
 */
static int wii_pm_suspend_final(void *arg)
{
	struct wii_pm_req *r = arg;
	unsigned long start = mftb();

	out_be32(r->kick, r->kick_val);

	while (mftb() - start < r->wait_tb)
		cpu_relax();

	return -ETIMEDOUT;
}

static int wii_pm_suspend_enter(suspend_state_t state)
{
	struct ipc_request_mini reply;
	struct wii_pm_req r = {
		.wake_events = IPC_MINI_PM_WAKE_POWER,
		.options = fan_on_suspend ? IPC_MINI_PM_OPT_KEEP_FAN : 0,
	};
	u32 code = IPC_MINI_CODE_PM_SUSPEND;
	void __iomem *aipprot;
	int ret;

	/* we only support mem anyways */
	(void)state;

	aipprot = ioremap(HW_AIPPROT, 4);
	if (!aipprot)
		return -ENODEV;

	r.kick = ipc_kick_reg_mini(&r.kick_val);
	if (!r.kick) {
		ret = -ENODEV;
		goto out;
	}
	r.wait_tb = (u32)tb_ticks_per_usec * PM_ENTER_WAIT_MS * 1000;

	r.tag = ipc_get_tag_mini();
	ret = ppc_cpuoff_enter(wii_pm_suspend_prepare, wii_pm_suspend_final, &r);
	/* re-enable ENAHBIOPI */
	out_be32(aipprot, in_be32(aipprot) | AIPPROT_ENAHBIOPI);
	if (ret) {
		pr_err("suspend failed: %d\n", ret);
		/* might have an answer waiting */
		if (ret == -ETIMEDOUT &&
		    !ipc_receive_tagged_mini(&reply, code, r.tag, 10, 1))
			ipc_put_tag_mini();
		goto out;
	}

	/* we're back, the answer is waiting */
	ret = ipc_receive_tagged_mini(&reply, code, r.tag, 3, 3);
	if (ret) {
		pr_err("no answer from Starlet after resume: %d\n", ret);
		goto out;
	}
	ipc_put_tag_mini();

	switch (reply.args[0]) {
	case IPC_MINI_PM_WAKE_POWER:
		pr_info("woken by the power button\n");
		break;
	case IPC_MINI_PM_WAKE_TIMEOUT:
		pr_info("woken by the timer\n");
		break;
	default:
		if ((int)reply.args[0] < 0) {
			pr_err("MINI refused to suspend (%08x)\n", reply.args[0]);
			ret = -EINVAL;
			goto out;
		}
		else
			pr_warn("unknown wake reason: %08x\n", reply.args[0]);
	}

out:
	iounmap(aipprot);
	return ret;
}

static const struct platform_suspend_ops wii_pm_suspend_ops = {
	.valid = suspend_valid_only_mem,
	.enter = wii_pm_suspend_enter,
};
#endif

static int __init wii_pm_init(void)
{
	if (!machine_is(wii))
		return 0;

	if (wii_pm_query())
		return 0;

	pm_supported = true;
#ifdef CONFIG_SUSPEND
	suspend_set_ops(&wii_pm_suspend_ops);
#endif
	pr_info("suspend to RAM available\n");
	return 0;
}
/* needs the IPC driver to be up */
late_initcall_sync(wii_pm_init);
