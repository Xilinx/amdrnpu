/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _AMDRNPU_INTERNAL_H_
#define _AMDRNPU_INTERNAL_H_

#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <amdrnpu/amdrnpu_accel.h>

/* rpmsg channel driver binds to (must match RPU firmware). */
#define AMDRNPU_RPMSG_NAME	"amdrnpu"

#define AMDRNPU_FIFO_DEPTH	16

/*
 * Per-channel device state.  One of these is allocated for each bound rpmsg
 * endpoint.  Reference counted: the rpmsg binding owns one ref while bound;
 * each open fd holds an additional ref.
 */
struct amdrnpu_dev {
	struct kref		  refcount;
	struct miscdevice	  mdev;
	struct rpmsg_device __rcu *rpdev;
	/* only one ioctl at a time can touch dev state; rx path uses kfifo lock */
	struct mutex		  lock;
	wait_queue_head_t	  rxwq;
	DECLARE_KFIFO(rxfifo, struct amdrnpu_msg, AMDRNPU_FIFO_DEPTH);
	char			  dev_name[32];
};

/* amdrnpu_drv.c - misc device lifecycle and refcounting. */
void amdrnpu_dev_get(struct amdrnpu_dev *dev);
void amdrnpu_dev_put(struct amdrnpu_dev *dev);
int  amdrnpu_chardev_register(struct amdrnpu_dev *dev);
void amdrnpu_chardev_deregister(struct amdrnpu_dev *dev);

/* amdrnpu_cmd.c - ioctl dispatch. */
long amdrnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* amdrnpu_rpmsg.c - rpmsg transport. */
int  amdrnpu_rpmsg_register(void);
void amdrnpu_rpmsg_unregister(void);
int  amdrnpu_rpmsg_send(struct amdrnpu_dev *dev, const void *data, size_t len);

#endif /* _AMDRNPU_INTERNAL_H_ */
