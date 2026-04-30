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
 * Returns 0 if the firmware ECHO matches, or a negative errno:
 *   -ENODEV   channel is down (firmware not connected),
 *   -EBADMSG  response shorter than the AIE header+resp,
 *   -EIO      firmware reported a non-zero status,
 *   -EBADE    val1 / val2 mismatch.
 */
RPURT_RPC_API int rpurt_rpc_aie_echo(rpurt_rpc_handle *h,
                                     uint32_t val1, uint32_t val2);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPURT_RPC_H_ */
