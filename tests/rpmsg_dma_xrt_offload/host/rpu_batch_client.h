/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * rpu_batch_client - x86 client for RPURuntime tests/xrt_batches over the
 * MDB5 PCIe-DMA rpmsg-lite link (wire protocol: apu_xrt_batch_ops.h).
 *
 *   rbc_open()     MDB5 nodes + rpmsg-lite DMA bring-up (sets DRIVER_OK)
 *   rbc_alloc()    bump-allocate device memory (RPU pool: ELF/results;
 *                  device pool: kernel-arg BOs), rbc_write/zero/read move data
 *   rbc_create()   APU_XRT_BATCH_CREATE  -> context id (ELF parsed once)
 *   rbc_run()      APU_XRT_BATCH_RUN     N entries against the context
 *   rbc_destroy()  APU_XRT_BATCH_DESTROY
 *
 * Commands are synchronous: send, then poll for the rpmsg reply or the DDR
 * result word. A client is not thread-safe; use it from one thread.
 */
#ifndef RPU_BATCH_CLIENT_H_
#define RPU_BATCH_CLIENT_H_

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "apu_xrt_batch_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Offload INI (same keys as xrt_offload_host / batch_host). */
struct rbc_config {
	char     mdb5_write[PATH_MAX];
	char     mdb5_read[PATH_MAX];

	uint64_t dev_base;
	uint64_t shm_size;
	uint64_t status_offset;
	uint64_t vring_tx_offset;
	uint64_t vring_rx_offset;
	uint64_t buf_offset;
	uint32_t vring_num;
	uint32_t vring_align;
	uint32_t buf_size;
	uint32_t buf_count;

	char     ept_name[32];
	uint32_t host_ept_addr;
	uint32_t peer_dst;

	uint64_t rpu_pool_base;  /* ELF / results (RPU-reachable) */
	uint64_t rpu_pool_size;
	uint64_t dev_pool_base;  /* kernel-arg BOs (AIE-reachable) */
	uint64_t dev_pool_size;
};

/* path NULL = built-in defaults. Returns 0 or negative errno. */
int rbc_config_load(struct rbc_config *c, const char *path);

struct rbc_open_opts {
	bool     verbose;      /* bring-up, hexdumps, completion trace */
	bool     trace_dma;    /* print the first access to each DMA (addr, len) */
	uint32_t timeout_s;    /* per-command completion timeout (0 = 60 s) */
	uint32_t poll_us;      /* sleep between completion checks (0 = busy poll) */
};

struct rbc_client;

int  rbc_open(struct rbc_client **out, const struct rbc_config *cfg,
	      const struct rbc_open_opts *o);
void rbc_close(struct rbc_client *c);

/* Make an in-flight or later command return -EINTR (async-signal-safe), and
 * re-arm the client (e.g. to still send DESTROY after an interrupt). */
void rbc_request_stop(struct rbc_client *c);
void rbc_clear_stop(struct rbc_client *c);

enum rbc_pool { RBC_POOL_RPU, RBC_POOL_DEV };

int rbc_alloc(struct rbc_client *c, enum rbc_pool p, uint64_t len, uint64_t *pa);
int rbc_write(struct rbc_client *c, uint64_t pa, const void *src, uint64_t len);
int rbc_zero(struct rbc_client *c, uint64_t pa, uint64_t len);
int rbc_read(struct rbc_client *c, void *dst, uint64_t pa, uint64_t len);

/* One kernel arg: buffer (pa, size), or scalar (size 0, value in pa). */
struct rbc_karg {
	uint32_t kidx;
	uint64_t pa;
	uint64_t size;
};

struct rbc_cmd_stat {
	uint64_t exec_ns;                   /* host: send -> completion seen */
	bool     via_rsp;                   /* completion seen via rpmsg reply */
	struct apu_xrt_batch_result res;    /* RPU_APP's result (status, timings) */
};

/* Commands. Return 0 when RPU_APP reported APU_XRT_BATCH_OK, negative errno
 * otherwise (st->res.status holds the RPU status when it got that far). */
int rbc_create(struct rbc_client *c, uint64_t elf_pa, uint64_t elf_size,
	       const struct rbc_karg *fixed, uint32_t n_fixed,
	       uint32_t *ctx_id, struct rbc_cmd_stat *st);
/* entries[e * kpe + k] is entry e's arg kidx[k] (its .kidx is ignored).
 * Request limit: APU_XRT_BATCH_RUN_NUM_ARGS(n, kpe) <= APU_XRT_BATCH_MAX_ARGS. */
int rbc_run(struct rbc_client *c, uint32_t ctx_id, uint32_t n_entries,
	    uint32_t kpe, const uint32_t *kidx, const struct rbc_karg *entries,
	    struct rbc_cmd_stat *st);
int rbc_destroy(struct rbc_client *c, uint32_t ctx_id, struct rbc_cmd_stat *st);

/* Host file helpers. rbc_load_elf inflates a zlib-compressed control ELF.
 * Free *data with free(). */
int rbc_load_file(const char *path, uint8_t **data, uint64_t *size);
int rbc_load_elf(const char *path, uint8_t **data, uint64_t *size);

const char *rbc_status_str(uint32_t status);

#ifdef __cplusplus
}
#endif

#endif /* RPU_BATCH_CLIENT_H_ */
