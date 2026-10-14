/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * Hardware-free unit tests for the bitmap pool allocator (amdrnpu_pool) and the
 * INI reader (amdrnpu_ini).  No device access required.
 */
#include "amdrnpu_ini.h"
#include "amdrnpu_pool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                        \
			failures++;                                            \
		}                                                              \
	} while (0)

#define PAGE AMDRNPU_POOL_PAGE_SIZE

static void test_pages_for(void)
{
	CHECK(amdrnpu_pool_pages_for(0) == 1);
	CHECK(amdrnpu_pool_pages_for(1) == 1);
	CHECK(amdrnpu_pool_pages_for(PAGE) == 1);
	CHECK(amdrnpu_pool_pages_for(PAGE + 1) == 2);
	CHECK(amdrnpu_pool_pages_for(4 * PAGE) == 4);
}

static void test_alloc_free(void)
{
	struct amdrnpu_pool p;
	long a, b, c;

	/* 8-page pool at a page-aligned base. */
	CHECK(amdrnpu_pool_init(&p, 0x80000000ull, 8 * PAGE) == 0);
	CHECK(p.npages == 8);

	a = amdrnpu_pool_alloc(&p, 1);
	CHECK(a == 0);
	CHECK(amdrnpu_pool_addr(&p, (size_t)a) == 0x80000000ull);

	b = amdrnpu_pool_alloc(&p, 3);
	CHECK(b == 1); /* first-fit, contiguous after page 0 */

	c = amdrnpu_pool_alloc(&p, 4);
	CHECK(c == 4);

	/* Pool is full now. */
	CHECK(amdrnpu_pool_alloc(&p, 1) == -ENOMEM);

	/* Free the 3-page run and reuse exactly. */
	amdrnpu_pool_free(&p, (size_t)b, 3);
	CHECK(amdrnpu_pool_alloc(&p, 3) == 1);

	/* A run larger than a freed hole fails even if total free >= request. */
	amdrnpu_pool_free(&p, 1, 1); /* free page 1 only */
	amdrnpu_pool_free(&p, 3, 1); /* and page 3: two 1-page holes */
	CHECK(amdrnpu_pool_alloc(&p, 2) == -ENOMEM);
	CHECK(amdrnpu_pool_alloc(&p, 1) == 1); /* first hole */

	amdrnpu_pool_fini(&p);
	CHECK(p.bitmap == NULL);
}

static void test_init_errors(void)
{
	struct amdrnpu_pool p;

	/* Unaligned base rejected. */
	CHECK(amdrnpu_pool_init(&p, 0x1001ull, 4 * PAGE) == -EINVAL);
	/* Sub-page size rejected. */
	CHECK(amdrnpu_pool_init(&p, 0x1000ull, 100) == -EINVAL);
	/* Size rounds down to whole pages. */
	CHECK(amdrnpu_pool_init(&p, 0x1000ull, 2 * PAGE + 7) == 0);
	CHECK(p.npages == 2);
	amdrnpu_pool_fini(&p);
}

static void test_ini(void)
{
	char path[] = "/tmp/amdrnpu_ini_XXXXXX";
	struct amdrnpu_config cfg;
	int fd;
	FILE *f;

	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	CHECK(f != NULL);
	fputs("; comment\n"
	      "[pool.device]\n"
	      "base = 0x50000000000\n"
	      "size = 0x100000000   ; inline comment\n"
	      "[pool.rpu]\n"
	      "base = 0x20000000\n"
	      "size = 536870912\n"
	      "[rpmsg]\n"
	      "shm_phys = 0x09800000\n"
	      "shm_size = 0xA8000\n"
	      "status_offset = 0\n"
	      "vring_rx_offset = 0x60000\n"
	      "vring_tx_offset = 0x64000\n"
	      "vring_num = 256\n"
	      "vring_align = 4096\n"
	      "buf_offset = 0x68000\n"
	      "buf_size = 512\n"
	      "buf_count = 512\n",
	      f);
	fclose(f);

	CHECK(amdrnpu_config_load(&cfg, path) == 0);
	CHECK(cfg.pool_device.base == 0x50000000000ull);
	CHECK(cfg.pool_device.size == 0x100000000ull);
	CHECK(cfg.pool_rpu.base == 0x20000000ull);
	CHECK(cfg.pool_rpu.size == 536870912ull);
	CHECK(cfg.rpmsg.shm_phys == 0x09800000ull);
	CHECK(cfg.rpmsg.vring_rx_offset == 0x60000ull);
	CHECK(cfg.rpmsg.vring_tx_offset == 0x64000ull);
	CHECK(cfg.rpmsg.vring_num == 256);
	CHECK(cfg.rpmsg.buf_count == 512);

	unlink(path);

	/* Missing path yields an errno, not a crash. */
	CHECK(amdrnpu_config_load(&cfg, "/nonexistent/amdrnpu.ini") < 0);
}

static void test_ini_incomplete(void)
{
	char path[] = "/tmp/amdrnpu_ini_XXXXXX";
	struct amdrnpu_config cfg;
	int fd;
	FILE *f;

	fd = mkstemp(path);
	CHECK(fd >= 0);
	if (fd < 0)
		return;
	f = fdopen(fd, "w");
	fputs("[pool.device]\nbase = 0x1000\nsize = 0x1000\n", f);
	fclose(f);

	/* Required keys missing -> -EINVAL. */
	CHECK(amdrnpu_config_load(&cfg, path) == -EINVAL);
	unlink(path);
}

int main(void)
{
	test_pages_for();
	test_alloc_free();
	test_init_errors();
	test_ini();
	test_ini_incomplete();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all pool/ini checks passed\n");
	return 0;
}
