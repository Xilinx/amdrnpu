// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "amdrnpu_internal.h"

/*
 * Monotonic instance counter for naming /dev/amdrnpu.N.  Only ever
 * incremented so disconnect/reconnect of the same remoteproc still gets a
 * fresh minor name (avoids stale-fd confusion in userspace).
 */
static atomic_t amdrnpu_instance_counter = ATOMIC_INIT(0);

int amdrnpu_rpmsg_send(struct amdrnpu_dev *dev, const void *data, size_t len)
{
	struct rpmsg_device *rpdev;
	int ret;

	rcu_read_lock();
	rpdev = rcu_dereference(dev->rpdev);
	if (!rpdev) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = rpmsg_send(rpdev->ept, (void *)data, len);
	rcu_read_unlock();
	return ret;
}

static int amdrnpu_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
			    int len, void *priv, u32 src)
{
	struct amdrnpu_dev *dev = dev_get_drvdata(&rpdev->dev);
	struct amdrnpu_msg *msg;

	if (!dev)
		return -ENODEV;

	if (len <= 0 || len > AMDRNPU_MAX_PAYLOAD) {
		dev_warn(&rpdev->dev,
			 "amdrnpu: dropping rx frame: len=%d (max=%u)\n",
			 len, AMDRNPU_MAX_PAYLOAD);
		return -EINVAL;
	}

	msg = kzalloc(sizeof(*msg), GFP_ATOMIC);
	if (!msg)
		return -ENOMEM;

	msg->len = (__u32)len;
	memcpy(msg->data, data, len);

	mutex_lock(&dev->lock);
	if (kfifo_is_full(&dev->rxfifo)) {
		mutex_unlock(&dev->lock);
		dev_warn_ratelimited(&rpdev->dev,
				     "amdrnpu: rx fifo full, dropping frame\n");
		kfree(msg);
		return -ENOSPC;
	}
	kfifo_in(&dev->rxfifo, msg, 1);
	mutex_unlock(&dev->lock);

	wake_up_interruptible(&dev->rxwq);
	kfree(msg);
	return 0;
}

static int amdrnpu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *dev;
	int idx, ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->refcount);
	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->rxwq);
	INIT_KFIFO(dev->rxfifo);
	rcu_assign_pointer(dev->rpdev, rpdev);

	idx = atomic_fetch_inc(&amdrnpu_instance_counter);
	snprintf(dev->dev_name, sizeof(dev->dev_name), "%s.%d",
		 AMDRNPU_DEVICE_NAME, idx);

	ret = amdrnpu_chardev_register(dev);
	if (ret) {
		dev_err(&rpdev->dev,
			"amdrnpu: misc_register(%s) failed: %d\n",
			dev->dev_name, ret);
		mutex_destroy(&dev->lock);
		kfree(dev);
		return ret;
	}

	dev_set_drvdata(&rpdev->dev, dev);
	dev_info(&rpdev->dev,
		 "amdrnpu: bound channel %s -> /dev/%s (ABI v%u, max payload %u)\n",
		 rpdev->id.name, dev->dev_name, AMDRNPU_ABI_VERSION,
		 AMDRNPU_MAX_PAYLOAD);
	return 0;
}

static void amdrnpu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *dev = dev_get_drvdata(&rpdev->dev);

	if (!dev)
		return;

	dev_info(&rpdev->dev, "amdrnpu: removing /dev/%s\n", dev->dev_name);
	amdrnpu_chardev_deregister(dev);

	/*
	 * Fence the send path: after synchronize_rcu() returns, no caller is
	 * inside rcu_read_lock() with a stale rpdev, so the rpmsg core can tear
	 * the channel down without UAF risk.  Then wake any blocked RECV
	 * waiters so they observe -ENODEV via the !rcu_access_pointer() check.
	 */
	rcu_assign_pointer(dev->rpdev, NULL);
	synchronize_rcu();
	wake_up_interruptible_all(&dev->rxwq);

	dev_set_drvdata(&rpdev->dev, NULL);
	amdrnpu_dev_put(dev);
}

static const struct rpmsg_device_id amdrnpu_rpmsg_id_table[] = {
	{ .name = AMDRNPU_RPMSG_NAME },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, amdrnpu_rpmsg_id_table);

static struct rpmsg_driver amdrnpu_rpmsg_driver = {
	.drv = {
		.name	= "amdrnpu_rpmsg",
	},
	.id_table	= amdrnpu_rpmsg_id_table,
	.probe		= amdrnpu_rpmsg_probe,
	.remove		= amdrnpu_rpmsg_remove,
	.callback	= amdrnpu_rpmsg_cb,
};

int amdrnpu_rpmsg_register(void)
{
	return register_rpmsg_driver(&amdrnpu_rpmsg_driver);
}

void amdrnpu_rpmsg_unregister(void)
{
	unregister_rpmsg_driver(&amdrnpu_rpmsg_driver);
}
