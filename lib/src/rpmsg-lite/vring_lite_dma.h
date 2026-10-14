/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * vring_lite_dma - DMA variant of vring-lite.
 *
 * Same split-virtqueue driver semantics as vring_lite (no chains, no indirect
 * descriptors, no EVENT_IDX), but the ring lives in device memory that the host
 * cannot map. Instead of dereferencing volatile pointers, every descriptor,
 * avail and used field is accessed by device address through a pluggable
 * rpmsg_mem backend. Indices are uint16_t free-running counters.
 */
#ifndef VRING_LITE_DMA_H
#define VRING_LITE_DMA_H

#include <stddef.h>
#include <stdint.h>

#include "rpmsg_mem.h"

#define VRING_LITE_MAX_NUM 256u
#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u
#define VRING_AVAIL_F_NO_INTERRUPT 1u

/* Single-threaded driver-side state; the ring lives in device memory reached
 * only through mem. desc_addr/avail_addr/used_addr are device addresses, never
 * host pointers.
 */
struct vring_lite_dma {
	const struct rpmsg_mem_ops *mem;
	uint64_t desc_addr;
	uint64_t avail_addr;
	uint64_t used_addr;
	uint32_t num;
	uint16_t avail_idx;
	uint16_t used_idx;
	uint16_t outstanding;
	uint8_t busy[VRING_LITE_MAX_NUM];
};

/* Includes the legacy trailing used_event/avail_event words. Returns 0 for
 * invalid parameters. Matches vring_lite_size exactly so both variants agree.
 */
size_t vring_lite_dma_size(uint32_t num, uint32_t align);

/* dev_base is the device address of the ring (must be align- and 16-aligned).
 * init clears the shared ring via the backend: the remote must be stopped
 * during setup. Returns 0 or negative errno.
 */
int vring_lite_dma_init(struct vring_lite_dma *vr, uint32_t num,
			uint64_t dev_base, uint32_t align,
			const struct rpmsg_mem_ops *mem);
int vring_lite_dma_add_buffer(struct vring_lite_dma *vr, uint16_t id,
			      uint64_t device_addr, uint32_t len, uint16_t flags);
/* Returns 1 for a completion, 0 if empty, or negative errno.
 * Corrupt IDs are consumed; an impossible used-index advance is not.
 * Either is reported as -EPROTO and requires stopping/resetting the peer.
 */
int vring_lite_dma_get_used(struct vring_lite_dma *vr, uint16_t *id,
			    uint32_t *len);

#endif /* VRING_LITE_DMA_H */
