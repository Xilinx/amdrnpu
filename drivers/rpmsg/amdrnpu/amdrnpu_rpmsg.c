// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "amdrnpu_internal.h"

static atomic_t amdrnpu_instance_counter = ATOMIC_INIT(0);

int amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *rnpu, struct amdrnpu_wire_cmd *msg,
			   u32 seq, u32 command_op, u32 flags, u32 num_args)
{
	size_t msg_len = struct_size(msg, args, num_args);
	int ret;

	if (msg_len > 4096)
		return -E2BIG;

	msg->seq         = cpu_to_le32(seq);
	msg->command_op  = cpu_to_le32(command_op);
	msg->flags       = cpu_to_le32(flags);
	msg->num_args    = cpu_to_le32(num_args);

	mutex_lock(&rnpu->rpmsg_lock);
	if (!rnpu->rpdev) {
		mutex_unlock(&rnpu->rpmsg_lock);
		RNPU_ERR(rnpu, "cmd submit failed: rpmsg channel not connected\n");
		return -ENOTCONN;
	}
	ret = rpmsg_send(rnpu->rpdev->ept, msg, msg_len);
	mutex_unlock(&rnpu->rpmsg_lock);

	return ret;
}

static int amdrnpu_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
			    int len, void *priv, u32 src)
{
	struct amdrnpu_dev *rnpu = dev_get_drvdata(&rpdev->dev);

	if (!rnpu) {
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

	amdrnpu_dev_cmd_dispatch_response(rnpu, data, (size_t)len);
	return 0;
}

/*
 * Walk up from the rpmsg channel to the first ancestor backed by a device-tree
 * node: the remoteproc-owned platform device (the R5F).  Firmware buffers are
 * attached to this device so the emitted DMA address is the one the RPU sees.
 * Returns NULL on non-DT platforms.
 */
static struct device *amdrnpu_rproc_dev(struct rpmsg_device *rpdev)
{
	struct device *dev;

	for (dev = rpdev->dev.parent; dev; dev = dev->parent) {
		if (dev_of_node(dev))
			return dev;
	}
	return NULL;
}

static int amdrnpu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *rnpu;
	int idx, ret;

	rnpu = kzalloc(sizeof(*rnpu), GFP_KERNEL);
	if (!rnpu)
		return -ENOMEM;

	kref_init(&rnpu->refcount);
	mutex_init(&rnpu->rpmsg_lock);
	amdrnpu_dev_cmd_init(rnpu);
	amdrnpu_dev_heap_init(rnpu);
	rnpu->rpdev = rpdev;

	idx = atomic_fetch_inc(&amdrnpu_instance_counter);
	snprintf(rnpu->dev_name, sizeof(rnpu->dev_name), "%s.%d",
		 AMDRNPU_DRIVER_NAME, idx);

	ret = amdrnpu_chardev_register(rnpu);
	if (ret) {
		dev_err(&rpdev->dev,
			"amdrnpu: misc_register(%s) failed: %d\n",
			rnpu->dev_name, ret);
		amdrnpu_dev_cmd_fini(rnpu);
		amdrnpu_dev_heap_fini(rnpu);
		mutex_destroy(&rnpu->rpmsg_lock);
		kfree(rnpu);
		return ret;
	}

	/*
	 * Hold a reference to the remoteproc device for the lifetime of @rnpu;
	 * released in amdrnpu_dev_release() after all buffer attachments (which
	 * may target this device) have been torn down.
	 */
	rnpu->rproc_dev = amdrnpu_rproc_dev(rpdev);
	if (rnpu->rproc_dev)
		get_device(rnpu->rproc_dev);

	dev_set_drvdata(&rpdev->dev, rnpu);
	RNPU_INFO(rnpu, "bound channel %s -> /dev/%s (ABI v%u)\n",
		  rpdev->id.name, rnpu->dev_name, AMDRNPU_ABI_VERSION);
	return 0;
}

static void amdrnpu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *rnpu = dev_get_drvdata(&rpdev->dev);

	if (!rnpu)
		return;

	RNPU_INFO(rnpu, "removing /dev/%s\n", rnpu->dev_name);
	amdrnpu_chardev_deregister(rnpu);

	mutex_lock(&rnpu->rpmsg_lock);
	rnpu->rpdev = NULL;
	mutex_unlock(&rnpu->rpmsg_lock);

	amdrnpu_dev_cmd_drain(rnpu, -ENOTCONN);

	dev_set_drvdata(&rpdev->dev, NULL);
	amdrnpu_dev_put(rnpu);
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
