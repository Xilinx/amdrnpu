/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_ini - minimal INI reader for the userspace amdrnpu backend.
 *
 * The library no longer talks to the amdrnpu kernel module; instead it draws
 * device/RPU memory from fixed physical address pools and submits commands over
 * the userspace rpmsg_lite leader.  All of that layout is declared in an INI
 * file (default /etc/amdrnpu.ini, overridable with the AMDRNPU_INI env var):
 *
 *   [pool.device]   base, size        device-memory pool (no-map DT region)
 *   [pool.rpu]      base, size        RPU/firmware-memory pool (no-map)
 *   [rpmsg]         shm_phys, shm_size, status_offset, vring_rx_offset,
 *                   vring_tx_offset, vring_num, vring_align, buf_offset,
 *                   buf_size, buf_count   (feeds struct rpmsg_lite_cfg)
 *
 * Values parse as hex (0x...) or decimal.
 */
#ifndef AMDRNPU_INI_H_
#define AMDRNPU_INI_H_

#include <stdint.h>

struct amdrnpu_pool_cfg {
	uint64_t base;
	uint64_t size;
};

struct amdrnpu_rpmsg_cfg {
	uint64_t shm_phys;
	uint64_t shm_size;
	uint64_t status_offset;
	uint64_t vring_rx_offset;
	uint64_t vring_tx_offset;
	uint32_t vring_num;
	uint32_t vring_align;
	uint64_t buf_offset;
	uint32_t buf_size;
	uint32_t buf_count;
};

struct amdrnpu_config {
	struct amdrnpu_pool_cfg pool_device;
	struct amdrnpu_pool_cfg pool_rpu;
	struct amdrnpu_rpmsg_cfg rpmsg;
};

/*
 * Load configuration from @path, or when @path is NULL from $AMDRNPU_INI, or
 * failing that /etc/amdrnpu.ini.  Unknown sections/keys are ignored; missing
 * required keys yield -EINVAL.  Returns 0 on success or a negative errno.
 */
int amdrnpu_config_load(struct amdrnpu_config *cfg, const char *path);

#endif /* AMDRNPU_INI_H_ */
