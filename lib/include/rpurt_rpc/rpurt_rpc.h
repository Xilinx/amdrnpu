/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc - public API of the userspace runtime library.
 *
 * The library is a thin wrapper around ioctls on /dev/rpurt_rpc exposed
 * by the kernel driver under drivers/misc/rpurt-rpc. The full transport
 * ABI is defined in <uapi/rpurt-rpc/rpurt_rpc.h>; clients normally only
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
 * rpurt_rpc_open - open the default RPC device (/dev/rpurt_rpc).
 *
 * Returns a handle on success, NULL on failure (errno is set).
 */
RPURT_RPC_API rpurt_rpc_handle *rpurt_rpc_open(void);

/**
 * rpurt_rpc_open_path - open a specific device node.
 * @path: path to the device, e.g. "/dev/rpurt_rpc".
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPURT_RPC_H_ */
