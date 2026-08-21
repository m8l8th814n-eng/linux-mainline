/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for Google/Samsung's Exynos CPU power-mode control.
 *
 * The real implementation lives in the vendor google-modules/soc tree. aoc.c
 * uses it to stop the AP entering system power-down while AoC boots, since
 * AoC needs DRAM up during that window.
 *
 * Mainline has no equivalent. These no-ops mean the AP is not held awake, so
 * an AoC boot that races a system suspend may fail. In practice suspend does
 * not happen in the couple of seconds after probe, and the alternative is not
 * building at all.
 */
#ifndef __SOC_GOOGLE_EXYNOS_CPUPM_H
#define __SOC_GOOGLE_EXYNOS_CPUPM_H

enum {
	POWERMODE_TYPE_CLUSTER = 0,
	POWERMODE_TYPE_SYSTEM,
};

static inline void disable_power_mode(int cpu, int type)
{
}

static inline void enable_power_mode(int cpu, int type)
{
}

#endif /* __SOC_GOOGLE_EXYNOS_CPUPM_H */
