/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_BTS_H
#define __SOC_GOOGLE_BTS_H

#include <linux/types.h>

static inline unsigned int bts_get_scenindex(const char *name)
{
	return 0;
}

static inline int bts_add_scenario(unsigned int index)
{
	return 0;
}

static inline int bts_del_scenario(unsigned int index)
{
	return 0;
}

#endif /* __SOC_GOOGLE_BTS_H */
