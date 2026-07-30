/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * APU → RPU_APP command opcodes that bridge amdrnpu wire commands
 * into the XRT Lite stack (RPU_APP → RPU_FW via the existing VE2 mgmt
 * transport). Keep lockstep with the APU gtest harness when one exists.
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
 * APU_XRT_TEST — full XRT end-to-end run (mirrors xrt_test/src/main.cpp).
 *
 * args[] layout (uint64_t each):
 *   args[0]  = iteration count
 *   args[1]  = ELF physical address  (in rpu_gem_cma)
 *   args[2]  = ELF size in bytes
 *   args[3]  = kernel arg[0] PA  (e.g. IFM; 0 = omit set_arg)
 *   args[4]  = kernel arg[1] PA  (e.g. OFM; 0 = omit set_arg)
 *   args[5]  = kernel arg[2] PA  (e.g. WTS; 0 = omit set_arg)
 *   ...      (up to 24 kernel args total, args[3..26])
 *   args[27] = result writeback PA  (0 = no writeback)
 *
 * When args[27] is non-zero, RPU writes apu_xrt_test_result there.
 */
#define APU_XRT_TEST                 0x58525454u  /* 'XRTT' */

#define APU_XRT_TEST_MAX_KARGS       24u
#define APU_XRT_TEST_RESULT_ARG_IDX  (3u + APU_XRT_TEST_MAX_KARGS)  /* 27 */

struct __attribute__((__packed__)) apu_xrt_test_result {
	uint32_t passed;           /* 1 = all iterations completed, 0 = fail */
	uint32_t iterations_done;  /* number of iterations completed */
	uint32_t latency_avg_us;   /* average start→wait round-trip */
	uint32_t latency_first_us; /* first iteration latency */
};

#define APU_XRT_TEST_RESULT_SIZE  sizeof(struct apu_xrt_test_result)

#ifdef __cplusplus
}
#endif

#endif /* APU_OPS_H_ */
