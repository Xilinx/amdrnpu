/*
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * xrt_batches wire protocol — host side. Keep lockstep with
 * RPURuntime/tests/xrt_batches/src/apu_xrt_batch_ops.h.
 *
 * A context is created once from an AIE control ELF (parsed once, hw_context
 * + kernel + run created once, "fixed" kernel args such as the weights
 * registered once), then any number of batches run against it, then it is
 * destroyed:
 *
 *   APU_XRT_BATCH_CREATE   ELF + fixed kargs        -> ctx_id (in the result)
 *   APU_XRT_BATCH_RUN      ctx_id + N entries       -> runs entry 0..N-1
 *   APU_XRT_BATCH_DESTROY  ctx_id                   -> context released
 *
 * Only one context exists at a time; CREATE while one exists fails.
 *
 * The rpmsg reply only carries {seq, status}, so every command also takes a
 * result PA (RPU-reachable DDR, 0 = none) where RPU_APP writes
 * struct apu_xrt_batch_result: the status, the context id (CREATE) and
 * timings. The host pre-fills it with 0xFF; status is written last, so a
 * non-0xFFFFFFFF status word means the result is complete.
 *
 * All wire args are uint64_t.
 *
 * APU_XRT_BATCH_CREATE:
 *   args[0] = ELF PA (must stay valid until DESTROY)
 *   args[1] = ELF size in bytes
 *   args[2] = result PA (0 = none)
 *   args[3] = num_fixed: kernel args that stay the same for the context's
 *             lifetime (wrapped as xrt::bo once; set_arg still per entry)
 *   args[4 + 3*i + 0] = fixed karg i: kernel-arg index
 *   args[4 + 3*i + 1] =               PA   (size 0 = scalar: value here)
 *   args[4 + 3*i + 2] =               size in bytes
 *   num_args = 4 + 3*num_fixed
 *
 * APU_XRT_BATCH_RUN:
 *   args[0] = ctx_id (from CREATE)
 *   args[1] = result PA (0 = none)
 *   args[2] = num_entries  (N)
 *   args[3] = kargs_per_entry (K)
 *   args[4 + k]              = kernel-arg index of per-entry karg k, k < K
 *   args[4 + K + 2*(e*K+k)]     = entry e, karg k: PA (size 0 = scalar)
 *   args[4 + K + 2*(e*K+k) + 1] = entry e, karg k: size in bytes
 *   num_args = 4 + K + 2*N*K
 *   Entries run in order; each sets every kernel arg (the fixed ones from
 *   CREATE and its own), then start + wait. Stops at the first failure.
 *
 * APU_XRT_BATCH_DESTROY:
 *   args[0] = ctx_id
 *   args[1] = result PA (0 = none)
 *   num_args = 2
 *
 * Size limit: a request is one rpmsg buffer (512 B = 16 B rpmsg header +
 * 16 B amdrnpu_wire_cmd header + 8 B per arg), so at most 60 args: e.g.
 * K = 3 per-entry kargs -> N <= 8 entries, K = 4 -> N <= 6.
 */

#ifndef APU_XRT_BATCH_OPS_H_
#define APU_XRT_BATCH_OPS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_XRT_BATCH_CREATE    0x58424352u  /* 'XBCR' */
#define APU_XRT_BATCH_RUN       0x5842524Eu  /* 'XBRN' */
#define APU_XRT_BATCH_DESTROY   0x58424453u  /* 'XBDS' */

#define APU_XRT_BATCH_MAX_KARGS 24u
#define APU_XRT_BATCH_MAX_ARGS  60u   /* one 512 B rpmsg buffer */

/* CREATE */
#define APU_XRT_BATCH_CREATE_HDR_ARGS   4u
#define APU_XRT_BATCH_CREATE_NUM_ARGS(num_fixed) \
	(APU_XRT_BATCH_CREATE_HDR_ARGS + 3u * (num_fixed))
#define APU_XRT_BATCH_CREATE_FIXED_IDX(i) \
	(APU_XRT_BATCH_CREATE_HDR_ARGS + 3u * (i))

/* RUN */
#define APU_XRT_BATCH_RUN_HDR_ARGS      4u
#define APU_XRT_BATCH_RUN_NUM_ARGS(n, k) \
	(APU_XRT_BATCH_RUN_HDR_ARGS + (k) + 2u * (n) * (k))
#define APU_XRT_BATCH_RUN_KIDX_IDX(k) \
	(APU_XRT_BATCH_RUN_HDR_ARGS + (k))
#define APU_XRT_BATCH_RUN_ENTRY_IDX(e, k, kpe) \
	(APU_XRT_BATCH_RUN_HDR_ARGS + (kpe) + 2u * ((e) * (kpe) + (k)))

/* DESTROY */
#define APU_XRT_BATCH_DESTROY_NUM_ARGS  2u

/* status values (result word 0) */
#define APU_XRT_BATCH_OK          0u
#define APU_XRT_BATCH_ERR_ARGS    1u  /* malformed request */
#define APU_XRT_BATCH_ERR_BUSY    2u  /* CREATE while a context exists */
#define APU_XRT_BATCH_ERR_NOCTX   3u  /* RUN/DESTROY with an unknown ctx_id */
#define APU_XRT_BATCH_ERR_XRT     4u  /* XRT exception (ELF/hw_context/bo/...) */
#define APU_XRT_BATCH_ERR_RUN     5u  /* an entry did not complete */

#define APU_XRT_BATCH_PROF_MAGIC  0x50524F46u  /* 'PROF' */

/*
 * Result written by RPU_APP for every command (64 bytes). status is written
 * last. Timings are RPU_APP-side, in ns (100 MHz ARM architected timer),
 * valid when prof_magic == APU_XRT_BATCH_PROF_MAGIC (profiling builds).
 */
struct __attribute__((__packed__)) apu_xrt_batch_result {
	uint32_t status;          /* APU_XRT_BATCH_OK or APU_XRT_BATCH_ERR_* */
	uint32_t ctx_id;          /* CREATE: new id; RUN/DESTROY: echoed */
	uint32_t entries_done;    /* RUN: entries that completed */
	uint32_t prof_magic;

	uint32_t rx_to_start_ns;  /* RPMsg RX callback -> handler start */
	uint32_t handler_ns;      /* handler start -> before result fill */
	uint32_t elf_parse_ns;    /* CREATE: xrt::elf + kernel lookup */
	uint32_t hwctx_ns;        /* CREATE: hw_context + kernel + run create */
	uint32_t bo_map_ns;       /* buffers wrapped as xrt::bo by this command */
	uint32_t bind_ns;         /* RUN: set_arg of all kargs, sum over entries */
	uint32_t exec_ns;         /* RUN: start -> wait, sum over entries */
	uint32_t exec_max_ns;     /* RUN: longest single entry */
	uint32_t teardown_ns;     /* DESTROY: run/bo/kernel/hw_context release */
	uint32_t prev_send_ns;    /* previous command's reply send (rpmsg + IPI) */
	uint32_t bos_cached;      /* xrt::bo wrappers held (fixed + per-entry cache) */
	uint32_t reserved;
};

#define APU_XRT_BATCH_RESULT_SIZE  sizeof(struct apu_xrt_batch_result)

#ifdef __cplusplus
}
#endif

#endif /* APU_XRT_BATCH_OPS_H_ */
