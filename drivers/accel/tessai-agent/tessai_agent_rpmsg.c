// SPDX-License-Identifier: GPL-2.0
/*
 * tessai-agent - rpmsg transport.
 *
 * The driver supports an arbitrary number of tessai-agent instances,
 * one per "amd,tessai-agent" device-tree node.  Each instance points
 * (via the "amd,remoteproc" phandle) at the specific remoteproc that
 * runs its RPU firmware; the firmware in turn advertises an rpmsg
 * channel named TESSAI_RPMSG_NAME.
 *
 * Routing - rpmsg channel -> tessai_dev:
 *
 *   1. Walk the rpmsg device's parent chain to find the underlying
 *      remoteproc DT node (the first ancestor with an of_node).
 *   2. Walk every "amd,tessai-agent" DT node and pick the one whose
 *      "amd,remoteproc" phandle resolves to that rproc node.
 *   3. Fetch the platform_device for that DT node via
 *      of_find_device_by_node().
 *   4. If the platform device hasn't probed yet
 *      (platform_get_drvdata() == NULL), return -EPROBE_DEFER so the
 *      driver core retries us later.  Once the matching tessai_dev is
 *      ready, we install a managed device_link from the rpmsg device
 *      (consumer) to the platform device (supplier), so the driver
 *      core also guarantees that:
 *
 *        - we won't be probed before the platform side, and
 *        - on platform unbind, our rpmsg_remove() runs first.
 *
 * The fast path uses dev_get_drvdata(&rpdev->dev) to recover the
 * tessai_dev pointer in O(1) inside the rpmsg callback.  No global
 * registry, no pending list, no extra locks beyond tdev->rpmsg_lock.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#include "tessai_agent_internal.h"
#include "tessai_agent_trace.h"

/* --------------------------------------------------------------- */
/* DT-based routing                                                */
/* --------------------------------------------------------------- */

/*
 * Find the device-tree node of the remoteproc that "owns" rpdev.
 * Typical hierarchy (rpmsg-virtio + remoteproc):
 *
 *     rpdev->dev                (no of_node)
 *       parent: virtio_rpmsg / virtio_device   (no of_node)
 *         parent: rproc->dev                   (no of_node)
 *           parent: platform_device for the R5F (HAS of_node)
 *
 * Walk up until we find the first device with an of_node attached -
 * that is the DT node referenced by "amd,remoteproc" in our binding.
 */
static struct device_node *tessai_rpdev_owner_np(struct rpmsg_device *rpdev)
{
	struct device *dev;

	for (dev = rpdev->dev.parent; dev; dev = dev->parent) {
		struct device_node *np = dev_of_node(dev);

		if (np)
			return np;
	}
	return NULL;
}

/*
 * Find the platform_device whose DT node has compatible
 * "amd,tessai-agent" and whose "amd,remoteproc" phandle resolves to
 * @rproc_np.
 *
 * If several DT nodes reference the same remoteproc phandle (e.g. a
 * leftover stub ahead of the real overlay in preorder), probing on the
 * *first* match would attach RPMsg to a tessai_dev that never receives the
 * DRM ioctls, while userspace submits commands on a different instance —
 * completions then show up as “unknown cmd id” on the RPMsg-bound tdev.
 *
 * Prefer matching nodes whose platform probe has finished
 * (platform_get_drvdata(pdev) != NULL).  When multiple qualify, take the
 * *last* in compatible-node preorder so DT fragments appended later win.
 *
 * Caller must put_device(&pdev->dev) on the returned pdev.
 */
static struct platform_device *
tessai_pdev_for_rproc(struct device_node *rproc_np)
{
	struct device_node *np;
	struct platform_device *pdev = NULL;
	struct platform_device *chosen = NULL;
	int dup = 0;

	if (!rproc_np)
		return NULL;

	for_each_compatible_node(np, NULL, "amd,tessai-agent") {
		struct device_node *peer;

		peer = of_parse_phandle(np, "amd,remoteproc", 0);
		if (!peer)
			continue;
		if (peer != rproc_np) {
			of_node_put(peer);
			continue;
		}
		of_node_put(peer);

		pdev = of_find_device_by_node(np);
		if (!pdev)
			continue;
		if (!platform_get_drvdata(pdev)) {
			put_device(&pdev->dev);
			continue;
		}

		dup++;
		if (chosen)
			put_device(&chosen->dev);
		chosen = pdev;
	}

	if (!chosen)
		return NULL;

	if (dup > 1)
		dev_warn_once(&chosen->dev,
			      "multiple amd,tessai-agent nodes reference rproc node %pOFn; RPMsg bound to DT-last probed instance (fix duplicate nodes)\n",
			      rproc_np);

	return chosen;
}

/* --------------------------------------------------------------- */
/* RPMsg driver                                                    */
/* --------------------------------------------------------------- */

static int tessai_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			   void *priv, u32 src)
{
	struct tessai_dev *tdev = dev_get_drvdata(&rpdev->dev);

	trace_tessai_rpmsg_cb_enter(len, src);

	if (!tdev) {
		/* Channel was unbound while a callback was racing in. */
		dev_warn_ratelimited(&rpdev->dev,
				     "rpmsg msg dropped: channel not bound\n");
		trace_tessai_rpmsg_cb_exit(0);
		return 0;
	}

	if ((size_t)len < sizeof(struct tessai_wire_rsp)) {
		dev_warn(tdev->dev,
			 "rpmsg msg too small (%d bytes, need packed rsp hdr %zu), dropping\n",
			 len, sizeof(struct tessai_wire_rsp));
		trace_tessai_rpmsg_cb_exit(0);
		return 0;
	}

	tessai_cmd_dispatch_response(tdev, data, (size_t)len);
	trace_tessai_rpmsg_cb_exit(0);
	return 0;
}

static int tessai_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device_node *rproc_np;
	struct platform_device *pdev;
	struct tessai_dev *tdev;
	struct device_link *link;

	rproc_np = tessai_rpdev_owner_np(rpdev);
	if (!rproc_np) {
		dev_err(&rpdev->dev, "cannot find owning remoteproc DT node\n");
		return -ENODEV;
	}

	pdev = tessai_pdev_for_rproc(rproc_np);
	if (!pdev) {
		/*
		 * Either no DT node references this rproc, or the
		 * platform device for the matching tessai-agent node has
		 * not been instantiated yet.  Defer and retry.
		 */
		dev_dbg(&rpdev->dev,
			"no tessai-agent platform device for rproc %pOFn yet, deferring\n",
			rproc_np);
		return -EPROBE_DEFER;
	}

	tdev = platform_get_drvdata(pdev);
	if (!tdev) {
		/* pdev exists but its driver has not finished probing. */
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}

	/*
	 * Hook the rpmsg device's lifetime to the platform device's.
	 * AUTOREMOVE_CONSUMER means: when the supplier (platform device)
	 * is unbound, the consumer (rpdev) is unbound first.  This is
	 * what guarantees tessai_platform_remove() never sees a live
	 * tdev->rpdev pointer.
	 */
	link = device_link_add(&rpdev->dev, tdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&pdev->dev);
		return -ENOMEM;
	}

	mutex_lock(&tdev->rpmsg_lock);
	tdev->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, tdev);
	mutex_unlock(&tdev->rpmsg_lock);

	dev_info(tdev->dev,
		 "rpmsg channel %s attached (rproc node %pOFn)\n",
		 dev_name(&rpdev->dev), rproc_np);

	put_device(&pdev->dev);
	return 0;
}

static void tessai_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct tessai_dev *tdev = dev_get_drvdata(&rpdev->dev);

	if (!tdev)
		return;

	mutex_lock(&tdev->rpmsg_lock);
	if (tdev->rpdev == rpdev)
		tdev->rpdev = NULL;
	dev_set_drvdata(&rpdev->dev, NULL);
	mutex_unlock(&tdev->rpmsg_lock);

	tessai_cmd_drain_inflight_rpmsg_detach(tdev, -ENOTCONN);

	dev_info(tdev->dev, "rpmsg channel %s detached\n",
		 dev_name(&rpdev->dev));
}

static const struct rpmsg_device_id tessai_rpmsg_id_table[] = {
	{ .name = TESSAI_RPMSG_NAME },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, tessai_rpmsg_id_table);

static struct rpmsg_driver tessai_rpmsg_driver = {
	.drv.name	= "tessai_agent_rpmsg",
	.id_table	= tessai_rpmsg_id_table,
	.probe		= tessai_rpmsg_probe,
	.callback	= tessai_rpmsg_cb,
	.remove		= tessai_rpmsg_remove,
};

int tessai_rpmsg_register(void)
{
	return register_rpmsg_driver(&tessai_rpmsg_driver);
}

void tessai_rpmsg_unregister(void)
{
	unregister_rpmsg_driver(&tessai_rpmsg_driver);
}

/* --------------------------------------------------------------- */
/* Send                                                            */
/* --------------------------------------------------------------- */

int tessai_rpmsg_send_cmd(struct tessai_dev *tdev, u32 seq,
			  u32 command_op, u32 flags, const __le64 *args,
			u32 num_args)
{
	struct rpmsg_device *rpdev;
	struct tessai_wire_cmd *msg;
	size_t hdr_off = offsetof(struct tessai_wire_cmd, args);
	size_t msg_len = hdr_off + (size_t)num_args * sizeof(__le64);
	int ret;

	if (msg_len > 4096)
		return -E2BIG;

	mutex_lock(&tdev->rpmsg_lock);
	rpdev = tdev->rpdev;
	if (!rpdev) {
		mutex_unlock(&tdev->rpmsg_lock);
		return -ENOTCONN;
	}

	msg = kmalloc(msg_len, GFP_KERNEL);
	if (!msg) {
		mutex_unlock(&tdev->rpmsg_lock);
		return -ENOMEM;
	}

	msg->seq         = cpu_to_le32(seq);
	msg->command_op  = cpu_to_le32(command_op);
	msg->flags       = cpu_to_le32(flags);
	msg->num_args    = cpu_to_le32(num_args);
	if (num_args)
		memcpy(msg->args, args, num_args * sizeof(__le64));

	trace_tessai_rpmsg_send_enter(seq, command_op, num_args);
	ret = rpmsg_send(rpdev->ept, msg, msg_len);
	trace_tessai_rpmsg_send_exit(seq, ret);
	mutex_unlock(&tdev->rpmsg_lock);

	kfree(msg);
	return ret;
}
