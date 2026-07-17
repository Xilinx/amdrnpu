/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */
#ifndef _UAPI_AMDRNPU_ACCEL_H_
#define _UAPI_AMDRNPU_ACCEL_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define AMDRNPU_DEVICE_NAME	"amdrnpu"
#define AMDRNPU_DEVICE_PATH	"/dev/" AMDRNPU_DEVICE_NAME ".0"

/* Bump on any incompatible change to the structures below. */
#define AMDRNPU_ABI_VERSION	1

/* Maximum payload (in bytes) carried in a single RPC message. */
#define AMDRNPU_MAX_PAYLOAD	256

/**
 * struct amdrnpu_msg - generic RPC message exchanged with the RPU.
 * @opcode:   caller-defined operation code
 * @flags:    reserved for future use, must be zero
 * @len:      number of valid bytes in @data
 * @data:     opaque payload
 */
struct amdrnpu_msg {
	__u32 opcode;
	__u32 flags;
	__u32 len;
	__u8  data[AMDRNPU_MAX_PAYLOAD];
};

/**
 * struct amdrnpu_info - driver/ABI information returned to userspace.
 * @abi_version: matches AMDRNPU_ABI_VERSION at build time of the driver
 * @max_payload: maximum payload supported by the driver
 */
struct amdrnpu_info {
	__u32 abi_version;
	__u32 max_payload;
};

#define AMDRNPU_IOC_MAGIC	'A'

/* Query driver/ABI information. */
#define AMDRNPU_IOC_GET_INFO	_IOR(AMDRNPU_IOC_MAGIC, 0x01, struct amdrnpu_info)
/* Send a message to the RPU. */
#define AMDRNPU_IOC_SEND	_IOW(AMDRNPU_IOC_MAGIC, 0x02, struct amdrnpu_msg)
/* Receive a message from the RPU (blocking until available or O_NONBLOCK). */
#define AMDRNPU_IOC_RECV	_IOR(AMDRNPU_IOC_MAGIC, 0x03, struct amdrnpu_msg)
/* Reset internal queues / state. */
#define AMDRNPU_IOC_RESET	_IO (AMDRNPU_IOC_MAGIC, 0x04)

#endif /* _UAPI_AMDRNPU_ACCEL_H_ */
