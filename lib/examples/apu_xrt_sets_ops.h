/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * APU_XRT_SETS_TEST wire protocol — host side. Keep lockstep with
 * RPURuntime/tests/xrt_apu_rpu_sets_test/src/apu_xrt_sets_ops.h.
 *
 * One AIE control ELF, one kernel, and an array of kernel-arg sets. Each set
 * is the full kernel-arg list for ONE iteration; iteration i runs with set
 * (i % num_sets). Layout mirrors vart_sentry_test's APU_VART_TEST: a fixed
 * header, then flat (PA, size) scalar pairs, then the result PA last. No
 * descriptor buffer.
 *
 * Fixed header args (indices 0..5):
 *   args[0]  = iteration count (0 = num_sets, i.e. each set runs once)
 *   args[1]  = ELF physical address (RPU-reachable carveout)
 *   args[2]  = ELF size in bytes
 *   args[3]  = num_sets   (number of kernel-arg sets)
 *   args[4]  = num_kargs  (kernel args per set; same kernel => same count)
 *   args[5]  = flags (APU_XRT_SETS_FLAG_*; 0 = none)
 *
 * Kernel-arg pairs (two scalars per karg, set-major):
 *   args[6 + 2*(s*num_kargs + k)]     = karg k PA for set s
 *   args[6 + 2*(s*num_kargs + k) + 1] = karg k size in bytes
 *                                       (size 0 = scalar: the PA slot holds
 *                                        the 32-bit scalar value)
 *
 * Result writeback arg (last):
 *   args[6 + 2*num_sets*num_kargs] = result PA (0 = no writeback)
 *     RPU writes apu_xrt_sets_test_result here before replying.
 *
 * Total args = 6 + 2*num_sets*num_kargs + 1.
 *
 * Size limit: the wire frame is one rpmsg buffer (512 B = 16 B rpmsg header +
 * 16 B amdrnpu_wire_cmd header + 8 B per arg), so at most 60 args, i.e.
 * num_sets * num_kargs <= 26 (e.g. 6 sets of 4 kargs, 5 sets of 5 kargs).
 */

#ifndef APU_XRT_SETS_OPS_H_
#define APU_XRT_SETS_OPS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_XRT_SETS_TEST              0x58525453u  /* 'XRTS' */

/* args[5] flags. RPU_APP keeps running per-phase profile totals (count, sum,
 * min, max) across commands:
 *   PROF_RESET  - clear the totals before this command (host: first round)
 *   PROF_REPORT - after this command's reply has been sent, print the
 *                 avg/min/max summary on the RPU_APP console (host: last
 *                 round), so the printing is not part of any measurement. */
#define APU_XRT_SETS_FLAG_PROF_RESET   0x1u
#define APU_XRT_SETS_FLAG_PROF_REPORT  0x2u
#define APU_XRT_SETS_TEST_FLAGS_ARG_IDX 5u

#define APU_XRT_SETS_TEST_HEADER_ARGS  6u
#define APU_XRT_SETS_TEST_MAX_KARGS    24u

#define APU_XRT_SETS_TEST_NUM_ARGS(num_sets, num_kargs) \
	(APU_XRT_SETS_TEST_HEADER_ARGS + 2u * (num_sets) * (num_kargs) + 1u /*result*/)

#define APU_XRT_SETS_TEST_KARG_ARG_IDX(set, karg, num_kargs) \
	(APU_XRT_SETS_TEST_HEADER_ARGS + 2u * ((set) * (num_kargs) + (karg)))

#define APU_XRT_SETS_TEST_RESULT_ARG_IDX(num_sets, num_kargs) \
	(APU_XRT_SETS_TEST_HEADER_ARGS + 2u * (num_sets) * (num_kargs))

/*
 * The first four words match APU_XRT_TEST's apu_xrt_test_result. They are
 * followed by an RPU_APP-side profile of this command, in nanoseconds from
 * the 100 MHz ARM architected timer (10 ns resolution), valid when
 * prof_magic == APU_XRT_SETS_PROF_MAGIC.
 *
 * The RPU picks commands up by polling the vring every 1 ms (no inbound
 * IPI), so up to ~1 ms between the host's write and rx_to_start_ns's start
 * point is not visible here. The reply (rpmsg_send + IPI kick) happens after
 * the result is written, so each result reports the PREVIOUS command's send
 * time in prev_send_ns (0 for the first command after boot).
 */
#define APU_XRT_SETS_PROF_MAGIC  0x50524F46u  /* 'PROF' */

struct __attribute__((__packed__)) apu_xrt_sets_test_result {
	uint32_t passed;           /* 1 = all iterations completed, 0 = fail */
	uint32_t iterations_done;  /* number of iterations completed */
	uint32_t latency_avg_us;   /* average start->wait round-trip */
	uint32_t latency_first_us; /* first iteration latency */

	uint32_t prof_magic;       /* APU_XRT_SETS_PROF_MAGIC when valid */
	uint32_t rx_to_start_ns;   /* RPMsg RX callback -> handler start */
	uint32_t elf_parse_ns;     /* xrt::elf parse + kernel lookup */
	uint32_t hwctx_ns;         /* hw_context + kernel create (RPU_FW) */
	uint32_t bo_map_ns;        /* wrap karg buffers + create the run */
	uint32_t bind_ns;          /* set_arg for all sets (sum) */
	uint32_t exec_ns;          /* run start->wait, sum over iterations */
	uint32_t exec_max_ns;      /* longest single run in this command */
	uint32_t teardown_ns;      /* run/bo/kernel/hw_context destroy */
	uint32_t handler_ns;       /* handler start -> before result fill */
	uint32_t result_fill_ns;   /* writing this struct (all but this word) */
	uint32_t prev_send_ns;     /* previous command's reply: rpmsg_send + IPI */
};

#define APU_XRT_SETS_TEST_RESULT_SIZE  sizeof(struct apu_xrt_sets_test_result)

#ifdef __cplusplus
}
#endif

#endif /* APU_XRT_SETS_OPS_H_ */
