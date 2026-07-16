/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private definitions shared between the .c files of the
 * amdrnpu out-of-tree driver.
 */
#ifndef _AMDRNPU_INTERNAL_H_
#define _AMDRNPU_INTERNAL_H_

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

#include <amdrnpu/amdrnpu.h>

#include "amdrnpu_cmd.h"

#define AMDRNPU_DRV_NAME		AMDRNPU_DRIVER_NAME
#define AMDRNPU_MAX_ARGS	64
#define AMDRNPU_RPMSG_NAME		"amdrnpu"

/*
 * Bank index is taken directly from the bank node's "reg" cell in DT.
 * Values are also the same as the AMDRNPU_MEMORY_* enum exported in UAPI, so
 * userspace and DT use the same identifiers as the kernel slot index.
 */
enum amdrnpu_bank_index {
	AMDRNPU_BANK_RPU	= 0,	/* matches AMDRNPU_MEMORY_RPU    */
	AMDRNPU_BANK_DEV	= 1,	/* matches AMDRNPU_MEMORY_DEVICE */
	AMDRNPU_NUM_BANKS,
};

struct amdrnpu_dev;
struct amdrnpu_cmd;

/* Visible part of ioctl amdrnpu_cmd_submit (flex array starts at amdrnpu_arg[]) */
#define AMDRNPU_SUBMIT_ABI_HDR_SZ	offsetof(struct amdrnpu_cmd_submit, args)

/* cmd not indexed in cmd_xa (wire seq 0 and UINT_MAX are both valid ids). */
#define AMDRNPU_CMD_SEQ_NONE	((u32)~0U)

/* --------------------------------------------------------------- */
/* Memory bank                                                     */
/* --------------------------------------------------------------- */

struct amdrnpu_bank {
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

struct amdrnpu_dev {
	struct drm_device	drm;
	struct device		*dev;
	/* Refcounted target of "amd,remoteproc" (DT binding). */
	struct device_node	*linked_rproc;

	struct amdrnpu_bank	banks[AMDRNPU_NUM_BANKS];

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

/* --------------------------------------------------------------- */
/* GEM object                                                      */
/* --------------------------------------------------------------- */

struct amdrnpu_gem {
	struct drm_gem_object	base;
	struct amdrnpu_bank	*bank;
	void			*kvaddr;
	dma_addr_t		dma_addr;
	size_t			size;
	enum dma_data_direction	dma_dir;
	struct page		*page;
	struct sg_table		*sgt;
};

static inline struct amdrnpu_gem *to_amdrnpu_gem(struct drm_gem_object *obj)
{
	return container_of(obj, struct amdrnpu_gem, base);
}

/* --------------------------------------------------------------- */
/* Per-command object                                              */
/* --------------------------------------------------------------- */

enum amdrnpu_cmd_state {
	AMDRNPU_CMD_SUBMITTED,
	AMDRNPU_CMD_TERMINATED,
};

struct amdrnpu_cmd {
	struct amdrnpu_dev		*tdev;
	struct drm_file		*owner;
	u32			seq;		/* wire amdrnpu_wire_cmd.seq / amdrnpu_wire_rsp.seq */
	bool			in_cmd_xa;
	u32			command_op;

	struct mutex		state_lock;	/* state, term_status, bo_refs */
	enum amdrnpu_cmd_state	state;
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

int  amdrnpu_bank_driver_register(void);
void amdrnpu_bank_driver_unregister(void);
int  amdrnpu_bank_check_all_present(struct amdrnpu_dev *tdev);
struct amdrnpu_bank *amdrnpu_bank_by_region(struct amdrnpu_dev *tdev, u32 region_id);
struct amdrnpu_bank *amdrnpu_bank_memory_type(struct amdrnpu_dev *tdev, u32 mem_type);

extern const struct drm_driver amdrnpu_drm_driver;
int  amdrnpu_drm_register(struct amdrnpu_dev *tdev);
void amdrnpu_drm_unregister(struct amdrnpu_dev *tdev);

long amdrnpu_ioctl_cmd_submit(struct drm_device *ddev, void __user *uarg, struct drm_file *file);
void amdrnpu_cmd_release_owner(struct amdrnpu_dev *tdev, struct drm_file *file);
void amdrnpu_cmd_dispatch_response(struct amdrnpu_dev *tdev,
				  const void *payload, size_t len);
int  amdrnpu_cmd_init(struct amdrnpu_dev *tdev);
void amdrnpu_cmd_fini(struct amdrnpu_dev *tdev);

void amdrnpu_cmd_drain_inflight_rpmsg_detach(struct amdrnpu_dev *tdev, int err);

int  amdrnpu_rpmsg_register(void);
void amdrnpu_rpmsg_unregister(void);
/* @seq: driver command id (amdrnpu_cmd.seq), not supplied by userspace. */
int  amdrnpu_rpmsg_send_cmd(struct amdrnpu_dev *tdev, u32 seq,
			   u32 command_op, u32 flags, const __le64 *args,
			 u32 num_args);

#endif /* _AMDRNPU_INTERNAL_H_ */
