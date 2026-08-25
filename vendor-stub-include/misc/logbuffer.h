/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GOOGLE_LOGBUFFER_H_
#define __GOOGLE_LOGBUFFER_H_

#include <linux/stdarg.h>
#include <linux/types.h>

struct device;
struct logbuffer;

static inline __printf(2, 3)
void logbuffer_log(struct logbuffer *instance, const char *fmt, ...)
{
}

static inline __printf(3, 4)
void logbuffer_logk(struct logbuffer *instance, int loglevel, const char *fmt, ...)
{
}

static inline __printf(2, 0)
void logbuffer_vlog(struct logbuffer *instance, const char *fmt, va_list args)
{
}

static inline __printf(4, 5)
int dev_logbuffer_logk(struct device *dev, struct logbuffer *instance,
		       int loglevel, const char *fmt, ...)
{
	return 0;
}

static inline struct logbuffer *logbuffer_register(const char *name)
{
	return NULL;
}

static inline void logbuffer_unregister(struct logbuffer *instance)
{
}

#endif /* __GOOGLE_LOGBUFFER_H_ */
