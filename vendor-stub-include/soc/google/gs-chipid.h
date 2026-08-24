/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_GS_CHIPID_H
#define __SOC_GOOGLE_GS_CHIPID_H

#include <linux/types.h>

u32 gs_chipid_get_revision(void);
u32 gs_chipid_get_type(void);
u32 gs_chipid_get_product_id(void);

#endif /* __SOC_GOOGLE_GS_CHIPID_H */
