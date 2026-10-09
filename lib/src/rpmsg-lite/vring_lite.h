/* SPDX-License-Identifier: MIT */
#ifndef VRING_LITE_H
#define VRING_LITE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vring-lite requires a little-endian host"
#endif

#define VRING_LITE_MAX_NUM 256u
#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u
#define VRING_AVAIL_F_NO_INTERRUPT 1u

struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[];
};

/* Single-threaded driver-side state; shared memory must be cache coherent.
 * No descriptor chains, indirect descriptors, or EVENT_IDX negotiation.
 * Indices are uint16_t free-running counters, not ring-slot indices.
 */
struct vring_lite {
	volatile struct vring_desc *desc;
	volatile struct vring_avail *avail;
	volatile struct vring_used *used;
	uint32_t num;
	uint16_t avail_idx;
	uint16_t used_idx;
	uint16_t outstanding;
	uint8_t busy[VRING_LITE_MAX_NUM];
};

/* Includes the legacy trailing used_event/avail_event words. Returns 0 for
 * invalid parameters. base_ptr must be aligned to both align and 16 bytes.
 * init clears the shared ring: the remote must be stopped during setup.
 */
size_t vring_lite_size(uint32_t num, uint32_t align);
int vring_lite_init(struct vring_lite *vr, uint32_t num, void *base_ptr,
		    uint32_t align);
int vring_lite_add_buffer(struct vring_lite *vr, uint16_t id,
			  uint64_t device_addr, uint32_t len, uint16_t flags);
/* Returns 1 for a completion, 0 if empty, or negative errno.
 * Corrupt IDs are consumed; an impossible used-index advance is not.
 * Either is reported as -EPROTO and requires stopping/resetting the peer.
 */
int vring_lite_get_used(struct vring_lite *vr, uint16_t *id, uint32_t *len);

#endif
