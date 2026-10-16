/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
#ifndef ECHO_DEVICE_H
#define ECHO_DEVICE_H

#include "echo_config.h"
#include "rpmsg_mem.h"

/* Runs the A78 virtio *device* side of the echo: consumes "ping" from the
 * driver TX vring and produces "pong" on the driver RX vring, all in OCM via
 * mem. Waits for the driver's DRIVER_OK status first. Processes max_echoes
 * messages (0 = run until error). Returns 0 on clean stop or negative errno.
 */
int device_echo_run(const struct rpmsg_mem_ops *mem,
		    const struct echo_config *cfg, unsigned long max_echoes);

#endif /* ECHO_DEVICE_H */
