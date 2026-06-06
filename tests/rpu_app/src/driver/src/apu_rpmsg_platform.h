/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Public accessors for the link A (APU<->RPURuntime) OpenAMP slave-side
 * transport. tessai_agent_endpoint.c calls apu_openamp_get_rpmsg_device()
 * to attach its "tessai-agent" rpmsg endpoint - same pattern as the
 * upstream rpu-fw openamp_platform_get_rpmsg_device().
 */

#ifndef TESSAI_APU_RPMSG_PLATFORM_H_
#define TESSAI_APU_RPMSG_PLATFORM_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rpmsg_device;

struct rpmsg_device *apu_openamp_get_rpmsg_device(void);
bool                 apu_openamp_platform_is_initialized(void);
const char          *apu_rpmsg_tessai_ept_name(void);

#ifdef __cplusplus
}
#endif

#endif /* TESSAI_APU_RPMSG_PLATFORM_H_ */
