/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * rpurt-rpc - userspace/kernel ABI
 *
 * Shared between the kernel driver under drivers/misc/rpurt-rpc and the
 * userspace runtime library under lib/. Keep this header free of any
 * kernel-only types so it can be installed as a UAPI header.
 */
#ifndef _UAPI_RPURT_RPC_H_
#define _UAPI_RPURT_RPC_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define RPURT_RPC_DEVICE_NAME	"rpurt_rpc"
#define RPURT_RPC_DEVICE_PATH	"/dev/" RPURT_RPC_DEVICE_NAME

/* Bump on any incompatible change to the structures below. */
#define RPURT_RPC_ABI_VERSION	1

/* Maximum payload (in bytes) carried in a single RPC message. */
#define RPURT_RPC_MAX_PAYLOAD	256

/**
 * struct rpurt_rpc_msg - generic RPC message exchanged with the RPU.
 * @opcode:   caller-defined operation code
 * @flags:    reserved for future use, must be zero
 * @len:      number of valid bytes in @data
 * @data:     opaque payload
 */
struct rpurt_rpc_msg {
	__u32 opcode;
	__u32 flags;
	__u32 len;
	__u8  data[RPURT_RPC_MAX_PAYLOAD];
};

/**
 * struct rpurt_rpc_info - driver/ABI information returned to userspace.
 * @abi_version: matches RPURT_RPC_ABI_VERSION at build time of the driver
 * @max_payload: maximum payload supported by the driver
 */
struct rpurt_rpc_info {
	__u32 abi_version;
	__u32 max_payload;
};

#define RPURT_RPC_IOC_MAGIC	'R'

/* Query driver/ABI information. */
#define RPURT_RPC_IOC_GET_INFO	_IOR(RPURT_RPC_IOC_MAGIC, 0x01, struct rpurt_rpc_info)
/* Send a message to the RPU. */
#define RPURT_RPC_IOC_SEND	_IOW(RPURT_RPC_IOC_MAGIC, 0x02, struct rpurt_rpc_msg)
/* Receive a message from the RPU (blocking until available or O_NONBLOCK). */
#define RPURT_RPC_IOC_RECV	_IOR(RPURT_RPC_IOC_MAGIC, 0x03, struct rpurt_rpc_msg)
/* Reset internal queues / state. */
#define RPURT_RPC_IOC_RESET	_IO (RPURT_RPC_IOC_MAGIC, 0x04)

#endif /* _UAPI_RPURT_RPC_H_ */
