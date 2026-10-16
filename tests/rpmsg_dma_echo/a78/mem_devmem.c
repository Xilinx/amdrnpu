/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "mem_devmem.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Translate a device address to an offset into the mapping, bounds-checked. */
static int xlate(struct mem_devmem *m, uint64_t dev_addr, size_t len,
		 size_t *off_out)
{
	uint64_t off;

	if (dev_addr < m->dev_base)
		return -EFAULT;
	off = (dev_addr - m->dev_base) + m->page_slop;
	if (len > m->map_len || off > m->map_len - len)
		return -EFAULT;
	*off_out = (size_t)off;
	return 0;
}

static int devmem_write(void *priv, uint64_t dev_addr, const void *src,
			size_t len)
{
	struct mem_devmem *m = priv;
	size_t off;
	int ret = xlate(m, dev_addr, len, &off);

	if (ret)
		return ret;
	memcpy((void *)(m->map + off), src, len);
	/* Make the store globally visible before returning (per rpmsg_mem
	 * contract); uncached OCM + DMB is sufficient on aarch64.
	 */
	__sync_synchronize();
	return 0;
}

static int devmem_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct mem_devmem *m = priv;
	size_t off;
	int ret = xlate(m, dev_addr, len, &off);

	if (ret)
		return ret;
	__sync_synchronize();
	memcpy(dst, (const void *)(m->map + off), len);
	return 0;
}

int mem_devmem_open(struct mem_devmem *m, struct rpmsg_mem_ops *ops,
		    uint64_t dev_base, uint64_t phys_base, size_t len)
{
	long pg = sysconf(_SC_PAGESIZE);
	uint64_t aligned;
	size_t slop, map_len;
	void *map;

	if (!m || !ops || !len || pg <= 0)
		return -EINVAL;
	aligned = phys_base & ~((uint64_t)pg - 1u);
	slop = (size_t)(phys_base - aligned);
	map_len = slop + len;
	/* Round the mapping up to a page. */
	map_len = (map_len + (size_t)pg - 1u) & ~((size_t)pg - 1u);

	memset(m, 0, sizeof(*m));
	m->fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (m->fd < 0)
		return -errno;
	map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd,
		   (off_t)aligned);
	if (map == MAP_FAILED) {
		int err = -errno;

		close(m->fd);
		m->fd = -1;
		return err;
	}
	m->map = map;
	m->map_len = map_len;
	m->dev_base = dev_base;
	m->phys_base = phys_base;
	m->page_slop = slop;

	ops->write = devmem_write;
	ops->read = devmem_read;
	ops->priv = m;
	return 0;
}

void mem_devmem_close(struct mem_devmem *m)
{
	if (!m)
		return;
	if (m->map && m->map_len)
		munmap((void *)m->map, m->map_len);
	if (m->fd >= 0)
		close(m->fd);
	m->map = NULL;
	m->map_len = 0;
	m->fd = -1;
}
