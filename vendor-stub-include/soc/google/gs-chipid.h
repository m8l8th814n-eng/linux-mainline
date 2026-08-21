/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the vendor gs-chipid interface.
 *
 * Mainline does have a gs101 chipid driver (drivers/soc/samsung/exynos-chipid.c,
 * compatible "google,gs101-otp", already present as efuse@10000000 in the
 * device tree) but it publishes what it reads through a soc_device attribute
 * rather than exporting getters, so there is nothing to forward these to.
 *
 * KNOWN APPROXIMATION: aoc.c passes these three values to AoC firmware as
 * chip_revision / chip_type / chip_product_id in its fw_data block. Returning
 * zero tells the firmware "unknown". If AoC turns out to branch on chip
 * revision -- errata workarounds are the likely case -- this is a prime
 * suspect, and the fix is to read PRO_ID (offset 0x00) and revision
 * (offset 0x10, sub_rev_shift 16) out of the efuse regmap directly.
 */
#ifndef __SOC_GOOGLE_GS_CHIPID_H
#define __SOC_GOOGLE_GS_CHIPID_H

#include <linux/types.h>

static inline u32 gs_chipid_get_revision(void)
{
	return 0;
}

static inline u32 gs_chipid_get_type(void)
{
	return 0;
}

static inline u32 gs_chipid_get_product_id(void)
{
	return 0;
}

#endif /* __SOC_GOOGLE_GS_CHIPID_H */
