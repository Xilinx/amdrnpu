// SPDX-License-Identifier: GPL-2.0
/*
 * amdrnpu - top-level driver registration.
 *
 * The driver registers three kernel-level handlers:
 *
 *   1. A platform_driver matched against compatible "amd,amdrnpu".
 *      Its probe publishes drvdata, then calls devm_of_platform_populate()
 *      which causes the bank platform_driver (#2) to bind each bank child
 *      synchronously.  After both banks are present, the DRM render node
 *      is registered.
 *
 *   2. A bank platform_driver matched against the single compatible
 *      "amd,amdrnpu-bank".  The two bank children are distinguished by
 *      the logical-index "reg" cell (0 = RPU bank / AMDRNPU_MEMORY_RPU, 1 = dev
 *      bank / AMDRNPU_MEMORY_DEVICE), which is also the slot index into
 *      amdrnpu_dev->banks[].  Each probe owns one bank's reserved-memory and
 *      dma-mask.  Registered from amdrnpu_bank_driver_register() in
 *      amdrnpu_bank.c so that it is available before populate runs.
 *
 *   3. An rpmsg_driver matched against the channel name AMDRNPU_RPMSG_NAME.
 *      The rpmsg probe arrives independently of the platform probe; the
 *      two are linked via the small registry in amdrnpu_rpmsg.c.
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

#include "amdrnpu_internal.h"

#define CREATE_TRACE_POINTS
#include "amdrnpu_trace.h"

static int amdrnpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *rproc_np;
	struct amdrnpu_dev *tdev;
	int ret;

	rproc_np = of_parse_phandle(dev->of_node, "amd,remoteproc", 0);
	if (!rproc_np) {
		dev_err(dev, "missing 'amd,remoteproc' phandle\n");
		return -EINVAL;
	}

	tdev = devm_drm_dev_alloc(dev, &amdrnpu_drm_driver, struct amdrnpu_dev, drm);
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

	ret = amdrnpu_cmd_init(tdev);
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

	ret = amdrnpu_bank_check_all_present(tdev);
	if (ret)
		goto err_cmd_fini;

	ret = amdrnpu_drm_register(tdev);
	if (ret)
		goto err_cmd_fini;

	dev_info(dev, "amdrnpu ready\n");
	return 0;

err_cmd_fini:
	platform_set_drvdata(pdev, NULL);
	amdrnpu_cmd_fini(tdev);
	if (tdev->linked_rproc) {
		of_node_put(tdev->linked_rproc);
		tdev->linked_rproc = NULL;
	}
	return ret;
}

static void amdrnpu_platform_remove(struct platform_device *pdev)
{
	struct amdrnpu_dev *tdev = platform_get_drvdata(pdev);

	/*
	 * Children created by devm_of_platform_populate() are torn down
	 * automatically; bank devm resources release on that path.
	 *
	 * If an rpmsg channel is still bound to us, the device_link
	 * created at rpmsg-probe time guarantees the rpmsg consumer is
	 * unbound *before* we get here, so tdev->rpdev is already NULL.
	 */
	amdrnpu_drm_unregister(tdev);
	amdrnpu_cmd_fini(tdev);
	of_node_put(tdev->linked_rproc);
	tdev->linked_rproc = NULL;
}

static const struct of_device_id amdrnpu_of_match[] = {
	{ .compatible = "amd,amdrnpu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amdrnpu_of_match);

static struct platform_driver amdrnpu_platform_driver = {
	.probe	= amdrnpu_platform_probe,
	.remove = amdrnpu_platform_remove,
	.driver	= {
		.name		= AMDRNPU_DRV_NAME,
		.of_match_table	= amdrnpu_of_match,
	},
};

static int __init amdrnpu_module_init(void)
{
	int ret;

	/* Register the bank driver first so populate can match its compatibles. */
	ret = amdrnpu_bank_driver_register();
	if (ret)
		return ret;

	ret = platform_driver_register(&amdrnpu_platform_driver);
	if (ret) {
		amdrnpu_bank_driver_unregister();
		return ret;
	}

	ret = amdrnpu_rpmsg_register();
	if (ret) {
		platform_driver_unregister(&amdrnpu_platform_driver);
		amdrnpu_bank_driver_unregister();
		return ret;
	}
	return 0;
}

static void __exit amdrnpu_module_exit(void)
{
	amdrnpu_rpmsg_unregister();
	platform_driver_unregister(&amdrnpu_platform_driver);
	amdrnpu_bank_driver_unregister();
}

module_init(amdrnpu_module_init);
module_exit(amdrnpu_module_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD amdrnpu RPU management DRM/RPMsg driver");
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
