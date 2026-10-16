/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * x86 host side of the rpmsg-lite DMA echo test (driver role).
 *
 * Drives the rpmsg_lite_dma driver over the MDB5 DMA backend: the vrings and
 * buffer pool live in the T20 endpoint's OCM, reached by positioned pwrite/pread
 * (one field access = one DMA). Sends "ping" to a fixed peer address, polls for
 * the A78 device peer's "pong", and reports the MDB5 DMA count per message by
 * wrapping the backend in a counting shim.
 *
 * Poll-only: no IPI/doorbell. The A78 device peer must be running first.
 */
#define _POSIX_C_SOURCE 200809L

#include "echo_config.h"
#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_mdb5.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- DMA-counting shim: wraps an rpmsg_mem backend and tallies transfers. -- */
struct dma_counter {
	const struct rpmsg_mem_ops *inner;
	unsigned long reads;
	unsigned long writes;
	unsigned long long rbytes;
	unsigned long long wbytes;
};

static int cnt_write(void *priv, uint64_t dev_addr, const void *src, size_t len)
{
	struct dma_counter *c = priv;

	c->writes++;
	c->wbytes += len;
	return c->inner->write(c->inner->priv, dev_addr, src, len);
}

static int cnt_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct dma_counter *c = priv;

	c->reads++;
	c->rbytes += len;
	return c->inner->read(c->inner->priv, dst, dev_addr, len);
}

struct dma_snapshot {
	unsigned long reads, writes;
};

static struct dma_snapshot snap(const struct dma_counter *c)
{
	return (struct dma_snapshot){ c->reads, c->writes };
}

static void report(const char *tag, const struct dma_counter *c,
		   struct dma_snapshot s0)
{
	fprintf(stderr, "  %-14s %lu DMAs (%lu read, %lu write)\n", tag,
		(c->reads - s0.reads) + (c->writes - s0.writes),
		c->reads - s0.reads, c->writes - s0.writes);
}

/* ---- echo endpoint callback: captures the "pong". ------------------------- */
struct echo_ctx {
	volatile int got;
	char buf[64];
	size_t len;
};

static void on_rx(struct rpmsg_lite_dma_ept *ept, const void *data, size_t len,
		  void *priv)
{
	struct echo_ctx *e = priv;

	(void)ept;
	e->len = len < sizeof(e->buf) ? len : sizeof(e->buf);
	memcpy(e->buf, data, e->len);
	e->got = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *cfg_path = argc > 1 ? argv[1] : "../echo_config.ini";
	struct echo_config cfg;
	struct rpmsg_mem_mdb5 mdb5;
	struct rpmsg_mem_ops mdb5_ops;
	struct dma_counter counter;
	struct rpmsg_mem_ops counting_ops;
	struct rpmsg_lite_dma ctx;
	struct rpmsg_lite_dma_ept ept;
	struct echo_ctx echo = { 0 };
	uint8_t *scratch;
	unsigned int i, ok = 0;
	int ret;

	ret = echo_config_load(&cfg, cfg_path);
	if (ret) {
		fprintf(stderr, "config load '%s': %s\n", cfg_path,
			strerror(-ret));
		return 1;
	}
	echo_config_dump(&cfg);

	scratch = malloc(cfg.buf_size);
	if (!scratch) {
		fprintf(stderr, "scratch alloc failed\n");
		return 1;
	}

	ret = rpmsg_mem_mdb5_open(&mdb5, &mdb5_ops, cfg.mdb5_write,
				  cfg.mdb5_read);
	if (ret) {
		fprintf(stderr, "mdb5 open (%s / %s): %s\n", cfg.mdb5_write,
			cfg.mdb5_read, strerror(-ret));
		free(scratch);
		return 1;
	}

	counter = (struct dma_counter){ .inner = &mdb5_ops };
	counting_ops.write = cnt_write;
	counting_ops.read = cnt_read;
	counting_ops.priv = &counter;

	struct rpmsg_lite_dma_cfg dcfg = {
		.mem = &counting_ops,
		.shm_phys = cfg.dev_base,
		.status_offset = cfg.status_offset,
		.vring_tx_offset = cfg.vring_tx_offset,
		.vring_rx_offset = cfg.vring_rx_offset,
		.vring_num = cfg.vring_num,
		.vring_align = cfg.vring_align,
		.buf_offset = cfg.buf_offset,
		.buf_size = cfg.buf_size,
		.buf_count = cfg.buf_count,
		.notify = NULL, /* poll-only */
		.priv = NULL,
		.scratch = scratch,
		.scratch_size = cfg.buf_size,
		.shm_size = cfg.shm_size,
	};

	fprintf(stderr, "init rings in OCM (this resets the shared region)...\n");
	ret = rpmsg_lite_dma_init(&ctx, &dcfg);
	if (ret) {
		fprintf(stderr, "rpmsg_lite_dma_init: %s\n", strerror(-ret));
		goto out;
	}
	ret = rpmsg_lite_dma_create_ept(&ctx, &ept, cfg.ept_name,
					cfg.host_ept_addr, on_rx, &echo);
	if (ret) {
		fprintf(stderr, "create_ept: %s\n", strerror(-ret));
		goto out;
	}
	fprintf(stderr, "ept '%s' local_addr=%u, sending to peer_dst=%u\n\n",
		cfg.ept_name, ept.local_addr, cfg.peer_dst);

	for (i = 0; i < cfg.iters; ++i) {
		struct dma_snapshot s;
		uint64_t t0, t1;
		uint64_t deadline;

		echo.got = 0;
		fprintf(stderr, "[%u] send \"ping\":\n", i);
		s = snap(&counter);
		t0 = now_ns();
		ret = rpmsg_lite_dma_sendto(&ctx, &ept, "ping", 4,
					    cfg.peer_dst);
		if (ret) {
			fprintf(stderr, "  sendto: %s\n", strerror(-ret));
			goto out;
		}
		report("send:", &counter, s);

		/* Spin-poll for the pong (poll-only transport). */
		deadline = now_ns() + 5ull * 1000000000ull;
		unsigned int polls = 0;
		struct dma_snapshot rs = snap(&counter);

		while (!echo.got) {
			ret = rpmsg_lite_dma_poll(&ctx);
			if (ret < 0) {
				fprintf(stderr, "  poll: %s\n", strerror(-ret));
				goto out;
			}
			++polls;
			if (now_ns() > deadline) {
				fprintf(stderr,
					"  TIMEOUT waiting for pong "
					"(is the A78 device peer running?)\n");
				goto out;
			}
		}
		t1 = now_ns();
		report("recv path:", &counter, rs);
		fprintf(stderr,
			"  got \"%.*s\" (%zu B) after %u poll(s), rt=%.1f us\n\n",
			(int)echo.len, echo.buf, echo.len, polls,
			(double)(t1 - t0) / 1000.0);
		if (echo.len == 4 && !memcmp(echo.buf, "pong", 4))
			++ok;
	}

	fprintf(stderr, "echo: %u/%u round-trips validated \"pong\"\n", ok,
		cfg.iters);
	fprintf(stderr,
		"totals: %lu read + %lu write DMAs (%llu rd B, %llu wr B)\n",
		counter.reads, counter.writes, counter.rbytes,
		counter.wbytes);
	ret = (ok == cfg.iters) ? 0 : 2;

out:
	rpmsg_lite_dma_deinit(&ctx);
	rpmsg_mem_mdb5_close(&mdb5);
	free(scratch);
	return ret ? (ret < 0 ? 1 : ret) : 0;
}
