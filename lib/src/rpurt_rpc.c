/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc - userspace runtime library (placeholder implementation).
 *
 * Wraps the ioctls exposed by the kernel driver under
 * drivers/misc/rpurt-rpc. Real applications can use this directly or as
 * a starting point for richer protocol layers (request/response
 * matching, async dispatch, etc.).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "rpurt_rpc/rpurt_rpc.h"

#include <errno.h>
#include <fcntl.h>
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
