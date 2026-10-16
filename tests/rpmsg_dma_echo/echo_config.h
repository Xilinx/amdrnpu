/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * Shared memory-map configuration for the rpmsg-lite DMA echo test. Parsed by
 * BOTH the x86 host (driver side) and the A78 device peer so the vring/buffer
 * layout can never drift between the two sides.
 *
 * Address model: every ring/buffer address used by either side lives in the
 * "device-address" namespace based at dev_base (this is rpmsg_lite_dma's
 * shm_phys and the MDB5 pwrite/pread offset). The A78 reaches the same bytes
 * through /dev/mem at a78_phys_base; its backend translates
 *   device_addr -> a78_phys_base + (device_addr - dev_base).
 * When dev_base == a78_phys_base the translation is identity (expected for a
 * raw OCM aperture; confirm with the address-equality bring-up step).
 */
#ifndef ECHO_CONFIG_H
#define ECHO_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define ECHO_PATH_MAX 256u
#define ECHO_NAME_MAX 32u

struct echo_config {
	uint64_t dev_base;	/* shm_phys / MDB5 device-address base.          */
	uint64_t a78_phys_base; /* Physical base the A78 maps via /dev/mem.      */
	size_t shm_size;	/* Region size for bounds checks (0 = unchecked).*/

	size_t status_offset;
	size_t vring_tx_offset; /* Driver TX (device consumes). OpenAMP svq.     */
	size_t vring_rx_offset; /* Driver RX (device produces). OpenAMP rvq.     */
	size_t buf_offset;

	uint32_t vring_num;
	uint32_t vring_align;
	uint32_t buf_count; /* Even; first half RX, second half TX.             */
	uint32_t buf_size;  /* Includes the 16-byte rpmsg header.               */

	uint32_t peer_dst;  /* Fixed destination addr the host sends "ping" to. */
	uint32_t host_ept_addr; /* Driver-side ept addr; A78 echoes "pong" here.*/
	uint32_t iters;	    /* Host echo iterations.                            */
	char ept_name[ECHO_NAME_MAX];

	char mdb5_write[ECHO_PATH_MAX]; /* x86 MEM_TO_DEV node.                  */
	char mdb5_read[ECHO_PATH_MAX];	/* x86 DEV_TO_MEM node.                  */
};

/* Fill cfg with defaults, then overlay key=value lines from path (if non-NULL
 * and readable). Numeric values accept 0x.. (strtoull base 0). Returns 0 on
 * success, negative errno on a parse/open error. Unknown keys are rejected.
 */
int echo_config_load(struct echo_config *cfg, const char *path);

/* Human-readable dump to stderr for bring-up. */
void echo_config_dump(const struct echo_config *cfg);

#endif /* ECHO_CONFIG_H */
