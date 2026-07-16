/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * One-shot libmetal initialiser. libmetal's metal_init() is not
 * idempotent; this wrapper makes it safe to call from multiple
 * SYS_INITs.
 */

#ifndef AMDRNPU_METAL_INIT_ONCE_H_
#define AMDRNPU_METAL_INIT_ONCE_H_

#ifdef __cplusplus
extern "C" {
#endif

int amdrnpu_metal_init_once(void);

#ifdef __cplusplus
}
#endif

#endif /* AMDRNPU_METAL_INIT_ONCE_H_ */
