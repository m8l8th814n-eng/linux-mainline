/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_GS_TMU_V3_H
#define __SOC_GOOGLE_GS_TMU_V3_H

enum thermal_pause_state {
	THERMAL_RESUME = 0,
	THERMAL_SUSPEND,
};

static inline void register_tpu_thermal_pause_cb(int (*tpu_cb)(enum thermal_pause_state, void *),
						 void *data)
{
}

#endif /* __SOC_GOOGLE_GS_TMU_V3_H */
