/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * APU → RPU_APP command opcodes that bridge amdrnpu wire commands
 * into the XRT Lite stack (RPU_APP → RPU_FW via the existing VE2 mgmt
 * transport). Keep lockstep with
 * RPURuntime/tests/xrt_apu_rpu_test/src/apu_xrt_ops.h.
 *
 * Wire layout for every opcode:
 *   struct amdrnpu_wire_cmd { seq, command_op, flags, num_args, args[] }
 *   struct amdrnpu_wire_rsp { seq, command_rsp_type }
 */

#ifndef APU_OPS_H_
#define APU_OPS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fw_echo — same opcode as amdrnpu unittests (AMDRNPU_FW_ECHO_OP). */
#define APU_XRT_OP_ECHO              0x42424242u

/* Local carveout memcpy (AMDRNPU_OP_COPY_IN_OUT). */
#define APU_XRT_OP_COPY_IN_OUT       0x00000001u

/*
 * APU_XRT_OP_COPY_IN_OUT args (uint64_t each, need num_args >= 6):
 *   args[0] = INPUT_BO_TAG
 *   args[1] = in_size
 *   args[2] = in_phys
 *   args[3] = OUTPUT_BO_TAG
 *   args[4] = out_size
 *   args[5] = out_phys
 * Copies min(in_size, out_size) bytes inside rpu_gem_cma.
 */

/*
 * APU_XRT_TEST — full XRT end-to-end run via descriptor buffer.
 *
 * Wire args[] layout (uint64_t each, num_args == APU_XRT_TEST_NUM_ARGS):
 *   args[0]  = iteration count (top-level)
 *   args[1]  = ELF physical address  (resolved from BO handle by driver)
 *   args[2]  = ELF size in bytes
 *   args[3]  = descriptor buffer PA  (resolved from BO handle by driver)
 *   args[4]  = descriptor buffer content size in bytes
 *   args[5]  = result writeback PA   (resolved from BO handle; 0 = skip)
 *
 * Intra-descriptor pointers (kernel_name_off, result_bo_off) are byte
 * offsets from the start of the descriptor buffer.  Kernel-arg PAs inside
 * apu_xrt_karg_entry are absolute physical addresses.
 */
#define APU_XRT_TEST                 0x58525454u  /* 'XRTT' */

#define APU_XRT_TEST_NUM_ARGS        6u
#define APU_XRT_TEST_MAX_KARGS       24u
/* Wire args[5] is the top-level result PA (descriptor protocol). */
#define APU_XRT_TEST_RESULT_ARG_IDX  5u

struct __attribute__((__packed__)) apu_xrt_karg_entry {
	uint64_t pa;    /* physical address (BO) or scalar value (scalar) */
	uint64_t size;  /* byte size for BO mapping; 0 = scalar arg */
};

#define APU_XRT_KARG_ENTRY_SIZE  sizeof(struct apu_xrt_karg_entry)

struct __attribute__((__packed__)) apu_xrt_run_entry {
	uint32_t iterations;
	uint32_t n_kargs;
	uint32_t kernel_name_off;    /* 0 = auto-discover from ELF */
	uint32_t kernel_name_len;
	uint32_t result_bo_off;      /* 0 = skip per-entry result */
	uint32_t reserved;
	struct apu_xrt_karg_entry kargs[];
};

#define APU_XRT_RUN_ENTRY_HDR_SIZE  (sizeof(uint32_t) * 6u)

#define APU_XRT_RUN_ENTRY_SIZE(n_kargs) \
	(APU_XRT_RUN_ENTRY_HDR_SIZE + (uint32_t)(n_kargs) * APU_XRT_KARG_ENTRY_SIZE)

struct __attribute__((__packed__)) apu_xrt_cmd_desc {
	uint32_t n_entries;
	uint32_t reserved;
};

#define APU_XRT_CMD_DESC_HDR_SIZE  sizeof(struct apu_xrt_cmd_desc)

struct __attribute__((__packed__)) apu_xrt_test_result {
	uint32_t passed;
	uint32_t iterations_done;
	uint32_t latency_avg_us;
	uint32_t latency_first_us;
};

#define APU_XRT_TEST_RESULT_SIZE  sizeof(struct apu_xrt_test_result)

#ifdef __cplusplus
}
#endif

#endif /* APU_OPS_H_ */
