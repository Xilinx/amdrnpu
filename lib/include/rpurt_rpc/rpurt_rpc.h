/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc - public API of the userspace runtime library.
 *
 * The library is a thin wrapper around ioctls on /dev/rpurt_rpc.N
 * (one device per bound RPU rpmsg-aie-mgmt channel) exposed by the
 * kernel driver under drivers/misc/rpurt-rpc.  The full transport ABI
 * is defined in <uapi/rpurt-rpc/rpurt_rpc.h>; clients normally only
 * need the helpers declared here.
 */
#ifndef RPURT_RPC_H_
#define RPURT_RPC_H_

#include <stddef.h>
#include <stdint.h>

#include <rpurt-rpc/rpurt_rpc.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  define RPURT_RPC_API
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define RPURT_RPC_API __attribute__((visibility("default")))
#else
#  define RPURT_RPC_API
#endif

/** Opaque handle to an open RPC channel. */
typedef struct rpurt_rpc_handle rpurt_rpc_handle;

/**
 * rpurt_rpc_open - open the default RPC device (RPURT_RPC_DEVICE_PATH).
 *
 * For single-RPU systems, multi-RPU callers should use
 * rpurt_rpc_open_path().
 *
 * Returns a handle on success, NULL on failure (errno is set).
 */
RPURT_RPC_API rpurt_rpc_handle *rpurt_rpc_open(void);

/**
 * rpurt_rpc_open_path - open a specific device node.
 * @path: path to the device, e.g. "/dev/rpurt_rpc.0".
 */
RPURT_RPC_API rpurt_rpc_handle *rpurt_rpc_open_path(const char *path);

/** Close a previously opened handle. Safe to call with NULL. */
RPURT_RPC_API void rpurt_rpc_close(rpurt_rpc_handle *h);

/** Query driver/ABI information. Returns 0 on success, -errno otherwise. */
RPURT_RPC_API int rpurt_rpc_get_info(rpurt_rpc_handle *h,
                                     struct rpurt_rpc_info *info);

/**
 * rpurt_rpc_send - send a message to the RPU.
 * @h:      open handle
 * @opcode: caller-defined operation code
 * @data:   payload buffer (may be NULL if @len == 0)
 * @len:    payload length, must be <= RPURT_RPC_MAX_PAYLOAD
 *
 * Returns 0 on success, -errno on failure.
 */
RPURT_RPC_API int rpurt_rpc_send(rpurt_rpc_handle *h,
                                 uint32_t opcode,
                                 const void *data,
                                 size_t len);

/**
 * rpurt_rpc_recv - receive a message from the RPU.
 * @h:   open handle
 * @msg: caller-allocated message structure to fill
 *
 * Blocks until a message is available unless the underlying file
 * descriptor was put in non-blocking mode. Returns 0 on success,
 * -errno on failure.
 */
RPURT_RPC_API int rpurt_rpc_recv(rpurt_rpc_handle *h,
                                 struct rpurt_rpc_msg *msg);

/** Reset internal queues / state. */
RPURT_RPC_API int rpurt_rpc_reset(rpurt_rpc_handle *h);

/** Return the underlying file descriptor (e.g. for poll/epoll). */
RPURT_RPC_API int rpurt_rpc_fd(rpurt_rpc_handle *h);

/** Library version string ("MAJOR.MINOR.PATCH"). */
RPURT_RPC_API const char *rpurt_rpc_version(void);

/**
 * rpurt_rpc_aie_echo - Run the AIE management ECHO round-trip.
 * @h:    open handle (caller owns it; not safe to share across threads)
 * @val1: first 32-bit pattern
 * @val2: second 32-bit pattern
 *
 * Builds an AIE management ECHO request (npu_mbox header + payload),
 * dispatches it via RPURT_RPC_IOC_SEND, blocks for the response on
 * RPURT_RPC_IOC_RECV, and validates that the firmware echoed @val1 and
 * @val2 verbatim with status=SUCCESS.
 *
 */
RPURT_RPC_API int rpurt_rpc_aie_echo(rpurt_rpc_handle *h,
                                     uint32_t val1, uint32_t val2);

/* ------------------------------------------------------------------ *
 * AIE management - public types for the M2 opcode set.
 *
 * The wire-format structs live in lib/src/aie_msg.h (private). The
 * types below are the lib's caller-visible mirror so consumers do not
 * have to depend on the firmware header.
 * ------------------------------------------------------------------ */

/** Firmware version returned by rpurt_rpc_aie_identify(). */
struct rpurt_rpc_aie_fw_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t build;
};

/** AIE tile geometry returned by rpurt_rpc_aie_tile_info(). */
struct rpurt_rpc_aie_tile_geometry {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    uint16_t cols;
    uint16_t rows;
    uint16_t core_rows;
    uint16_t mem_rows;
    uint16_t shim_rows;
    uint16_t core_row_start;
    uint16_t mem_row_start;
    uint16_t shim_row_start;
    uint16_t core_dma_channels;
    uint16_t mem_dma_channels;
    uint16_t shim_dma_channels;
    uint16_t core_locks;
    uint16_t mem_locks;
    uint16_t shim_locks;
    uint16_t core_events;
    uint16_t mem_events;
    uint16_t shim_events;
};

/**
 * Job life-cycle states reported by rpurt_rpc_aie_job_status() /
 * _job_wait().  Values are wire-stable with the firmware's enum.
 */
enum rpurt_rpc_aie_job_state {
    RPURT_RPC_AIE_JOB_FREE      = 0,
    RPURT_RPC_AIE_JOB_PENDING   = 1,
    RPURT_RPC_AIE_JOB_RUNNING   = 2,
    RPURT_RPC_AIE_JOB_COMPLETED = 3,
    RPURT_RPC_AIE_JOB_FAILED    = 4,
};

/** Aggregate job counters returned by rpurt_rpc_aie_job_list(). */
struct rpurt_rpc_aie_job_summary {
    uint32_t total;
    uint32_t pending;
    uint32_t running;
    uint32_t completed;
};

/* ------------------------------------------------------------------ *
 * AIE management - per-opcode helpers.
 *
 * All return 0 on success or a negative errno.  Common errors:
 *   -ENODEV   rpmsg channel is down (firmware not connected),
 *   -EBADMSG  response shorter than expected for the opcode,
 *   -EIO      firmware reported a non-zero status,
 *   -ETIMEDOUT  rpurt_rpc_aie_job_wait() deadline reached.
 * ------------------------------------------------------------------ */

/**
 * rpurt_rpc_aie_identify - Query firmware version.
 * @h:           open handle
 * @out_version: caller-allocated; populated on success.
 */
RPURT_RPC_API int rpurt_rpc_aie_identify(rpurt_rpc_handle *h,
                                         struct rpurt_rpc_aie_fw_version *out_version);

/**
 * rpurt_rpc_aie_tile_info - Query AIE array geometry.
 * @h:        open handle
 * @out_geom: caller-allocated; populated on success.
 */
RPURT_RPC_API int rpurt_rpc_aie_tile_info(rpurt_rpc_handle *h,
                                          struct rpurt_rpc_aie_tile_geometry *out_geom);

/**
 * rpurt_rpc_aie_partition_create - Create an AIE partition.
 * @h:                open handle
 * @col_start:        starting column of the partition
 * @col_count:        number of columns
 * @out_partition_id: filled with firmware-assigned partition id.
 */
RPURT_RPC_API int rpurt_rpc_aie_partition_create(rpurt_rpc_handle *h,
                                                 uint32_t col_start,
                                                 uint32_t col_count,
                                                 uint32_t *out_partition_id);

/**
 * rpurt_rpc_aie_partition_destroy - Destroy a previously created partition.
 * @h:            open handle
 * @partition_id: id returned by rpurt_rpc_aie_partition_create().
 */
RPURT_RPC_API int rpurt_rpc_aie_partition_destroy(rpurt_rpc_handle *h,
                                                  uint32_t partition_id);

/**
 * rpurt_rpc_aie_hwctx_create - Create a hardware context on a partition.
 * @h:                   open handle
 * @partition_id:        partition to allocate the context on
 * @num_tiles:           number of tiles to reserve
 * @priority:            priority band (0 = default)
 * @out_ctx_id:          filled with firmware-assigned context id
 * @out_doorbell_offset: filled with the context's doorbell MMIO offset
 *                       (may be NULL)
 * @out_msix_idx:        filled with the job-completion MSI-X index
 *                       (may be NULL)
 *
 * pasid and hsa_addr are zeroed in the wire request to match the
 * apu_aie_test reference flow.
 */
RPURT_RPC_API int rpurt_rpc_aie_hwctx_create(rpurt_rpc_handle *h,
                                             uint32_t partition_id,
                                             uint32_t num_tiles,
                                             uint32_t priority,
                                             uint32_t *out_ctx_id,
                                             uint32_t *out_doorbell_offset,
                                             uint32_t *out_msix_idx);

/**
 * rpurt_rpc_aie_hwctx_destroy - Tear down a hardware context.
 * @h:        open handle
 * @ctx_id:   context id from rpurt_rpc_aie_hwctx_create()
 * @graceful: non-zero requests graceful drain before tearing down
 */
RPURT_RPC_API int rpurt_rpc_aie_hwctx_destroy(rpurt_rpc_handle *h,
                                              uint32_t ctx_id,
                                              int graceful);

/**
 * rpurt_rpc_aie_job_submit - Submit a (test) job to a context.
 * @h:            open handle
 * @ctx_id:       hw context id
 * @job_type:     opaque job type understood by firmware
 * @exec_time_ms: simulated execution time in milliseconds
 * @out_job_id:   filled with firmware-assigned job id
 */
RPURT_RPC_API int rpurt_rpc_aie_job_submit(rpurt_rpc_handle *h,
                                           uint32_t ctx_id,
                                           uint32_t job_type,
                                           uint32_t exec_time_ms,
                                           uint32_t *out_job_id);

/**
 * rpurt_rpc_aie_job_status - One-shot query of a job's state.
 * @h:          open handle
 * @job_id:     job id from rpurt_rpc_aie_job_submit()
 * @out_state:  filled with the current job state (may be NULL)
 * @out_result: filled with the firmware-provided result code
 *              (may be NULL)
 */
RPURT_RPC_API int rpurt_rpc_aie_job_status(rpurt_rpc_handle *h,
                                           uint32_t job_id,
                                           enum rpurt_rpc_aie_job_state *out_state,
                                           int32_t *out_result);

/**
 * rpurt_rpc_aie_job_wait - Block until a job reaches a terminal state.
 * @h:          open handle
 * @job_id:     job id from rpurt_rpc_aie_job_submit()
 * @timeout_ms: maximum wait in milliseconds; 0 means a single status
 *              poll with no wait
 * @out_result: filled with the firmware-provided result code on
 *              COMPLETED (may be NULL)
 *
 * Returns 0 on COMPLETED, -EIO on FAILED, -ETIMEDOUT on deadline,
 * or another negative errno on transport failure.  Implemented as a
 * status-poll loop at ~50 ms cadence.
 */
RPURT_RPC_API int rpurt_rpc_aie_job_wait(rpurt_rpc_handle *h,
                                         uint32_t job_id,
                                         unsigned int timeout_ms,
                                         int32_t *out_result);

/**
 * rpurt_rpc_aie_job_list - Query firmware-side aggregate job counters.
 * @h:           open handle
 * @out_summary: caller-allocated; populated on success.
 *
 * This is a test-extension opcode (NPU_MSG_OP_LIST_JOBS, 0x30012) and
 * mirrors the `job list` subcommand of the apu_aie_test reference
 * binary.  It returns the firmware's view of all jobs in aggregate
 * (total / pending / running / completed); the firmware does not
 * expose per-job entries through this opcode.
 */
RPURT_RPC_API int rpurt_rpc_aie_job_list(rpurt_rpc_handle *h,
                                         struct rpurt_rpc_aie_job_summary *out_summary);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPURT_RPC_H_ */
