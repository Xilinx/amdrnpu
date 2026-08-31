// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "amdrnpu_internal.h"

int amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *rnpu, u32 seq, u32 command_op,
			   u32 flags, const void *args, u32 num_args)
{
	size_t hdr_off = offsetof(struct amdrnpu_wire_cmd, args);
	size_t msg_len = hdr_off + (size_t)num_args * sizeof(__le64);
	struct amdrnpu_wire_cmd *msg;
	int ret;

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

	mutex_lock(&rnpu->rpmsg_lock);
	if (!rnpu->rpdev) {
		mutex_unlock(&rnpu->rpmsg_lock);
		kfree(msg);
		return -ENOTCONN;
	}
	ret = rpmsg_send(rnpu->rpdev->ept, msg, msg_len);
	mutex_unlock(&rnpu->rpmsg_lock);

	kfree(msg);
	return ret;
}

/*
 * Find the DT node of the RPU core that "owns" @rpdev: get the remoteproc that
 * @rpdev is a child of, then take its parent's of_node.
 */
static struct device_node *amdrnpu_rpu_node_of_channel(struct rpmsg_device *rpdev)
{
	struct device_node *core_np;
	struct rproc *rp;

	rp = rproc_get_by_child(&rpdev->dev);
	if (!rp)
		return NULL;

	core_np = rp->dev.parent ? rp->dev.parent->of_node : NULL;
	return core_np;
}

static int amdrnpu_match_rpu(struct device *dev, const void *data)
{
	struct amdrnpu_dev *rnpu = platform_get_drvdata(to_platform_device(dev));
	const struct device_node *rpu_np = data;

	return rnpu && rnpu->ctrl_node == rpu_np;
}

/*
 * Find the amdrnpu platform_device whose "amd,rpu" target (ctrl_node) resolves
 * to @rpu_np, by searching the devices bound to amdrnpu_platform_driver.
 */
static struct platform_device *
amdrnpu_pdev_for_rpu(struct device_node *rpu_np)
{
	struct device *dev;

	if (!rpu_np)
		return NULL;

	dev = driver_find_device(&amdrnpu_platform_driver.driver, NULL,
				 rpu_np, amdrnpu_match_rpu);
	if (!dev)
		return NULL;

	return to_platform_device(dev);
}

static int amdrnpu_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			    void *priv, u32 src)
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

static int amdrnpu_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device_node *rpu_np;
	struct platform_device *pdev;
	struct device_link *link;
	struct amdrnpu_dev *rnpu;

	rpu_np = amdrnpu_rpu_node_of_channel(rpdev);
	if (!rpu_np) {
		dev_err(&rpdev->dev, "cannot find owning RPU DT node\n");
		return -ENODEV;
	}

	pdev = amdrnpu_pdev_for_rpu(rpu_np);
	if (!pdev) {
		/*
		 * Either no DT node references this rpu, or the platform
		 * device for the matching amdrnpu node has not been
		 * instantiated yet.  Defer and retry.
		 */
		dev_dbg(&rpdev->dev,
			"no amdrnpu platform device for rpu %pOFn yet, deferring\n",
			rpu_np);
		return -EPROBE_DEFER;
	}

	rnpu = platform_get_drvdata(pdev);
	if (!rnpu) {
		/* pdev exists but its driver has not finished probing. */
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}

	/*
	 * Hook the rpmsg device's lifetime to the platform device's.
	 * AUTOREMOVE_CONSUMER means: when the supplier (platform device) is
	 * unbound, the consumer (rpdev) is unbound first.  That guarantees
	 * amdrnpu_platform_remove() never sees a live rnpu->rpdev pointer.
	 */
	link = device_link_add(&rpdev->dev, rnpu->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&pdev->dev);
		return -ENOMEM;
	}

	mutex_lock(&rnpu->rpmsg_lock);
	rnpu->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, rnpu);
	mutex_unlock(&rnpu->rpmsg_lock);

	dev_info(rnpu->dev, "amdrnpu: rpmsg channel %s attached (rpu node %pOFn)\n",
		 dev_name(&rpdev->dev), rpu_np);

	put_device(&pdev->dev);
	return 0;
}

static void amdrnpu_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct amdrnpu_dev *rnpu = dev_get_drvdata(&rpdev->dev);

	if (!rnpu)
		return;

	mutex_lock(&rnpu->rpmsg_lock);
	if (rnpu->rpdev == rpdev)
		rnpu->rpdev = NULL;
	dev_set_drvdata(&rpdev->dev, NULL);
	mutex_unlock(&rnpu->rpmsg_lock);

	amdrnpu_dev_cmd_drain(rnpu, -ENOTCONN);

	dev_info(rnpu->dev, "amdrnpu: rpmsg channel %s detached\n",
		 dev_name(&rpdev->dev));
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
