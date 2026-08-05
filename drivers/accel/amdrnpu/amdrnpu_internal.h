/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _AMDRNPU_INTERNAL_H_
#define _AMDRNPU_INTERNAL_H_

#include <linux/atomic.h>
#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include <amdrnpu/amdrnpu.h>

#include "amdrnpu_cmd.h"

#define AMDRNPU_DRV_NAME	AMDRNPU_DRIVER_NAME
#define AMDRNPU_MAX_ARGS	64
#define AMDRNPU_RPMSG_NAME	"amdrnpu"

#define AMDRNPU_SUBMIT_HDR_SZ	offsetof(struct amdrnpu_cmd_submit, args)

struct amdrnpu_dev;
struct amdrnpu_cmd;

/*
 * Per-channel device state.  One of these is allocated for each bound rpmsg
 * endpoint.  Reference counted: the rpmsg binding owns one ref while bound;
 * each open fd holds an additional ref.
 *
 * Completion is asynchronous but single-outstanding: only one command may be
 * in flight per device, referenced by @pending_cmd.  The command owns its own
 * fence; the device merely tracks which one is in flight.  @fence_context and
 * @fence_seqno are dma_fence timeline bookkeeping, which is device-scoped.
 */
struct amdrnpu_dev {
	struct kref		refcount;
	struct miscdevice	mdev;
	struct device		*dev;		/* mdev.this_device; for logging */

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;	/* rpmsg send path */

	spinlock_t		cmd_lock;	/* pending_cmd */
	struct amdrnpu_cmd	*pending_cmd;	/* in-flight command, or NULL */
	atomic_t		fence_seqno;
	u64			fence_context;

	char			dev_name[32];
};

/* amdrnpu_drv.c */
void amdrnpu_dev_get(struct amdrnpu_dev *dev);
void amdrnpu_dev_put(struct amdrnpu_dev *dev);
int  amdrnpu_chardev_register(struct amdrnpu_dev *dev);
void amdrnpu_chardev_deregister(struct amdrnpu_dev *dev);

/* amdrnpu_cmd.c */
long amdrnpu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
void amdrnpu_dev_cmd_init(struct amdrnpu_dev *dev);
void amdrnpu_dev_cmd_fini(struct amdrnpu_dev *dev);
void amdrnpu_dev_cmd_dispatch_response(struct amdrnpu_dev *dev,
				   const void *payload, size_t len);
void amdrnpu_dev_cmd_abort(struct amdrnpu_dev *dev, int err);

/* amdrnpu_rpmsg.c */
int  amdrnpu_rpmsg_register(void);
void amdrnpu_rpmsg_unregister(void);
int  amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *dev, u32 command_op, u32 flags,
			    const void *args, u32 num_args);

#endif /* _AMDRNPU_INTERNAL_H_ */
