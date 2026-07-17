// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "amdrnpu_internal.h"

static void amdrnpu_dev_release(struct kref *ref)
{
	struct amdrnpu_dev *dev =
		container_of(ref, struct amdrnpu_dev, refcount);

	mutex_destroy(&dev->lock);
	kfree(dev);
}

void amdrnpu_dev_get(struct amdrnpu_dev *dev)
{
	kref_get(&dev->refcount);
}

void amdrnpu_dev_put(struct amdrnpu_dev *dev)
{
	kref_put(&dev->refcount, amdrnpu_dev_release);
}

static int amdrnpu_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct amdrnpu_dev *dev =
		container_of(mdev, struct amdrnpu_dev, mdev);

	amdrnpu_dev_get(dev);
	filp->private_data = dev;
	return 0;
}

static int amdrnpu_release(struct inode *inode, struct file *filp)
{
	struct amdrnpu_dev *dev = filp->private_data;

	filp->private_data = NULL;
	if (dev)
		amdrnpu_dev_put(dev);
	return 0;
}

static const struct file_operations amdrnpu_fops = {
	.owner		= THIS_MODULE,
	.open		= amdrnpu_open,
	.release	= amdrnpu_release,
	.unlocked_ioctl	= amdrnpu_ioctl,
	.compat_ioctl	= amdrnpu_ioctl,
};

int amdrnpu_chardev_register(struct amdrnpu_dev *dev)
{
	dev->mdev.minor = MISC_DYNAMIC_MINOR;
	dev->mdev.name  = dev->dev_name;
	dev->mdev.fops  = &amdrnpu_fops;
	dev->mdev.mode  = 0660;

	return misc_register(&dev->mdev);
}

void amdrnpu_chardev_deregister(struct amdrnpu_dev *dev)
{
	misc_deregister(&dev->mdev);
}

static int __init amdrnpu_module_init(void)
{
	int ret = amdrnpu_rpmsg_register();

	if (ret)
		pr_err("amdrnpu: register_rpmsg_driver failed: %d\n", ret);
	else
		pr_info("amdrnpu: registered (waiting for rpmsg channels)\n");
	return ret;
}

static void __exit amdrnpu_module_exit(void)
{
	amdrnpu_rpmsg_unregister();
	pr_info("amdrnpu: unregistered\n");
}

module_init(amdrnpu_module_init);
module_exit(amdrnpu_module_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("AMD amdrnpu RPU runtime rpmsg misc driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
