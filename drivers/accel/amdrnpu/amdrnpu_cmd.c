// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "amdrnpu_internal.h"

static long amdrnpu_ioctl_get_info(struct amdrnpu_dev *dev, unsigned long arg)
{
	struct amdrnpu_info info = {
		.abi_version = AMDRNPU_ABI_VERSION,
		.max_payload = AMDRNPU_MAX_PAYLOAD,
	};

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long amdrnpu_ioctl_send(struct amdrnpu_dev *dev, unsigned long arg)
{
	struct amdrnpu_msg *msg;
	long ret = 0;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	if (copy_from_user(msg, (void __user *)arg, sizeof(*msg))) {
		ret = -EFAULT;
		goto out;
	}

	if (msg->flags || msg->len == 0 || msg->len > AMDRNPU_MAX_PAYLOAD) {
		ret = -EINVAL;
		goto out;
	}

	ret = amdrnpu_rpmsg_send(dev, msg->data, msg->len);

out:
	kfree(msg);
	return ret;
}

static long amdrnpu_ioctl_recv(struct amdrnpu_dev *dev, struct file *filp,
			       unsigned long arg)
{
	struct amdrnpu_msg *msg;
	long ret = 0;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (;;) {
		mutex_lock(&dev->lock);
		if (kfifo_out(&dev->rxfifo, msg, 1) == 1) {
			mutex_unlock(&dev->lock);
			break;
		}
		mutex_unlock(&dev->lock);

		if (!rcu_access_pointer(dev->rpdev)) {
			ret = -ENODEV;
			goto out;
		}

		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(dev->rxwq,
					     !kfifo_is_empty(&dev->rxfifo) ||
					     !rcu_access_pointer(dev->rpdev))) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	if (copy_to_user((void __user *)arg, msg, sizeof(*msg)))
		ret = -EFAULT;

out:
	kfree(msg);
	return ret;
}

long amdrnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct amdrnpu_dev *dev = filp->private_data;

	if (!dev)
		return -ENODEV;

	switch (cmd) {
	case AMDRNPU_IOC_GET_INFO:
		return amdrnpu_ioctl_get_info(dev, arg);
	case AMDRNPU_IOC_SEND:
		return amdrnpu_ioctl_send(dev, arg);
	case AMDRNPU_IOC_RECV:
		return amdrnpu_ioctl_recv(dev, filp, arg);
	case AMDRNPU_IOC_RESET:
		mutex_lock(&dev->lock);
		kfifo_reset(&dev->rxfifo);
		mutex_unlock(&dev->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}
