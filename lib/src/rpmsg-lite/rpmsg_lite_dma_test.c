/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * Simulated-peer tests for the DMA variant. The "device memory" is a host byte
 * array (shm) reached through the host rpmsg_mem backend; the simulated remote
 * peer reaches the same ring fields and buffers through that same backend, by
 * device address, exactly as the driver side does.
 */
#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Always active, including CMake's default Release builds. */
#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static _Alignas(64) uint8_t shm[8192];
static uint8_t scratch[2048];
static struct rpmsg_mem_host host;
static struct rpmsg_mem_ops ops;
static struct rpmsg_lite_dma ctx;
static struct rpmsg_lite_dma_cfg cfg = {
	.mem = &ops,
	.shm_phys = UINT64_C(0x1234000000),
	.status_offset = 0,
	.vring_tx_offset = 64,
	.vring_rx_offset = 512,
	.vring_num = 4,
	.vring_align = 64,
	.buf_offset = 1024,
	.buf_count = 8,
	.scratch = scratch,
	.scratch_size = sizeof(scratch),
	.shm_size = sizeof(shm),
};
static unsigned int calls, notifications[2];
static uint16_t peer_rx_idx, peer_tx_idx;

/* Device-memory accessors used by the simulated peer (CHECK the backend). */
static uint16_t dev_rd16(uint64_t a)
{
	uint16_t v;

	CHECK(rpmsg_mem_rd16(&ops, a, &v) == 0);
	return v;
}

static uint32_t dev_rd32(uint64_t a)
{
	uint32_t v;

	CHECK(rpmsg_mem_rd32(&ops, a, &v) == 0);
	return v;
}

static uint64_t dev_rd64(uint64_t a)
{
	uint64_t v;

	CHECK(rpmsg_mem_rd64(&ops, a, &v) == 0);
	return v;
}

static void dev_wr16(uint64_t a, uint16_t v)
{
	CHECK(rpmsg_mem_wr16(&ops, a, v) == 0);
}

static void dev_wr32(uint64_t a, uint32_t v)
{
	CHECK(rpmsg_mem_wr32(&ops, a, v) == 0);
}

static void dev_write(uint64_t a, const void *src, size_t len)
{
	CHECK(rpmsg_mem_write(&ops, a, src, len) == 0);
}

static void dev_read(void *dst, uint64_t a, size_t len)
{
	CHECK(rpmsg_mem_read(&ops, dst, a, len) == 0);
}

static void on_notify(void *priv, int id)
{
	CHECK(priv == notifications);
	CHECK(id == 0 || id == 1);
	CHECK(shm[0] == 7);
	++notifications[id];
}

static void on_rx(struct rpmsg_lite_dma_ept *ept, const void *data, size_t len,
		  void *priv)
{
	CHECK(priv == &calls);
	CHECK(len == 4 && !memcmp(data, "ping", len));
	++calls;
	/* Reply while the RX buffer is still owned by the driver. */
	CHECK(rpmsg_lite_dma_send(&ctx, ept, "pong", 4) == 0);
}

static uint64_t peer_buffer(struct vring_lite_dma *vq, uint16_t id)
{
	uint64_t addr = dev_rd64(vq->desc_addr + 16u * id);

	CHECK(addr >= cfg.shm_phys);
	CHECK(addr - cfg.shm_phys < sizeof(shm));
	return addr;
}

static void peer_complete(struct vring_lite_dma *vq, uint32_t id, uint32_t len)
{
	uint16_t idx = dev_rd16(vq->used_addr + 2u);
	uint32_t slot = idx & (vq->num - 1u);

	dev_wr32(vq->used_addr + 4u + 8u * slot, id);
	dev_wr32(vq->used_addr + 8u + 8u * slot, len);
	dev_wr16(vq->used_addr + 2u, (uint16_t)(idx + 1u));
}

static void peer_rx(uint32_t dst, const void *data, uint16_t len,
		    uint32_t reported_len)
{
	struct rpmsg_hdr hdr = { 42, dst, 0, len, 0 };
	uint64_t buf;
	uint16_t id;

	CHECK(peer_rx_idx != dev_rd16(ctx.rx.avail_addr + 2u));
	id = dev_rd16(ctx.rx.avail_addr + 4u +
		      2u * (peer_rx_idx++ & (ctx.rx.num - 1u)));
	CHECK(dev_rd16(ctx.rx.desc_addr + 16u * id + 12u) == VRING_DESC_F_WRITE);
	CHECK(dev_rd32(ctx.rx.desc_addr + 16u * id + 8u) == 512);
	buf = peer_buffer(&ctx.rx, id);
	dev_write(buf, &hdr, sizeof(hdr));
	if (data)
		dev_write(buf + sizeof(hdr), data, len);
	peer_complete(&ctx.rx, id, reported_len);
}

static void peer_tx(uint32_t dst, const void *data, uint16_t len)
{
	struct rpmsg_hdr hdr;
	uint8_t payload[512];
	uint64_t buf;
	uint16_t id;

	CHECK(peer_tx_idx != dev_rd16(ctx.tx.avail_addr + 2u));
	id = dev_rd16(ctx.tx.avail_addr + 4u +
		      2u * (peer_tx_idx++ & (ctx.tx.num - 1u)));
	CHECK(dev_rd16(ctx.tx.desc_addr + 16u * id + 12u) == 0);
	/* Pinned OpenAMP publishes/recycles capacity, not message length. */
	CHECK(dev_rd32(ctx.tx.desc_addr + 16u * id + 8u) == 512);
	buf = peer_buffer(&ctx.tx, id);
	dev_read(&hdr, buf, sizeof(hdr));
	CHECK(hdr.dst == dst && hdr.len == len);
	CHECK(!hdr.flags && !hdr.reserved);
	if (len) {
		dev_read(payload, buf + sizeof(hdr), len);
		CHECK(!memcmp(payload, data, len));
	}
	peer_complete(&ctx.tx, id, 512);
}

static void test_vring(void)
{
	struct vring_lite_dma vr;
	uint16_t id;
	uint32_t len;
	unsigned int i;

	CHECK(vring_lite_dma_size(4, 64) == 166);
	CHECK(vring_lite_dma_size(3, 64) == 0);
	CHECK(vring_lite_dma_size(4, 3) == 0);
	CHECK(vring_lite_dma_size(VRING_LITE_MAX_NUM * 2, 4) == 0);
	CHECK(vring_lite_dma_init(&vr, 4, cfg.shm_phys + 1, 64, &ops) == -EINVAL);
	CHECK(vring_lite_dma_init(&vr, 4, cfg.shm_phys, 64, &ops) == 0);
	CHECK(vr.avail_addr == cfg.shm_phys + 64);
	CHECK(vr.used_addr == cfg.shm_phys + 128);
	CHECK(vring_lite_dma_add_buffer(&vr, 0, cfg.shm_phys, 512,
					VRING_DESC_F_NEXT) == -EINVAL);
	for (i = 0; i < 4; ++i)
		CHECK(vring_lite_dma_add_buffer(&vr, i, cfg.shm_phys + i * 512,
						512, VRING_DESC_F_WRITE) == 0);
	CHECK(vring_lite_dma_add_buffer(&vr, 0, cfg.shm_phys, 512, 0) == -EAGAIN);
	/* Out-of-order completion, then every uint16_t index wrap boundary. */
	peer_complete(&vr, 2, 37);
	CHECK(vring_lite_dma_get_used(&vr, &id, &len) == 1 && id == 2 && len == 37);
	for (i = 0; i < 70000; ++i) {
		CHECK(vring_lite_dma_add_buffer(&vr, 2, cfg.shm_phys, 512, 0) == 0);
		peer_complete(&vr, 2, 512);
		CHECK(vring_lite_dma_get_used(&vr, &id, &len) == 1 && id == 2);
		CHECK(vring_lite_dma_get_used(&vr, &id, &len) == 0);
	}
	peer_complete(&vr, 4, 512);
	CHECK(vring_lite_dma_get_used(&vr, &id, &len) == -EPROTO);
	CHECK(vring_lite_dma_init(&vr, 4, cfg.shm_phys, 4, &ops) == 0);
	CHECK(vr.used_addr == cfg.shm_phys + 80);
	CHECK(vring_lite_dma_add_buffer(&vr, 0, cfg.shm_phys, 512, 0) == 0);
	peer_complete(&vr, 0, 512);
	peer_complete(&vr, 0, 512);
	CHECK(vring_lite_dma_get_used(&vr, &id, &len) == -EPROTO);
}

static void test_config(void)
{
	struct rpmsg_lite_dma_cfg bad;

	bad = cfg;
	bad.buf_count = 3;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.buf_count = 10;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.vring_rx_offset = bad.vring_tx_offset;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.status_offset = bad.buf_offset;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	++bad.vring_tx_offset;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.buf_offset = SIZE_MAX - 100;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.shm_phys = UINT64_MAX - 100;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.shm_size = 2048;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.buf_size = 15;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.scratch = NULL;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
	bad = cfg;
	bad.scratch_size = 32;
	CHECK(rpmsg_lite_dma_init(&ctx, &bad) == -EINVAL);
}

static void test_rpmsg(void)
{
	struct rpmsg_lite_dma_ept ept, other;
	struct rpmsg_ns_msg ns = { "echo", 42, RPMSG_NS_CREATE };
	unsigned int i;

	memset(shm, 0xa5, sizeof(shm));
	cfg.notify = on_notify;
	cfg.priv = notifications;
	CHECK(rpmsg_lite_dma_init(&ctx, &cfg) == 0);
	CHECK(shm[0] == 7 && ctx.rx.avail_idx == 4 && ctx.tx.avail_idx == 0);
	CHECK(ctx.cfg.buf_size == 512 && ctx.tx_free_count == 4);
	CHECK(dev_rd64(ctx.rx.desc_addr) == cfg.shm_phys + cfg.buf_offset);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &ept, "echo", RPMSG_ADDR_ANY,
					on_rx, &calls) == 0);
	CHECK(ept.local_addr == 1024);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &other, "other", 1024,
					on_rx, &calls) == -EEXIST);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &other, "echo", 25,
					on_rx, &calls) == -EEXIST);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &other, "other", RPMSG_NS_ADDR,
					on_rx, &calls) == -EINVAL);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &other, "other", RPMSG_ADDR_ANY,
					on_rx, &calls) == 0);
	CHECK(other.local_addr == 1025);
	CHECK(rpmsg_lite_dma_send(&ctx, &ept, "ping", 4) == -ENOTCONN);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &ept, NULL, 1, 42) == -EINVAL);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &ept, "ping", 497, 42) == -EMSGSIZE);
	peer_rx(RPMSG_NS_ADDR, &ns, sizeof(ns), 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == 1 && ept.dest_addr == 42);
	CHECK(rpmsg_lite_dma_ns_announce(&ctx, &ept) == 0);
	ns.addr = ept.local_addr;
	peer_tx(RPMSG_NS_ADDR, &ns, sizeof(ns));
	CHECK(rpmsg_lite_dma_poll(&ctx) == 0 && ctx.tx_free_count == 4);
	for (i = 0; i < 4; ++i)
		CHECK(rpmsg_lite_dma_send(&ctx, &ept, "ping", 4) == 0);
	CHECK(rpmsg_lite_dma_send(&ctx, &ept, "ping", 4) == -EAGAIN);
	for (i = 0; i < 4; ++i)
		peer_tx(42, "ping", 4);
	/* send reclaims completions without requiring a preceding poll. */
	CHECK(rpmsg_lite_dma_send(&ctx, &ept, "ping", 4) == 0);
	peer_tx(42, "ping", 4);
	for (i = 0; i < 70000; ++i) {
		peer_rx(ept.local_addr, "ping", 4, 512);
		CHECK(rpmsg_lite_dma_poll(&ctx) == 1);
		peer_tx(42, "pong", 4);
	}
	CHECK(calls == 70000 && notifications[0] >= 70001);
	CHECK(notifications[1] >= 70001);
	/* Both queues wrap; multiple RX completions are consumed in one poll. */
	peer_rx(ept.local_addr, "ping", 4, 20);
	peer_rx(ept.local_addr, "ping", 4, 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == 2);
	peer_tx(42, "pong", 4);
	peer_tx(42, "pong", 4);
	peer_rx(999, "ping", 4, 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == 1);
	peer_rx(ept.local_addr, "ping", 4, 17);
	CHECK(rpmsg_lite_dma_poll(&ctx) == -EPROTO);
	peer_rx(ept.local_addr, "ping", 4, 513);
	CHECK(rpmsg_lite_dma_poll(&ctx) == -EPROTO);
	memset(ns.name, 'x', sizeof(ns.name));
	peer_rx(RPMSG_NS_ADDR, &ns, sizeof(ns), 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == -EPROTO);
	memcpy(ns.name, "echo", 5);
	ns.addr = 42;
	ns.flags = RPMSG_NS_DESTROY;
	peer_rx(RPMSG_NS_ADDR, &ns, sizeof(ns), 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == 1 && ept.dest_addr == RPMSG_ADDR_ANY);
	CHECK(rpmsg_lite_dma_send(&ctx, &ept, "ping", 4) == -ENOTCONN);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &ept, NULL, 0, 42) == 0);
	peer_tx(42, "", 0);
	CHECK(rpmsg_lite_dma_destroy_ept(&ctx, &other) == 0);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &other, "", 0, 42) == -EINVAL);
	rpmsg_lite_dma_deinit(&ctx);
	CHECK(shm[0] == 0 && !ept.ctx && !ctx.initialized);
	CHECK(rpmsg_lite_dma_poll(&ctx) == -EINVAL);
	CHECK(rpmsg_lite_dma_send(&ctx, &ept, "", 0) == -EINVAL);
	rpmsg_lite_dma_deinit(&ctx);
	cfg.notify = NULL;
	CHECK(rpmsg_lite_dma_init(&ctx, &cfg) == 0);
	rpmsg_lite_dma_deinit(&ctx);
}

static void test_endpoints_and_small_ring(void)
{
	struct rpmsg_lite_dma_ept epts[RPMSG_LITE_MAX_EPTS], extra;
	struct rpmsg_lite_dma_cfg small = cfg;
	char name[RPMSG_LITE_NAME_SIZE];
	char payload[48] = { 0 };
	unsigned int i;

	small.vring_num = 1;
	small.vring_align = 4;
	small.buf_count = 2;
	small.buf_size = 64;
	small.notify = NULL;
	small.shm_size = 0;
	CHECK(rpmsg_lite_dma_init(&ctx, &small) == 0);
	for (i = 0; i < RPMSG_LITE_MAX_EPTS; ++i) {
		snprintf(name, sizeof(name), "service%u", i);
		CHECK(rpmsg_lite_dma_create_ept(&ctx, &epts[i], name,
						RPMSG_ADDR_ANY, on_rx, &calls) == 0);
		CHECK(epts[i].local_addr == 1024 + i);
	}
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &extra, "overflow", RPMSG_ADDR_ANY,
					on_rx, &calls) == -ENOSPC);
	CHECK(rpmsg_lite_dma_destroy_ept(&ctx, &epts[0]) == 0);
	memset(name, 'x', sizeof(name));
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &extra, name, RPMSG_ADDR_ANY,
					on_rx, &calls) == -EINVAL);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &extra, "replacement", RPMSG_ADDR_ANY,
					on_rx, &calls) == 0);
	CHECK(extra.local_addr == 1024);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &extra, payload, sizeof(payload), 42) == 0);
	CHECK(dev_rd32(ctx.tx.desc_addr + 8u) == 64);
	CHECK(dev_rd64(ctx.tx.desc_addr) == small.shm_phys + small.buf_offset + 64);
	CHECK(rpmsg_lite_dma_sendto(&ctx, &extra, payload, sizeof(payload), 42) == -EAGAIN);
	peer_complete(&ctx.tx, 0, 64);
	CHECK(rpmsg_lite_dma_poll(&ctx) == 0);
	rpmsg_lite_dma_deinit(&ctx);
	CHECK(!extra.ctx && !epts[1].ctx);
}

static void test_duplicate_rx_completion(void)
{
	struct rpmsg_lite_dma_ept ept;
	unsigned int before = calls;
	uint16_t id;

	cfg.notify = NULL;
	peer_rx_idx = peer_tx_idx = 0;
	CHECK(rpmsg_lite_dma_init(&ctx, &cfg) == 0);
	CHECK(rpmsg_lite_dma_create_ept(&ctx, &ept, "echo", RPMSG_ADDR_ANY,
					on_rx, &calls) == 0);
	ept.dest_addr = 42;
	peer_rx(ept.local_addr, "ping", 4, 512);
	id = (uint16_t)dev_rd32(ctx.rx.used_addr + 4u);
	peer_complete(&ctx.rx, id, 512);
	CHECK(rpmsg_lite_dma_poll(&ctx) == -EPROTO);
	CHECK(calls == before + 1);
	CHECK(ctx.rx.avail_idx == 5 && ctx.rx.outstanding == 4);
	rpmsg_lite_dma_deinit(&ctx);
}

int main(void)
{
	rpmsg_mem_host_init(&host, &ops, shm, cfg.shm_phys, sizeof(shm));
	test_vring();
	test_config();
	test_rpmsg();
	test_endpoints_and_small_ring();
	test_duplicate_rx_completion();
	puts("rpmsg-lite-dma: simulated-peer tests passed");
	return 0;
}
