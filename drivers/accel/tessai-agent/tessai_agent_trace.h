/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM tessai_agent

#if !defined(_TESSAI_AGENT_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TESSAI_AGENT_TRACE_H_

#include <linux/tracepoint.h>
#include <linux/types.h>

/* Buffer allocation ioctl (TESSAI_BUF_ALLOC). */
TRACE_EVENT(tessai_buf_alloc_enter,
	TP_PROTO(u32 memory_type, u64 size),
	TP_ARGS(memory_type, size),
	TP_STRUCT__entry(
		__field(u32, memory_type)
		__field(u64, size)
	),
	TP_fast_assign(
		__entry->memory_type = memory_type;
		__entry->size = size;
	),
	TP_printk("memory_type=%u size=%llu",
		  __entry->memory_type, __entry->size)
);

TRACE_EVENT(tessai_buf_alloc_exit,
	TP_PROTO(u32 handle, int ret),
	TP_ARGS(handle, ret),
	TP_STRUCT__entry(
		__field(u32, handle)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->handle = handle;
		__entry->ret = ret;
	),
	TP_printk("handle=%u ret=%d", __entry->handle, __entry->ret)
);

/* Command submit ioctl (TESSAI_CMD_SUBMIT). */
TRACE_EVENT(tessai_cmd_submit_enter,
	TP_PROTO(u32 command_op, u32 num_args),
	TP_ARGS(command_op, num_args),
	TP_STRUCT__entry(
		__field(u32, command_op)
		__field(u32, num_args)
	),
	TP_fast_assign(
		__entry->command_op = command_op;
		__entry->num_args = num_args;
	),
	TP_printk("op=%#x num_args=%u",
		  __entry->command_op, __entry->num_args)
);

TRACE_EVENT(tessai_cmd_submit_exit,
	TP_PROTO(u32 seq, u32 syncobj_handle, int ret),
	TP_ARGS(seq, syncobj_handle, ret),
	TP_STRUCT__entry(
		__field(u32, seq)
		__field(u32, syncobj_handle)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->seq = seq;
		__entry->syncobj_handle = syncobj_handle;
		__entry->ret = ret;
	),
	TP_printk("seq=%u syncobj_handle=%u ret=%d",
		  __entry->seq, __entry->syncobj_handle, __entry->ret)
);

/* rpmsg_send() around the wire command. */
TRACE_EVENT(tessai_rpmsg_send_enter,
	TP_PROTO(u32 seq, u32 command_op, u32 num_args),
	TP_ARGS(seq, command_op, num_args),
	TP_STRUCT__entry(
		__field(u32, seq)
		__field(u32, command_op)
		__field(u32, num_args)
	),
	TP_fast_assign(
		__entry->seq = seq;
		__entry->command_op = command_op;
		__entry->num_args = num_args;
	),
	TP_printk("seq=%u op=%#x num_args=%u",
		  __entry->seq, __entry->command_op, __entry->num_args)
);

TRACE_EVENT(tessai_rpmsg_send_exit,
	TP_PROTO(u32 seq, int ret),
	TP_ARGS(seq, ret),
	TP_STRUCT__entry(
		__field(u32, seq)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->seq = seq;
		__entry->ret = ret;
	),
	TP_printk("seq=%u ret=%d", __entry->seq, __entry->ret)
);

/* rpmsg receive callback. */
TRACE_EVENT(tessai_rpmsg_cb_enter,
	TP_PROTO(int len, u32 src),
	TP_ARGS(len, src),
	TP_STRUCT__entry(
		__field(int, len)
		__field(u32, src)
	),
	TP_fast_assign(
		__entry->len = len;
		__entry->src = src;
	),
	TP_printk("len=%d src=%u", __entry->len, __entry->src)
);

TRACE_EVENT(tessai_rpmsg_cb_exit,
	TP_PROTO(int dummy),
	TP_ARGS(dummy),
	TP_STRUCT__entry(
		__field(int, dummy)
	),
	TP_fast_assign(
		__entry->dummy = dummy;
	),
	TP_printk("%s", "")
);

#endif /* _TESSAI_AGENT_TRACE_H_ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE tessai_agent_trace
#include <trace/define_trace.h>
