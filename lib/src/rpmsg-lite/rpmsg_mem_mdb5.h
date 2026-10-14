/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * rpmsg_mem_mdb5 - rpmsg_mem backend over the MDB5 DMA char-device driver
 * (dma_ip_drivers/MDB5/client-driver).
 *
 * The MDB5 client exposes per-direction channel nodes whose file offset is the
 * device/endpoint address: pwrite(fd, src, len, dev_addr) DMAs host->device and
 * pread(fd, dst, len, dev_addr) DMAs device->host. A 4-byte (or any size >= 1)
 * transfer is accepted by the client; the only hard limit is 0 < len <= 4 GiB,
 * and the real alignment/burst-width constraints live in the underlying MDB5
 * dmaengine provider. The write channel must be opened on a MEM_TO_DEV channel
 * and the read channel on a DEV_TO_MEM channel.
 */
#ifndef RPMSG_MEM_MDB5_H
#define RPMSG_MEM_MDB5_H

#include "rpmsg_mem.h"

struct rpmsg_mem_mdb5 {
	int wr_fd; /* MEM_TO_DEV channel (host -> device). */
	int rd_fd; /* DEV_TO_MEM channel (device -> host). */
};

/* Opens write_path and read_path as the DMA channels and fills m/ops. Returns 0
 * or a negative errno; on failure no descriptor is left open. m must outlive ops.
 */
int rpmsg_mem_mdb5_open(struct rpmsg_mem_mdb5 *m, struct rpmsg_mem_ops *ops,
			const char *write_path, const char *read_path);

/* Wraps already-open channel descriptors (caller retains ownership). */
void rpmsg_mem_mdb5_init(struct rpmsg_mem_mdb5 *m, struct rpmsg_mem_ops *ops,
			 int wr_fd, int rd_fd);

/* Closes descriptors opened by rpmsg_mem_mdb5_open. */
void rpmsg_mem_mdb5_close(struct rpmsg_mem_mdb5 *m);

#endif /* RPMSG_MEM_MDB5_H */
