// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "amdrnpu_internal.h"

static void amdrnpu_dev_release(struct kref *ref)
{
	struct amdrnpu_dev *rnpu = container_of(ref, struct amdrnpu_dev, refcount);

	amdrnpu_dev_cmd_fini(rnpu);
	if (rnpu->rproc_dev)
		put_device(rnpu->rproc_dev);
	mutex_destroy(&rnpu->rpmsg_lock);
	kfree(rnpu);
}

void amdrnpu_dev_get(struct amdrnpu_dev *rnpu)
{
	kref_get(&rnpu->refcount);
}

void amdrnpu_dev_put(struct amdrnpu_dev *rnpu)
{
	kref_put(&rnpu->refcount, amdrnpu_dev_release);
}

static int amdrnpu_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct amdrnpu_dev *rnpu = container_of(mdev, struct amdrnpu_dev, mdev);
	struct amdrnpu_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	amdrnpu_dev_get(rnpu);
	client->rnpu = rnpu;
	xa_init_flags(&client->buf_xa, XA_FLAGS_ALLOC1);

	filp->private_data = client;
	return 0;
}

static int amdrnpu_release(struct inode *inode, struct file *filp)
{
	struct amdrnpu_client *client = filp->private_data;

	filp->private_data = NULL;
	if (client) {
		amdrnpu_client_buf_release(client);
		xa_destroy(&client->buf_xa);
		amdrnpu_dev_put(client->rnpu);
		kfree(client);
	}
	return 0;
}

static const struct file_operations amdrnpu_fops = {
	.owner		= THIS_MODULE,
	.open		= amdrnpu_open,
	.release	= amdrnpu_release,
	.unlocked_ioctl	= amdrnpu_ioctl,
	.compat_ioctl	= amdrnpu_ioctl,
};

int amdrnpu_chardev_register(struct amdrnpu_dev *rnpu)
{
	int ret;

	rnpu->mdev.minor = MISC_DYNAMIC_MINOR;
	rnpu->mdev.name  = rnpu->dev_name;
	rnpu->mdev.fops  = &amdrnpu_fops;
	rnpu->mdev.mode  = 0660;

	ret = misc_register(&rnpu->mdev);
	if (ret)
		return ret;

	rnpu->dev = rnpu->mdev.this_device;

	/*
	 * Imported dma-bufs are mapped against this device.  It is a plain
	 * miscdevice with no of_node/iommu, so mappings are direct; allow a
	 * 64-bit mask so >32-bit device buffers are not bounced.
	 */
	if (dma_coerce_mask_and_coherent(rnpu->dev, DMA_BIT_MASK(64)))
		RNPU_WARN(rnpu,
			  "64-bit DMA mask unavailable; buffer import may be limited\n");

	return 0;
}

void amdrnpu_chardev_deregister(struct amdrnpu_dev *rnpu)
{
	misc_deregister(&rnpu->mdev);
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
