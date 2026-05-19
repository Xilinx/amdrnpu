/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private definitions shared between the .c files of the
 * tessai-agent out-of-tree driver.
 */
#ifndef _TESSAI_AGENT_INTERNAL_H_
#define _TESSAI_AGENT_INTERNAL_H_

#include <linux/dma-fence.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/rpmsg.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>

#include <tessai-agent/tessai_agent.h>

#include "tessai_agent_cmd.h"

#define TESSAI_DRV_NAME		TESSAI_AGENT_DRIVER_NAME
#define TESSAI_MAX_ARGS	64
#define TESSAI_RPMSG_NAME		"tessai-agent"

/*
 * Bank index is taken directly from the bank node's "reg" cell in DT.
 * Values are also the same as the TESSAI_MEMORY_* enum exported in UAPI, so
 * userspace and DT use the same identifiers as the kernel slot index.
 */
enum tessai_bank_index {
	TESSAI_BANK_RPU	= 0,	/* matches TESSAI_MEMORY_RPU    */
	TESSAI_BANK_DEV	= 1,	/* matches TESSAI_MEMORY_DEVICE */
	TESSAI_NUM_BANKS,
};

struct tessai_dev;
struct tessai_cmd;

/* Driver ↔ RPU ioctl payload (drm_file → tessai_rpmsg). Not exported in UAPI. */
enum tessai_op_ctrl {
	TESSAI_OP_EXEC = 0,
	TESSAI_OP_PING = 1,
};

/**
 * @cmd_op: packed little-endian halves on the RPMsg correlator LE32:
 *	     bits [31..16] = tessai_op_ctrl, bits [15..0] = op_app.
 */
struct tessai_submit_cmd {
	__s32 drm_sync_obj_handle;
	__u32 cmd_op;
	__u32 command_op;
	__u32 num_args;
	struct tessai_arg args[];
};

static inline u32 tessai_pack_cmd_op(u16 op_ctrl, u16 op_app)
{
	return ((__u32)op_ctrl << 16) | (u16)op_app;
}

static inline void tessai_unpack_cmd_op(u32 packed, u16 *op_ctrl, u16 *op_app)
{
	*op_ctrl = (u16)((packed >> 16) & 0xffff);
	*op_app = (u16)(packed & 0xffff);
}

/* Visible part of ioctl tessai_cmd_submit (flex array starts at tessai_arg[]) */
#define TESSAI_SUBMIT_ABI_HDR_SZ	offsetof(struct tessai_cmd_submit, args)

/* tessai_wire_cmd.flags: ask firmware to drop work keyed by correlation @op_app. */
#define TESSAI_WIRE_FLAG_ABORT	0x80000001u

/* --------------------------------------------------------------- */
/* Memory bank                                                     */
/* --------------------------------------------------------------- */

struct tessai_bank {
	struct device		*dev;
	struct platform_device	*pdev;
	struct device_node	*np;
	u32			region_id;
	const char		*label;
	bool			has_reserved_mem;
};

/* --------------------------------------------------------------- */
/* Driver-wide state                                               */
/* --------------------------------------------------------------- */

struct tessai_dev {
	struct drm_device	drm;
	struct device		*dev;
	/* Refcounted target of "amd,remoteproc"; used to find peer instances. */
	struct device_node	*linked_rproc;

	struct tessai_bank	banks[TESSAI_NUM_BANKS];

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;

	struct mutex		cmd_lock;
	struct idr		cmd_idr;
	atomic_t		fence_seqno;
	u64			fence_context;
};

static inline struct tessai_dev *to_tessai_dev(struct drm_device *ddev)
{
	return container_of(ddev, struct tessai_dev, drm);
}

/* --------------------------------------------------------------- */
/* GEM object                                                      */
/* --------------------------------------------------------------- */

struct tessai_gem {
	struct drm_gem_object	base;
	struct tessai_bank	*bank;
	void			*kvaddr;
	dma_addr_t		dma_addr;
	size_t			size;
	struct sg_table		*sgt;
};

static inline struct tessai_gem *to_tessai_gem(struct drm_gem_object *obj)
{
	return container_of(obj, struct tessai_gem, base);
}

/* --------------------------------------------------------------- */
/* Per-command object                                              */
/* --------------------------------------------------------------- */

enum tessai_cmd_state {
	TESSAI_CMD_SUBMITTED,
	TESSAI_CMD_TERMINATED,
};

struct tessai_cmd {
	struct tessai_dev		*tdev;
	struct drm_file		*owner;
	s32			handle;
	u32			command_op;

	struct mutex		state_lock;
	enum tessai_cmd_state	state;
	int			term_status;

	struct drm_syncobj	*syncobj;
	spinlock_t		fence_lock;
	struct dma_fence	*pending_fence;

	struct drm_gem_object	**bo_refs;
	u32			num_bo_refs;

	struct kref		ref;
};

/* --------------------------------------------------------------- */
/* Module-internal API                                             */
/* --------------------------------------------------------------- */

int  tessai_bank_driver_register(void);
void tessai_bank_driver_unregister(void);
int  tessai_bank_check_all_present(struct tessai_dev *tdev);
struct tessai_bank *tessai_bank_by_region(struct tessai_dev *tdev, u32 region_id);
struct tessai_bank *tessai_bank_memory_type(struct tessai_dev *tdev, u32 mem_type);

extern const struct drm_driver tessai_drm_driver;
int  tessai_drm_register(struct tessai_dev *tdev);
void tessai_drm_unregister(struct tessai_dev *tdev);

long tessai_ioctl_cmd_submit(struct drm_device *ddev, void __user *uarg, struct drm_file *file);
void tessai_cmd_release_owner(struct tessai_dev *tdev, struct drm_file *file);
void tessai_cmd_dispatch_response(struct tessai_dev *tdev,
				const void *payload, size_t len);
int  tessai_cmd_init(struct tessai_dev *tdev);
void tessai_cmd_fini(struct tessai_dev *tdev);

void tessai_cmd_abort_inflight_rpmsg_detach(struct tessai_dev *tdev, int err);

int  tessai_rpmsg_register(void);
void tessai_rpmsg_unregister(void);
int  tessai_rpmsg_send_cmd(struct tessai_dev *tdev, u16 op_ctrl, u16 op_app,
			 u32 command_op, u32 flags, const __le64 *args,
			 u32 num_args);

#endif /* _TESSAI_AGENT_INTERNAL_H_ */
