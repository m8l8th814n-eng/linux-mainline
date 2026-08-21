/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the downstream GSA (Google Security Assistant) AoC interface.
 *
 * The real implementation lives in the vendor google-modules/gsa tree and
 * authenticates a signed AoC firmware image via the Titan M security chip.
 * There is no GSA driver in mainline.
 *
 * aoc.c only reaches these calls when the "gsa-enabled" device tree property
 * is present. Leave that property out and the driver takes the unsigned path
 * instead (write_reset_trampoline() + aoc_release_from_reset()), which needs
 * nothing from GSA. These stubs exist purely so the module links; if one is
 * ever reached it means "gsa-enabled" was set by mistake.
 */
#ifndef __LINUX_GSA_GSA_AOC_H
#define __LINUX_GSA_GSA_AOC_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/types.h>

enum gsa_aoc_cmd {
	GSA_AOC_LOAD = 0,
	GSA_AOC_START = 1,
	GSA_AOC_SHUTDOWN = 2,
	GSA_AOC_STATE_LOADED = 3,
};

static inline int gsa_load_aoc_fw_image(struct device *gsa, dma_addr_t hdr,
					phys_addr_t body)
{
	return -ENODEV;
}

/* int rather than enum gsa_aoc_cmd: aoc.c:1556 passes a bare 4. */
static inline int gsa_send_aoc_cmd(struct device *gsa, int cmd)
{
	return -ENODEV;
}

static inline int gsa_unload_aoc_fw_image(struct device *gsa)
{
	return -ENODEV;
}

#endif /* __LINUX_GSA_GSA_AOC_H */
