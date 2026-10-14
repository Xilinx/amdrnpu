/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * rpmsg_mem - pluggable host<->device memory backend for the DMA variant of
 * rpmsg-lite.
 *
 * On targets with no shared memory mapped into a host BAR, the vring and buffer
 * pool cannot be dereferenced directly. Every ring-field access and payload copy
 * is instead routed through this backend, which moves bytes by device address
 * (shm_phys + offset). The MDB5 DMA engine is one such backend; a host-memory
 * mock is another (for hardware-free tests).
 *
 * The backend exposes only bulk read/write. The typed READ16/32/64 and
 * WRITE16/32/64 helpers below wrap those two calls, little-endian, and are the
 * replacements for the mapped-pointer dereferences of the CPU variant.
 *
 * Ordering: write() and read() must make their effect globally visible before
 * returning (synchronous DMA completion for the real backend). Each call is
 * therefore its own ordering barrier, replacing the CPU variant's atomic fences.
 */
#ifndef RPMSG_MEM_H
#define RPMSG_MEM_H

#include <stddef.h>
#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "rpmsg-mem requires a little-endian host"
#endif

/*
 * Bulk copy between a host buffer and device memory addressed by dev_addr.
 * len may be any size >= 1. Both calls return 0 on success or a negative errno;
 * a short or failed transfer must be reported as an error, not a partial copy.
 */
struct rpmsg_mem_ops {
	int (*write)(void *priv, uint64_t dev_addr, const void *src, size_t len);
	int (*read)(void *priv, void *dst, uint64_t dev_addr, size_t len);
	void *priv;
};

static inline int rpmsg_mem_write(const struct rpmsg_mem_ops *m,
				  uint64_t dev_addr, const void *src, size_t len)
{
	return m->write(m->priv, dev_addr, src, len);
}

static inline int rpmsg_mem_read(const struct rpmsg_mem_ops *m, void *dst,
				 uint64_t dev_addr, size_t len)
{
	return m->read(m->priv, dst, dev_addr, len);
}

static inline int rpmsg_mem_wr16(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };

	return m->write(m->priv, dev_addr, b, sizeof(b));
}

static inline int rpmsg_mem_wr32(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
			 (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

	return m->write(m->priv, dev_addr, b, sizeof(b));
}

static inline int rpmsg_mem_wr64(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint64_t v)
{
	uint8_t b[8];
	unsigned int i;

	for (i = 0; i < 8; ++i)
		b[i] = (uint8_t)(v >> (8u * i));
	return m->write(m->priv, dev_addr, b, sizeof(b));
}

static inline int rpmsg_mem_rd16(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint16_t *out)
{
	uint8_t b[2];
	int ret = m->read(m->priv, b, dev_addr, sizeof(b));

	if (ret)
		return ret;
	*out = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
	return 0;
}

static inline int rpmsg_mem_rd32(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint32_t *out)
{
	uint8_t b[4];
	int ret = m->read(m->priv, b, dev_addr, sizeof(b));

	if (ret)
		return ret;
	*out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

static inline int rpmsg_mem_rd64(const struct rpmsg_mem_ops *m,
				 uint64_t dev_addr, uint64_t *out)
{
	uint8_t b[8];
	uint64_t v = 0;
	unsigned int i;
	int ret = m->read(m->priv, b, dev_addr, sizeof(b));

	if (ret)
		return ret;
	for (i = 0; i < 8; ++i)
		v |= (uint64_t)b[i] << (8u * i);
	*out = v;
	return 0;
}

#endif /* RPMSG_MEM_H */
