/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for Google's subsystem coredump (sscd) framework.
 *
 * The real driver lives in the vendor tree and ships AoC crash dumps to a
 * userspace daemon. Mainline has no equivalent.
 *
 * aoc.c declares a static, zero-initialised struct sscd_platform_data, and
 * guards every use with "if (!sscd_pdata.sscd_report)". With no sscd driver
 * to fill that pointer in, AoC crash dumps are simply skipped and logged --
 * which is the intended fallback, not a failure path.
 */
#ifndef __LINUX_PLATFORM_DATA_SSCOREDUMP_H
#define __LINUX_PLATFORM_DATA_SSCOREDUMP_H

#include <linux/platform_device.h>
#include <linux/types.h>

/* Driver name aoc.c overrides its platform device onto. */
#define SSCD_NAME			"sscoredump"

#define SSCD_FLAGS_ELFARM32HDR		BIT(0)
#define SSCD_FLAGS_ELFARM64HDR		BIT(1)

struct sscd_segment {
	void *addr;
	size_t size;
	void *paddr;
	void *vaddr;
};

struct sscd_platform_data {
	int (*sscd_report)(struct platform_device *pdev,
			   struct sscd_segment *segs, u16 nsegs, u64 flags,
			   const char *reason);
};

#endif /* __LINUX_PLATFORM_DATA_SSCOREDUMP_H */
