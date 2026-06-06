/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * tessai-agent RPMsg endpoint inside RPURuntime (link A handler table).
 *
 * Byte-compatible with the rpu-fw mirror so the APU-side kernel driver
 * and userspace binaries work unchanged regardless of which remoteproc
 * the rpmsg channel is bound to. Wire opcodes / structs / bounds live
 * in tessai_agent_msg_def.h. The handler surface:
 *
 *   ECHO          whole-BO writeback + COMPLETE
 *   ECHO_REGION   partial-window writeback (proves both BUF_SYNC dirs)
 *   DAPP          device-shmem PA echo into a small carveout BO (RPU
 *                 NEVER dereferences the echoed PA)
 *   COPY_IN_OUT   local-R5_1 memcpy inside tess_rpu_gem_carveout
 *   REMI/NOTS/CANC/BADC  fault-rsp_type variants
 *   TESSAI_WIRE_FLAG_ABORT  -> CANCELLED, wins over command_op
 *   everything else         -> COMPLETE
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/devicetree.h>

#include <openamp/open_amp.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apu_rpmsg_platform.h"
#include "tessai_agent_endpoint.h"
#include "tessai_agent_msg_def.h"

LOG_MODULE_REGISTER(tessai_agent_ep, CONFIG_LOG_DEFAULT_LEVEL);

#define TESSAI_AGENT_EPT_ADDR       RPMSG_ADDR_ANY

typedef struct {
	struct rpmsg_endpoint ept;
	bool                  bound;
} tessai_agent_ctx_t;

static tessai_agent_ctx_t tessai_agent_ctx;

static int tessai_send_short_rsp(struct rpmsg_endpoint *ept,
				 uint32_t seq, uint32_t rsp_type)
{
	struct tessai_rsp_wire_hdr rsp = {
		.seq              = sys_cpu_to_le32(seq),
		.command_rsp_type = sys_cpu_to_le32(rsp_type),
		.reserved         = { 0, 0, 0, 0 },
	};

	return rpmsg_send(ept, &rsp, sizeof(rsp));
}

/*
 * fw_echo: write "rsp: command id: %u, num_args: %u\n" into the
 * carveout BO at args[0]. Caller always sends COMPLETE; -EINVAL
 * means writeback was skipped (bad PA / arg count).
 */
static int tessai_fw_echo_writeback(uint32_t seq, uint32_t num_args,
				    const void *args_blob)
{
	uint64_t dst_pa_le;
	uint64_t dst_pa;
	uint8_t *dst;
	char buf[TESSAI_FW_RSP_TEXT_MAX];
	int n;

	if (num_args < 1 || args_blob == NULL) {
		LOG_DBG("fw_echo: skip writeback (num_args=%u)", num_args);
		return -EINVAL;
	}

	memcpy(&dst_pa_le, args_blob, sizeof(dst_pa_le));
	dst_pa = sys_le64_to_cpu(dst_pa_le);

	if (dst_pa < TESSAI_RPU_BASE ||
	    dst_pa - TESSAI_RPU_BASE > TESSAI_RPU_SIZE - TESSAI_FW_RSP_TEXT_MAX) {
		LOG_WRN("fw_echo: skip writeback - dst_pa=0x%llx outside "
			"[0x%llx, 0x%llx)",
			(unsigned long long)dst_pa,
			(unsigned long long)TESSAI_RPU_BASE,
			(unsigned long long)(TESSAI_RPU_BASE + TESSAI_RPU_SIZE));
		return -EINVAL;
	}

	n = snprintf(buf, sizeof(buf),
		     "rsp: command id: %u, num_args: %u\n",
		     (unsigned int)seq, (unsigned int)num_args);
	if (n < 0) {
		LOG_ERR("fw_echo: snprintf failed (n=%d)", n);
		return -EIO;
	}
	if (n >= (int)sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
		buf[n] = '\0';
	}

	dst = (uint8_t *)(uintptr_t)dst_pa;
	memcpy(dst, buf, (size_t)n + 1);

	/* Carveout is mapped non-cacheable normal memory; one dsb
	 * orders the stores ahead of the COMPLETE doorbell IPI. */
	barrier_dsync_fence_full();

	LOG_DBG("fw_echo: wrote %d bytes to dst=0x%llx (seq=%u)",
		n, (unsigned long long)dst_pa, seq);
	return 0;
}

/*
 * fw_echo_region: partial-window round-trip for the
 * TessaiAgentMemory.BufSyncPartialRoundTrip gtest.
 *
 * args[0]=dst_pa, args[1]=offset (8-byte aligned), args[2]=length
 * (16..TESSAI_FW_ECHO_REGION_MAX). Reads 8 bytes at dst_pa+offset
 * (proves BUF_SYNC TO_DEVICE flushed), writes a sentinel banner
 * over [dst_pa+offset, dst_pa+offset+length) (proves BUF_SYNC
 * FROM_DEVICE invalidate), pads tail with 0xAA so the gtest can
 * verify the partial window wasn't widened. Same -EINVAL / always-
 * send-COMPLETE discipline as tessai_fw_echo_writeback.
 */
static int tessai_fw_echo_region(uint32_t seq, uint32_t num_args,
				 const void *args_blob)
{
	uint64_t dst_pa_le, offset_le, length_le;
	uint64_t dst_pa, offset, length;
	uint64_t input_echo_le;
	uint8_t *win;
	char buf[TESSAI_FW_ECHO_REGION_MAX];
	int n;

	if (num_args < 3 || args_blob == NULL) {
		LOG_WRN("fw_echo_region: skip (num_args=%u, need >=3)",
			num_args);
		return -EINVAL;
	}

	memcpy(&dst_pa_le, (const uint8_t *)args_blob + 0 * 8, sizeof(dst_pa_le));
	memcpy(&offset_le, (const uint8_t *)args_blob + 1 * 8, sizeof(offset_le));
	memcpy(&length_le, (const uint8_t *)args_blob + 2 * 8, sizeof(length_le));
	dst_pa = sys_le64_to_cpu(dst_pa_le);
	offset = sys_le64_to_cpu(offset_le);
	length = sys_le64_to_cpu(length_le);

	/* offset must be 8-byte aligned for the natural-aligned input
	 * echo load; length must hold an 8-byte echo + banner. */
	if ((offset & 0x7u) != 0) {
		LOG_WRN("fw_echo_region: offset=0x%llx not 8-byte aligned",
			(unsigned long long)offset);
		return -EINVAL;
	}
	if (length < 16 || length > TESSAI_FW_ECHO_REGION_MAX) {
		LOG_WRN("fw_echo_region: length=%llu out of range [16, %u]",
			(unsigned long long)length, TESSAI_FW_ECHO_REGION_MAX);
		return -EINVAL;
	}

	/* Whole partial window must fit inside the carveout; two-step
	 * to dodge UINT64_MAX overflow even though length is capped. */
	if (dst_pa < TESSAI_RPU_BASE ||
	    dst_pa - TESSAI_RPU_BASE > TESSAI_RPU_SIZE - length ||
	    offset > TESSAI_RPU_SIZE - length ||
	    (dst_pa - TESSAI_RPU_BASE) + offset >
		    TESSAI_RPU_SIZE - length) {
		LOG_WRN("fw_echo_region: window dst=0x%llx+0x%llx+%llu "
			"outside carveout [0x%llx, 0x%llx)",
			(unsigned long long)dst_pa,
			(unsigned long long)offset,
			(unsigned long long)length,
			(unsigned long long)TESSAI_RPU_BASE,
			(unsigned long long)(TESSAI_RPU_BASE + TESSAI_RPU_SIZE));
		return -EINVAL;
	}

	win = (uint8_t *)(uintptr_t)(dst_pa + offset);

	/* READ phase: snapshot the first 8 bytes BEFORE overwriting. */
	memcpy(&input_echo_le, win, sizeof(input_echo_le));

	/* WRITE phase: banner is the wire contract — keep in lockstep
	 * with the std::sscanf in tests/unittests/src/test-memory.cpp
	 * (TessaiAgentMemory.BufSyncPartialRoundTrip). */
	n = snprintf(buf, sizeof(buf),
		     "region seq=%u off=%llu len=%llu in=0x%016llx",
		     (unsigned int)seq,
		     (unsigned long long)offset,
		     (unsigned long long)length,
		     (unsigned long long)sys_le64_to_cpu(input_echo_le));
	if (n < 0) {
		LOG_ERR("fw_echo_region: snprintf failed (n=%d)", n);
		return -EIO;
	}
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;

	/* 0xAA tail proves length-honouring (distinct from the test's
	 * pre-submit 0xCC/0x33 sentinels); starts past the banner's
	 * '\0' so it doesn't disturb the sscanf parse. */
	memcpy(win, buf, (size_t)n + 1);
	if (length > (uint64_t)(n + 1))
		memset(win + n + 1, 0xAA, (size_t)length - (n + 1));

	barrier_dsync_fence_full();

	LOG_DBG("fw_echo_region: wrote %d-byte banner + 0xAA tail to "
		"dst=0x%llx+0x%llx (len=%llu, in_echo=0x%016llx, seq=%u)",
		n, (unsigned long long)dst_pa, (unsigned long long)offset,
		(unsigned long long)length,
		(unsigned long long)sys_le64_to_cpu(input_echo_le), seq);
	return 0;
}

/*
 * DAPP: echo a device-shmem PA + size into a small carveout BO so
 * userspace can verify the wire round-trip without the RPU ever
 * touching the device-shmem memory (which is outside the MPU window).
 *
 * args[0]=ack_pa (carveout writeback target), args[1]=device_pa
 * (echoed verbatim, NEVER dereferenced), args[2]=device_sz. Banner
 * is the wire contract — keep in lockstep with the std::sscanf in
 * tests/unittests/src/test-cmd.cpp (DeviceShmemAddrRoundTrip).
 * Same -EINVAL / always-send-COMPLETE discipline as the other
 * writeback handlers.
 */
static int tessai_fw_device_addr_ping(uint32_t seq, uint32_t num_args,
				      const void *args_blob)
{
	uint64_t ack_pa_le, device_pa_le, device_sz_le;
	uint64_t ack_pa, device_pa, device_sz;
	uint8_t *win;
	char buf[TESSAI_FW_DAPP_BANNER_MAX];
	int n;

	if (num_args < 3 || args_blob == NULL) {
		LOG_WRN("fw_device_addr_ping: skip (num_args=%u, need >=3)",
			num_args);
		return -EINVAL;
	}

	memcpy(&ack_pa_le,    (const uint8_t *)args_blob + 0 * 8, sizeof(ack_pa_le));
	memcpy(&device_pa_le, (const uint8_t *)args_blob + 1 * 8, sizeof(device_pa_le));
	memcpy(&device_sz_le, (const uint8_t *)args_blob + 2 * 8, sizeof(device_sz_le));
	ack_pa    = sys_le64_to_cpu(ack_pa_le);
	device_pa = sys_le64_to_cpu(device_pa_le);
	device_sz = sys_le64_to_cpu(device_sz_le);

	/* Bounds-check ack_pa only; device_pa/device_sz are intentionally
	 * outside any MPU window we own. */
	if (ack_pa < TESSAI_RPU_BASE ||
	    ack_pa - TESSAI_RPU_BASE > TESSAI_RPU_SIZE - TESSAI_FW_DAPP_BANNER_MAX) {
		LOG_WRN("fw_device_addr_ping: ack_pa=0x%llx outside carveout "
			"[0x%llx, 0x%llx) (banner_max=%u)",
			(unsigned long long)ack_pa,
			(unsigned long long)TESSAI_RPU_BASE,
			(unsigned long long)(TESSAI_RPU_BASE + TESSAI_RPU_SIZE),
			TESSAI_FW_DAPP_BANNER_MAX);
		return -EINVAL;
	}

	win = (uint8_t *)(uintptr_t)ack_pa;

	n = snprintf(buf, sizeof(buf),
		     "dev-ping pa=0x%016llx size=%llu seq=%u",
		     (unsigned long long)device_pa,
		     (unsigned long long)device_sz,
		     (unsigned int)seq);
	if (n < 0) {
		LOG_ERR("fw_device_addr_ping: snprintf failed (n=%d)", n);
		return -EIO;
	}
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;

	memcpy(win, buf, (size_t)n + 1);

	barrier_dsync_fence_full();

	LOG_DBG("fw_device_addr_ping: wrote %d-byte banner to ack_pa=0x%llx "
		"(echoed dev_pa=0x%016llx dev_sz=%llu seq=%u)",
		n, (unsigned long long)ack_pa,
		(unsigned long long)device_pa,
		(unsigned long long)device_sz, seq);
	return 0;
}

/*
 * COPY_IN_OUT: local-R5_1 memcpy inside tess_rpu_gem_carveout.
 *
 * Wire args: [0]=INPUT_BO_TAG, [1]=in_size, [2]=in_phys,
 *            [3]=OUTPUT_BO_TAG, [4]=out_size, [5]=out_phys.
 * Bounds-checks both PAs against the carveout MPU window, copies
 * min(in_size, out_size) bytes. Returns COMPLETE on success,
 * REMOTEIO on bad args / OOB PA / size overflow.
 */
static uint32_t tessai_copy_in_out(uint32_t seq, uint32_t num_args,
				   const void *args_blob)
{
	uint64_t in_size_le, in_pa_le, out_size_le, out_pa_le;
	uint64_t in_size, in_pa, out_size, out_pa;
	size_t   copy_len;
	uint8_t *in_va, *out_va;

	if (num_args < 6 || args_blob == NULL) {
		LOG_WRN("copy_in_out: malformed cmd (num_args=%u, need >=6)",
			num_args);
		return TESSAI_RSP_REMOTEIO;
	}

	memcpy(&in_size_le,  (const uint8_t *)args_blob + 1 * 8, sizeof(in_size_le));
	memcpy(&in_pa_le,    (const uint8_t *)args_blob + 2 * 8, sizeof(in_pa_le));
	memcpy(&out_size_le, (const uint8_t *)args_blob + 4 * 8, sizeof(out_size_le));
	memcpy(&out_pa_le,   (const uint8_t *)args_blob + 5 * 8, sizeof(out_pa_le));

	in_size  = sys_le64_to_cpu(in_size_le);
	in_pa    = sys_le64_to_cpu(in_pa_le);
	out_size = sys_le64_to_cpu(out_size_le);
	out_pa   = sys_le64_to_cpu(out_pa_le);

	if (in_size == 0 || out_size == 0 || in_pa == 0 || out_pa == 0) {
		LOG_WRN("copy_in_out: zero arg "
			"(in_sz=%llu in_pa=0x%llx out_sz=%llu out_pa=0x%llx)",
			(unsigned long long)in_size, (unsigned long long)in_pa,
			(unsigned long long)out_size, (unsigned long long)out_pa);
		return TESSAI_RSP_REMOTEIO;
	}

	copy_len = (size_t)((in_size < out_size) ? in_size : out_size);

	/* Two-step overflow-safe bound check: guard copy_len against
	 * the carveout, then check each PA fits inside [BASE, BASE+SIZE). */
	if (copy_len > TESSAI_RPU_SIZE) {
		LOG_WRN("copy_in_out: copy_len=%zu > carveout %llu",
			copy_len, (unsigned long long)TESSAI_RPU_SIZE);
		return TESSAI_RSP_REMOTEIO;
	}
	if (in_pa < TESSAI_RPU_BASE ||
	    in_pa - TESSAI_RPU_BASE > TESSAI_RPU_SIZE - copy_len) {
		LOG_WRN("copy_in_out: in_pa=0x%llx+%zu outside [0x%llx, 0x%llx)",
			(unsigned long long)in_pa, copy_len,
			(unsigned long long)TESSAI_RPU_BASE,
			(unsigned long long)(TESSAI_RPU_BASE + TESSAI_RPU_SIZE));
		return TESSAI_RSP_REMOTEIO;
	}
	if (out_pa < TESSAI_RPU_BASE ||
	    out_pa - TESSAI_RPU_BASE > TESSAI_RPU_SIZE - copy_len) {
		LOG_WRN("copy_in_out: out_pa=0x%llx+%zu outside [0x%llx, 0x%llx)",
			(unsigned long long)out_pa, copy_len,
			(unsigned long long)TESSAI_RPU_BASE,
			(unsigned long long)(TESSAI_RPU_BASE + TESSAI_RPU_SIZE));
		return TESSAI_RSP_REMOTEIO;
	}

	in_va  = (uint8_t *)(uintptr_t)in_pa;
	out_va = (uint8_t *)(uintptr_t)out_pa;
	memcpy(out_va, in_va, copy_len);
	barrier_dsync_fence_full();

	/* LOG_INF: boot_vek385.py greps for this marker. */
	LOG_INF("COPY op seq=%u len=%zu in=0x%llx out=0x%llx -> COMPLETE",
		seq, copy_len,
		(unsigned long long)in_pa,
		(unsigned long long)out_pa);
	return TESSAI_RSP_COMPLETE;
}

static int tessai_agent_ept_cb(struct rpmsg_endpoint *ept, void *data,
			       size_t len, uint32_t src, void *priv)
{
	const struct tessai_wire_cmd *cmd = (const struct tessai_wire_cmd *)data;
	uint32_t seq;
	uint32_t command_op, flags, num_args;
	size_t   need;
	uint32_t rsp_type;
	int      ret;

	ARG_UNUSED(src);
	ARG_UNUSED(priv);

	if (len < offsetof(struct tessai_wire_cmd, args)) {
		LOG_WRN("dropping short rpmsg (len=%zu, need >=%zu)",
			len, offsetof(struct tessai_wire_cmd, args));
		return RPMSG_SUCCESS;
	}

	seq        = sys_le32_to_cpu(cmd->seq);
	command_op = sys_le32_to_cpu(cmd->command_op);
	flags      = sys_le32_to_cpu(cmd->flags);
	num_args   = sys_le32_to_cpu(cmd->num_args);

	need = offsetof(struct tessai_wire_cmd, args) +
	       (size_t)num_args * sizeof(uint64_t);
	if (len < need) {
		LOG_WRN("dropping truncated rpmsg (len=%zu need=%zu num_args=%u)",
			len, need, num_args);
		return RPMSG_SUCCESS;
	}

	if (flags & TESSAI_WIRE_FLAG_ABORT) {
		rsp_type = TESSAI_RSP_CANCELLED;
	} else {
		switch (command_op) {
		case TESSAI_FW_OP_FAULT_REMOTEIO:
			rsp_type = TESSAI_RSP_REMOTEIO;
			break;
		case TESSAI_FW_OP_FAULT_NOTSUPP:
			rsp_type = TESSAI_RSP_NOTSUPPORTED;
			break;
		case TESSAI_FW_OP_FAULT_CANCELLED:
			rsp_type = TESSAI_RSP_CANCELLED;
			break;
		case TESSAI_FW_OP_FAULT_BADRSP:
			rsp_type = TESSAI_RSP_TYPE_INVALID;
			break;
		case TESSAI_OP_COPY_IN_OUT: {
			const uint8_t *args_blob = (const uint8_t *)data +
				offsetof(struct tessai_wire_cmd, args);
			rsp_type = tessai_copy_in_out(seq, num_args, args_blob);
			break;
		}
		default:
			rsp_type = TESSAI_RSP_COMPLETE;
			break;
		}
	}

	LOG_DBG("RX seq=%u command_op=0x%08x flags=0x%08x num_args=%u rsp=%u",
		seq, command_op, flags, num_args, rsp_type);

	if ((flags & TESSAI_WIRE_FLAG_ABORT) == 0) {
		const uint8_t *args_blob = (const uint8_t *)data +
			offsetof(struct tessai_wire_cmd, args);

		/* All writeback handlers send COMPLETE regardless of
		 * -EINVAL; userspace diagnoses "wait succeeded but banner
		 * absent" via its bytes-dump path. */
		switch (command_op) {
		case TESSAI_FW_ECHO_OP:
			(void)tessai_fw_echo_writeback(seq, num_args,
						       args_blob);
			break;
		case TESSAI_FW_ECHO_REGION_OP:
			(void)tessai_fw_echo_region(seq, num_args,
						    args_blob);
			break;
		case TESSAI_FW_OP_DEVICE_ADDR_PING:
			(void)tessai_fw_device_addr_ping(seq, num_args,
							 args_blob);
			break;
		default:
			break;
		}
	}

	ret = tessai_send_short_rsp(ept, seq, rsp_type);
	if (ret < 0) {
		LOG_ERR("rpmsg_send response failed: %d", ret);
	}

	return RPMSG_SUCCESS;
}

static void tessai_agent_ept_destroy_cb(struct rpmsg_endpoint *ept)
{
	LOG_WRN("tessai-agent endpoint destroyed");
	tessai_agent_ctx.bound = false;
	rpmsg_destroy_ept(ept);
}

int tessai_agent_endpoint_init(void)
{
	struct rpmsg_device *rdev;
	int ret;

	if (tessai_agent_ctx.bound) {
		LOG_WRN("tessai-agent endpoint already bound; refusing duplicate init");
		return -EALREADY;
	}

	rdev = apu_openamp_get_rpmsg_device();
	if (rdev == NULL) {
		LOG_ERR("apu_openamp_get_rpmsg_device() returned NULL "
			"(apu_rpmsg_platform_init must have succeeded first)");
		return -ENODEV;
	}

	LOG_INF("Creating tessai-agent endpoint: %s", TESSAI_AGENT_EPT_NAME);

	memset(&tessai_agent_ctx, 0, sizeof(tessai_agent_ctx));

	ret = rpmsg_create_ept(&tessai_agent_ctx.ept, rdev, TESSAI_AGENT_EPT_NAME,
			       TESSAI_AGENT_EPT_ADDR, RPMSG_ADDR_ANY,
			       tessai_agent_ept_cb,
			       tessai_agent_ept_destroy_cb);
	if (ret < 0) {
		LOG_ERR("rpmsg_create_ept(tessai-agent) failed: %d", ret);
		return ret;
	}

	tessai_agent_ctx.ept.priv = &tessai_agent_ctx;
	tessai_agent_ctx.bound    = true;

	LOG_INF("tessai-agent endpoint created at addr=%u", tessai_agent_ctx.ept.addr);
	return 0;
}

/*
 * Auto-register at APPLICATION-level SYS_INIT priority 99 (the max in
 * Zephyr 3.7) so we run after apu_rpmsg_platform_sys_init at priority
 * CONFIG_APPLICATION_INIT_PRIORITY=90 has finished its blocking
 * DRIVER_OK wait.
 */
static int tessai_agent_endpoint_sys_init(void)
{
	int ret = tessai_agent_endpoint_init();

	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("tessai-agent endpoint SYS_INIT failed: %d", ret);
	}
	return 0;
}

SYS_INIT(tessai_agent_endpoint_sys_init, APPLICATION, 99);
