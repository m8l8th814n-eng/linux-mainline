/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_GOOGLE_EXYNOS_PM_QOS_H
#define __SOC_GOOGLE_EXYNOS_PM_QOS_H

#include <linux/plist.h>
#include <linux/types.h>
#include <linux/workqueue.h>

enum {
	EXYNOS_PM_QOS_RESERVED = 0,
	PM_QOS_DEVICE_THROUGHPUT,
	PM_QOS_DEVICE_THROUGHPUT_MAX,
	PM_QOS_BUS_THROUGHPUT,
	PM_QOS_BUS_THROUGHPUT_MAX,
};

struct exynos_pm_qos_request {
	struct plist_node node;
	int exynos_pm_qos_class;
	struct delayed_work work;
	const char *func;
	unsigned int line;
};

static inline void exynos_pm_qos_add_request(struct exynos_pm_qos_request *req,
					     int exynos_pm_qos_class, s32 value)
{
}

static inline void exynos_pm_qos_update_request(struct exynos_pm_qos_request *req,
						s32 new_value)
{
}

static inline void exynos_pm_qos_remove_request(struct exynos_pm_qos_request *req)
{
}

#endif /* __SOC_GOOGLE_EXYNOS_PM_QOS_H */
