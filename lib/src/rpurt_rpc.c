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
    /*
     * One-shot stack buffer sized to the kernel's max payload. The
     * pragma-pack(4) layout in aie_msg.h guarantees no padding inside
     * or after the header, so a flat buffer with header at offset 0
     * and payload immediately after matches the wire format.
     */
    uint8_t  buf[sizeof(struct rpurt_rpc_aie_msg_header) +
                 RPURT_RPC_MAX_PAYLOAD];
    struct rpurt_rpc_aie_msg_header *hdr = (struct rpurt_rpc_aie_msg_header *)buf;
    size_t   total;

    if (payload_len > sizeof(buf) - sizeof(*hdr))
        return -E2BIG;

    rpurt_rpc_aie_msg_init_header(hdr, opcode, /*msg_id=*/0,
                                  (uint32_t)payload_len);
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
