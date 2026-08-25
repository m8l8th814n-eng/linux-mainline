/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_CAL_IF_H
#define __SOC_GOOGLE_CAL_IF_H

#include <linux/types.h>

extern unsigned long cal_dfs_get_max_freq(unsigned int id);

extern int cal_cp_init(void);
extern int cal_cp_status(void);
extern int cal_cp_reset_assert(void);
extern int cal_cp_reset_release(void);

extern void cal_cp_enable_dump_pc_no_pg(void);
extern void cal_cp_disable_dump_pc_no_pg(void);

#endif /* __SOC_GOOGLE_CAL_IF_H */
