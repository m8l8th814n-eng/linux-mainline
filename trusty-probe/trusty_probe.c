// SPDX-License-Identifier: GPL-2.0
/*
 * gs101 Trusty presence probe.
 *
 * Go/no-go for AoC "path 2": is the stock secure-world Trusty still resident
 * behind ABL, reachable via SMC, on the mainline (no-pKVM/no-Trusty-DT) boot?
 *
 * Replicates Trusty's own detection: SMC_FC_API_VERSION fast call.
 *   SMC_FASTCALL_NR(SMC_ENTITY_SECURE_MONITOR=60, fn=11) = 0xBC00000B
 *   arg a0 = TRUSTY_API_VERSION_CURRENT (5)
 * If Trusty answers it returns the negotiated api version (0..5).
 * If no Trusty, EL3 returns SMCCC_UNKNOWN (0xFFFFFFFF / -1) or an SM_ERR (<0).
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/arm-smccc.h>

#define SMC_FC_API_VERSION          0xBC00000BUL  /* fastcall, entity 60, fn 11 */
#define SMC_FC_GET_VERSION_STR      0xBC00000AU  /* fastcall, entity 60, fn 10 */
#define TRUSTY_API_VERSION_CURRENT  5

static int __init trusty_probe_init(void)
{
	struct arm_smccc_res res;
	long a0;

	arm_smccc_smc(SMC_FC_API_VERSION, TRUSTY_API_VERSION_CURRENT,
		      0, 0, 0, 0, 0, 0, &res);
	a0 = (long)res.a0;
	pr_info("trusty_probe: SMC_FC_API_VERSION(0x%08x) -> a0=0x%lx (%ld)\n",
		SMC_FC_API_VERSION, res.a0, a0);

	if (a0 >= 0 && res.a0 <= TRUSTY_API_VERSION_CURRENT) {
		pr_info("trusty_probe: *** TRUSTY IS RESIDENT *** (api version %lu) -- path 2 is OPEN\n",
			res.a0);
		/* Try the version string (returned one char at a time). */
		{
			char v[33]; int i;
			for (i = 0; i < 32; i++) {
				arm_smccc_smc(SMC_FC_GET_VERSION_STR, i,
					      0, 0, 0, 0, 0, 0, &res);
				if ((long)res.a0 <= 0)
					break;
				v[i] = (char)res.a0;
			}
			v[i] = '\0';
			if (i)
				pr_info("trusty_probe: version string: \"%s\"\n", v);
		}
	} else {
		pr_info("trusty_probe: no Trusty (a0=0x%lx) -- secure world NOT reachable from the normal world; path 2 closed\n",
			res.a0);
	}

	return 0; /* stays loaded; rmmod to remove */
}

static void __exit trusty_probe_exit(void) { }

module_init(trusty_probe_init);
module_exit(trusty_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gs101 Trusty presence probe (SMC_FC_API_VERSION)");
