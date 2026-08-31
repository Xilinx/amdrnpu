// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>

#include "amdrnpu_internal.h"

static int amdrnpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *rpu_np;
	struct amdrnpu_dev *rnpu;
	int ret;

	rpu_np = of_parse_phandle(dev->of_node, "amd,rpu", 0);
	if (!rpu_np) {
		dev_err(dev, "missing 'amd,rpu' phandle\n");
		return -EINVAL;
	}

	rnpu = devm_drm_dev_alloc(dev, &amdrnpu_drm_driver, struct amdrnpu_dev, drm);
	if (IS_ERR(rnpu)) {
		of_node_put(rpu_np);
		return PTR_ERR(rnpu);
	}

	rnpu->ctrl_node = rpu_np;
	rnpu->dev = dev;
	mutex_init(&rnpu->rpmsg_lock);

	amdrnpu_dev_cmd_init(rnpu);

	platform_set_drvdata(pdev, rnpu);

	ret = amdrnpu_drm_register(rnpu);
	if (ret)
		goto err_cmd_fini;

	dev_info(dev, "amdrnpu ready\n");
	return 0;

err_cmd_fini:
	platform_set_drvdata(pdev, NULL);
	amdrnpu_dev_cmd_fini(rnpu);
	of_node_put(rnpu->ctrl_node);
	rnpu->ctrl_node = NULL;
	return ret;
}

static void amdrnpu_platform_remove(struct platform_device *pdev)
{
	struct amdrnpu_dev *rnpu = platform_get_drvdata(pdev);

	/*
	 * If an rpmsg channel is still bound, the device_link created at
	 * rpmsg-probe time guarantees the rpmsg consumer is unbound before we
	 * get here, so rnpu->rpdev is already NULL.
	 */
	amdrnpu_drm_unregister(rnpu);
	amdrnpu_dev_cmd_fini(rnpu);
	of_node_put(rnpu->ctrl_node);
	rnpu->ctrl_node = NULL;
}

static const struct of_device_id amdrnpu_of_match[] = {
	{ .compatible = "amd,amdrnpu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amdrnpu_of_match);

struct platform_driver amdrnpu_platform_driver = {
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

	ret = platform_driver_register(&amdrnpu_platform_driver);
	if (ret)
		return ret;

	ret = amdrnpu_rpmsg_register();
	if (ret) {
		platform_driver_unregister(&amdrnpu_platform_driver);
		return ret;
	}
	return 0;
}

static void __exit amdrnpu_module_exit(void)
{
	amdrnpu_rpmsg_unregister();
	platform_driver_unregister(&amdrnpu_platform_driver);
}

module_init(amdrnpu_module_init);
module_exit(amdrnpu_module_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD amdrnpu RPU management DRM/RPMsg driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
