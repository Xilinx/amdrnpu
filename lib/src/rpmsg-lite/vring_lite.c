/* SPDX-License-Identifier: MIT */
#include "vring_lite.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct vring_desc) == 16, "virtio descriptor layout");
_Static_assert(sizeof(struct vring_used_elem) == 8, "virtio used layout");

static size_t used_offset(uint32_t num, uint32_t align)
{
	size_t end = sizeof(struct vring_desc) * num + 6u + 2u * num;

	return (end + align - 1u) & ~((size_t)align - 1u);
}

size_t vring_lite_size(uint32_t num, uint32_t align)
{
	size_t offset;

	if (!num || num > VRING_LITE_MAX_NUM || (num & (num - 1u)) ||
	    align < 4u || (align & (align - 1u)))
		return 0;
	/* Also guard 32-bit size_t with very large alignment. */
	if ((size_t)align - 1u > SIZE_MAX - (16u * num + 6u + 2u * num))
		return 0;
	offset = used_offset(num, align);
	if (offset > SIZE_MAX - (6u + 8u * num))
		return 0;
	return offset + 6u + 8u * num;
}

int vring_lite_init(struct vring_lite *vr, uint32_t num, void *base_ptr,
		    uint32_t align)
{
	size_t size = vring_lite_size(num, align);
	volatile uint8_t *base = base_ptr;
	size_t i;

	if (!vr || !base || !size || ((uintptr_t)base & (align - 1u)) ||
	    ((uintptr_t)base & 15u) || size > UINTPTR_MAX - (uintptr_t)base)
		return -EINVAL;
	memset(vr, 0, sizeof(*vr));
	for (i = 0; i < size; ++i)
		base[i] = 0;
	vr->num = num;
	vr->desc = (volatile struct vring_desc *)base;
	vr->avail = (volatile struct vring_avail *)(base + 16u * num);
	vr->used = (volatile struct vring_used *)(base + used_offset(num, align));
	vr->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

int vring_lite_add_buffer(struct vring_lite *vr, uint16_t id,
			  uint64_t device_addr, uint32_t len, uint16_t flags)
{
	if (!vr || !vr->num || id >= vr->num || !len ||
	    (flags & ~VRING_DESC_F_WRITE))
		return -EINVAL;
	if (vr->busy[id] || vr->outstanding == vr->num)
		return -EAGAIN;
	vr->desc[id].addr = device_addr;
	vr->desc[id].len = len;
	vr->desc[id].flags = flags;
	vr->desc[id].next = 0;
	vr->busy[id] = 1;
	++vr->outstanding;
	vr->avail->ring[vr->avail_idx & (vr->num - 1u)] = id;
	/* Publish descriptor, payload and ring entry before making it available. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	vr->avail->idx = ++vr->avail_idx;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	return 0;
}

int vring_lite_get_used(struct vring_lite *vr, uint16_t *id, uint32_t *len)
{
	uint16_t pending;
	uint32_t slot, used_id;

	if (!vr || !vr->num || !id || !len)
		return -EINVAL;
	pending = (uint16_t)(vr->used->idx - vr->used_idx);
	if (!pending)
		return 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (pending > vr->outstanding)
		return -EPROTO;
	slot = vr->used_idx & (vr->num - 1u);
	used_id = vr->used->ring[slot].id;
	*len = vr->used->ring[slot].len;
	++vr->used_idx;
	if (used_id >= vr->num || !vr->busy[used_id])
		return -EPROTO;
	*id = (uint16_t)used_id;
	vr->busy[used_id] = 0;
	--vr->outstanding;
	return 1;
}
