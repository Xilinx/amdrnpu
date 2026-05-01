/* SPDX-License-Identifier: MIT */
/*
 * AIE management mailbox wire format - PRIVATE to the rpurt_rpc lib.
 *
 * Vendored from the RPU firmware tree:
 *   rpu-fw/tools/apu_aie_test/npu_msg_protocol.h
 *
 * The on-the-wire layout is owned by the firmware team (LX7 / Strix
 * Medusa NPU).  This file is a manually-synced snapshot.  When the
 * firmware bumps the protocol, copy the new definitions here and bump
 * PROTOCOL_MINOR (or MAJOR for ABI breaks).
 *
 * Lives under lib/src/ (not lib/include/) and is NOT installed,
 * because it is an implementation detail of how this lib speaks to
 * the firmware - not a stable lib API.  Lib consumers see only the
 * high-level helpers in <rpurt_rpc/rpurt_rpc.h>.
 *
 * The kernel driver is a transparent byte pipe over rpmsg today; all
 * mailbox framing happens here in userspace.  If we ever shift to a
 * driver-side framing model (xdna-driver style, with per-opcode
 * ioctls), this header would migrate to drivers/misc/rpurt-rpc/ as
 * part of that change.
 *
 * Copyright (C) 2024-2026, Advanced Micro Devices, Inc.
 */

#ifndef RPURT_RPC_AIE_MSG_H_
#define RPURT_RPC_AIE_MSG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 4)

/* Protocol version - matches lx7_fw npu_msg_priv.h. */
#define RPURT_RPC_AIE_PROTOCOL_MAJOR	5
#define RPURT_RPC_AIE_PROTOCOL_MINOR	24

/*
 * Mailbox message header - prepended to all AIE management messages.
 * From rpu-fw npu_mbox_comm_protocol.h.
 */
#define RPURT_RPC_AIE_MBOX_PROTOCOL_VERSION	0x1

#define RPURT_RPC_AIE_MBOX_MSG_SIZE_BITS	11
#define RPURT_RPC_AIE_MBOX_RSVD_SIZE_BITS	(16 - RPURT_RPC_AIE_MBOX_MSG_SIZE_BITS)
#define RPURT_RPC_AIE_MBOX_MSG_SEQ_BITS		8
#define RPURT_RPC_AIE_MBOX_PROTO_VER_BITS	8

struct rpurt_rpc_aie_msg_header {
	uint32_t total_msg_size;
	uint32_t msg_size		: RPURT_RPC_AIE_MBOX_MSG_SIZE_BITS;
	uint32_t rsvd0			: RPURT_RPC_AIE_MBOX_RSVD_SIZE_BITS;
	uint32_t protocol_version	: RPURT_RPC_AIE_MBOX_PROTO_VER_BITS;
	uint32_t msg_sequence_num	: RPURT_RPC_AIE_MBOX_MSG_SEQ_BITS;
	uint32_t msg_id;
	uint32_t msg_opcode;
};

/* Opcodes (subset; extend in lockstep with rpu-fw). */
enum rpurt_rpc_aie_opcode {
	RPURT_RPC_AIE_OP_ECHO				= 0x10001,
	RPURT_RPC_AIE_OP_IDENTIFY			= 0x10002,

	RPURT_RPC_AIE_OP_CREATE_PARTITION	= 0x30001,
	RPURT_RPC_AIE_OP_DESTROY_PARTITION	= 0x30002,
	RPURT_RPC_AIE_OP_CREATE_HW_CONTEXT	= 0x30003,
	RPURT_RPC_AIE_OP_DESTROY_HW_CONTEXT	= 0x30004,
	RPURT_RPC_AIE_OP_AIE_TILE_INFO		= 0x30006,
	RPURT_RPC_AIE_OP_SUBMIT_JOB			= 0x30010,
	RPURT_RPC_AIE_OP_QUERY_JOB			= 0x30011,
	RPURT_RPC_AIE_OP_LIST_JOBS			= 0x30012,
};

/* Status codes. */
enum rpurt_rpc_aie_status {
	RPURT_RPC_AIE_STATUS_SUCCESS	= 0x0,
	RPURT_RPC_AIE_STATUS_ERROR	= 0x1,
	RPURT_RPC_AIE_STATUS_NOTSUPP	= 0x2,
};

/*
 * Wire job state values (mirrored by enum rpurt_rpc_aie_job_state in the
 * public header; values must stay in sync).
 */
enum rpurt_rpc_aie_wire_job_state {
	RPURT_RPC_AIE_WIRE_JOB_FREE	= 0,
	RPURT_RPC_AIE_WIRE_JOB_PENDING	= 1,
	RPURT_RPC_AIE_WIRE_JOB_RUNNING	= 2,
	RPURT_RPC_AIE_WIRE_JOB_COMPLETED = 3,
	RPURT_RPC_AIE_WIRE_JOB_FAILED	= 4,
};

/* PASID format (for IOMMU address translation). */
union rpurt_rpc_aie_pasid {
	uint32_t raw;
	struct {
		uint32_t pasid		: 20;
		uint32_t resvd0		: 11;
		uint32_t pasid_vld	: 1;
	} f;
};

/* RPURT_RPC_AIE_OP_ECHO. */
struct rpurt_rpc_aie_echo_req {
	uint32_t val1;
	uint32_t val2;
};

struct rpurt_rpc_aie_echo_resp {
	uint32_t status;
	uint32_t val1;
	uint32_t val2;
};

/* RPURT_RPC_AIE_OP_IDENTIFY. */
struct rpurt_rpc_aie_identify_req {
	uint32_t resvd;
};

struct rpurt_rpc_aie_identify_resp {
	uint32_t status;
	uint32_t fw_major;
	uint32_t fw_minor;
	uint32_t fw_patch;
	uint32_t fw_build;
};

/* RPURT_RPC_AIE_OP_AIE_TILE_INFO. */
struct rpu_aie4_tile_info {
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
	uint16_t resvd;
};

struct rpurt_rpc_aie_tile_info_req {
	uint32_t resvd;
};

struct rpurt_rpc_aie_tile_info_resp {
	uint32_t status;
	struct rpu_aie4_tile_info info;
};

/* RPURT_RPC_AIE_OP_CREATE_PARTITION / DESTROY_PARTITION. */
struct rpurt_rpc_aie_create_partition_req {
	uint32_t partition_col_start;
	uint32_t partition_col_count;
};

struct rpurt_rpc_aie_create_partition_resp {
	uint32_t status;
	uint32_t partition_id;
};

struct rpurt_rpc_aie_destroy_partition_req {
	uint32_t partition_id;
};

struct rpurt_rpc_aie_destroy_partition_resp {
	uint32_t status;
};

/* RPURT_RPC_AIE_OP_CREATE_HW_CONTEXT / DESTROY_HW_CONTEXT. */
struct rpurt_rpc_aie_create_hw_context_req {
	uint32_t partition_id;
	uint32_t request_num_tiles;
	uint32_t hsa_addr_high;
	uint32_t hsa_addr_low;
	union rpurt_rpc_aie_pasid pasid;
	uint32_t priority_band;
};

struct rpurt_rpc_aie_create_hw_context_resp {
	uint32_t status;
	uint32_t hw_context_id;
	uint32_t doorbell_offset;
	uint32_t job_complete_msix_idx;
};

struct rpurt_rpc_aie_destroy_hw_context_req {
	uint32_t hw_context_id;
	uint32_t graceful_flag	: 1;
	uint32_t resvd1		: 31;
};

struct rpurt_rpc_aie_destroy_hw_context_resp {
	uint32_t status;
};

/* RPURT_RPC_AIE_OP_SUBMIT_JOB / QUERY_JOB (test extension). */
struct rpurt_rpc_aie_submit_job_req {
	uint32_t hw_context_id;
	uint32_t job_type;
	uint32_t exec_time_ms;
	uint32_t resvd;
};

struct rpurt_rpc_aie_submit_job_resp {
	uint32_t status;
	uint32_t job_id;
};

struct rpurt_rpc_aie_query_job_req {
	uint32_t job_id;
};

struct rpurt_rpc_aie_query_job_resp {
	uint32_t status;
	uint32_t job_state;
	int32_t  result;
};

/* RPURT_RPC_AIE_OP_LIST_JOBS: aggregate job counters (test extension). */
struct rpurt_rpc_aie_list_jobs_req {
	uint32_t resvd0;
};

struct rpurt_rpc_aie_list_jobs_resp {
	uint32_t status;
	uint32_t total_jobs;
	uint32_t pending_jobs;
	uint32_t running_jobs;
	uint32_t completed_jobs;
};

#pragma pack(pop)

/*
 * Initialise an AIE message header.  payload_size is the body size
 * (excluding this header).  Both total_msg_size and msg_size carry the
 * full header+body length, matching the firmware reference helper in
 * npu_msg_protocol.h:npu_msg_init_header().
 */
static inline void
rpurt_rpc_aie_msg_init_header(struct rpurt_rpc_aie_msg_header *hdr,
			      uint32_t opcode, uint32_t msg_id,
			      uint32_t payload_size)
{
	uint32_t total = (uint32_t)sizeof(*hdr) + payload_size;

	hdr->total_msg_size	 = total;
	hdr->msg_size		 = total;
	hdr->rsvd0		 = 0;
	hdr->protocol_version	 = RPURT_RPC_AIE_MBOX_PROTOCOL_VERSION;
	hdr->msg_sequence_num	 = 0;
	hdr->msg_id		 = msg_id;
	hdr->msg_opcode		 = opcode;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RPURT_RPC_AIE_MSG_H_ */
