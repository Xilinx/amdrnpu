/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _AMDRNPU_INTERNAL_H_
#define _AMDRNPU_INTERNAL_H_

#include <linux/atomic.h>
#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>

#include <amdrnpu/amdrnpu.h>

#include "amdrnpu_cmd.h"

#define AMDRNPU_DRV_NAME	AMDRNPU_DRIVER_NAME
#define AMDRNPU_MAX_ARGS	64
#define AMDRNPU_RPMSG_NAME	"amdrnpu"

#define AMDRNPU_SUBMIT_HDR_SZ	offsetof(struct amdrnpu_cmd_submit, args)

/* cmd not indexed in cmd_xa (wire seq 0 and UINT_MAX are both valid ids). */
#define AMDRNPU_CMD_SEQ_NONE	((u32)~0U)

/*
 * Driver-wide state.  One per "amd,amdrnpu" DT node; the struct embeds the
 * drm_device and is allocated by devm_drm_dev_alloc() in the platform probe, so
 * its lifetime is managed by DRM/devm.
 *
 * Completion is asynchronous and many-in-flight: submitted commands live in
 * @cmd_xa keyed by a driver-assigned wire seq, and are correlated back to the
 * RPU response by that seq.  Each command owns its own fence; @fence_context
 * and @fence_seqno are device-scoped dma_fence timeline bookkeeping.
 */
struct amdrnpu_dev {
	struct drm_device	drm;
	struct device		*dev;		/* platform device; for logging */
	struct device_node	*ctrl_node;	/* "amd,rpu" target */

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;	/* rpmsg send path */

	struct mutex		cmd_lock;	/* cmd_xa alloc/erase */
	struct xarray		cmd_xa;		/* in-flight cmds; id via xa_alloc_cyclic */
	u32			cmd_xa_next;	/* xa_alloc_cyclic cursor */
	atomic_t		fence_seqno;
	u64			fence_context;
};

static inline struct amdrnpu_dev *to_amdrnpu_dev(struct drm_device *ddev)
{
	return container_of(ddev, struct amdrnpu_dev, drm);
}

/*
 * Per-command object, tracked in amdrnpu_dev.cmd_xa at @seq for the duration of
 * a submit.  Single-reffed: the kref is owned by the cmd_xa entry, and whoever
 * wins the erase from cmd_xa completes the fence and drops that reference.  The
 * fence is exported to userspace as a drm_syncobj (an independently
 * reference-counted object) so it can outlive the command.
 */
struct amdrnpu_cmd {
	struct amdrnpu_dev	*rnpu;
	u32			seq;		/* cmd_xa id / wire seq, or SEQ_NONE */
	u32			command_op;

	struct drm_syncobj	*syncobj;
	spinlock_t		fence_lock;	/* pending_fence */
	struct dma_fence	*pending_fence;

	struct kref		ref;
};

/* amdrnpu_drv.c */
extern struct platform_driver amdrnpu_platform_driver;

/* amdrnpu_drm.c */
extern const struct drm_driver amdrnpu_drm_driver;
int  amdrnpu_drm_register(struct amdrnpu_dev *rnpu);
void amdrnpu_drm_unregister(struct amdrnpu_dev *rnpu);

/* amdrnpu_cmd.c */
long amdrnpu_ioctl_cmd_submit(struct amdrnpu_dev *rnpu, void __user *uarg,
			      struct drm_file *file);
void amdrnpu_dev_cmd_init(struct amdrnpu_dev *rnpu);
void amdrnpu_dev_cmd_fini(struct amdrnpu_dev *rnpu);
void amdrnpu_dev_cmd_dispatch_response(struct amdrnpu_dev *rnpu,
				       const void *payload, size_t len);
void amdrnpu_dev_cmd_drain(struct amdrnpu_dev *rnpu, int err);

/* amdrnpu_rpmsg.c */
int  amdrnpu_rpmsg_register(void);
void amdrnpu_rpmsg_unregister(void);
/* @seq: driver command id (amdrnpu_cmd.seq), not supplied by userspace. */
int  amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *rnpu, u32 seq, u32 command_op,
			    u32 flags, const void *args, u32 num_args);

#endif /* _AMDRNPU_INTERNAL_H_ */
