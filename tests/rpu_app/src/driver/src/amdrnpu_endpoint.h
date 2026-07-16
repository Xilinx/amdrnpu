/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * amdrnpu RPMsg endpoint - public init API for RPURuntime, plus
 * the endpoint contract (opcodes, per-handler size caps, RPU-bank
 * carveout bounds) that the wire definitions in amdrnpu_msg_def.h
 * are paired with on the slave side.
 *
 * Ported from rpu-fw/rpu_fw/app/include/transport/amdrnpu_endpoint.h.
 * Layer between RPURuntime's link A slave-side OpenAMP transport
 * (apu_rpmsg_platform.c) and the on-wire amdrnpu_wire_cmd / amdrnpu_wire_rsp
 * contract documented in tessai-agent/docs/rpu-firmware-contract.md.
 */

#ifndef AMDRNPU_ENDPOINT_H_
#define AMDRNPU_ENDPOINT_H_

#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>     /* BUILD_ASSERT */

#ifdef __cplusplus
extern "C" {
#endif

/* RPMsg endpoint name advertised by the slave. */
#define AMDRNPU_EPT_NAME           "amdrnpu"

/* Wire flag: client requests cancellation. */
#define AMDRNPU_WIRE_FLAG_ABORT          0x80000001u

/*
 * Opcodes. ECHO/ECHO_REGION/DAPP/COPY_IN_OUT are the four functional
 * paths; REMI/NOTS/CANC/BADC drive the four documented amdrnpu_rsp_type
 * fault paths plus the invalid-rsp_type fallback.
 */
#define AMDRNPU_FW_ECHO_OP               0x42424242u
#define AMDRNPU_FW_ECHO_REGION_OP        0x52474E45u  /* 'RGNE' */
#define AMDRNPU_FW_OP_DEVICE_ADDR_PING   0x44415050u  /* 'DAPP' */
#define AMDRNPU_OP_COPY_IN_OUT           0x00000001u
#define AMDRNPU_FW_OP_FAULT_REMOTEIO     0x52454D49u  /* 'REMI' */
#define AMDRNPU_FW_OP_FAULT_NOTSUPP      0x4E4F5453u  /* 'NOTS' */
#define AMDRNPU_FW_OP_FAULT_CANCELLED    0x43414E43u  /* 'CANC' */
#define AMDRNPU_FW_OP_FAULT_BADRSP       0x42414443u  /* 'BADC' */

/* Per-handler banner / response size caps. */
#define AMDRNPU_FW_RSP_TEXT_MAX          400u
#define AMDRNPU_FW_ECHO_REGION_MAX       400u
#define AMDRNPU_FW_DAPP_BANNER_MAX       128u

/* Out-of-band rsp_type used to drive the BADC fault path. */
#define AMDRNPU_RSP_TYPE_INVALID         99u

/*
 * RPU-bank carveout bounds, pulled from the tess_rpu_gem_carveout DT
 * node so wire-side bounds checks stay in lockstep with the MPU
 * window. MUST stay aligned with the Linux rpu_gem_cma@20000000 pool
 * (apply_amdrnpu_patch.py RPU_GEM_CMA_BLOCK) and rpu-fw's per-board
 * overlay (tess_rpu_gem_carveout).
 */
#define AMDRNPU_RPU_NODE                 DT_NODELABEL(tess_rpu_gem_carveout)
BUILD_ASSERT(DT_NODE_EXISTS(AMDRNPU_RPU_NODE),
	     "tess_rpu_gem_carveout DT node missing");
#define AMDRNPU_RPU_BASE                 ((uint64_t)DT_REG_ADDR(AMDRNPU_RPU_NODE))
#define AMDRNPU_RPU_SIZE                 ((uint64_t)DT_REG_SIZE(AMDRNPU_RPU_NODE))

/**
 * @brief Initialise the "amdrnpu" RPMsg endpoint.
 *
 * Must be called AFTER apu_rpmsg_platform's SYS_INIT has succeeded (the
 * rdev is fetched via apu_openamp_get_rpmsg_device() internally). When
 * compiled with CONFIG_AMDRNPU_APU_RPMSG_SLAVE this is invoked from a later
 * SYS_INIT priority automatically; call sites that want to retry after
 * a deferred slave bring-up can also call it directly.
 *
 * Idempotent: a second call after a successful one returns -EALREADY.
 *
 * @return 0 on success, negative errno on failure.
 */
int amdrnpu_endpoint_init(void);

#ifdef __cplusplus
}
#endif

#endif /* AMDRNPU_ENDPOINT_H_ */
