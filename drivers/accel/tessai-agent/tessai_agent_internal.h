/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private definitions shared between the .c files of the
 * tessai-agent out-of-tree driver.
 */
#ifndef _TESSAI_AGENT_INTERNAL_H_
#define _TESSAI_AGENT_INTERNAL_H_

#include <linux/dma-fence.h>
#include <linux/xarray.h>
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

/* Visible part of ioctl tessai_cmd_submit (flex array starts at tessai_arg[]) */
#define TESSAI_SUBMIT_ABI_HDR_SZ	offsetof(struct tessai_cmd_submit, args)

/* cmd not indexed in cmd_xa (wire seq 0 and UINT_MAX are both valid ids). */
#define TESSAI_CMD_SEQ_NONE	((u32)~0U)

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
	/* Refcounted target of "amd,remoteproc" (DT binding). */
	struct device_node	*linked_rproc;

	struct tessai_bank	banks[TESSAI_NUM_BANKS];

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;	/* rpmsg send path */

	struct mutex		cmd_lock;	/* cmd_xa alloc/erase */
	struct xarray		cmd_xa;		/* in-flight cmds; id via xa_alloc_cyclic */
	u32			cmd_xa_next;	/* xa_alloc_cyclic cursor */
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
	enum dma_data_direction	dma_dir;
	struct page		*page;
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
	u32			seq;		/* wire tessai_wire_cmd.seq / tessai_wire_rsp.seq */
	bool			in_cmd_xa;
	u32			command_op;

	struct mutex		state_lock;	/* state, term_status, bo_refs */
	enum tessai_cmd_state	state;
	int			term_status;

	struct drm_syncobj	*syncobj;
	spinlock_t		fence_lock;	/* pending_fence */
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

void tessai_cmd_drain_inflight_rpmsg_detach(struct tessai_dev *tdev, int err);

int  tessai_rpmsg_register(void);
void tessai_rpmsg_unregister(void);
/* @seq: driver command id (tessai_cmd.seq), not supplied by userspace. */
int  tessai_rpmsg_send_cmd(struct tessai_dev *tdev, u32 seq,
			   u32 command_op, u32 flags, const __le64 *args,
			 u32 num_args);

#endif /* _TESSAI_AGENT_INTERNAL_H_ */
