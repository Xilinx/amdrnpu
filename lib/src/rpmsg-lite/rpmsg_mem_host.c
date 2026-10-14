/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#include "rpmsg_mem_host.h"

#include <errno.h>
#include <string.h>

static int in_bounds(const struct rpmsg_mem_host *h, uint64_t addr, size_t len)
{
	if (addr < h->phys || len > h->size)
		return 0;
	return addr - h->phys <= h->size - len;
}

static int host_write(void *priv, uint64_t dev_addr, const void *src,
		      size_t len)
{
	struct rpmsg_mem_host *h = priv;

	if (!len || !in_bounds(h, dev_addr, len))
		return -EFAULT;
	memcpy(h->base + (size_t)(dev_addr - h->phys), src, len);
	return 0;
}

static int host_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct rpmsg_mem_host *h = priv;

	if (!len || !in_bounds(h, dev_addr, len))
		return -EFAULT;
	memcpy(dst, h->base + (size_t)(dev_addr - h->phys), len);
	return 0;
}

void rpmsg_mem_host_init(struct rpmsg_mem_host *host, struct rpmsg_mem_ops *ops,
			 uint8_t *base, uint64_t phys, size_t size)
{
	host->base = base;
	host->phys = phys;
	host->size = size;
	ops->write = host_write;
	ops->read = host_read;
	ops->priv = host;
}
