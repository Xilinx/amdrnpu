/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * A78 virtio *device* peer for the echo test. Mirrors the simulated peer in
 * rpmsg_lite_dma_test.c (peer_tx / peer_rx / peer_complete), but reaches OCM
 * through the /dev/mem backend. Addresses are in the driver's device-address
 * namespace (based at cfg->dev_base); the backend translates to physical.
 *
 * Wire layout (must match vring_lite_dma):
 *   desc[i]: addr u64@0, len u32@8, flags u16@12, next u16@14 (16 B).
 *   avail:   flags u16@0, idx u16@2, ring[k] u16@(4 + 2k).
 *   used:    flags u16@0, idx u16@2, ring[k].id u32@(4 + 8k), .len u32@(8 + 8k).
 */
#define _POSIX_C_SOURCE 200809L

#include "echo_device.h"

#include "rpmsg_lite_dma.h" /* struct rpmsg_hdr, VIRTIO_CONFIG_STATUS_DRIVER_OK */
#include "vring_lite_dma.h" /* VRING_DESC_F_WRITE */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DESC_STRIDE 16u
#define IDLE_SLEEP_NS 50000L /* 50 us when no work; device core stays light. */

/* Device-side view of one split virtqueue: consumer of avail, producer of used. */
struct vq_dev {
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	uint32_t num;
	uint16_t avail_seen; /* consumer index into avail ring */
	uint16_t used_idx;   /* producer index into used ring  */
};

static size_t used_offset(uint32_t num, uint32_t align)
{
	size_t end = DESC_STRIDE * num + 6u + 2u * num;

	return (end + align - 1u) & ~((size_t)align - 1u);
}

static void vq_init(struct vq_dev *vq, uint64_t base, uint32_t num,
		    uint32_t align)
{
	vq->desc_addr = base;
	vq->avail_addr = base + DESC_STRIDE * num;
	vq->used_addr = base + used_offset(num, align);
	vq->num = num;
	vq->avail_seen = 0;
	vq->used_idx = 0;
}

/* Pop the next available descriptor id, or 0 if none. Negative on backend err. */
static int vq_pop_avail(const struct rpmsg_mem_ops *mem, struct vq_dev *vq,
			uint16_t *id)
{
	uint16_t avail_idx, slot_id;
	int ret;

	ret = rpmsg_mem_rd16(mem, vq->avail_addr + 2u, &avail_idx);
	if (ret)
		return ret;
	if ((uint16_t)(avail_idx - vq->avail_seen) == 0)
		return 0;
	ret = rpmsg_mem_rd16(mem,
			     vq->avail_addr + 4u +
				     2u * (vq->avail_seen & (vq->num - 1u)),
			     &slot_id);
	if (ret)
		return ret;
	if (slot_id >= vq->num)
		return -EPROTO;
	++vq->avail_seen;
	*id = slot_id;
	return 1;
}

/* Publish a used-ring completion for descriptor id with reported length len. */
static int vq_complete(const struct rpmsg_mem_ops *mem, struct vq_dev *vq,
		       uint16_t id, uint32_t len)
{
	uint32_t slot = vq->used_idx & (vq->num - 1u);
	int ret;

	ret = rpmsg_mem_wr32(mem, vq->used_addr + 4u + 8u * slot, id);
	if (!ret)
		ret = rpmsg_mem_wr32(mem, vq->used_addr + 8u + 8u * slot, len);
	if (ret)
		return ret;
	++vq->used_idx;
	return rpmsg_mem_wr16(mem, vq->used_addr + 2u, vq->used_idx);
}

static int in_region(const struct echo_config *cfg, uint64_t addr, uint32_t len)
{
	if (addr < cfg->dev_base)
		return 0;
	if (cfg->shm_size &&
	    (addr - cfg->dev_base) + len > cfg->shm_size)
		return 0;
	return 1;
}

static int wait_driver_ok(const struct rpmsg_mem_ops *mem,
			  const struct echo_config *cfg)
{
	fprintf(stderr, "device: waiting for driver DRIVER_OK at status 0x%llx...\n",
		(unsigned long long)(cfg->dev_base + cfg->status_offset));
	for (;;) {
		uint8_t status;
		int ret = rpmsg_mem_read(mem, &status,
					 cfg->dev_base + cfg->status_offset, 1);

		if (ret)
			return ret;
		if (status & VIRTIO_CONFIG_STATUS_DRIVER_OK)
			return 0;
		struct timespec ts = { 0, 1000000L }; /* 1 ms */

		nanosleep(&ts, NULL);
	}
}

int device_echo_run(const struct rpmsg_mem_ops *mem,
		    const struct echo_config *cfg, unsigned long max_echoes)
{
	struct vq_dev tx, rx; /* driver TX (we consume), driver RX (we produce) */
	unsigned long done = 0;
	int ret;

	vq_init(&tx, cfg->dev_base + cfg->vring_tx_offset, cfg->vring_num,
		cfg->vring_align);
	vq_init(&rx, cfg->dev_base + cfg->vring_rx_offset, cfg->vring_num,
		cfg->vring_align);

	ret = wait_driver_ok(mem, cfg);
	if (ret)
		return ret;
	/* Sync our used-ring producer indices to the freshly reset ring. */
	ret = rpmsg_mem_rd16(mem, tx.used_addr + 2u, &tx.used_idx);
	if (!ret)
		ret = rpmsg_mem_rd16(mem, rx.used_addr + 2u, &rx.used_idx);
	if (ret)
		return ret;
	fprintf(stderr, "device: DRIVER_OK; echoing \"pong\" -> addr %u\n\n",
		cfg->host_ept_addr);

	while (!max_echoes || done < max_echoes) {
		uint16_t tid;
		int got = vq_pop_avail(mem, &tx, &tid);

		if (got < 0)
			return got;
		if (!got) {
			struct timespec ts = { 0, IDLE_SLEEP_NS };

			nanosleep(&ts, NULL);
			continue;
		}

		/* Read the "ping" out of the driver's TX buffer. */
		struct rpmsg_hdr hdr;
		uint64_t pbuf;
		uint8_t payload[512];

		ret = rpmsg_mem_rd64(mem, tx.desc_addr + DESC_STRIDE * tid,
				     &pbuf);
		if (ret)
			return ret;
		if (!in_region(cfg, pbuf, cfg->buf_size))
			return -EFAULT;
		ret = rpmsg_mem_read(mem, &hdr, pbuf, sizeof(hdr));
		if (ret)
			return ret;
		if (hdr.len > cfg->buf_size - sizeof(hdr) ||
		    hdr.len > sizeof(payload))
			return -EPROTO;
		if (hdr.len) {
			ret = rpmsg_mem_read(mem, payload, pbuf + sizeof(hdr),
					     hdr.len);
			if (ret)
				return ret;
		}
		/* Recycle the TX descriptor (capacity, like OpenAMP). */
		ret = vq_complete(mem, &tx, tid, cfg->buf_size);
		if (ret)
			return ret;

		/* Produce the "pong" into a free driver-RX buffer. */
		uint16_t rid;
		int rgot = vq_pop_avail(mem, &rx, &rid);

		if (rgot < 0)
			return rgot;
		if (!rgot) {
			fprintf(stderr,
				"device: no free RX buffer for pong (driver not "
				"polling?)\n");
			return -EAGAIN;
		}
		uint64_t rbuf;
		uint16_t rflags;

		ret = rpmsg_mem_rd64(mem, rx.desc_addr + DESC_STRIDE * rid,
				     &rbuf);
		if (!ret)
			ret = rpmsg_mem_rd16(mem,
					     rx.desc_addr + DESC_STRIDE * rid +
						     12u,
					     &rflags);
		if (ret)
			return ret;
		if (!(rflags & VRING_DESC_F_WRITE) ||
		    !in_region(cfg, rbuf, cfg->buf_size))
			return -EPROTO;

		struct rpmsg_hdr pong = { cfg->peer_dst, cfg->host_ept_addr, 0,
					  4, 0 };

		ret = rpmsg_mem_write(mem, rbuf, &pong, sizeof(pong));
		if (!ret)
			ret = rpmsg_mem_write(mem, rbuf + sizeof(pong), "pong",
					      4);
		if (!ret)
			ret = vq_complete(mem, &rx, rid,
					  (uint32_t)sizeof(pong) + 4u);
		if (ret)
			return ret;

		++done;
		if (done <= 4 || (done % 1000) == 0)
			fprintf(stderr,
				"device: echo #%lu: got \"%.*s\" -> sent \"pong\"\n",
				done, (int)hdr.len, payload);
	}
	fprintf(stderr, "device: %lu echoes done\n", done);
	return 0;
}
