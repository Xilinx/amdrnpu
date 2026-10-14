/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * rpmsg_mem_host - mock rpmsg_mem backend over a host backing buffer.
 *
 * Simulates device memory as a plain byte array: device address dev_addr maps
 * to base[dev_addr - phys]. Used by the hardware-free DMA tests, where the
 * "remote peer" reads and writes the same backing buffer directly. Not for
 * production: real targets use the MDB5 backend.
 */
#ifndef RPMSG_MEM_HOST_H
#define RPMSG_MEM_HOST_H

#include "rpmsg_mem.h"

struct rpmsg_mem_host {
	uint8_t *base;	/* Backing buffer standing in for device memory. */
	uint64_t phys;	/* Device address that maps to base[0]. */
	size_t size;	/* Length of base in bytes. */
};

/* Fills ops to point at host; host must outlive any use of ops. */
void rpmsg_mem_host_init(struct rpmsg_mem_host *host, struct rpmsg_mem_ops *ops,
			 uint8_t *base, uint64_t phys, size_t size);

#endif /* RPMSG_MEM_HOST_H */
