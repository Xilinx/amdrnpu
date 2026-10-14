/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#include "rpmsg_lite_dma.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct rpmsg_hdr) == 16, "rpmsg header layout");
_Static_assert(sizeof(struct rpmsg_ns_msg) == 40, "rpmsg NS layout");

static uint64_t buffer_addr(const struct rpmsg_lite_dma *ctx, int tx,
			    uint16_t id)
{
	size_t index = id + (tx ? ctx->half : 0u);

	return ctx->cfg.shm_phys + ctx->cfg.buf_offset +
	       index * ctx->cfg.buf_size;
}

static void notify(struct rpmsg_lite_dma *ctx, int vq_id)
{
	if (ctx->cfg.notify)
		ctx->cfg.notify(ctx->cfg.priv, vq_id);
}

static int validate_cfg(const struct rpmsg_lite_dma_cfg *cfg)
{
	size_t start[4], size[4], end[4];
	unsigned int i, j;
	uint64_t phys = cfg->shm_phys;

	size[0] = 1;
	size[1] = size[2] = vring_lite_dma_size(cfg->vring_num, cfg->vring_align);
	if (!cfg->mem || !cfg->mem->write || !cfg->mem->read || !cfg->scratch ||
	    cfg->scratch_size < cfg->buf_size || !size[1] || !cfg->buf_count ||
	    (cfg->buf_count & 1u) || cfg->buf_count / 2u > cfg->vring_num ||
	    cfg->buf_size < sizeof(struct rpmsg_hdr) ||
	    cfg->buf_size > UINT16_MAX + sizeof(struct rpmsg_hdr) ||
	    cfg->buf_count > SIZE_MAX / cfg->buf_size)
		return -EINVAL;
	size[3] = (size_t)cfg->buf_count * cfg->buf_size;
	start[0] = cfg->status_offset;
	start[1] = cfg->vring_tx_offset;
	start[2] = cfg->vring_rx_offset;
	start[3] = cfg->buf_offset;
	for (i = 0; i < 4; ++i) {
		if (size[i] > SIZE_MAX - start[i])
			return -EINVAL;
		end[i] = start[i] + size[i];
		if (end[i] > UINT64_MAX - phys ||
		    (cfg->shm_size && end[i] > cfg->shm_size))
			return -EINVAL;
		for (j = 0; j < i; ++j)
			if (start[i] < end[j] && start[j] < end[i])
				return -EINVAL;
	}
	for (i = 1; i <= 2; ++i)
		if (((phys + start[i]) & (cfg->vring_align - 1u)) ||
		    ((phys + start[i]) & 15u))
			return -EINVAL;
	return 0;
}

int rpmsg_lite_dma_init(struct rpmsg_lite_dma *ctx,
			const struct rpmsg_lite_dma_cfg *cfg)
{
	struct rpmsg_lite_dma_cfg config;
	uint8_t status;
	uint16_t id;
	int ret;

	if (!ctx || !cfg)
		return -EINVAL;
	config = *cfg;
	if (!config.buf_size)
		config.buf_size = 512;
	ret = validate_cfg(&config);
	if (ret)
		return ret;
	memset(ctx, 0, sizeof(*ctx));
	ctx->cfg = config;
	ctx->half = (uint16_t)(config.buf_count / 2u);
	ret = vring_lite_dma_init(&ctx->tx, config.vring_num,
				  config.shm_phys + config.vring_tx_offset,
				  config.vring_align, config.mem);
	if (ret)
		return ret;
	ret = vring_lite_dma_init(&ctx->rx, config.vring_num,
				  config.shm_phys + config.vring_rx_offset,
				  config.vring_align, config.mem);
	if (ret)
		return ret;
	for (id = 0; id < ctx->half; ++id) {
		ret = vring_lite_dma_add_buffer(&ctx->rx, id,
						buffer_addr(ctx, 0, id),
						config.buf_size,
						VRING_DESC_F_WRITE);
		if (ret)
			return ret;
		ctx->tx_free[ctx->tx_free_count++] = id;
	}
	ctx->initialized = 1;
	/* ACKNOWLEDGE | DRIVER | DRIVER_OK. Rings are published (synchronous
	 * backend writes) before releasing the peer.
	 */
	status = 0x01u | 0x02u | VIRTIO_CONFIG_STATUS_DRIVER_OK;
	ret = rpmsg_mem_write(config.mem, config.shm_phys + config.status_offset,
			      &status, 1);
	if (ret) {
		ctx->initialized = 0;
		return ret;
	}
	notify(ctx, 0);
	return 0;
}

static int registered(struct rpmsg_lite_dma *ctx,
		      struct rpmsg_lite_dma_ept *ept)
{
	unsigned int i;

	if (!ctx || !ctx->initialized || !ept)
		return 0;
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i)
		if (ctx->epts[i] == ept)
			return 1;
	return 0;
}

int rpmsg_lite_dma_create_ept(struct rpmsg_lite_dma *ctx,
			      struct rpmsg_lite_dma_ept *ept, const char *name,
			      uint32_t local_addr, rpmsg_lite_dma_rx_cb cb,
			      void *priv)
{
	size_t len = 0;
	unsigned int i, slot = RPMSG_LITE_MAX_EPTS;
	uint32_t addr = local_addr == RPMSG_ADDR_ANY ? 1024u : local_addr;

	if (!ctx || !ctx->initialized || !ept || !name || !cb ||
	    local_addr == RPMSG_NS_ADDR)
		return -EINVAL;
	while (len < RPMSG_LITE_NAME_SIZE && name[len])
		++len;
	if (!len || len == RPMSG_LITE_NAME_SIZE)
		return -EINVAL;
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i) {
		if (!ctx->epts[i]) {
			if (slot == RPMSG_LITE_MAX_EPTS)
				slot = i;
		} else if (ctx->epts[i] == ept ||
			   !strcmp(ctx->epts[i]->name, name)) {
			return -EEXIST;
		}
	}
	if (slot == RPMSG_LITE_MAX_EPTS)
		return -ENOSPC;
	/* Restart on collision so automatic addresses are unique across holes. */
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i) {
		if (ctx->epts[i] && ctx->epts[i]->local_addr == addr) {
			if (local_addr != RPMSG_ADDR_ANY)
				return -EEXIST;
			++addr;
			i = (unsigned int)-1;
		}
	}
	memset(ept, 0, sizeof(*ept));
	memcpy(ept->name, name, len);
	ept->local_addr = addr;
	ept->dest_addr = RPMSG_ADDR_ANY;
	ept->cb = cb;
	ept->priv = priv;
	ept->ctx = ctx;
	ctx->epts[slot] = ept;
	return 0;
}

int rpmsg_lite_dma_destroy_ept(struct rpmsg_lite_dma *ctx,
			       struct rpmsg_lite_dma_ept *ept)
{
	unsigned int i;

	if (!registered(ctx, ept))
		return -EINVAL;
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i)
		if (ctx->epts[i] == ept)
			ctx->epts[i] = NULL;
	ept->ctx = NULL;
	ept->dest_addr = RPMSG_ADDR_ANY;
	return 0;
}

static int reclaim_tx(struct rpmsg_lite_dma *ctx)
{
	uint16_t id;
	uint32_t len;
	int ret;

	while ((ret = vring_lite_dma_get_used(&ctx->tx, &id, &len)) > 0) {
		if (id >= ctx->half || ctx->tx_free_count >= ctx->half)
			return -EPROTO;
		ctx->tx_free[ctx->tx_free_count++] = id;
	}
	return ret;
}

static int send_raw(struct rpmsg_lite_dma *ctx, uint32_t src, uint32_t dst,
		    const void *data, size_t len)
{
	struct rpmsg_hdr hdr = { src, dst, 0, (uint16_t)len, 0 };
	uint8_t *scratch = ctx->cfg.scratch;
	uint16_t id;
	int ret;

	if ((!data && len) || dst == RPMSG_ADDR_ANY)
		return -EINVAL;
	if (len > ctx->cfg.buf_size - sizeof(hdr) || len > UINT16_MAX)
		return -EMSGSIZE;
	ret = reclaim_tx(ctx);
	if (ret)
		return ret;
	if (!ctx->tx_free_count)
		return -EAGAIN;
	id = ctx->tx_free[--ctx->tx_free_count];
	memcpy(scratch, &hdr, sizeof(hdr));
	if (len)
		memcpy(scratch + sizeof(hdr), data, len);
	ret = rpmsg_mem_write(ctx->cfg.mem, buffer_addr(ctx, 1, id), scratch,
			      sizeof(hdr) + len);
	if (ret) {
		ctx->tx_free[ctx->tx_free_count++] = id;
		return ret;
	}
	ret = vring_lite_dma_add_buffer(&ctx->tx, id, buffer_addr(ctx, 1, id),
					ctx->cfg.buf_size, 0);
	if (ret) {
		ctx->tx_free[ctx->tx_free_count++] = id;
		return ret;
	}
	notify(ctx, 1);
	return 0;
}

int rpmsg_lite_dma_sendto(struct rpmsg_lite_dma *ctx,
			  struct rpmsg_lite_dma_ept *ept, const void *data,
			  size_t len, uint32_t dst)
{
	if (!registered(ctx, ept))
		return -EINVAL;
	return send_raw(ctx, ept->local_addr, dst, data, len);
}

int rpmsg_lite_dma_send(struct rpmsg_lite_dma *ctx,
			struct rpmsg_lite_dma_ept *ept, const void *data,
			size_t len)
{
	if (!registered(ctx, ept))
		return -EINVAL;
	if (ept->dest_addr == RPMSG_ADDR_ANY)
		return -ENOTCONN;
	return send_raw(ctx, ept->local_addr, ept->dest_addr, data, len);
}

int rpmsg_lite_dma_ns_announce(struct rpmsg_lite_dma *ctx,
			       struct rpmsg_lite_dma_ept *ept)
{
	struct rpmsg_ns_msg msg = { { 0 }, 0, RPMSG_NS_CREATE };

	if (!registered(ctx, ept))
		return -EINVAL;
	memcpy(msg.name, ept->name, sizeof(msg.name));
	msg.addr = ept->local_addr;
	return send_raw(ctx, ept->local_addr, RPMSG_NS_ADDR, &msg, sizeof(msg));
}

static int handle_ns(struct rpmsg_lite_dma *ctx, const void *data, size_t len)
{
	struct rpmsg_ns_msg msg;
	unsigned int i;

	if (len != sizeof(msg))
		return -EPROTO;
	memcpy(&msg, data, sizeof(msg));
	if (!memchr(msg.name, '\0', sizeof(msg.name)) || !msg.name[0] ||
	    msg.addr == RPMSG_ADDR_ANY || msg.addr == RPMSG_NS_ADDR ||
	    (msg.flags != RPMSG_NS_CREATE && msg.flags != RPMSG_NS_DESTROY))
		return -EPROTO;
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i) {
		struct rpmsg_lite_dma_ept *ept = ctx->epts[i];

		if (!ept || strcmp(ept->name, msg.name))
			continue;
		if (msg.flags == RPMSG_NS_CREATE)
			ept->dest_addr = msg.addr;
		else if (ept->dest_addr == msg.addr)
			ept->dest_addr = RPMSG_ADDR_ANY;
	}
	return 0;
}

static int dispatch(struct rpmsg_lite_dma *ctx, uint16_t id, uint32_t used_len)
{
	struct rpmsg_hdr hdr;
	uint8_t *scratch = ctx->cfg.scratch;
	uint64_t addr = buffer_addr(ctx, 0, id);
	unsigned int i;
	int ret;

	if (used_len < sizeof(hdr) || used_len > ctx->cfg.buf_size)
		return -EPROTO;
	ret = rpmsg_mem_read(ctx->cfg.mem, &hdr, addr, sizeof(hdr));
	if (ret)
		return ret;
	if (hdr.len > used_len - sizeof(hdr) || hdr.flags || hdr.reserved)
		return -EPROTO;
	if (hdr.len) {
		ret = rpmsg_mem_read(ctx->cfg.mem, scratch, addr + sizeof(hdr),
				     hdr.len);
		if (ret)
			return ret;
	}
	if (hdr.dst == RPMSG_NS_ADDR)
		return handle_ns(ctx, scratch, hdr.len);
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i) {
		struct rpmsg_lite_dma_ept *ept = ctx->epts[i];

		if (ept && ept->local_addr == hdr.dst) {
			ept->cb(ept, scratch, hdr.len, ept->priv);
			break;
		}
	}
	return 0;
}

int rpmsg_lite_dma_poll(struct rpmsg_lite_dma *ctx)
{
	uint16_t recycle[VRING_LITE_MAX_NUM];
	uint16_t id;
	uint32_t len;
	int ret, error = 0, count = 0, i;

	if (!ctx || !ctx->initialized)
		return -EINVAL;
	ret = reclaim_tx(ctx);
	if (ret)
		return ret;
	while ((ret = vring_lite_dma_get_used(&ctx->rx, &id, &len)) > 0) {
		if (id >= ctx->half)
			return -EPROTO;
		recycle[count++] = id;
		ret = dispatch(ctx, id, len);
		if (ret && !error)
			error = ret;
	}
	if (ret < 0)
		error = ret;
	/* Drain completions before reposting: otherwise a queued duplicate ID
	 * could appear busy again and publish the same buffer twice.
	 */
	for (i = 0; i < count; ++i) {
		id = recycle[i];
		ret = vring_lite_dma_add_buffer(&ctx->rx, id,
						buffer_addr(ctx, 0, id),
						ctx->cfg.buf_size,
						VRING_DESC_F_WRITE);
		if (ret)
			return ret;
		notify(ctx, 0);
	}
	ret = reclaim_tx(ctx);
	if (ret)
		return ret;
	return error ? error : count;
}

void rpmsg_lite_dma_deinit(struct rpmsg_lite_dma *ctx)
{
	uint8_t status = 0;
	unsigned int i;

	if (!ctx || !ctx->initialized)
		return;
	rpmsg_mem_write(ctx->cfg.mem, ctx->cfg.shm_phys + ctx->cfg.status_offset,
			&status, 1);
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i)
		if (ctx->epts[i]) {
			ctx->epts[i]->ctx = NULL;
			ctx->epts[i]->dest_addr = RPMSG_ADDR_ANY;
		}
	memset(ctx, 0, sizeof(*ctx));
}
