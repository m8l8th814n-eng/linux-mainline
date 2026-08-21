/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shim for the vendor Exynos PMU interface.
 *
 * The vendor tree exposes exynos_pmu_read()/write() directly. Mainline
 * instead hands out a regmap for the PMU via exynos_get_pmu_regmap(), so
 * this is a real implementation on top of that rather than a stub.
 *
 * aoc_v1.c only reads, and only to log PMU state when an AoC reset times out.
 */
#ifndef __SOC_GOOGLE_EXYNOS_PMU_IF_H
#define __SOC_GOOGLE_EXYNOS_PMU_IF_H

#include <linux/errno.h>
#include <linux/regmap.h>
#include <linux/soc/samsung/exynos-pmu.h>

static inline int exynos_pmu_read(unsigned int offset, unsigned int *val)
{
	struct regmap *pmu = exynos_get_pmu_regmap();

	if (IS_ERR_OR_NULL(pmu))
		return -ENODEV;

	return regmap_read(pmu, offset, val);
}

static inline int exynos_pmu_write(unsigned int offset, unsigned int val)
{
	struct regmap *pmu = exynos_get_pmu_regmap();

	if (IS_ERR_OR_NULL(pmu))
		return -ENODEV;

	return regmap_write(pmu, offset, val);
}

static inline int exynos_pmu_update(unsigned int offset, unsigned int mask,
				    unsigned int val)
{
	struct regmap *pmu = exynos_get_pmu_regmap();

	if (IS_ERR_OR_NULL(pmu))
		return -ENODEV;

	return regmap_update_bits(pmu, offset, mask, val);
}

#endif /* __SOC_GOOGLE_EXYNOS_PMU_IF_H */
