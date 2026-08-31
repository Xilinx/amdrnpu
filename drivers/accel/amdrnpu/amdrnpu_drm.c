// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/fs.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#include "amdrnpu_internal.h"

/*
 * amdrnpu_drm_ioctl() handles 'R'-magic ioctls and delegates everything
 * else to drm_ioctl(), which in turn handles DRM core and syncobj ioctls.
 */
static long amdrnpu_drm_ioctl(struct file *filp, unsigned int cmd,
			      unsigned long arg)
{
	struct drm_file *file = filp->private_data;
	struct drm_device *ddev = file->minor->dev;
	struct amdrnpu_dev *rnpu = to_amdrnpu_dev(ddev);
	void __user *uarg = (void __user *)arg;

	if (_IOC_TYPE(cmd) != AMDRNPU_IOC_MAGIC)
		return drm_ioctl(filp, cmd, arg);

	switch (cmd) {
	case AMDRNPU_CMD_SUBMIT:
		return amdrnpu_ioctl_cmd_submit(rnpu, uarg, file);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations amdrnpu_drm_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= amdrnpu_drm_ioctl,
	.compat_ioctl	= amdrnpu_drm_ioctl,
	.llseek		= noop_llseek,
	.fop_flags	= FOP_UNSIGNED_OFFSET,
};

const struct drm_driver amdrnpu_drm_driver = {
	.driver_features	= DRIVER_RENDER | DRIVER_SYNCOBJ,
	.fops			= &amdrnpu_drm_fops,
	.name			= AMDRNPU_DRV_NAME,
	.desc			= "AMD amdrnpu RPU management driver",
	.major			= 1,
	.minor			= 0,
};

/*
 * The drm_device is allocated in the platform probe via devm_drm_dev_alloc() so
 * its lifetime is tied to the parent device.  amdrnpu_drm_register() therefore
 * only needs to publish the device.
 */
int amdrnpu_drm_register(struct amdrnpu_dev *rnpu)
{
	return drm_dev_register(&rnpu->drm, 0);
}

void amdrnpu_drm_unregister(struct amdrnpu_dev *rnpu)
{
	drm_dev_unregister(&rnpu->drm);
}
