/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * libmetal init guarded by a Zephyr mutex (libmetal's metal_init()
 * isn't idempotent). See metal_init_once.h for usage.
 */

#include <zephyr/kernel.h>
#include <metal/sys.h>

#include "metal_init_once.h"

static K_MUTEX_DEFINE(metal_init_lock);
static bool metal_init_done;
static int  metal_init_status;

int tessai_metal_init_once(void)
{
	int ret;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;

	k_mutex_lock(&metal_init_lock, K_FOREVER);

	if (metal_init_done) {
		ret = metal_init_status;
		goto out;
	}

	ret = metal_init(&metal_params);
	metal_init_status = ret;
	metal_init_done   = true;

out:
	k_mutex_unlock(&metal_init_lock);
	return ret;
}
