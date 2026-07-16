// SPDX-License-Identifier: GPL-2.0
/*
 * amdrnpu - command lifecycle and a single-stage dma_fence on drm_syncobj.
 *
 * Submit allocates a command id in cmd_xa and returns a drm_syncobj handle.
 * Userspace waits/queries only that syncobj.  A terminal RPMsg response (or
 * ioctl/fd teardown) xa_erases the slot, signals the fence (with errno on fault),
 * and drops the command object.
 */

#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/printk.h>
#include <linux/xarray.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/stddef.h>
#include <linux/uaccess.h>

#include <drm/drm_syncobj.h>

#include "amdrnpu_internal.h"
#include "amdrnpu_trace.h"

/* --------------------------------------------------------------- */
/* Custom dma_fence                                                */
/* --------------------------------------------------------------- */

struct amdrnpu_fence {
	struct dma_fence	base;
	spinlock_t		lock;	/* dma_fence lock */
};

static const char *amdrnpu_fence_get_driver_name(struct dma_fence *fence)
{
	return AMDRNPU_DRV_NAME;
}

static const char *amdrnpu_fence_get_timeline_name(struct dma_fence *fence)
{
	return "tess-event";
}

static const struct dma_fence_ops amdrnpu_fence_ops = {
	.get_driver_name	= amdrnpu_fence_get_driver_name,
	.get_timeline_name	= amdrnpu_fence_get_timeline_name,
};

static struct dma_fence *amdrnpu_fence_create(struct amdrnpu_dev *tdev)
{
	struct amdrnpu_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &amdrnpu_fence_ops, &f->lock,
		       tdev->fence_context,
		       atomic_inc_return(&tdev->fence_seqno));
	return &f->base;
}

static void amdrnpu_cmd_drop_bos(struct amdrnpu_cmd *cmd)
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

static bool amdrnpu_cmd_set_terminated(struct amdrnpu_cmd *cmd, int term_status)
{
	bool first;

	mutex_lock(&cmd->state_lock);
	first = (cmd->state != AMDRNPU_CMD_TERMINATED);
	if (first) {
		cmd->state = AMDRNPU_CMD_TERMINATED;
		cmd->term_status = term_status;
	}
	mutex_unlock(&cmd->state_lock);

	if (first)
		amdrnpu_cmd_drop_bos(cmd);
	return first;
}

static void amdrnpu_cmd_release(struct kref *ref)
{
	struct amdrnpu_cmd *cmd = container_of(ref, struct amdrnpu_cmd, ref);
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

	amdrnpu_cmd_drop_bos(cmd);
	kfree(cmd->bo_refs);
	kfree(cmd);
}

static void amdrnpu_cmd_put(struct amdrnpu_cmd *cmd) { kref_put(&cmd->ref, amdrnpu_cmd_release); }

/* Remove @seq from cmd_xa; NULL if unknown or already completed. */
static struct amdrnpu_cmd *amdrnpu_cmd_xa_take(struct amdrnpu_dev *tdev, u32 seq)
{
	struct amdrnpu_cmd *cmd;

	mutex_lock(&tdev->cmd_lock);
	cmd = xa_erase(&tdev->cmd_xa, seq);
	if (cmd) {
		cmd->in_cmd_xa = false;
		cmd->seq = AMDRNPU_CMD_SEQ_NONE;
	}
	mutex_unlock(&tdev->cmd_lock);
	return cmd;
}

/*
 * Allocate a wire command id with xa_alloc_cyclic (xa_limit_32b, reuse when free).
 */
static int amdrnpu_cmd_xa_alloc_id(struct amdrnpu_dev *tdev, struct amdrnpu_cmd *cmd)
{
	u32 id;
	int err;

	mutex_lock(&tdev->cmd_lock);
	err = xa_alloc_cyclic(&tdev->cmd_xa, &id, cmd, xa_limit_32b,
			      &tdev->cmd_xa_next, GFP_KERNEL);
	mutex_unlock(&tdev->cmd_lock);
	if (err)
		return err;
	cmd->seq = id;
	cmd->in_cmd_xa = true;
	return 0;
}

static struct amdrnpu_cmd *amdrnpu_cmd_alloc(struct amdrnpu_dev *tdev,
					   struct drm_file *file, u32 op)
{
	struct amdrnpu_cmd *cmd;
	struct dma_fence *fence;
	int ret;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);

	cmd->tdev	 = tdev;
	cmd->owner	 = file;
	cmd->command_op	 = op;
	cmd->state	 = AMDRNPU_CMD_SUBMITTED;
	cmd->term_status = 0;
	mutex_init(&cmd->state_lock);
	spin_lock_init(&cmd->fence_lock);
	kref_init(&cmd->ref);

	fence = amdrnpu_fence_create(tdev);
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
	cmd->seq = AMDRNPU_CMD_SEQ_NONE;
	cmd->in_cmd_xa = false;

	ret = amdrnpu_cmd_xa_alloc_id(tdev, cmd);
	if (ret)
		goto err_syncobj;
	return cmd;

err_syncobj:
	drm_syncobj_put(cmd->syncobj);
	cmd->syncobj = NULL;
	dma_fence_put(fence);
err_free:
	kfree(cmd);
	return ERR_PTR(ret);
}

/* Terminal completion: record status, signal and drop the lone pending fence. */
static void amdrnpu_cmd_signal_completion(struct amdrnpu_cmd *cmd, int term_status)
{
	struct dma_fence *old;
	unsigned long flags;

	(void)amdrnpu_cmd_set_terminated(cmd, term_status);

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

/* Erase from cmd_xa (if present), signal syncobj fence, drop cmd ref. */
static void amdrnpu_cmd_finish(struct amdrnpu_dev *tdev, struct amdrnpu_cmd *cmd,
			      int term_status)
{
	u32 seq = cmd->seq;

	mutex_lock(&tdev->cmd_lock);
	if (cmd->in_cmd_xa) {
		xa_erase(&tdev->cmd_xa, seq);
		cmd->in_cmd_xa = false;
	}
	cmd->seq = AMDRNPU_CMD_SEQ_NONE;
	mutex_unlock(&tdev->cmd_lock);

	amdrnpu_cmd_signal_completion(cmd, term_status);
	amdrnpu_cmd_put(cmd);
}

static void amdrnpu_diag_cmd_take_miss(struct amdrnpu_dev *tdev, u32 wanted_seq,
				      const void *payload, size_t len,
				      u32 rtp)
{
	struct amdrnpu_cmd *oc;
	unsigned long index;
	unsigned int nprinted = 0;

	dev_notice(tdev->dev,
		   "amdrnpu: rsp miss wanted seq=%u type=%u tdev=%p drm=%s platform=%s; listing cmd_xa on RPMsg amdrnpu_dev:\n",
		   wanted_seq, rtp, tdev,
		   dev_name(tdev->drm.dev),
		   dev_name(tdev->dev));

	mutex_lock(&tdev->cmd_lock);
	xa_for_each(&tdev->cmd_xa, index, oc) {
		dev_notice(tdev->dev,
			   "amdrnpu:   seq %lu: op=%u state=%u\n",
			   index, oc->command_op,
			   (unsigned int)oc->state);
		nprinted++;
		if (nprinted >= 24)
			break;
	}
	mutex_unlock(&tdev->cmd_lock);

	if (!nprinted)
		dev_notice(tdev->dev,
			   "amdrnpu: cmd_xa empty - seq already completed or stripped (ioctl err / fd close before this rsp)\n");

	print_hex_dump(KERN_NOTICE, "amdrnpu: rsp payload: ",
		       DUMP_PREFIX_OFFSET, 16, 1,
		       payload, min(len, (size_t)64), true);
}

void amdrnpu_cmd_dispatch_response(struct amdrnpu_dev *tdev, const void *payload,
				  size_t len)
{
	struct amdrnpu_wire_rsp wr;
	struct amdrnpu_cmd *cmd;
	u32 rsp_seq;
	u32 rsp_type;
	int term_status;

	if (len < sizeof(wr))
		return;

	memcpy(&wr, payload, sizeof(wr));
	rsp_seq = le32_to_cpu(wr.seq);
	rsp_type = le32_to_cpu(wr.command_rsp_type);

	switch (rsp_type) {
	case AMDRNPU_RSP_COMPLETE:
		term_status = 0;
		break;
	case AMDRNPU_RSP_REMOTEIO:
		term_status = -EREMOTEIO;
		break;
	case AMDRNPU_RSP_NOTSUPPORTED:
		term_status = -EOPNOTSUPP;
		break;
	case AMDRNPU_RSP_CANCELLED:
		term_status = -ECANCELED;
		break;
	default:
		dev_err_ratelimited(tdev->dev,
				    "amdrnpu: rpm rsp unexpected command_rsp_type=%u seq=%u; completing with -EREMOTEIO\n",
				    rsp_type, rsp_seq);
		term_status = -EREMOTEIO;
		break;
	}

	cmd = amdrnpu_cmd_xa_take(tdev, rsp_seq);
	if (!cmd) {
		amdrnpu_diag_cmd_take_miss(tdev, rsp_seq, payload, len, rsp_type);
		dev_notice_ratelimited(tdev->dev,
				       "amdrnpu: rpm rsp dropped (unknown seq %u)\n",
				       rsp_seq);
		return;
	}

	amdrnpu_cmd_finish(tdev, cmd, term_status);
}

void amdrnpu_cmd_release_owner(struct amdrnpu_dev *tdev, struct drm_file *file)
{
	struct amdrnpu_cmd *cmd;
	unsigned long index;

	for (;;) {
		struct amdrnpu_cmd *victim = NULL;

		mutex_lock(&tdev->cmd_lock);
		xa_for_each(&tdev->cmd_xa, index, cmd) {
			if (cmd->owner != file)
				continue;
			dev_notice(tdev->dev,
				   "amdrnpu: drm fd closed with in-flight cmd seq=%lu op=%#x drm_file=%p - signal fence, drop GEM refs (pid=%d comm=%s)\n",
				   index,
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
		amdrnpu_cmd_drop_bos(victim);
		amdrnpu_cmd_finish(tdev, victim, -ECONNRESET);
	}
}

int amdrnpu_cmd_init(struct amdrnpu_dev *tdev)
{
	mutex_init(&tdev->cmd_lock);
	xa_init_flags(&tdev->cmd_xa, XA_FLAGS_ALLOC);
	tdev->cmd_xa_next = 0;
	tdev->fence_context = dma_fence_context_alloc(1);
	atomic_set(&tdev->fence_seqno, 0);
	return 0;
}

/**
 * amdrnpu_cmd_drain_xa - forcibly complete every in-flight command.
 * @err: dma_fence completion errno (negative).
 */
static void amdrnpu_cmd_drain_xa(struct amdrnpu_dev *tdev, int err)
{
	struct amdrnpu_cmd *cmd = NULL;
	unsigned long index;

	for (;;) {
		cmd = NULL;
		mutex_lock(&tdev->cmd_lock);
		xa_for_each(&tdev->cmd_xa, index, cmd) {
			xa_erase(&tdev->cmd_xa, index);
			cmd->in_cmd_xa = false;
			cmd->seq = AMDRNPU_CMD_SEQ_NONE;
			break;
		}
		mutex_unlock(&tdev->cmd_lock);
		if (!cmd)
			break;

		amdrnpu_cmd_signal_completion(cmd, err);
		amdrnpu_cmd_put(cmd);
	}
}

void amdrnpu_cmd_drain_inflight_rpmsg_detach(struct amdrnpu_dev *tdev, int err)
{
	if (!err)
		err = -ENOTCONN;
	amdrnpu_cmd_drain_xa(tdev, err);
}

void amdrnpu_cmd_fini(struct amdrnpu_dev *tdev)
{
	amdrnpu_cmd_drain_xa(tdev, -ENODEV);
	xa_destroy(&tdev->cmd_xa);
}

static int amdrnpu_resolve_args(struct drm_file *file, struct amdrnpu_cmd *cmd,
			       struct amdrnpu_arg *args, u32 num_args,
			     __le64 *out_words)
{
	u32 i;

	for (i = 0; i < num_args; i++) {
		struct amdrnpu_arg *a = &args[i];

		if (a->size == 0) {
			out_words[i] = cpu_to_le64(a->value);
		} else {
			struct drm_gem_object *obj;
			struct amdrnpu_gem *bo;
			u64 dma;

			obj = drm_gem_object_lookup(file, a->bo.drm_buffer_handle);
			if (!obj)
				return -ENOENT;
			bo = to_amdrnpu_gem(obj);
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

long amdrnpu_ioctl_cmd_submit(struct drm_device *ddev, void __user *uarg,
			     struct drm_file *file)
{
	struct amdrnpu_dev *tdev = to_amdrnpu_dev(ddev);
	struct amdrnpu_cmd_submit uhdr;
	struct amdrnpu_arg *args = NULL;
	__le64 *words = NULL;
	struct amdrnpu_cmd *cmd;
	u32 syncobj_handle = 0;
	long ret;
	size_t args_bytes;

	if (copy_from_user(&uhdr, uarg, AMDRNPU_SUBMIT_ABI_HDR_SZ))
		return -EFAULT;
	trace_amdrnpu_cmd_submit_enter(uhdr.command_op, uhdr.num_args);
	if (uhdr.drm_sync_obj_handle)
		return -EINVAL;
	if (uhdr.num_args > AMDRNPU_MAX_ARGS)
		return -E2BIG;

	args_bytes = (size_t)uhdr.num_args * sizeof(*args);
	if (uhdr.num_args) {
		args = kvmalloc(args_bytes, GFP_KERNEL);
		if (!args)
			return -ENOMEM;
		if (copy_from_user(args,
				   (u8 __user *)uarg + AMDRNPU_SUBMIT_ABI_HDR_SZ,
				   args_bytes)) {
			ret = -EFAULT;
			goto out_free_args;
		}
	}

	cmd = amdrnpu_cmd_alloc(tdev, file, uhdr.command_op);
	if (IS_ERR(cmd)) {
		ret = PTR_ERR(cmd);
		goto out_free_args;
	}

	if (uhdr.num_args) {
		cmd->bo_refs = kcalloc(uhdr.num_args, sizeof(*cmd->bo_refs),
				       GFP_KERNEL);
		if (!cmd->bo_refs) {
			ret = -ENOMEM;
			goto out_finish_cmd;
		}
		words = kvmalloc(uhdr.num_args * sizeof(*words), GFP_KERNEL);
		if (!words) {
			ret = -ENOMEM;
			goto out_finish_cmd;
		}
		ret = amdrnpu_resolve_args(file, cmd, args, uhdr.num_args, words);
		if (ret)
			goto out_finish_cmd;
	}

	ret = amdrnpu_rpmsg_send_cmd(tdev, cmd->seq, uhdr.command_op, 0,
				    words, uhdr.num_args);
	if (ret)
		goto out_finish_cmd;

	ret = drm_syncobj_get_handle(file, cmd->syncobj, &syncobj_handle);
	if (ret)
		goto out_finish_cmd;

	uhdr.drm_sync_obj_handle = syncobj_handle;
	if (copy_to_user(uarg, &uhdr, AMDRNPU_SUBMIT_ABI_HDR_SZ)) {
		ret = -EFAULT;
		goto out_finish_cmd;
	}

	dev_dbg(tdev->dev,
		"AMDRNPU_CMD_SUBMIT ok: seq=%u op=%#x tdev=%p drm=%s platform=%s minors P=%d R=%d pid=%d comm=%s\n",
		cmd->seq, uhdr.command_op, tdev,
		dev_name(ddev->dev),
		dev_name(tdev->dev),
		ddev->primary ? ddev->primary->index : -1,
		ddev->render ? ddev->render->index : -1,
		task_pid_nr(current),
		current->comm);

	trace_amdrnpu_cmd_submit_exit(cmd->seq, syncobj_handle, 0);
	kvfree(words);
	kvfree(args);
	return 0;

out_finish_cmd:
	trace_amdrnpu_cmd_submit_exit(cmd->seq, 0, (int)ret);
	amdrnpu_cmd_finish(tdev, cmd, ret < 0 ? (int)ret : -EIO);
	kvfree(words);
out_free_args:
	kvfree(args);
	return ret;
}
