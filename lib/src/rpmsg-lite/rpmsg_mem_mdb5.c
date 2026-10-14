/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "rpmsg_mem_mdb5.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

/* The MDB5 channel programs the DMA endpoint address from the file offset, so
 * each transfer is a positioned pwrite/pread. Loop over short transfers and
 * retry on EINTR; a zero-length return before completion is treated as an error.
 */
static int mdb5_write(void *priv, uint64_t dev_addr, const void *src, size_t len)
{
	struct rpmsg_mem_mdb5 *m = priv;
	const uint8_t *p = src;
	off_t off = (off_t)dev_addr;

	while (len) {
		ssize_t n = pwrite(m->wr_fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += (size_t)n;
		off += n;
		len -= (size_t)n;
	}
	return 0;
}

static int mdb5_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct rpmsg_mem_mdb5 *m = priv;
	uint8_t *p = dst;
	off_t off = (off_t)dev_addr;

	while (len) {
		ssize_t n = pread(m->rd_fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		p += (size_t)n;
		off += n;
		len -= (size_t)n;
	}
	return 0;
}

void rpmsg_mem_mdb5_init(struct rpmsg_mem_mdb5 *m, struct rpmsg_mem_ops *ops,
			 int wr_fd, int rd_fd)
{
	m->wr_fd = wr_fd;
	m->rd_fd = rd_fd;
	ops->write = mdb5_write;
	ops->read = mdb5_read;
	ops->priv = m;
}

int rpmsg_mem_mdb5_open(struct rpmsg_mem_mdb5 *m, struct rpmsg_mem_ops *ops,
			const char *write_path, const char *read_path)
{
	int wr_fd, rd_fd;

	if (!m || !ops || !write_path || !read_path)
		return -EINVAL;
	wr_fd = open(write_path, O_WRONLY | O_CLOEXEC);
	if (wr_fd < 0)
		return -errno;
	rd_fd = open(read_path, O_RDONLY | O_CLOEXEC);
	if (rd_fd < 0) {
		int err = -errno;

		close(wr_fd);
		return err;
	}
	rpmsg_mem_mdb5_init(m, ops, wr_fd, rd_fd);
	return 0;
}

void rpmsg_mem_mdb5_close(struct rpmsg_mem_mdb5 *m)
{
	if (!m)
		return;
	if (m->wr_fd >= 0)
		close(m->wr_fd);
	if (m->rd_fd >= 0)
		close(m->rd_fd);
	m->wr_fd = -1;
	m->rd_fd = -1;
}
