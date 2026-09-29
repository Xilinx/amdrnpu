// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/array_size.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "amdrnpu_internal.h"

struct amdrnpu_fence {
	struct dma_fence	base;
	spinlock_t		lock;	/* dma_fence lock */
};

static const char *amdrnpu_fence_get_driver_name(struct dma_fence *fence)
{
	return AMDRNPU_DRV_NAME;
}

static const char *amdrnpu_fence_get_timeline_name(struct dma_fence *fence)
{
	return "amdrnpu-event";
}

static const struct dma_fence_ops amdrnpu_fence_ops = {
	.get_driver_name	= amdrnpu_fence_get_driver_name,
	.get_timeline_name	= amdrnpu_fence_get_timeline_name,
};

static struct dma_fence *amdrnpu_fence_create(struct amdrnpu_dev *rnpu)
{
	struct amdrnpu_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &amdrnpu_fence_ops, &f->lock,
		       rnpu->fence_context,
		       atomic_inc_return(&rnpu->fence_seqno));
	return &f->base;
}

static void amdrnpu_cmd_release(struct kref *ref)
{
	struct amdrnpu_cmd *cmd = container_of(ref, struct amdrnpu_cmd, ref);
	struct dma_fence *f;
	unsigned long flags;

	spin_lock_irqsave(&cmd->fence_lock, flags);
	f = cmd->pending_fence;
	cmd->pending_fence = NULL;
	spin_unlock_irqrestore(&cmd->fence_lock, flags);
	if (f) {
		dma_fence_set_error(f, -ECONNRESET);
		dma_fence_signal(f);
		dma_fence_put(f);
	}

	/* Drop the buffers pinned for this command's arguments. */
	while (cmd->num_bo_refs)
		amdrnpu_buf_put(cmd->bo_refs[--cmd->num_bo_refs]);
	kfree(cmd->bo_refs);

	kfree(cmd);
}

static void amdrnpu_cmd_put(struct amdrnpu_cmd *cmd)
{
	kref_put(&cmd->ref, amdrnpu_cmd_release);
}

/* Remove @seq from cmd_xa; NULL if unknown or already completed. */
static struct amdrnpu_cmd *amdrnpu_cmd_xa_take(struct amdrnpu_dev *rnpu, u32 seq)
{
	struct amdrnpu_cmd *cmd;

	mutex_lock(&rnpu->cmd_lock);
	cmd = xa_erase(&rnpu->cmd_xa, seq);
	mutex_unlock(&rnpu->cmd_lock);
	return cmd;
}

/*
 * Remove @cmd from cmd_xa only if it is still the entry at its seq (identity
 * check via cmpxchg guards against the id having been reused).  Returns true if
 * this caller removed it.
 */
static bool amdrnpu_cmd_xa_remove(struct amdrnpu_dev *rnpu, struct amdrnpu_cmd *cmd)
{
	bool removed;

	mutex_lock(&rnpu->cmd_lock);
	removed = xa_cmpxchg(&rnpu->cmd_xa, cmd->seq, cmd, NULL, GFP_KERNEL) == cmd;
	mutex_unlock(&rnpu->cmd_lock);
	return removed;
}

/*
 * Allocate a wire command id with xa_alloc_cyclic (xa_limit_32b, reused when
 * free).  Returns 0 on success (both the no-wrap and wrap-around cases).
 */
static int amdrnpu_cmd_xa_alloc_id(struct amdrnpu_dev *rnpu, struct amdrnpu_cmd *cmd)
{
	int err;
	u32 id;

	mutex_lock(&rnpu->cmd_lock);
	err = xa_alloc_cyclic(&rnpu->cmd_xa, &id, cmd, xa_limit_32b,
			      &rnpu->cmd_xa_next, GFP_KERNEL);
	mutex_unlock(&rnpu->cmd_lock);
	if (err < 0)
		return err;
	cmd->seq = id;
	return 0;
}

static struct amdrnpu_cmd *amdrnpu_cmd_create(struct amdrnpu_dev *rnpu, u32 op)
{
	struct amdrnpu_cmd *cmd;
	struct dma_fence *fence;
	int ret;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);

	cmd->rnpu	= rnpu;
	cmd->command_op	= op;
	cmd->seq	= AMDRNPU_CMD_SEQ_NONE;
	spin_lock_init(&cmd->fence_lock);
	kref_init(&cmd->ref);

	fence = amdrnpu_fence_create(rnpu);
	if (!fence) {
		ret = -ENOMEM;
		goto err_free;
	}
	cmd->pending_fence = fence;

	ret = amdrnpu_cmd_xa_alloc_id(rnpu, cmd);
	if (ret)
		goto err_fence;
	return cmd;

err_fence:
	dma_fence_put(fence);
err_free:
	kfree(cmd);
	return ERR_PTR(ret);
}

/* Terminal completion: record status, signal and drop the lone pending fence. */
static void amdrnpu_cmd_signal(struct amdrnpu_cmd *cmd, int status)
{
	struct dma_fence *old;
	unsigned long flags;

	spin_lock_irqsave(&cmd->fence_lock, flags);
	old = cmd->pending_fence;
	cmd->pending_fence = NULL;
	spin_unlock_irqrestore(&cmd->fence_lock, flags);

	if (!old)
		return;

	if (status)
		dma_fence_set_error(old, status);
	dma_fence_signal(old);
	dma_fence_put(old);
}

/*
 * Terminal completion, called by whoever removed @cmd from cmd_xa: signal the
 * fence (idempotent) and drop the reference the cmd_xa entry owned.
 */
static void amdrnpu_cmd_complete(struct amdrnpu_cmd *cmd, int status)
{
	amdrnpu_cmd_signal(cmd, status);
	amdrnpu_cmd_put(cmd);
}

void amdrnpu_dev_cmd_init(struct amdrnpu_dev *rnpu)
{
	mutex_init(&rnpu->cmd_lock);
	xa_init_flags(&rnpu->cmd_xa, XA_FLAGS_ALLOC);
	rnpu->cmd_xa_next = 0;
	rnpu->fence_context = dma_fence_context_alloc(1);
	atomic_set(&rnpu->fence_seqno, 0);
}

/* Forcibly complete every in-flight command with @err. */
static void amdrnpu_cmd_drain_xa(struct amdrnpu_dev *rnpu, int err)
{
	struct amdrnpu_cmd *cmd;
	unsigned long index;

	for (;;) {
		cmd = NULL;
		mutex_lock(&rnpu->cmd_lock);
		xa_for_each(&rnpu->cmd_xa, index, cmd) {
			xa_erase(&rnpu->cmd_xa, index);
			break;
		}
		mutex_unlock(&rnpu->cmd_lock);
		if (!cmd)
			break;

		amdrnpu_cmd_complete(cmd, err);
	}
}

void amdrnpu_dev_cmd_drain(struct amdrnpu_dev *rnpu, int err)
{
	if (!err)
		err = -ENOTCONN;
	amdrnpu_cmd_drain_xa(rnpu, err);
}

void amdrnpu_dev_cmd_fini(struct amdrnpu_dev *rnpu)
{
	amdrnpu_cmd_drain_xa(rnpu, -ENODEV);
	xa_destroy(&rnpu->cmd_xa);
	mutex_destroy(&rnpu->cmd_lock);
}

void amdrnpu_dev_cmd_dispatch_response(struct amdrnpu_dev *rnpu, const void *payload,
				       size_t len)
{
	struct amdrnpu_wire_rsp wr;
	struct amdrnpu_cmd *cmd;
	u32 rsp_type;
	u32 rsp_seq;
	int status;

	if (len < sizeof(wr))
		return;

	memcpy(&wr, payload, sizeof(wr));
	rsp_seq = le32_to_cpu(wr.seq);
	rsp_type = le32_to_cpu(wr.command_rsp_type);

	if (rsp_type >= ARRAY_SIZE(amdrnpu_rsp_to_err))
		dev_err_ratelimited(rnpu->dev,
				    "amdrnpu: unexpected response type %u (seq %u); completing -EREMOTEIO\n",
				    rsp_type, rsp_seq);
	status = amdrnpu_rsp_err(rsp_type);

	cmd = amdrnpu_cmd_xa_take(rnpu, rsp_seq);
	if (!cmd) {
		dev_notice_ratelimited(rnpu->dev,
				       "amdrnpu: response for unknown seq %u dropped\n",
				       rsp_seq);
		return;
	}

	amdrnpu_cmd_complete(cmd, status);
}

static int amdrnpu_resolve_args(struct amdrnpu_client *client, struct amdrnpu_cmd *cmd,
				struct amdrnpu_arg *args, u32 num_args,
				__le64 *out_words)
{
	struct amdrnpu_dev *rnpu = client->rnpu;
	u32 i;

	for (i = 0; i < num_args; i++) {
		struct amdrnpu_arg *a = &args[i];
		struct amdrnpu_buf *buf;

		if (a->size == 0) {
			out_words[i] = cpu_to_le64(a->value);
			continue;
		}

		/* Buffer arg: look up the imported handle and bounds-check. */
		if (a->bo.buffer_handle <= 0) {
			RNPU_ERR(rnpu, "arg %u: invalid buffer handle %d\n",
				 i, a->bo.buffer_handle);
			return -EINVAL;
		}
		buf = amdrnpu_buf_get(client, (u32)a->bo.buffer_handle);
		if (!buf) {
			RNPU_ERR(rnpu, "arg %u: unknown buffer handle %d\n",
				 i, a->bo.buffer_handle);
			return -ENOENT;
		}
		if (a->bo.offset > buf->size ||
		    a->size > (u64)(buf->size - a->bo.offset)) {
			RNPU_ERR(rnpu,
				 "arg %u: slice out of bounds (off=%u size=%llu buf=%zu)\n",
				 i, a->bo.offset, (unsigned long long)a->size,
				 buf->size);
			amdrnpu_buf_put(buf);
			return -EINVAL;
		}

		out_words[i] = cpu_to_le64((u64)buf->dma_addr + a->bo.offset);
		/* Transfer the reference from amdrnpu_buf_get() into the cmd. */
		cmd->bo_refs[cmd->num_bo_refs++] = buf;
	}
	return 0;
}

static long amdrnpu_ioctl_cmd_submit(struct amdrnpu_client *client, void __user *uarg)
{
	struct amdrnpu_dev *rnpu = client->rnpu;
	struct sync_file *sync_file = NULL;
	struct amdrnpu_arg *args = NULL;
	struct amdrnpu_cmd_submit uhdr;
	struct dma_fence *fence = NULL;
	struct amdrnpu_cmd *cmd;
	struct amdrnpu_wire_cmd *msg = NULL;
	size_t args_bytes;
	int fd = -1;
	long ret;

	if (copy_from_user(&uhdr, uarg, AMDRNPU_SUBMIT_HDR_SZ))
		return -EFAULT;
	if (uhdr.sync_handle)
		return -EINVAL;
	if (uhdr.num_args > AMDRNPU_MAX_ARGS)
		return -E2BIG;

	args_bytes = (size_t)uhdr.num_args * sizeof(*args);
	if (uhdr.num_args) {
		args = kvzalloc(args_bytes, GFP_KERNEL);
		if (!args)
			return -ENOMEM;
		if (copy_from_user(args,
				   (u8 __user *)uarg + AMDRNPU_SUBMIT_HDR_SZ,
				   args_bytes)) {
			ret = -EFAULT;
			goto out_free_args;
		}
	}

	cmd = amdrnpu_cmd_create(rnpu, uhdr.command_op);
	if (IS_ERR(cmd)) {
		ret = PTR_ERR(cmd);
		goto out_free_args;
	}
	/*
	 * Hold call-scoped references so a response that completes @cmd via
	 * cmd_xa cannot free the command or its fence while we still use them.
	 */
	kref_get(&cmd->ref);
	fence = dma_fence_get(cmd->pending_fence);

	msg = kmalloc(struct_size(msg, args, uhdr.num_args), GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_cancel_cmd;
	}

	if (uhdr.num_args) {
		cmd->bo_refs = kcalloc(uhdr.num_args, sizeof(*cmd->bo_refs),
				       GFP_KERNEL);
		if (!cmd->bo_refs) {
			ret = -ENOMEM;
			goto out_cancel_cmd;
		}
		ret = amdrnpu_resolve_args(client, cmd, args, uhdr.num_args,
					   msg->args);
		if (ret)
			goto out_cancel_cmd;
	}

	ret = amdrnpu_rpmsg_send_cmd(rnpu, msg, cmd->seq, uhdr.command_op, 0,
				     uhdr.num_args);
	if (ret)
		goto out_cancel_cmd;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_cancel_cmd;
	}

	sync_file = sync_file_create(fence);
	if (!sync_file) {
		ret = -ENOMEM;
		goto out_put_fd;
	}

	uhdr.sync_handle = fd;
	if (copy_to_user(uarg, &uhdr, AMDRNPU_SUBMIT_HDR_SZ)) {
		ret = -EFAULT;
		goto out_put_sync_file;
	}

	fd_install(fd, sync_file->file);
	dma_fence_put(fence);
	amdrnpu_cmd_put(cmd);
	kfree(msg);
	kvfree(args);
	return 0;

out_put_sync_file:
	fput(sync_file->file);
out_put_fd:
	put_unused_fd(fd);
out_cancel_cmd:
	/*
	 * If @cmd is still in cmd_xa we won the race and own its completion;
	 * otherwise a response already took it and will complete it.
	 */
	if (amdrnpu_cmd_xa_remove(rnpu, cmd))
		amdrnpu_cmd_complete(cmd, ret < 0 ? (int)ret : -EIO);
	dma_fence_put(fence);
	amdrnpu_cmd_put(cmd);
	kfree(msg);
out_free_args:
	kvfree(args);
	return ret;
}

long amdrnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct amdrnpu_client *client = filp->private_data;
	void __user *uarg = (void __user *)arg;

	if (!client || !client->rnpu)
		return -ENODEV;

	switch (cmd) {
	case AMDRNPU_CMD_SUBMIT:
		return amdrnpu_ioctl_cmd_submit(client, uarg);
	case AMDRNPU_BUF_IMPORT:
		return amdrnpu_ioctl_buf_import(client, uarg);
	case AMDRNPU_BUF_DESTROY:
		return amdrnpu_ioctl_buf_destroy(client, uarg);
	default:
		return -ENOTTY;
	}
}
