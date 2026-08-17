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
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

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

static struct dma_fence *amdrnpu_fence_create(struct amdrnpu_dev *dev)
{
	struct amdrnpu_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &amdrnpu_fence_ops, &f->lock,
		       dev->fence_context,
		       atomic_inc_return(&dev->fence_seqno));
	return &f->base;
}

void amdrnpu_dev_cmd_init(struct amdrnpu_dev *dev)
{
	spin_lock_init(&dev->cmd_lock);
	dev->pending_fence = NULL;
	dev->fence_context = dma_fence_context_alloc(1);
	atomic_set(&dev->fence_seqno, 0);
}

static struct dma_fence *amdrnpu_dev_cmd_take(struct amdrnpu_dev *dev)
{
	struct dma_fence *f;
	unsigned long flags;

	spin_lock_irqsave(&dev->cmd_lock, flags);
	f = dev->pending_fence;
	dev->pending_fence = NULL;
	spin_unlock_irqrestore(&dev->cmd_lock, flags);
	return f;
}

static void amdrnpu_cmd_signal(struct dma_fence *fence, int err)
{
	if (!fence)
		return;
	if (err)
		dma_fence_set_error(fence, err);
	dma_fence_signal(fence);
	dma_fence_put(fence);
}

/* Complete the in-flight command (if any) with @err (0 = success). */
static void amdrnpu_dev_cmd_complete(struct amdrnpu_dev *dev, int err)
{
	amdrnpu_cmd_signal(amdrnpu_dev_cmd_take(dev), err);
}

void amdrnpu_dev_cmd_abort(struct amdrnpu_dev *dev, int err)
{
	if (!err)
		err = -ENOTCONN;
	amdrnpu_dev_cmd_complete(dev, err);
}

void amdrnpu_dev_cmd_fini(struct amdrnpu_dev *dev)
{
	amdrnpu_dev_cmd_complete(dev, -ENODEV);
}

void amdrnpu_dev_cmd_dispatch_response(struct amdrnpu_dev *dev, const void *payload,
				   size_t len)
{
	struct amdrnpu_wire_rsp wr;
	u32 rsp_type;
	int err;

	if (len < sizeof(wr))
		return;

	memcpy(&wr, payload, sizeof(wr));
	rsp_type = le32_to_cpu(wr.command_rsp_type);
	err = amdrnpu_rsp_err(rsp_type);
	amdrnpu_dev_cmd_complete(dev, err);
}

static int amdrnpu_resolve_args(struct amdrnpu_dev *dev, struct amdrnpu_arg *args,
				u32 num_args, __le64 *out_words)
{
	u32 i;

	for (i = 0; i < num_args; i++) {
		struct amdrnpu_arg *a = &args[i];

		/* Buffer args (size != 0) are not implemented */
		if (a->size != 0) {
			dev_dbg(dev->dev,
				"arg %u: buffer args not supported yet (size=%llu)\n",
				i, (unsigned long long)a->size);
			return -EINVAL;
		}
		out_words[i] = cpu_to_le64(a->value);
	}
	return 0;
}

static long amdrnpu_ioctl_cmd_submit(struct amdrnpu_dev *dev, void __user *uarg)
{
	struct sync_file *sync_file = NULL;
	struct amdrnpu_arg *args = NULL;
	struct amdrnpu_cmd_submit uhdr;
	struct dma_fence *fence = NULL;
	bool published = false;
	__le64 *words = NULL;
	unsigned long flags;
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
		words = kvcalloc(uhdr.num_args, sizeof(*words), GFP_KERNEL);
		if (!words) {
			ret = -ENOMEM;
			goto out_free_args;
		}
		ret = amdrnpu_resolve_args(dev, args, uhdr.num_args, words);
		if (ret)
			goto out_free_words;
	}

	/* Local reference keeps @fence alive for the whole call. */
	fence = amdrnpu_fence_create(dev);
	if (!fence) {
		ret = -ENOMEM;
		goto out_free_words;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto out_unpublish;
	}

	sync_file = sync_file_create(fence);
	if (!sync_file) {
		ret = -ENOMEM;
		goto out_unpublish;
	}

	uhdr.sync_handle = fd;
	if (copy_to_user(uarg, &uhdr, AMDRNPU_SUBMIT_HDR_SZ)) {
		ret = -EFAULT;
		goto out_unpublish;
	}

	/* Reserve the single in-flight slot; multiple in-flight comes next commit. */
	spin_lock_irqsave(&dev->cmd_lock, flags);
	if (dev->pending_fence) {
		spin_unlock_irqrestore(&dev->cmd_lock, flags);
		ret = -EBUSY;
		goto out_unpublish;
	}
	dev->pending_fence = fence;
	spin_unlock_irqrestore(&dev->cmd_lock, flags);	/* unlock right after publish */
	published = true;

	ret = amdrnpu_rpmsg_send_cmd(dev, uhdr.command_op, 0, words,
				     uhdr.num_args);
	if (ret)
		goto out_unpublish;

	fd_install(fd, sync_file->file);
	kvfree(words);
	kvfree(args);
	return 0;

out_unpublish:
	spin_lock_irqsave(&dev->cmd_lock, flags);
	if (dev->pending_fence == fence) {
		dev->pending_fence = NULL;
		published = false;
	}
	spin_unlock_irqrestore(&dev->cmd_lock, flags);
	if (!published)
		dma_fence_put(fence);

	if (sync_file)
		fput(sync_file->file);
	if (fd >= 0)
		put_unused_fd(fd);
out_free_words:
	kvfree(words);
out_free_args:
	kvfree(args);
	return ret;
}

long amdrnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct amdrnpu_dev *dev = filp->private_data;

	if (!dev)
		return -ENODEV;

	switch (cmd) {
	case AMDRNPU_CMD_SUBMIT:
		return amdrnpu_ioctl_cmd_submit(dev, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}
