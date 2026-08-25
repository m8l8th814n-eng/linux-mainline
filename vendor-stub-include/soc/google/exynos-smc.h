/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_EXYNOS_SMC_H
#define __SOC_GOOGLE_EXYNOS_SMC_H

#include <linux/arm-smccc.h>
#include <linux/types.h>

static inline unsigned long exynos_smc(unsigned long cmd, unsigned long arg1,
				       unsigned long arg2, unsigned long arg3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(cmd, arg1, arg2, arg3, 0, 0, 0, 0, &res);

	return res.a0;
}

#endif /* __SOC_GOOGLE_EXYNOS_SMC_H */
