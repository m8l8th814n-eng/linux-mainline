/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Stub for Google's pKVM S2MPU (Stage-2 Memory Protection Unit) interface.
 *
 * Under Android, pKVM owns the S2MPUs and samsung-iommu-v9 links each SysMMU
 * to its S2MPU power domain through this. This kernel takes EL2 for itself
 * ("CPU: All CPU(s) started at EL2", "Hyp nVHE mode initialized"), so no
 * protected hypervisor is resident and the S2MPUs stay in whatever state the
 * bootloader left them.
 *
 * The single call site is guarded by IS_ENABLED(CONFIG_PKVM_S2MPU_V9), which
 * is left unset, so this declaration exists only to satisfy the unconditional
 * #include in samsung-iommu-v9.c.
 */
#ifndef __SOC_GOOGLE_PKVM_S2MPU_H
#define __SOC_GOOGLE_PKVM_S2MPU_H

#include <linux/device.h>
#include <linux/errno.h>

static inline int pkvm_s2mpu_of_link_v9(struct device *dev)
{
	return -ENODEV;
}

static inline int pkvm_s2mpu_of_link(struct device *dev)
{
	return -ENODEV;
}

#endif /* __SOC_GOOGLE_PKVM_S2MPU_H */
