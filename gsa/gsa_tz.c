// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 Google LLC
 */

#include <linux/errno.h>
#include <linux/string.h>

#include "gsa_tz.h"

void gsa_tz_chan_ctx_init(struct gsa_tz_chan_ctx *ctx, const char *port,
			  struct device *dev)
{
	mutex_init(&ctx->req_lock);
	mutex_init(&ctx->rsp_lock);
	init_completion(&ctx->reply_comp);
	strscpy(ctx->port, port, sizeof(ctx->port));
	ctx->dev = dev;
	ctx->chan = NULL;
}

void gsa_tz_chan_close(struct gsa_tz_chan_ctx *ctx)
{
}

int gsa_tz_chan_msg_xchg(struct gsa_tz_chan_ctx *ctx,
			 const void *req, size_t req_len,
			 void *rsp, size_t rsp_size)
{
	return -ENODEV;
}
