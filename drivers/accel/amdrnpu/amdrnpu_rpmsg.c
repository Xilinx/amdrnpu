// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "amdrnpu_internal.h"

static atomic_t amdrnpu_instance_counter = ATOMIC_INIT(0);

int amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *dev, u32 seq, u32 command_op,
			   u32 flags, const void *args, u32 num_args)
{
	struct amdrnpu_wire_cmd *msg;
	size_t hdr_off;
	size_t msg_len;
	int ret;

	hdr_off = offsetof(struct amdrnpu_wire_cmd, args);
	msg_len = hdr_off + (size_t)num_args * sizeof(__le64);

	if (msg_len > 4096)
		return -E2BIG;

	msg = kmalloc(msg_len, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->seq         = cpu_to_le32(seq);
	msg->command_op  = cpu_to_le32(command_op);
	msg->flags       = cpu_to_le32(flags);
	msg->num_args    = cpu_to_le32(num_args);
	if (num_args)
		memcpy(msg->args, args, num_args * sizeof(__le64));

	mutex_lock(&dev->rpmsg_lock);
	if (!dev->rpdev) {
		mutex_unlock(&dev->rpmsg_lock);
		dev_err(dev->dev, "cmd submit failed: rpmsg channel not connected\n");
		kfree(msg);
		return -ENOTCONN;
	}
	ret = rpmsg_send(dev->rpdev->ept, msg, msg_len);
	mutex_unlock(&dev->rpmsg_lock);

	kfree(msg);
	return ret;
}

static int amdrnpu_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
			    int len, void *priv, u32 src)
{
	struct amdrnpu_dev *dev = dev_get_drvdata(&rpdev->dev);

	if (!dev) {
		/* Channel was unbound while a callback was racing in. */
		dev_warn_ratelimited(&rpdev->dev,
				     "amdrnpu: rpmsg msg dropped: channel not bound\n");
		return 0;
	}

	if ((size_t)len < sizeof(struct amdrnpu_wire_rsp)) {
		dev_warn_ratelimited(&rpdev->dev,
				     "amdrnpu: rpmsg msg too small (%d bytes, need %zu), dropping\n",
				     len, sizeof(struct amdrnpu_wire_rsp));
		return 0;
	}

	amdrnpu_dev_cmd_dispatch_response(dev, data, (size_t)len);
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
	mutex_init(&dev->rpmsg_lock);
	amdrnpu_dev_cmd_init(dev);
	dev->rpdev = rpdev;

	idx = atomic_fetch_inc(&amdrnpu_instance_counter);
	snprintf(dev->dev_name, sizeof(dev->dev_name), "%s.%d",
		 AMDRNPU_DRIVER_NAME, idx);

	ret = amdrnpu_chardev_register(dev);
	if (ret) {
		dev_err(&rpdev->dev,
			"amdrnpu: misc_register(%s) failed: %d\n",
			dev->dev_name, ret);
		amdrnpu_dev_cmd_fini(dev);
		mutex_destroy(&dev->rpmsg_lock);
		kfree(dev);
		return ret;
	}

	dev->dev = dev->mdev.this_device;
	dev_set_drvdata(&rpdev->dev, dev);
	dev_info(&rpdev->dev,
		 "amdrnpu: bound channel %s -> /dev/%s (ABI v%u)\n",
		 rpdev->id.name, dev->dev_name, AMDRNPU_ABI_VERSION);
	return 0;
}

static void amdrnpu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *dev = dev_get_drvdata(&rpdev->dev);

	if (!dev)
		return;

	dev_info(&rpdev->dev, "amdrnpu: removing /dev/%s\n", dev->dev_name);
	amdrnpu_chardev_deregister(dev);

	mutex_lock(&dev->rpmsg_lock);
	dev->rpdev = NULL;
	mutex_unlock(&dev->rpmsg_lock);

	amdrnpu_dev_cmd_drain(dev, -ENOTCONN);

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
