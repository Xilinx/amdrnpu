// SPDX-License-Identifier: GPL-2.0
/*
 * rpurt-rpc - misc character driver providing an ioctl-based RPC channel
 * between userspace and an RPU runtime, bound to an rpmsg endpoint.
 *
 * This is a skeleton implementation: it registers a misc device at
 * /dev/rpurt_rpc, accepts the ioctls defined in the UAPI header, and
 * keeps a tiny in-memory FIFO so the userspace runtime can be exercised
 * end-to-end before the real RPU transport is wired in.
 */

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <uapi/rpurt-rpc/rpurt_rpc.h>

#define RPURT_RPC_FIFO_DEPTH	16

struct rpurt_rpc_dev {
	struct kref		  refcount;
	struct miscdevice	  mdev;
	struct rpmsg_device __rcu *rpdev;
	/* only one ioctl at a time can touch dev state; rx path uses kfifo lock */
	struct mutex		  lock;
	wait_queue_head_t	  rxwq;
	DECLARE_KFIFO(rxfifo, struct rpurt_rpc_msg, RPURT_RPC_FIFO_DEPTH);
	char			  dev_name[32];
};

/* Monotonic instance counter for naming /dev/rpurt_rpc.N */
static atomic_t rpurt_rpc_instance_counter = ATOMIC_INIT(0);

static void rpurt_rpc_dev_release(struct kref *ref)
{
	struct rpurt_rpc_dev *dev =
		container_of(ref, struct rpurt_rpc_dev, refcount);

	mutex_destroy(&dev->lock);
	kfree(dev);
}

static inline void rpurt_rpc_dev_get(struct rpurt_rpc_dev *dev)
{
	kref_get(&dev->refcount);
}

static inline void rpurt_rpc_dev_put(struct rpurt_rpc_dev *dev)
{
	kref_put(&dev->refcount, rpurt_rpc_dev_release);
}

static int rpurt_rpc_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct rpurt_rpc_dev *dev =
		container_of(mdev, struct rpurt_rpc_dev, mdev);

	rpurt_rpc_dev_get(dev);
	filp->private_data = dev;
	return 0;
}

static int rpurt_rpc_release(struct inode *inode, struct file *filp)
{
	struct rpurt_rpc_dev *dev = filp->private_data;

	filp->private_data = NULL;
	if (dev)
		rpurt_rpc_dev_put(dev);
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
	struct rpmsg_device *rpdev;
	long ret = 0;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	if (copy_from_user(msg, (void __user *)arg, sizeof(*msg))) {
		ret = -EFAULT;
		goto out;
	}

	if (msg->flags || msg->len == 0 ||
	    msg->len > RPURT_RPC_MAX_PAYLOAD) {
		ret = -EINVAL;
		goto out;
	}

	rcu_read_lock();
	rpdev = rcu_dereference(dev->rpdev);
	if (!rpdev) {
		rcu_read_unlock();
		ret = -ENODEV;
		goto out;
	}
	ret = rpmsg_send(rpdev->ept, msg->data, msg->len);
	rcu_read_unlock();

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

		if (!rcu_access_pointer(dev->rpdev)) {
			ret = -ENODEV;
			goto out;
		}

		if (filp->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(dev->rxwq,
					     !kfifo_is_empty(&dev->rxfifo) ||
					     !rcu_access_pointer(dev->rpdev))) {
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
};

static int rpurt_rpc_rpmsg_cb(struct rpmsg_device *rpdev, void *data,
			      int len, void *priv, u32 src)
{
	struct rpurt_rpc_dev *dev = dev_get_drvdata(&rpdev->dev);
	struct rpurt_rpc_msg *msg;

	if (!dev)
		return -ENODEV;

	if (len <= 0 || len > RPURT_RPC_MAX_PAYLOAD) {
		dev_warn(&rpdev->dev,
			 "rpurt-rpc: dropping rx frame: len=%d (max=%u)\n",
			 len, RPURT_RPC_MAX_PAYLOAD);
		return -EINVAL;
	}

	msg = kzalloc(sizeof(*msg), GFP_ATOMIC);
	if (!msg)
		return -ENOMEM;

	msg->len = (__u32)len;
	memcpy(msg->data, data, len);

	mutex_lock(&dev->lock);
	if (kfifo_is_full(&dev->rxfifo)) {
		mutex_unlock(&dev->lock);
		dev_warn_ratelimited(&rpdev->dev,
				     "rpurt-rpc: rx fifo full, dropping frame\n");
		kfree(msg);
		return -ENOSPC;
	}
	kfifo_in(&dev->rxfifo, msg, 1);
	mutex_unlock(&dev->lock);

	wake_up_interruptible(&dev->rxwq);
	kfree(msg);
	return 0;
}

static int rpurt_rpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct rpurt_rpc_dev *dev;
	int idx, ret;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->refcount);
	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->rxwq);
	INIT_KFIFO(dev->rxfifo);
	rcu_assign_pointer(dev->rpdev, rpdev);

	idx = atomic_fetch_inc(&rpurt_rpc_instance_counter);
	snprintf(dev->dev_name, sizeof(dev->dev_name), "%s.%d",
		 RPURT_RPC_DEVICE_NAME, idx);

	dev->mdev.minor = MISC_DYNAMIC_MINOR;
	dev->mdev.name  = dev->dev_name;
	dev->mdev.fops  = &rpurt_rpc_fops;
	dev->mdev.mode  = 0660;

	ret = misc_register(&dev->mdev);
	if (ret) {
		dev_err(&rpdev->dev,
			"rpurt-rpc: misc_register(%s) failed: %d\n",
			dev->dev_name, ret);
		mutex_destroy(&dev->lock);
		kfree(dev);
		return ret;
	}

	dev_set_drvdata(&rpdev->dev, dev);
	dev_info(&rpdev->dev,
		 "rpurt-rpc: bound channel %s -> /dev/%s (ABI v%u, max payload %u)\n",
		 rpdev->id.name, dev->dev_name, RPURT_RPC_ABI_VERSION,
		 RPURT_RPC_MAX_PAYLOAD);
	return 0;
}

static void rpurt_rpc_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct rpurt_rpc_dev *dev = dev_get_drvdata(&rpdev->dev);

	if (!dev)
		return;

	dev_info(&rpdev->dev, "rpurt-rpc: removing /dev/%s\n", dev->dev_name);
	misc_deregister(&dev->mdev);

	rcu_assign_pointer(dev->rpdev, NULL);
	synchronize_rcu();
	wake_up_interruptible_all(&dev->rxwq);

	dev_set_drvdata(&rpdev->dev, NULL);
	rpurt_rpc_dev_put(dev);
}

static const struct rpmsg_device_id rpurt_rpc_id_table[] = {
	{ .name = "rpmsg-aie-mgmt" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpurt_rpc_id_table);

static struct rpmsg_driver rpurt_rpc_rpmsg_driver = {
	.drv = {
		.name	= "rpurt_rpc",
	},
	.id_table	= rpurt_rpc_id_table,
	.probe		= rpurt_rpc_rpmsg_probe,
	.remove		= rpurt_rpc_rpmsg_remove,
	.callback	= rpurt_rpc_rpmsg_cb,
};

static int __init rpurt_rpc_init(void)
{
	int ret = register_rpmsg_driver(&rpurt_rpc_rpmsg_driver);

	if (ret)
		pr_err("rpurt-rpc: register_rpmsg_driver failed: %d\n", ret);
	else
		pr_info("rpurt-rpc: registered (waiting for rpmsg channels)\n");
	return ret;
}

static void __exit rpurt_rpc_exit(void)
{
	unregister_rpmsg_driver(&rpurt_rpc_rpmsg_driver);
	pr_info("rpurt-rpc: unregistered\n");
}

module_init(rpurt_rpc_init);
module_exit(rpurt_rpc_exit);

MODULE_AUTHOR("AMD");
MODULE_DESCRIPTION("RPU runtime RPC misc driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.0");
