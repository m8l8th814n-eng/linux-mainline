/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for Google's debug-snapshot (DSS) crash logging framework.
 *
 * The real implementation lives in the vendor google-modules/soc tree and
 * captures a ramdump before rebooting. Mainline has no equivalent.
 *
 * aoc_v1.c makes exactly one call, on an AoC reset timeout. Rebooting the
 * whole phone at that point would be actively unhelpful while the driver is
 * being brought up, and the caller already returns -ETIMEDOUT, so this logs
 * loudly and lets the error propagate instead.
 */
#ifndef __SOC_GOOGLE_DEBUG_SNAPSHOT_H
#define __SOC_GOOGLE_DEBUG_SNAPSHOT_H

#include <linux/printk.h>

/*
 * Device policy actions. Only GO_PANIC_ID is referenced (samsung-iommu-fault-v9
 * compares drvdata->panic_action against it); the value just has to be
 * distinct from whatever the device tree supplies.
 */
#define GO_DEFAULT_ID	0
#define GO_PANIC_ID	1
#define GO_WATCHDOG_ID	2
#define GO_S2D_ID	3

static inline void dbg_snapshot_emergency_reboot(const char *str)
{
	pr_err("aoc: emergency reboot requested (%s) - ignored, no debug-snapshot in mainline\n",
	       str);
}

static inline void dbg_snapshot_do_dpm_policy(unsigned int policy,
					      const char *str)
{
	pr_err("debug-snapshot: policy %u requested (%s) - ignored\n",
	       policy, str);
}

#endif /* __SOC_GOOGLE_DEBUG_SNAPSHOT_H */
