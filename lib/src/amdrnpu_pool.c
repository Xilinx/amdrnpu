/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#include "amdrnpu_pool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define POOL_WORD_BITS 64u

static int pool_test(const struct amdrnpu_pool *p, size_t bit)
{
	return (p->bitmap[bit / POOL_WORD_BITS] >>
		(bit % POOL_WORD_BITS)) & 1u;
}

static void pool_set(struct amdrnpu_pool *p, size_t bit)
{
	p->bitmap[bit / POOL_WORD_BITS] |=
		(uint64_t)1 << (bit % POOL_WORD_BITS);
}

static void pool_clear(struct amdrnpu_pool *p, size_t bit)
{
	p->bitmap[bit / POOL_WORD_BITS] &=
		~((uint64_t)1 << (bit % POOL_WORD_BITS));
}

size_t amdrnpu_pool_pages_for(uint64_t bytes)
{
	if (bytes == 0)
		return 1;
	return (size_t)((bytes + AMDRNPU_POOL_PAGE_SIZE - 1) >>
			AMDRNPU_POOL_PAGE_SHIFT);
}

int amdrnpu_pool_init(struct amdrnpu_pool *p, uint64_t base, uint64_t size)
{
	size_t npages, words;

	if (!p || (base & (AMDRNPU_POOL_PAGE_SIZE - 1)))
		return -EINVAL;

	npages = (size_t)(size >> AMDRNPU_POOL_PAGE_SHIFT);
	if (npages == 0)
		return -EINVAL;

	words = (npages + POOL_WORD_BITS - 1) / POOL_WORD_BITS;
	p->bitmap = calloc(words, sizeof(*p->bitmap));
	if (!p->bitmap)
		return -ENOMEM;

	p->base = base;
	p->size = (uint64_t)npages << AMDRNPU_POOL_PAGE_SHIFT;
	p->npages = npages;
	p->words = words;
	return 0;
}

void amdrnpu_pool_fini(struct amdrnpu_pool *p)
{
	if (!p)
		return;
	free(p->bitmap);
	memset(p, 0, sizeof(*p));
}

long amdrnpu_pool_alloc(struct amdrnpu_pool *p, size_t npages)
{
	size_t start, run;

	if (!p || !p->bitmap || npages == 0)
		return -EINVAL;
	if (npages > p->npages)
		return -ENOMEM;

	/* First-fit: scan for @npages consecutive clear bits. */
	run = 0;
	for (start = 0; start < p->npages; start++) {
		if (pool_test(p, start)) {
			run = 0;
			continue;
		}
		if (++run == npages) {
			size_t first = start + 1 - npages;
			size_t i;

			for (i = first; i < first + npages; i++)
				pool_set(p, i);
			return (long)first;
		}
	}
	return -ENOMEM;
}

void amdrnpu_pool_free(struct amdrnpu_pool *p, size_t index, size_t npages)
{
	size_t i;

	if (!p || !p->bitmap || npages == 0)
		return;
	if (index >= p->npages || npages > p->npages - index)
		return;

	for (i = index; i < index + npages; i++)
		pool_clear(p, i);
}
