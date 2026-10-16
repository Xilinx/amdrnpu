/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * rpmsg_mem backend over /dev/mem for the A78 device peer.
 *
 * The A78 reaches the shared OCM directly (no DMA) by mmap'ing /dev/mem. The
 * rest of the device code addresses memory in the same "device-address"
 * namespace the x86 driver uses (based at dev_base); this backend translates
 *   device_addr -> phys_base + (device_addr - dev_base)
 * and copies to/from the mapping. Opened O_SYNC so the OCM window is mapped
 * uncached, matching devmem2 and keeping the x86 PCIe-DMA writes coherent with
 * A78 reads. The OCM region should be reserved no-map for this to hold.
 */
#ifndef MEM_DEVMEM_H
#define MEM_DEVMEM_H

#include <stddef.h>
#include <stdint.h>

#include "rpmsg_mem.h"

struct mem_devmem {
	int fd;
	volatile uint8_t *map;	/* mmap base (page-aligned to phys_base).     */
	size_t map_len;
	uint64_t dev_base;	/* device-address base (translation origin).  */
	uint64_t phys_base;	/* physical base actually mapped.             */
	size_t page_slop;	/* phys_base - page_aligned(phys_base).       */
};

/* Maps [phys_base, phys_base+len) of /dev/mem and fills m/ops. dev_base is the
 * device-address value that corresponds to phys_base. Returns 0 or -errno.
 */
int mem_devmem_open(struct mem_devmem *m, struct rpmsg_mem_ops *ops,
		    uint64_t dev_base, uint64_t phys_base, size_t len);

void mem_devmem_close(struct mem_devmem *m);

#endif /* MEM_DEVMEM_H */
