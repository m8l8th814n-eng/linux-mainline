/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Google LLC
 */

#ifndef __LINUX_GSA_TZ_H
#define __LINUX_GSA_TZ_H

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/mutex.h>

#define MAX_PORT_NAME_SIZE 256

struct gsa_tz_chan_ctx {
	struct device *dev;
	char port[MAX_PORT_NAME_SIZE];
	void *chan;
	struct mutex req_lock;
	struct mutex rsp_lock;
	struct completion reply_comp;
	void *rsp_buf;
	size_t rsp_buf_size;
	int rsp_res;
};

void gsa_tz_chan_ctx_init(struct gsa_tz_chan_ctx *ctx, const char *port,
			  struct device *dev);

void gsa_tz_chan_close(struct gsa_tz_chan_ctx *ctx);

int gsa_tz_chan_msg_xchg(struct gsa_tz_chan_ctx *ctx,
			 const void *req, size_t req_len,
			 void *rsp, size_t rsp_size);

#endif /* __LINUX_GSA_TZ_H */
