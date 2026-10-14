/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_pool - bitmap allocator over a fixed physical address pool.
 *
 * The pool is a contiguous, no-map (kernel-unmanaged) physical range declared
 * in amdrnpu.ini.  It is carved into 4 KiB units tracked by a bitmap; every
 * allocation is a run of whole pages, so the smallest allocation is 4 KiB.
 * Allocation is first-fit over contiguous free pages.  No locking here; the
 * caller serializes access.
 */
#ifndef AMDRNPU_POOL_H_
#define AMDRNPU_POOL_H_

#include <stddef.h>
#include <stdint.h>

#define AMDRNPU_POOL_PAGE_SHIFT 12
#define AMDRNPU_POOL_PAGE_SIZE (1u << AMDRNPU_POOL_PAGE_SHIFT) /* 4096 */

struct amdrnpu_pool {
	uint64_t base;	/* physical base address; must be page-aligned */
	uint64_t size;	/* usable bytes (whole pages) */
	size_t npages;	/* size >> AMDRNPU_POOL_PAGE_SHIFT */
	uint64_t *bitmap; /* 1 bit per page, 1 = allocated */
	size_t words;	/* number of 64-bit words in @bitmap */
};

/*
 * Initialize @p over [@base, @base + @size).  @base must be 4 KiB aligned;
 * @size is rounded down to a whole number of pages and must be nonzero.
 * Allocates the bitmap.  Returns 0 or a negative errno.
 */
int amdrnpu_pool_init(struct amdrnpu_pool *p, uint64_t base, uint64_t size);

/* Release the bitmap; @p is zeroed and must not be used until re-init. */
void amdrnpu_pool_fini(struct amdrnpu_pool *p);

/* Round a byte count up to whole 4 KiB pages (minimum 1). */
size_t amdrnpu_pool_pages_for(uint64_t bytes);

/*
 * Allocate @npages contiguous pages.  Returns the starting page index (>= 0)
 * or a negative errno (-EINVAL, -ENOMEM when no run is large enough).
 */
long amdrnpu_pool_alloc(struct amdrnpu_pool *p, size_t npages);

/* Free @npages previously allocated starting at @index. */
void amdrnpu_pool_free(struct amdrnpu_pool *p, size_t index, size_t npages);

/* Physical address of page @index. */
static inline uint64_t amdrnpu_pool_addr(const struct amdrnpu_pool *p,
					 size_t index)
{
	return p->base + ((uint64_t)index << AMDRNPU_POOL_PAGE_SHIFT);
}

#endif /* AMDRNPU_POOL_H_ */
