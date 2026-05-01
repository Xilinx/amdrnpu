/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc - userspace runtime library.
 *
 * Wraps the ioctls exposed by the kernel driver under
 * drivers/misc/rpurt-rpc. The kernel side is a transparent byte pipe
 * over rpmsg; framing for specific firmware services lives here in
 * userspace (see rpurt_rpc_aie_echo() for the AIE management mailbox).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "rpurt_rpc/rpurt_rpc.h"
#include "aie_msg.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef RPURT_RPC_VERSION_STR
#define RPURT_RPC_VERSION_STR "0.1.0"
#endif

struct rpurt_rpc_handle {
    int fd;
};

static int xlate_errno(int rc)
{
    if (rc == 0)
        return 0;
    return -errno;
}

rpurt_rpc_handle *rpurt_rpc_open(void)
{
    return rpurt_rpc_open_path(RPURT_RPC_DEVICE_PATH);
}

rpurt_rpc_handle *rpurt_rpc_open_path(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    rpurt_rpc_handle *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;

    h->fd = open(path, O_RDWR | O_CLOEXEC);
    if (h->fd < 0) {
        int saved = errno;
        free(h);
        errno = saved;
        return NULL;
    }
    return h;
}

void rpurt_rpc_close(rpurt_rpc_handle *h)
{
    if (!h)
        return;
    if (h->fd >= 0)
        close(h->fd);
    free(h);
}

int rpurt_rpc_fd(rpurt_rpc_handle *h)
{
    if (!h)
        return -EINVAL;
    return h->fd;
}

int rpurt_rpc_get_info(rpurt_rpc_handle *h, struct rpurt_rpc_info *info)
{
    if (!h || !info)
        return -EINVAL;
    return xlate_errno(ioctl(h->fd, RPURT_RPC_IOC_GET_INFO, info));
}

int rpurt_rpc_send(rpurt_rpc_handle *h, uint32_t opcode,
                   const void *data, size_t len)
{
    struct rpurt_rpc_msg msg;

    if (!h)
        return -EINVAL;
    if (len > RPURT_RPC_MAX_PAYLOAD)
        return -E2BIG;
    if (len && !data)
        return -EINVAL;

    memset(&msg, 0, sizeof(msg));
    msg.opcode = opcode;
    msg.flags  = 0;
    msg.len    = (uint32_t)len;
    if (len)
        memcpy(msg.data, data, len);

    return xlate_errno(ioctl(h->fd, RPURT_RPC_IOC_SEND, &msg));
}

int rpurt_rpc_recv(rpurt_rpc_handle *h, struct rpurt_rpc_msg *msg)
{
    if (!h || !msg)
        return -EINVAL;
    return xlate_errno(ioctl(h->fd, RPURT_RPC_IOC_RECV, msg));
}

int rpurt_rpc_reset(rpurt_rpc_handle *h)
{
    if (!h)
        return -EINVAL;
    return xlate_errno(ioctl(h->fd, RPURT_RPC_IOC_RESET));
}

const char *rpurt_rpc_version(void)
{
    return RPURT_RPC_VERSION_STR;
}

/*
 * AIE management mailbox helpers.
 *
 * The on-the-wire format is:
 *
 *   [ rpurt_rpc_aie_msg_header ][ <opcode-specific request body>  ]    TX
 *   [ rpurt_rpc_aie_msg_header ][ <opcode-specific response body> ]    RX
 *
 * aie_send_msg() prepends the header to a caller-supplied request
 * payload and sends one rpmsg packet via RPURT_RPC_IOC_SEND.
 * aie_recv_msg() blocks for a response, validates the header (size
 * sanity), then returns a pointer to the body and its length.
 *
 * msg_id is currently always zero. The reference apu_aie_test from
 * the firmware tree hardcodes msg_id=1 and the firmware does not
 * enforce sequencing for the opcodes we handle today, so a constant
 * is fine. When we add async/multi-flight dispatch (response routing
 * across concurrent IOC_SENDs), the per-handle counter and a request
 * map come back together in the same change.
 */

static int aie_send_msg(rpurt_rpc_handle *h, uint32_t opcode,
                        const void *payload, size_t payload_len)
{
    uint8_t  buf[sizeof(struct rpurt_rpc_aie_msg_header) +
                 RPURT_RPC_MAX_PAYLOAD];
    struct rpurt_rpc_aie_msg_header *hdr = (struct rpurt_rpc_aie_msg_header *)buf;
    size_t   total;

    if (payload_len > sizeof(buf) - sizeof(*hdr))
        return -E2BIG;

    rpurt_rpc_aie_msg_init_header(hdr, opcode, 0, (uint32_t)payload_len);
    if (payload_len)
        memcpy(buf + sizeof(*hdr), payload, payload_len);

    total = sizeof(*hdr) + payload_len;

    return rpurt_rpc_send(h, opcode, buf, total);
}

/*
 * Receive an AIE message and hand back a pointer into the supplied
 * rx buffer for the response body (header stripped, body length set).
 * Caller-owned @rx must outlive any use of *body_out.
 */
static int aie_recv_msg(rpurt_rpc_handle *h, struct rpurt_rpc_msg *rx,
                        const void **body_out, size_t *body_len_out)
{
    const struct rpurt_rpc_aie_msg_header *hdr;
    int ret;

    memset(rx, 0, sizeof(*rx));
    ret = rpurt_rpc_recv(h, rx);
    if (ret < 0)
        return ret;

    if (rx->len < sizeof(*hdr))
        return -EBADMSG;

    hdr = (const struct rpurt_rpc_aie_msg_header *)rx->data;
    if (hdr->total_msg_size != rx->len)
        return -EBADMSG;

    *body_out     = rx->data + sizeof(*hdr);
    *body_len_out = rx->len - sizeof(*hdr);
    return 0;
}

int rpurt_rpc_aie_echo(rpurt_rpc_handle *h, uint32_t val1, uint32_t val2)
{
    struct rpurt_rpc_aie_echo_req  req = { .val1 = val1, .val2 = val2 };
    struct rpurt_rpc_msg           rx;
    const struct rpurt_rpc_aie_echo_resp *resp;
    const void *body;
    size_t      body_len;
    int         ret;

    if (!h)
        return -EINVAL;

    ret = aie_send_msg(h, RPURT_RPC_AIE_OP_ECHO, &req, sizeof(req));
    if (ret < 0)
        return ret;

    ret = aie_recv_msg(h, &rx, &body, &body_len);
    if (ret < 0)
        return ret;

    if (body_len < sizeof(*resp))
        return -EBADMSG;

    resp = (const struct rpurt_rpc_aie_echo_resp *)body;
    if (resp->status != RPURT_RPC_AIE_STATUS_SUCCESS)
        return -EIO;
    if (resp->val1 != val1 || resp->val2 != val2)
        return -EBADE;

    return 0;
}

/*
 * Generic request/response helper for opcodes whose entire
 * post-send work is "validate body length, validate status, hand the
 * body back to the caller".
 */
static int aie_op_msg_helper(rpurt_rpc_handle *h, uint32_t opcode,
                    const void *req, size_t req_len,
                    void *resp, size_t resp_len)
{
    struct rpurt_rpc_msg rx;
    const void *body;
    size_t      body_len;
    uint32_t    status;
    int         ret;

    if (!h)
        return -EINVAL;

    ret = aie_send_msg(h, opcode, req, req_len);
    if (ret < 0)
        return ret;

    ret = aie_recv_msg(h, &rx, &body, &body_len);
    if (ret < 0)
        return ret;

    if (body_len < resp_len || body_len < sizeof(status))
        return -EBADMSG;

    memcpy(&status, body, sizeof(status));
    if (status != RPURT_RPC_AIE_STATUS_SUCCESS)
        return -EIO;

    if (resp && resp_len)
        memcpy(resp, body, resp_len);
    return 0;
}

int rpurt_rpc_aie_identify(rpurt_rpc_handle *h,
                           struct rpurt_rpc_aie_fw_version *out_version)
{
    struct rpurt_rpc_aie_identify_req  req  = { .resvd = 0 };
    struct rpurt_rpc_aie_identify_resp resp;
    int ret;

    if (!out_version)
        return -EINVAL;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_IDENTIFY,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    out_version->major = resp.fw_major;
    out_version->minor = resp.fw_minor;
    out_version->patch = resp.fw_patch;
    out_version->build = resp.fw_build;
    return 0;
}

int rpurt_rpc_aie_tile_info(rpurt_rpc_handle *h,
                            struct rpurt_rpc_aie_tile_geometry *out_geom)
{
    struct rpurt_rpc_aie_tile_info_req  req = { .resvd = 0 };
    struct rpurt_rpc_aie_tile_info_resp resp;
    int ret;

    if (!out_geom)
        return -EINVAL;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_AIE_TILE_INFO,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    out_geom->size              = resp.info.size;
    out_geom->major             = resp.info.major;
    out_geom->minor             = resp.info.minor;
    out_geom->cols              = resp.info.cols;
    out_geom->rows              = resp.info.rows;
    out_geom->core_rows         = resp.info.core_rows;
    out_geom->mem_rows          = resp.info.mem_rows;
    out_geom->shim_rows         = resp.info.shim_rows;
    out_geom->core_row_start    = resp.info.core_row_start;
    out_geom->mem_row_start     = resp.info.mem_row_start;
    out_geom->shim_row_start    = resp.info.shim_row_start;
    out_geom->core_dma_channels = resp.info.core_dma_channels;
    out_geom->mem_dma_channels  = resp.info.mem_dma_channels;
    out_geom->shim_dma_channels = resp.info.shim_dma_channels;
    out_geom->core_locks        = resp.info.core_locks;
    out_geom->mem_locks         = resp.info.mem_locks;
    out_geom->shim_locks        = resp.info.shim_locks;
    out_geom->core_events       = resp.info.core_events;
    out_geom->mem_events        = resp.info.mem_events;
    out_geom->shim_events       = resp.info.shim_events;
    return 0;
}

int rpurt_rpc_aie_partition_create(rpurt_rpc_handle *h,
                                   uint32_t col_start, uint32_t col_count,
                                   uint32_t *out_partition_id)
{
    struct rpurt_rpc_aie_create_partition_req  req = {
        .partition_col_start = col_start,
        .partition_col_count = col_count,
    };
    struct rpurt_rpc_aie_create_partition_resp resp;
    int ret;

    if (!out_partition_id)
        return -EINVAL;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_CREATE_PARTITION,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    *out_partition_id = resp.partition_id;
    return 0;
}

int rpurt_rpc_aie_partition_destroy(rpurt_rpc_handle *h, uint32_t partition_id)
{
    struct rpurt_rpc_aie_destroy_partition_req  req = {
        .partition_id = partition_id,
    };
    struct rpurt_rpc_aie_destroy_partition_resp resp;

    return aie_op_msg_helper(h, RPURT_RPC_AIE_OP_DESTROY_PARTITION,
                    &req, sizeof(req), &resp, sizeof(resp));
}

int rpurt_rpc_aie_hwctx_create(rpurt_rpc_handle *h,
                               uint32_t partition_id, uint32_t num_tiles,
                               uint32_t priority,
                               uint32_t *out_ctx_id,
                               uint32_t *out_doorbell_offset,
                               uint32_t *out_msix_idx)
{
    struct rpurt_rpc_aie_create_hw_context_req  req = {
        .partition_id      = partition_id,
        .request_num_tiles = num_tiles,
        .hsa_addr_high     = 0,
        .hsa_addr_low      = 0,
        .pasid             = { .raw = 0 },
        .priority_band     = priority,
    };
    struct rpurt_rpc_aie_create_hw_context_resp resp;
    int ret;

    if (!out_ctx_id)
        return -EINVAL;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_CREATE_HW_CONTEXT,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    *out_ctx_id = resp.hw_context_id;
    if (out_doorbell_offset)
        *out_doorbell_offset = resp.doorbell_offset;
    if (out_msix_idx)
        *out_msix_idx = resp.job_complete_msix_idx;
    return 0;
}

int rpurt_rpc_aie_hwctx_destroy(rpurt_rpc_handle *h,
                                uint32_t ctx_id, int graceful)
{
    struct rpurt_rpc_aie_destroy_hw_context_req  req = {
        .hw_context_id = ctx_id,
        .graceful_flag = graceful ? 1 : 0,
        .resvd1        = 0,
    };
    struct rpurt_rpc_aie_destroy_hw_context_resp resp;

    return aie_op_msg_helper(h, RPURT_RPC_AIE_OP_DESTROY_HW_CONTEXT,
                    &req, sizeof(req), &resp, sizeof(resp));
}

int rpurt_rpc_aie_job_submit(rpurt_rpc_handle *h,
                             uint32_t ctx_id, uint32_t job_type,
                             uint32_t exec_time_ms, uint32_t *out_job_id)
{
    struct rpurt_rpc_aie_submit_job_req  req = {
        .hw_context_id = ctx_id,
        .job_type      = job_type,
        .exec_time_ms  = exec_time_ms,
        .resvd         = 0,
    };
    struct rpurt_rpc_aie_submit_job_resp resp;
    int ret;

    if (!out_job_id)
        return -EINVAL;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_SUBMIT_JOB,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    *out_job_id = resp.job_id;
    return 0;
}

int rpurt_rpc_aie_job_status(rpurt_rpc_handle *h, uint32_t job_id,
                             enum rpurt_rpc_aie_job_state *out_state,
                             int32_t *out_result)
{
    struct rpurt_rpc_aie_query_job_req  req = { .job_id = job_id };
    struct rpurt_rpc_aie_query_job_resp resp;
    int ret;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_QUERY_JOB,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    if (out_state)
        *out_state = (enum rpurt_rpc_aie_job_state)resp.job_state;
    if (out_result)
        *out_result = resp.result;
    return 0;
}

/*
 * Returns true once @now has caught up to or passed @deadline.
 * Both are CLOCK_MONOTONIC timespecs.
 */
static int monotonic_deadline_passed(const struct timespec *now,
                                     const struct timespec *deadline)
{
    if (now->tv_sec != deadline->tv_sec)
        return now->tv_sec > deadline->tv_sec;
    return now->tv_nsec >= deadline->tv_nsec;
}

int rpurt_rpc_aie_job_wait(rpurt_rpc_handle *h, uint32_t job_id,
                           unsigned int timeout_ms, int32_t *out_result)
{
    /*
     * 50 ms cadence is plenty for the test/job opcodes the firmware
     * exposes today (typical exec_time_ms is in the 100s of ms). We
     * size up later if/when that turns into a real bottleneck.
     */
    static const long POLL_INTERVAL_NS = 50L * 1000L * 1000L;
    struct timespec deadline, now;

    if (!h)
        return -EINVAL;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return -errno;
    deadline.tv_sec  += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)((timeout_ms % 1000U) * 1000000UL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        enum rpurt_rpc_aie_job_state state = RPURT_RPC_AIE_JOB_FREE;
        int32_t result = 0;
        int ret;

        ret = rpurt_rpc_aie_job_status(h, job_id, &state, &result);
        if (ret < 0)
            return ret;

        if (state == RPURT_RPC_AIE_JOB_COMPLETED) {
            if (out_result)
                *out_result = result;
            return 0;
        }
        if (state == RPURT_RPC_AIE_JOB_FAILED) {
            if (out_result)
                *out_result = result;
            return -EIO;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -errno;
        if (monotonic_deadline_passed(&now, &deadline))
            return -ETIMEDOUT;

        {
            struct timespec sleep_for = {
                .tv_sec  = 0,
                .tv_nsec = POLL_INTERVAL_NS,
            };
            (void)nanosleep(&sleep_for, NULL);
        }
    }
}

int rpurt_rpc_aie_job_list(rpurt_rpc_handle *h,
                           struct rpurt_rpc_aie_job_summary *out_summary)
{
    struct rpurt_rpc_aie_list_jobs_req  req  = { .resvd0 = 0 };
    struct rpurt_rpc_aie_list_jobs_resp resp;
    int ret;

    ret = aie_op_msg_helper(h, RPURT_RPC_AIE_OP_LIST_JOBS,
                   &req, sizeof(req), &resp, sizeof(resp));
    if (ret < 0)
        return ret;

    if (out_summary) {
        out_summary->total     = resp.total_jobs;
        out_summary->pending   = resp.pending_jobs;
        out_summary->running   = resp.running_jobs;
        out_summary->completed = resp.completed_jobs;
    }
    return 0;
}
