/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * No-hardware co-simulation of the echo test. Runs the real rpmsg_lite_dma
 * driver (as the x86 host would) against device_echo_run (the A78 peer) in one
 * process, over the host-mock rpmsg_mem backend standing in for OCM. Validates
 * the vring offset math, device counter sync and the ping/pong handshake
 * without the MDB5 DMA or /dev/mem transports (those are thin copy wrappers).
 *
 * Build (native):
 *   R=../../lib/src/rpmsg-lite
 *   gcc -O2 -std=c11 -Wall -I. -Ia78 -I$R cosim_test.c a78/echo_device.c \
 *       echo_config.c $R/rpmsg_lite_dma.c $R/vring_lite_dma.c \
 *       $R/rpmsg_mem_host.c -lpthread -o cosim_test
 */
#define _POSIX_C_SOURCE 200809L

#include "echo_config.h"
#include "a78/echo_device.h"
#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_host.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define N 8

static uint8_t shm[0x80000] __attribute__((aligned(64)));
static struct rpmsg_mem_host host;
static struct rpmsg_mem_ops ops;
static struct echo_config cfg;

static void *dev_thread(void *arg)
{
	(void)arg;
	int r = device_echo_run(&ops, &cfg, N);

	fprintf(stderr, "device thread exit: %d\n", r);
	return NULL;
}

static volatile int got;
static char pbuf[64];
static size_t plen;

static void on_rx(struct rpmsg_lite_dma_ept *e, const void *d, size_t l, void *p)
{
	(void)e;
	(void)p;
	plen = l;
	memcpy(pbuf, d, l);
	got = 1;
}

int main(void)
{
	struct rpmsg_lite_dma ctx;
	struct rpmsg_lite_dma_ept ept;
	static uint8_t scratch[512];
	pthread_t th;
	int i;

	echo_config_load(&cfg, NULL);
	cfg.dev_base = 0; /* host backend maps base 0 -> shm[0] */
	cfg.a78_phys_base = 0;
	cfg.shm_size = sizeof(shm);

	rpmsg_mem_host_init(&host, &ops, shm, cfg.dev_base, sizeof(shm));

	struct rpmsg_lite_dma_cfg dcfg = {
		.mem = &ops,
		.shm_phys = cfg.dev_base,
		.status_offset = cfg.status_offset,
		.vring_tx_offset = cfg.vring_tx_offset,
		.vring_rx_offset = cfg.vring_rx_offset,
		.vring_num = cfg.vring_num,
		.vring_align = cfg.vring_align,
		.buf_offset = cfg.buf_offset,
		.buf_size = cfg.buf_size,
		.buf_count = cfg.buf_count,
		.notify = NULL,
		.scratch = scratch,
		.scratch_size = sizeof(scratch),
		.shm_size = sizeof(shm),
	};
	if (rpmsg_lite_dma_init(&ctx, &dcfg)) {
		puts("init fail");
		return 1;
	}
	if (rpmsg_lite_dma_create_ept(&ctx, &ept, cfg.ept_name,
				      cfg.host_ept_addr, on_rx, NULL)) {
		puts("ept fail");
		return 1;
	}
	pthread_create(&th, NULL, dev_thread, NULL);

	for (i = 0; i < N; ++i) {
		long spins = 0;

		got = 0;
		if (rpmsg_lite_dma_sendto(&ctx, &ept, "ping", 4, cfg.peer_dst)) {
			puts("send fail");
			return 1;
		}
		while (!got) {
			if (rpmsg_lite_dma_poll(&ctx) < 0) {
				puts("poll fail");
				return 1;
			}
			if (++spins > 500000000L) {
				puts("TIMEOUT");
				return 1;
			}
		}
		if (plen != 4 || memcmp(pbuf, "pong", 4)) {
			printf("bad echo '%.*s'\n", (int)plen, pbuf);
			return 1;
		}
		printf("echo %d ok: '%.*s'\n", i, (int)plen, pbuf);
	}
	pthread_join(th, NULL);
	puts("COSIM PASS");
	return 0;
}
