// SPDX-License-Identifier: GPL-2.0
/*
 * tessai-agent - top-level driver registration.
 *
 * The driver registers three kernel-level handlers:
 *
 *   1. A platform_driver matched against compatible "amd,tessai-agent".
 *      Its probe publishes drvdata, then calls devm_of_platform_populate()
 *      which causes the bank platform_driver (#2) to bind each bank child
 *      synchronously.  After both banks are present, the DRM render node
 *      is registered.
 *
 *   2. A bank platform_driver matched against the single compatible
 *      "amd,tessai-agent-bank".  The two bank children are distinguished by
 *      the logical-index "reg" cell (0 = RPU bank / TESSAI_MEMORY_RPU, 1 = dev
 *      bank / TESSAI_MEMORY_DEVICE), which is also the slot index into
 *      tessai_dev->banks[].  Each probe owns one bank's reserved-memory and
 *      dma-mask.  Registered from tessai_bank_driver_register() in
 *      tessai_agent_bank.c so that it is available before populate runs.
 *
 *   3. An rpmsg_driver matched against the channel name TESSAI_RPMSG_NAME.
 *      The rpmsg probe arrives independently of the platform probe; the
 *      two are linked via the small registry in tessai_agent_rpmsg.c.
 *
 * Both DRM and RPMsg handlers are tolerant of arrival order: the DRM render
 * node from the platform device stays registered regardless of RPMsg; buffer
 * allocation works even when no channel is attached.  Command submission
 * fails with -ENOTCONN until rpmsg binds, and every in-flight command is
 * completed with -ENOTCONN on the syncobj dma_fence when the channel is torn
 * down.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>

#include <drm/drm_drv.h>

#include "tessai_agent_internal.h"

#define CREATE_TRACE_POINTS
#include "tessai_agent_trace.h"

static int tessai_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *rproc_np;
	struct tessai_dev *tdev;
	int ret;

	rproc_np = of_parse_phandle(dev->of_node, "amd,remoteproc", 0);
	if (!rproc_np) {
		dev_err(dev, "missing 'amd,remoteproc' phandle\n");
		return -EINVAL;
	}

	tdev = devm_drm_dev_alloc(dev, &tessai_drm_driver, struct tessai_dev, drm);
	if (IS_ERR(tdev)) {
		of_node_put(rproc_np);
		return PTR_ERR(tdev);
	}

	tdev->linked_rproc = rproc_np;
	tdev->dev = dev;
	mutex_init(&tdev->rpmsg_lock);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		dev_warn(dev, "64-bit DMA mask on primary device failed: %d\n", ret);

	ret = tessai_cmd_init(tdev);
	if (ret) {
		of_node_put(tdev->linked_rproc);
		tdev->linked_rproc = NULL;
		return ret;
	}

	/*
	 * Publish drvdata before populating children: the bank platform_driver
	 * looks it up via dev_get_drvdata(parent) during its synchronous probe
	 * triggered by devm_of_platform_populate().
	 */
	platform_set_drvdata(pdev, tdev);

	ret = devm_of_platform_populate(dev);
	if (ret) {
		dev_err(dev, "devm_of_platform_populate failed (%d)\n", ret);
		goto err_cmd_fini;
	}

	ret = tessai_bank_check_all_present(tdev);
	if (ret)
		goto err_cmd_fini;

	ret = tessai_drm_register(tdev);
	if (ret)
		goto err_cmd_fini;

	dev_info(dev, "tessai-agent ready\n");
	return 0;

err_cmd_fini:
	platform_set_drvdata(pdev, NULL);
	tessai_cmd_fini(tdev);
	if (tdev->linked_rproc) {
		of_node_put(tdev->linked_rproc);
		tdev->linked_rproc = NULL;
	}
	return ret;
}

static void tessai_platform_remove(struct platform_device *pdev)
{
	struct tessai_dev *tdev = platform_get_drvdata(pdev);

	/*
	 * Children created by devm_of_platform_populate() are torn down
	 * automatically; bank devm resources release on that path.
	 *
	 * If an rpmsg channel is still bound to us, the device_link
	 * created at rpmsg-probe time guarantees the rpmsg consumer is
	 * unbound *before* we get here, so tdev->rpdev is already NULL.
	 */
	tessai_drm_unregister(tdev);
	tessai_cmd_fini(tdev);
	of_node_put(tdev->linked_rproc);
	tdev->linked_rproc = NULL;
}

static const struct of_device_id tessai_of_match[] = {
	{ .compatible = "amd,tessai-agent" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tessai_of_match);

static struct platform_driver tessai_platform_driver = {
	.probe	= tessai_platform_probe,
	.remove = tessai_platform_remove,
	.driver	= {
		.name		= TESSAI_DRV_NAME,
		.of_match_table	= tessai_of_match,
	},
};

static int __init tessai_module_init(void)
{
	int ret;

	/* Register the bank driver first so populate can match its compatibles. */
	ret = tessai_bank_driver_register();
	if (ret)
		return ret;

	ret = platform_driver_register(&tessai_platform_driver);
	if (ret) {
		tessai_bank_driver_unregister();
		return ret;
	}

	ret = tessai_rpmsg_register();
	if (ret) {
		platform_driver_unregister(&tessai_platform_driver);
		tessai_bank_driver_unregister();
		return ret;
	}
	return 0;
}

static void __exit tessai_module_exit(void)
{
	tessai_rpmsg_unregister();
	platform_driver_unregister(&tessai_platform_driver);
	tessai_bank_driver_unregister();
}

module_init(tessai_module_init);
module_exit(tessai_module_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD tessai RPU management DRM/RPMsg driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
/*
 * dma_buf_* helpers were moved into the DMA_BUF symbol namespace in
 * Linux 6.6; importing it here keeps modpost happy on 6.12 and newer.
 * The namespace argument became a string literal in Linux 6.13.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
