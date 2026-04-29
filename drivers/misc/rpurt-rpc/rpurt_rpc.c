// SPDX-License-Identifier: GPL-2.0
/*
 * rpurt-rpc - simple misc character driver providing an ioctl-based
 * RPC channel between userspace and the RPU runtime.
 *
 * This is a skeleton implementation: it registers a misc device at
 * /dev/rpurt_rpc, accepts the ioctls defined in the UAPI header, and
 * keeps a tiny in-memory FIFO so the userspace runtime can be exercised
 * end-to-end before the real RPU transport is wired in.
 */

#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <uapi/rpurt-rpc/rpurt_rpc.h>

#define RPURT_RPC_FIFO_DEPTH	16

struct rpurt_rpc_dev {
	struct miscdevice	mdev;
	struct mutex		lock;
	wait_queue_head_t	rxwq;
	DECLARE_KFIFO(rxfifo, struct rpurt_rpc_msg, RPURT_RPC_FIFO_DEPTH);
};

static struct rpurt_rpc_dev *g_dev;

static int rpurt_rpc_open(struct inode *inode, struct file *filp)
{
	filp->private_data = g_dev;
	return 0;
}

static int rpurt_rpc_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	return 0;
}

static long rpurt_rpc_ioctl_get_info(struct rpurt_rpc_dev *dev,
				     unsigned long arg)
{
	struct rpurt_rpc_info info = {
		.abi_version = RPURT_RPC_ABI_VERSION,
		.max_payload = RPURT_RPC_MAX_PAYLOAD,
	};

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long rpurt_rpc_ioctl_send(struct rpurt_rpc_dev *dev,
				 unsigned long arg)
{
	struct rpurt_rpc_msg *msg;
	long ret = 0;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	if (copy_from_user(msg, (void __user *)arg, sizeof(*msg))) {
		ret = -EFAULT;
		goto out;
	}

	if (msg->flags || msg->len > RPURT_RPC_MAX_PAYLOAD) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Placeholder transport: loop the message back into the rx fifo so
	 * the userspace runtime can validate the round-trip. Replace this
	 * with the real RPU mailbox/ipi/openamp transport when wired up.
	 */
	mutex_lock(&dev->lock);
	if (kfifo_is_full(&dev->rxfifo)) {
		ret = -EAGAIN;
	} else {
		kfifo_in(&dev->rxfifo, msg, 1);
		wake_up_interruptible(&dev->rxwq);
	}
	mutex_unlock(&dev->lock);

out:
	kfree(msg);
	return ret;
}

static long rpurt_rpc_ioctl_recv(struct rpurt_rpc_dev *dev,
				 struct file *filp, unsigned long arg)
{
	struct rpurt_rpc_msg *msg;
	long ret = 0;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (;;) {
		mutex_lock(&dev->lock);
		if (kfifo_out(&dev->rxfifo, msg, 1) == 1) {
			mutex_unlock(&dev->lock);
			break;
		}
		mutex_unlock(&dev->lock);

		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(dev->rxwq,
					     !kfifo_is_empty(&dev->rxfifo))) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	if (copy_to_user((void __user *)arg, msg, sizeof(*msg)))
		ret = -EFAULT;

out:
	kfree(msg);
	return ret;
}

static long rpurt_rpc_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct rpurt_rpc_dev *dev = filp->private_data;

	if (!dev)
		return -ENODEV;

	switch (cmd) {
	case RPURT_RPC_IOC_GET_INFO:
		return rpurt_rpc_ioctl_get_info(dev, arg);
	case RPURT_RPC_IOC_SEND:
		return rpurt_rpc_ioctl_send(dev, arg);
	case RPURT_RPC_IOC_RECV:
		return rpurt_rpc_ioctl_recv(dev, filp, arg);
	case RPURT_RPC_IOC_RESET:
		mutex_lock(&dev->lock);
		kfifo_reset(&dev->rxfifo);
		mutex_unlock(&dev->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations rpurt_rpc_fops = {
	.owner		= THIS_MODULE,
	.open		= rpurt_rpc_open,
	.release	= rpurt_rpc_release,
	.unlocked_ioctl	= rpurt_rpc_ioctl,
	.compat_ioctl	= rpurt_rpc_ioctl,
	/*
	 * Intentionally no .llseek: the device is ioctl-only and does not
	 * support seek. Older kernels (< 6.12) used .llseek = no_llseek for
	 * this, but no_llseek was removed in v6.12; leaving the field unset
	 * gives the safe default on every supported kernel.
	 */
};

static int __init rpurt_rpc_init(void)
{
	struct rpurt_rpc_dev *dev;
	int ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->rxwq);
	INIT_KFIFO(dev->rxfifo);

	dev->mdev.minor = MISC_DYNAMIC_MINOR;
	dev->mdev.name  = RPURT_RPC_DEVICE_NAME;
	dev->mdev.fops  = &rpurt_rpc_fops;
	dev->mdev.mode  = 0660;

	ret = misc_register(&dev->mdev);
	if (ret) {
		kfree(dev);
		return ret;
	}

	g_dev = dev;
	pr_info("rpurt-rpc: registered /dev/%s (ABI v%u, max payload %u)\n",
		RPURT_RPC_DEVICE_NAME, RPURT_RPC_ABI_VERSION,
		RPURT_RPC_MAX_PAYLOAD);
	return 0;
}

static void __exit rpurt_rpc_exit(void)
{
	struct rpurt_rpc_dev *dev = g_dev;

	if (!dev)
		return;

	misc_deregister(&dev->mdev);
	kfree(dev);
	g_dev = NULL;
	pr_info("rpurt-rpc: unregistered\n");
}

module_init(rpurt_rpc_init);
module_exit(rpurt_rpc_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("RPU runtime RPC misc driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
MODULE_ALIAS("char-major-10-rpurt_rpc");
