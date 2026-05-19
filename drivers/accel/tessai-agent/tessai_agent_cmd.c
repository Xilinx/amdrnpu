// SPDX-License-Identifier: GPL-2.0
/*
 * tessai-agent - command lifecycle and a single-stage dma_fence on drm_syncobj.
 *
 * After TESSAI_CMD_SUBMIT, userspace waits on the returned drm_syncobj.  One
 * tessai_fence backs that syncobj until a terminal RPMsg response; the driver
 * then dma_fence_signals it (failure paths set dma_fence error from errno).
 */

#include <linux/device.h>
#include <linux/build_bug.h>
#include <linux/dma-fence.h>
#include <linux/printk.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/stddef.h>
#include <linux/uaccess.h>

#include <drm/drm_syncobj.h>

#include "tessai_agent_internal.h"

/* --------------------------------------------------------------- */
/* Custom dma_fence                                                */
/* --------------------------------------------------------------- */

struct tessai_fence {
	struct dma_fence	base;
	spinlock_t		lock;	/* dma_fence lock */
};

static const char *tessai_fence_get_driver_name(struct dma_fence *fence)
{
	return TESSAI_DRV_NAME;
}

static const char *tessai_fence_get_timeline_name(struct dma_fence *fence)
{
	return "tess-event";
}

static const struct dma_fence_ops tessai_fence_ops = {
	.get_driver_name	= tessai_fence_get_driver_name,
	.get_timeline_name	= tessai_fence_get_timeline_name,
};

static struct dma_fence *tessai_fence_create(struct tessai_dev *tdev)
{
	struct tessai_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &tessai_fence_ops, &f->lock,
		       tdev->fence_context,
		       atomic_inc_return(&tdev->fence_seqno));
	return &f->base;
}

static void tessai_cmd_drop_bos(struct tessai_cmd *cmd)
{
	u32 i, n;

	mutex_lock(&cmd->state_lock);
	n = cmd->num_bo_refs;
	if (!n) {
		mutex_unlock(&cmd->state_lock);
		return;
	}
	cmd->num_bo_refs = 0;
	mutex_unlock(&cmd->state_lock);

	for (i = 0; i < n; i++) {
		if (cmd->bo_refs[i]) {
			drm_gem_object_put(cmd->bo_refs[i]);
			cmd->bo_refs[i] = NULL;
		}
	}
}

static bool tessai_cmd_set_terminated(struct tessai_cmd *cmd, int term_status)
{
	bool first;

	mutex_lock(&cmd->state_lock);
	first = (cmd->state != TESSAI_CMD_TERMINATED);
	if (first) {
		cmd->state = TESSAI_CMD_TERMINATED;
		cmd->term_status = term_status;
	}
	mutex_unlock(&cmd->state_lock);

	if (first)
		tessai_cmd_drop_bos(cmd);
	return first;
}

static void tessai_cmd_release(struct kref *ref)
{
	struct tessai_cmd *cmd = container_of(ref, struct tessai_cmd, ref);
	struct dma_fence *f;

	spin_lock(&cmd->fence_lock);
	f = cmd->pending_fence;
	cmd->pending_fence = NULL;
	spin_unlock(&cmd->fence_lock);
	if (f) {
		dma_fence_set_error(f, -ECONNRESET);
		dma_fence_signal(f);
		dma_fence_put(f);
	}
	if (cmd->syncobj)
		drm_syncobj_put(cmd->syncobj);

	tessai_cmd_drop_bos(cmd);
	kfree(cmd->bo_refs);
	kfree(cmd);
}

static void tessai_cmd_get(struct tessai_cmd *cmd) { kref_get(&cmd->ref); }
static void tessai_cmd_put(struct tessai_cmd *cmd) { kref_put(&cmd->ref, tessai_cmd_release); }

static struct tessai_cmd *tessai_cmd_lookup(struct tessai_dev *tdev, s32 handle)
{
	struct tessai_cmd *cmd;

	mutex_lock(&tdev->cmd_lock);
	cmd = idr_find(&tdev->cmd_idr, handle);
	if (cmd)
		tessai_cmd_get(cmd);
	mutex_unlock(&tdev->cmd_lock);
	return cmd;
}

/*
 * With two "amd,tessai-agent" DT nodes aimed at the same remoteproc, RPMsg may
 * bind to one platform instance while userspace ioctls another; idr slots then
 * live on a peer tessai_dev.  Try (1) the RPMsg tessai_dev idr; (2) every other tess
 * node whose amd,remoteproc phandle resolves to linked_rproc (pointer identity).
 *
 * Overlay/merge trees sometimes duplicate the logical RPU behind different struct
 * device_node pointers so (2) never matches; (3) walk all probed amd,tessai-agent
 * platform devices except skip and lookup the command id there.  That can mis-route
 * only if multiple RPU instances concurrently reuse the same small command ids.
 */
struct tessai_cmd_find_ctx {
	struct tessai_dev *skip_tdev;
	u32 id;
	struct tessai_cmd *found;
};

static int tessai_cmd_find_on_tess_pdev(struct device *dev, void *arg)
{
	struct tessai_cmd_find_ctx *c = arg;
	struct platform_device *pdev;
	struct tessai_dev *owner;

	if (!dev->of_node)
		return 0;
	if (!of_device_is_compatible(dev->of_node, "amd,tessai-agent"))
		return 0;
	pdev = to_platform_device(dev);
	owner = platform_get_drvdata(pdev);
	if (!owner || owner->dev != dev || owner == c->skip_tdev)
		return 0;
	c->found = tessai_cmd_lookup(owner, c->id);
	return c->found ? 1 : 0;
}

static struct tessai_cmd *tessai_cmd_lookup_via_platform_fallback(struct tessai_dev *skip,
								u32 handle)
{
	struct tessai_cmd_find_ctx c = {
		.skip_tdev = skip,
		.id = handle,
		.found = NULL,
	};

	bus_for_each_dev(&platform_bus_type, NULL, &c,
			 tessai_cmd_find_on_tess_pdev);
	return c.found;
}

static struct tessai_cmd *tessai_cmd_lookup_any_peer(struct tessai_dev *tdev, u32 handle)
{
	struct tessai_cmd *cmd;
	struct device_node *np;
	struct device_node *rproc_anchor;

	cmd = tessai_cmd_lookup(tdev, handle);
	if (cmd)
		return cmd;

	rproc_anchor = READ_ONCE(tdev->linked_rproc);
	if (rproc_anchor) {
		for_each_compatible_node(np, NULL, "amd,tessai-agent") {
			struct device_node *peer;
			struct platform_device *pdev;
			struct tessai_dev *other;

			peer = of_parse_phandle(np, "amd,remoteproc", 0);
			if (!peer)
				continue;
			if (peer != rproc_anchor) {
				of_node_put(peer);
				continue;
			}
			of_node_put(peer);

			pdev = of_find_device_by_node(np);
			if (!pdev)
				continue;
			other = platform_get_drvdata(pdev);
			put_device(&pdev->dev);
			if (!other || other == tdev)
				continue;

			cmd = tessai_cmd_lookup(other, handle);
			if (cmd)
				return cmd;
		}
	}

	return tessai_cmd_lookup_via_platform_fallback(tdev, handle);
}

static struct tessai_cmd *tessai_cmd_alloc(struct tessai_dev *tdev,
				       struct drm_file *file, u32 op)
{
	struct tessai_cmd *cmd;
	struct dma_fence *fence;
	int id;
	int ret;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);

	cmd->tdev	 = tdev;
	cmd->owner	 = file;
	cmd->command_op	 = op;
	cmd->state	 = TESSAI_CMD_SUBMITTED;
	cmd->term_status = 0;
	mutex_init(&cmd->state_lock);
	spin_lock_init(&cmd->fence_lock);
	kref_init(&cmd->ref);

	fence = tessai_fence_create(tdev);
	if (!fence) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = drm_syncobj_create(&cmd->syncobj, 0, fence);
	if (ret) {
		dma_fence_put(fence);
		goto err_free;
	}
	cmd->pending_fence = fence;

	mutex_lock(&tdev->cmd_lock);
	id = idr_alloc(&tdev->cmd_idr, cmd, 1, (int)65536, GFP_KERNEL);
	mutex_unlock(&tdev->cmd_lock);
	if (id < 0) {
		ret = id;
		goto err_syncobj;
	}
	cmd->handle = id;
	return cmd;

err_syncobj:
	drm_syncobj_put(cmd->syncobj);
	cmd->syncobj = NULL;
	dma_fence_put(fence);
err_free:
	kfree(cmd);
	return ERR_PTR(ret);
}

static void tessai_rpmsg_try_abort_cmd(struct tessai_dev *tdev, struct tessai_cmd *cmd,
				    u32 op)
{
	unsigned int hid = cmd->handle;

	if (!hid)
		return;
	(void)tessai_rpmsg_send_cmd(tdev, TESSAI_OP_EXEC, (u16)hid, op,
				  TESSAI_WIRE_FLAG_ABORT, NULL,
				  0);
}

static bool tessai_cmd_idr_unlink(struct tessai_dev *tdev, struct tessai_cmd *cmd)
{
	bool had_slot = false;

	mutex_lock(&tdev->cmd_lock);
	if (cmd->handle > 0) {
		idr_remove(&tdev->cmd_idr, cmd->handle);
		cmd->handle = 0;
		had_slot = true;
	}
	mutex_unlock(&tdev->cmd_lock);
	return had_slot;
}

static void tessai_cmd_remove(struct tessai_dev *tdev, struct tessai_cmd *cmd)
{
	unsigned int hid = cmd->handle;

	mutex_lock(&tdev->cmd_lock);
	if (hid > 0)
		idr_remove(&tdev->cmd_idr, hid);
	cmd->handle = 0;
	mutex_unlock(&tdev->cmd_lock);

	dev_notice(tdev->dev,
		   "tessai-agent: cmd_remove ioctl err/aborted path hid=%u tdev=%p pid=%d comm=%s\n",
		   hid, tdev,
		   task_pid_nr(current),
		   current->comm);
}

/* Terminal completion: record status, signal and drop the lone pending fence. */
static void tessai_cmd_signal_completion(struct tessai_cmd *cmd, int term_status)
{
	struct dma_fence *old;
	unsigned long flags;

	(void)tessai_cmd_set_terminated(cmd, term_status);

	spin_lock_irqsave(&cmd->fence_lock, flags);
	old = cmd->pending_fence;
	cmd->pending_fence = NULL;
	spin_unlock_irqrestore(&cmd->fence_lock, flags);

	if (!old)
		return;

	if (term_status)
		dma_fence_set_error(old, term_status);
	dma_fence_signal(old);
	dma_fence_put(old);
}

/*
 * Fixed 24-byte RPMsg prefix (Zephyr management response header / opaque payload trail).
 * `status1` may be padded; opaque payload trailing the header may follow.
 */
struct tessai_rsp_wire_hdr {
	__le16 op_ctrl;
	__le16 op_app;
	__le32 command_rsp_type;
	__le32 status0;
	__le32 status1;
	__le32 payload_qwords;
	__le32 reserved;
} __packed;

static void tessai_diag_cmd_lookup_miss(struct tessai_dev *tdev, u32 wanted_corr,
				      const void *payload, size_t len,
				      u32 rtp, u32 firmware_status, u32 pq)
{
	struct tessai_cmd *oc;
	int oid, nprinted = 0;

	dev_notice(tdev->dev,
		   "tessai-agent: rsp miss wanted op_app(correlation)=%u type=%u firmware_status=%u pq=%u tdev=%p drm=%s platform=%s; listing idr on RPMsg tessai_dev:\n",
		   wanted_corr, rtp, firmware_status, pq, tdev,
		   dev_name(tdev->drm.dev),
		   dev_name(tdev->dev));

	mutex_lock(&tdev->cmd_lock);
	idr_for_each_entry(&tdev->cmd_idr, oc, oid) {
		dev_notice(tdev->dev,
			   "tessai-agent:   idr slot %d: op=%u state=%u\n",
			   oid, oc->command_op,
			   (unsigned int)oc->state);
		nprinted++;
		if (nprinted >= 24)
			break;
	}
	mutex_unlock(&tdev->cmd_lock);

	if (!nprinted)
		dev_notice(tdev->dev,
			   "tessai-agent: idr empty on RPMsg tessai_dev - wrong DRM/instance for this RPMsg bind, duplicate amd,tessai-agent nodes, or cmds were stripped (ioctl err path/postclose before response)\n");

	print_hex_dump(KERN_NOTICE, "tessai-agent: rsp payload: ",
		       DUMP_PREFIX_OFFSET, 16, 1,
		       payload, min(len, (size_t)64), true);
}

void tessai_cmd_dispatch_response(struct tessai_dev *tdev, const void *payload,
				size_t len)
{
	struct tessai_rsp_wire_hdr wh;
	struct tessai_cmd *cmd;
	u32 rsp_correlation;
	u32 rsp_type;
	u32 payload_qw;
	u32 fw_status0;
	const u8 *pay = (const u8 *)payload;
	int term_status;

	BUILD_BUG_ON(sizeof(struct tessai_rsp_wire_hdr) != 24);

	if (len < sizeof(wh))
		return;

	memcpy(&wh, pay, sizeof(wh));
	rsp_correlation = le16_to_cpu(wh.op_app);
	rsp_type = le32_to_cpu(wh.command_rsp_type);
	payload_qw = le32_to_cpu(wh.payload_qwords);
	fw_status0 = le32_to_cpu(wh.status0);

	(void)payload_qw;

	switch (rsp_type) {
	case TESSAI_RSP_COMPLETE:
		term_status = 0;
		break;
	case TESSAI_RSP_REMOTEIO:
		term_status = -EREMOTEIO;
		break;
	case TESSAI_RSP_NOTSUPPORTED:
		term_status = -EOPNOTSUPP;
		break;
	case TESSAI_RSP_CANCELLED:
		term_status = -ECANCELED;
		break;
	default:
		dev_err_ratelimited(tdev->dev,
				    "tessai-agent: rpm rsp unexpected command_rsp_type=%u op_app_corr=%u; completing with -EREMOTEIO\n",
				    rsp_type, rsp_correlation);
		term_status = -EREMOTEIO;
		break;
	}

	cmd = tessai_cmd_lookup_any_peer(tdev, rsp_correlation);
	if (!cmd) {
		tessai_diag_cmd_lookup_miss(tdev, rsp_correlation, payload, len,
					  rsp_type, fw_status0,
					  payload_qw);
		dev_notice_ratelimited(tdev->dev,
				       "tessai-agent: rpm rsp dropped (unknown correlation %u)\n",
				       rsp_correlation);
		return;
	}

	if (cmd->tdev != tdev) {
		dev_notice_ratelimited(cmd->tdev->dev,
				       "tessai-agent: rpm rsp cmd %u delivered on tessai_dev different from RPMsg binding (duplicate/fragment DT?)\n",
				       rsp_correlation);
	}

	tessai_cmd_signal_completion(cmd, term_status);
	tessai_cmd_idr_unlink(cmd->tdev, cmd);
	tessai_cmd_put(cmd);
	tessai_cmd_put(cmd);
}

void tessai_cmd_release_owner(struct tessai_dev *tdev, struct drm_file *file)
{
	struct tessai_cmd *cmd;
	int id;

	for (;;) {
		struct tessai_cmd *victim = NULL;

		mutex_lock(&tdev->cmd_lock);
		idr_for_each_entry(&tdev->cmd_idr, cmd, id) {
			if (cmd->owner != file)
				continue;
			dev_notice(tdev->dev,
				   "tessai-agent: drm fd closed with in-flight cmd id=%d op=%#x drm_file=%p - abort FW, dropping GEM refs; idr kept until RPMsg terminates (pid=%d comm=%s)\n",
				   id,
				   cmd->command_op,
				   file,
				   task_pid_nr(current),
				   current->comm);
			cmd->owner = NULL;
			victim = cmd;
			break;
		}
		mutex_unlock(&tdev->cmd_lock);
		if (!victim)
			break;
		tessai_rpmsg_try_abort_cmd(tdev, victim, victim->command_op);
		tessai_cmd_drop_bos(victim);
	}
}

int tessai_cmd_init(struct tessai_dev *tdev)
{
	mutex_init(&tdev->cmd_lock);
	idr_init(&tdev->cmd_idr);
	tdev->fence_context = dma_fence_context_alloc(1);
	atomic_set(&tdev->fence_seqno, 0);
	return 0;
}

/**
 * tessai_cmd_drain_idr - forcibly complete every command slot.
 * @err: dma_fence completion errno (negative).
 */
static void tessai_cmd_drain_idr(struct tessai_dev *tdev, int err)
{
	struct tessai_cmd *cmd = NULL;
	int id;

	mutex_lock(&tdev->cmd_lock);
	for (;;) {
		cmd = NULL;
		idr_for_each_entry(&tdev->cmd_idr, cmd, id) {
			idr_remove(&tdev->cmd_idr, id);
			cmd->handle = 0;
			break;
		}
		if (!cmd) {
			mutex_unlock(&tdev->cmd_lock);
			break;
		}
		mutex_unlock(&tdev->cmd_lock);

		tessai_cmd_signal_completion(cmd, err);
		tessai_cmd_put(cmd);
		mutex_lock(&tdev->cmd_lock);
	}
}

void tessai_cmd_abort_inflight_rpmsg_detach(struct tessai_dev *tdev, int err)
{
	if (!err)
		err = -ENOTCONN;
	tessai_cmd_drain_idr(tdev, err);
}

void tessai_cmd_fini(struct tessai_dev *tdev)
{
	tessai_cmd_drain_idr(tdev, -ENODEV);
	idr_destroy(&tdev->cmd_idr);
}

static int tessai_resolve_args(struct drm_file *file, struct tessai_cmd *cmd,
			     struct tessai_arg *args, u32 num_args,
			     __le64 *out_words)
{
	u32 i;

	for (i = 0; i < num_args; i++) {
		struct tessai_arg *a = &args[i];

		if (a->size == 0) {
			out_words[i] = cpu_to_le64(a->value);
		} else {
			struct drm_gem_object *obj;
			struct tessai_gem *bo;
			u64 dma;

			obj = drm_gem_object_lookup(file, a->bo.drm_buffer_handle);
			if (!obj)
				return -ENOENT;
			bo = to_tessai_gem(obj);
			if ((u64)a->bo.offset + a->size > bo->size) {
				drm_gem_object_put(obj);
				return -EINVAL;
			}
			dma = (u64)bo->dma_addr + a->bo.offset;
			out_words[i] = cpu_to_le64(dma);
			cmd->bo_refs[cmd->num_bo_refs++] = obj;
		}
	}
	return 0;
}

long tessai_ioctl_cmd_submit(struct drm_device *ddev, void __user *uarg,
			   struct drm_file *file)
{
	struct tessai_dev *tdev = to_tessai_dev(ddev);
	struct tessai_cmd_submit uhdr;
	struct tessai_arg *args = NULL;
	__le64 *words = NULL;
	struct tessai_cmd *cmd;
	u32 syncobj_handle = 0;
	long ret;
	size_t args_bytes;

	if (copy_from_user(&uhdr, uarg, TESSAI_SUBMIT_ABI_HDR_SZ))
		return -EFAULT;
	if (uhdr.drm_sync_obj_handle)
		return -EINVAL;
	if (uhdr.num_args > TESSAI_MAX_ARGS)
		return -E2BIG;

	args_bytes = (size_t)uhdr.num_args * sizeof(*args);
	if (uhdr.num_args) {
		args = kvmalloc(args_bytes, GFP_KERNEL);
		if (!args)
			return -ENOMEM;
		if (copy_from_user(args,
				   (u8 __user *)uarg + TESSAI_SUBMIT_ABI_HDR_SZ,
				   args_bytes)) {
			ret = -EFAULT;
			goto out_free_args;
		}
	}

	cmd = tessai_cmd_alloc(tdev, file, uhdr.command_op);
	if (IS_ERR(cmd)) {
		ret = PTR_ERR(cmd);
		goto out_free_args;
	}

	if (uhdr.num_args) {
		cmd->bo_refs = kcalloc(uhdr.num_args, sizeof(*cmd->bo_refs),
				       GFP_KERNEL);
		if (!cmd->bo_refs) {
			ret = -ENOMEM;
			goto out_remove_cmd;
		}
		words = kvmalloc(uhdr.num_args * sizeof(*words), GFP_KERNEL);
		if (!words) {
			ret = -ENOMEM;
			goto out_remove_cmd;
		}
		ret = tessai_resolve_args(file, cmd, args, uhdr.num_args, words);
		if (ret)
			goto out_remove_cmd;
	}

	ret = tessai_rpmsg_send_cmd(tdev, TESSAI_OP_EXEC, (u16)cmd->handle,
				  uhdr.command_op, 0, words, uhdr.num_args);
	if (ret)
		goto out_remove_cmd;

	ret = drm_syncobj_get_handle(file, cmd->syncobj, &syncobj_handle);
	if (ret) {
		tessai_rpmsg_try_abort_cmd(tdev, cmd, uhdr.command_op);
		goto out_remove_cmd;
	}

	uhdr.drm_sync_obj_handle = syncobj_handle;
	if (copy_to_user(uarg, &uhdr, TESSAI_SUBMIT_ABI_HDR_SZ)) {
		tessai_rpmsg_try_abort_cmd(tdev, cmd, uhdr.command_op);
		tessai_cmd_signal_completion(cmd, -EFAULT);
		ret = -EFAULT;
		goto out_remove_cmd;
	}

	dev_dbg(tdev->dev,
		"TESSAI_CMD_SUBMIT ok: cmd_id=%u op=%#x tdev=%p drm=%s platform=%s minors P=%d R=%d pid=%d comm=%s\n",
		cmd->handle, uhdr.command_op, tdev,
		dev_name(ddev->dev),
		dev_name(tdev->dev),
		ddev->primary ? ddev->primary->index : -1,
		ddev->render ? ddev->render->index : -1,
		task_pid_nr(current),
		current->comm);

	kvfree(words);
	kvfree(args);
	return 0;

out_remove_cmd:
	tessai_cmd_remove(tdev, cmd);
	tessai_cmd_put(cmd);
	kvfree(words);
out_free_args:
	kvfree(args);
	return ret;
}
