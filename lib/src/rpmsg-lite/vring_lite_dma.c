/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#include "vring_lite_dma.h"

#include <errno.h>
#include <string.h>

/* Wire field offsets, matching the __packed virtio layout of vring_lite:
 * desc[i]: addr u64@0, len u32@8, flags u16@12, next u16@14 (16 bytes each).
 * avail:   flags u16@0, idx u16@2, ring[k] u16@(4 + 2k).
 * used:    flags u16@0, idx u16@2, ring[k].id u32@(4 + 8k), .len u32@(8 + 8k).
 */
#define DESC_STRIDE 16u

static size_t used_offset(uint32_t num, uint32_t align)
{
	size_t end = DESC_STRIDE * num + 6u + 2u * num;

	return (end + align - 1u) & ~((size_t)align - 1u);
}

size_t vring_lite_dma_size(uint32_t num, uint32_t align)
{
	size_t offset;

	if (!num || num > VRING_LITE_MAX_NUM || (num & (num - 1u)) ||
	    align < 4u || (align & (align - 1u)))
		return 0;
	if ((size_t)align - 1u > SIZE_MAX - (16u * num + 6u + 2u * num))
		return 0;
	offset = used_offset(num, align);
	if (offset > SIZE_MAX - (6u + 8u * num))
		return 0;
	return offset + 6u + 8u * num;
}

static int zero_region(const struct rpmsg_mem_ops *mem, uint64_t addr,
		       size_t size)
{
	uint8_t zeros[256] = { 0 };

	while (size) {
		size_t chunk = size < sizeof(zeros) ? size : sizeof(zeros);
		int ret = rpmsg_mem_write(mem, addr, zeros, chunk);

		if (ret)
			return ret;
		addr += chunk;
		size -= chunk;
	}
	return 0;
}

int vring_lite_dma_init(struct vring_lite_dma *vr, uint32_t num,
			uint64_t dev_base, uint32_t align,
			const struct rpmsg_mem_ops *mem)
{
	size_t size = vring_lite_dma_size(num, align);
	int ret;

	if (!vr || !mem || !mem->write || !mem->read || !size ||
	    (dev_base & (align - 1u)) || (dev_base & 15u) ||
	    size > UINT64_MAX - dev_base)
		return -EINVAL;
	memset(vr, 0, sizeof(*vr));
	vr->mem = mem;
	vr->num = num;
	vr->desc_addr = dev_base;
	vr->avail_addr = dev_base + DESC_STRIDE * num;
	vr->used_addr = dev_base + used_offset(num, align);
	ret = zero_region(mem, dev_base, size);
	if (ret)
		return ret;
	return rpmsg_mem_wr16(mem, vr->avail_addr, VRING_AVAIL_F_NO_INTERRUPT);
}

int vring_lite_dma_add_buffer(struct vring_lite_dma *vr, uint16_t id,
			      uint64_t device_addr, uint32_t len, uint16_t flags)
{
	uint64_t desc = vr->desc_addr + (uint64_t)DESC_STRIDE * id;
	uint32_t slot;
	int ret;

	if (!vr || !vr->num || id >= vr->num || !len ||
	    (flags & ~VRING_DESC_F_WRITE))
		return -EINVAL;
	if (vr->busy[id] || vr->outstanding == vr->num)
		return -EAGAIN;
	ret = rpmsg_mem_wr64(vr->mem, desc, device_addr);
	if (!ret)
		ret = rpmsg_mem_wr32(vr->mem, desc + 8u, len);
	if (!ret)
		ret = rpmsg_mem_wr16(vr->mem, desc + 12u, flags);
	if (!ret)
		ret = rpmsg_mem_wr16(vr->mem, desc + 14u, 0);
	if (ret)
		return ret;
	slot = vr->avail_idx & (vr->num - 1u);
	ret = rpmsg_mem_wr16(vr->mem, vr->avail_addr + 4u + 2u * slot, id);
	if (ret)
		return ret;
	vr->busy[id] = 1;
	++vr->outstanding;
	/* Publish descriptor, payload and ring entry (all completed above)
	 * before advancing avail->idx.
	 */
	ret = rpmsg_mem_wr16(vr->mem, vr->avail_addr + 2u,
			     (uint16_t)(vr->avail_idx + 1u));
	if (ret) {
		vr->busy[id] = 0;
		--vr->outstanding;
		return ret;
	}
	++vr->avail_idx;
	return 0;
}

int vring_lite_dma_get_used(struct vring_lite_dma *vr, uint16_t *id,
			    uint32_t *len)
{
	uint16_t used_idx, pending;
	uint32_t slot, used_id;
	int ret;

	if (!vr || !vr->num || !id || !len)
		return -EINVAL;
	ret = rpmsg_mem_rd16(vr->mem, vr->used_addr + 2u, &used_idx);
	if (ret)
		return ret;
	pending = (uint16_t)(used_idx - vr->used_idx);
	if (!pending)
		return 0;
	if (pending > vr->outstanding)
		return -EPROTO;
	slot = vr->used_idx & (vr->num - 1u);
	ret = rpmsg_mem_rd32(vr->mem, vr->used_addr + 4u + 8u * slot, &used_id);
	if (!ret)
		ret = rpmsg_mem_rd32(vr->mem, vr->used_addr + 8u + 8u * slot, len);
	if (ret)
		return ret;
	++vr->used_idx;
	if (used_id >= vr->num || !vr->busy[used_id])
		return -EPROTO;
	*id = (uint16_t)used_id;
	vr->busy[used_id] = 0;
	--vr->outstanding;
	return 1;
}
