/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for the vendor ACPM IPC channel API.
 *
 * Mainline does have ACPM support (drivers/firmware/samsung/exynos-acpm.c),
 * but it exposes a different interface built around acpm_do_xfer() rather
 * than the vendor's named-channel callback registration, so it is not a
 * drop-in replacement.
 *
 * aoc_v1.c uses this only to register acpm_aoc_reset_callback(), which ACPM
 * invokes to signal that an AoC reset finished. Returning success without
 * ever calling back means aoc_reset_done is never set from this path, so an
 * AoC *reset* will fall through to its timeout. Initial boot does not depend
 * on it. Wiring this to the mainline ACPM transport is the proper fix once
 * the driver otherwise works.
 */
#ifndef __SOC_GOOGLE_ACPM_IPC_CTRL_H
#define __SOC_GOOGLE_ACPM_IPC_CTRL_H

#include <linux/of.h>
#include <linux/types.h>

typedef void (*ipc_callback)(unsigned int *cmd, unsigned int size);

static inline int acpm_ipc_request_channel(struct device_node *np,
					   ipc_callback handler,
					   unsigned int *id,
					   unsigned int *size)
{
	if (id)
		*id = 0;
	if (size)
		*size = 0;

	return 0;
}

static inline int acpm_ipc_release_channel(struct device_node *np,
					   unsigned int id)
{
	return 0;
}

#endif /* __SOC_GOOGLE_ACPM_IPC_CTRL_H */
