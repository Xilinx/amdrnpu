// SPDX-License-Identifier: GPL-2.0
/*
 * tessai-agent - DRM front-end and GEM buffer management.
 *
 * The driver registers a DRM render node so that userspace gets:
 *   - per-process DRM handle space and GEM mmap dispatch for free,
 *   - the standard drm_syncobj ioctls (DRM_IOCTL_SYNCOBJ_*) and PRIME
 *     ioctls in addition to our custom ones, and
 *   - dma-buf import/export plumbing.
 *
 * Custom ioctls use magic 'T' for TESSAI_BUF_ALLOC and TESSAI_CMD_SUBMIT only.
 * PRIME import/export and drm_syncobj use the core DRM ioctl path ('d').
 *
 * GEM allocations are NOT done with the drm_gem_dma helpers because
 * each buffer needs to be attached to a *bank* device (RPU 32-bit or
 * dev 64-bit) rather than the DRM device itself.  We implement the GEM
 * ops directly on top of dma_alloc_coherent() and use
 * drm_gem_create_mmap_offset() to expose the result via mmap().
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mode.h>
#include <drm/drm_prime.h>

#include "tessai_agent_internal.h"

/* --------------------------------------------------------------- */
/* GEM object lifecycle                                            */
/* --------------------------------------------------------------- */

static const struct drm_gem_object_funcs tessai_gem_funcs;

static struct tessai_gem *tessai_gem_alloc(struct tessai_dev *tdev,
				       struct tessai_bank *bank,
				       size_t size)
{
	struct tessai_gem *bo;
	int ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	bo->bank = bank;
	bo->size = PAGE_ALIGN(size);
	bo->base.funcs = &tessai_gem_funcs;

	ret = drm_gem_object_init(&tdev->drm, &bo->base, bo->size);
	if (ret) {
		kfree(bo);
		return ERR_PTR(ret);
	}
	return bo;
}

static void tessai_gem_free(struct drm_gem_object *obj)
{
	struct tessai_gem *bo = to_tessai_gem(obj);

	if (obj->import_attach) {
		if (bo->sgt)
			dma_buf_unmap_attachment(obj->import_attach,
						 bo->sgt, DMA_BIDIRECTIONAL);
		drm_prime_gem_destroy(obj, bo->sgt);
	} else if (bo->kvaddr) {
		dma_free_coherent(bo->bank->dev, bo->size,
				  bo->kvaddr, bo->dma_addr);
	}

	drm_gem_object_release(obj);
	kfree(bo);
}

static int tessai_gem_mmap_native(struct tessai_gem *bo, struct vm_area_struct *vma)
{
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (vsize > bo->size)
		return -EINVAL;

	vm_flags_clear(vma, VM_PFNMAP);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_pgoff = 0;

	return dma_mmap_coherent(bo->bank->dev, vma,
				 bo->kvaddr, bo->dma_addr, vsize);
}

static int tessai_gem_obj_mmap(struct drm_gem_object *obj,
			     struct vm_area_struct *vma)
{
	struct tessai_gem *bo = to_tessai_gem(obj);

	if (obj->import_attach) {
		vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
		return dma_buf_mmap(obj->dma_buf, vma, 0);
	}
	return tessai_gem_mmap_native(bo, vma);
}

/*
 * drm_gem_mmap_obj() takes a refcount and assigns vma->vm_ops from here.
 * vm_close must run on unmap to drm_gem_object_put(); open handles dup/fork.
 */
static const struct vm_operations_struct tessai_gem_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int tessai_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct tessai_gem *bo = to_tessai_gem(obj);

	if (obj->import_attach)
		return dma_buf_vmap(obj->dma_buf, map);

	if (!bo->kvaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, bo->kvaddr);
	return 0;
}

static void tessai_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	if (obj->import_attach)
		dma_buf_vunmap(obj->dma_buf, map);
}

static struct sg_table *tessai_gem_get_sg_table(struct drm_gem_object *obj)
{
	struct tessai_gem *bo = to_tessai_gem(obj);
	struct sg_table *sgt;
	int ret;

	if (obj->import_attach)
		return ERR_PTR(-EINVAL);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(bo->bank->dev, sgt, bo->kvaddr,
			      bo->dma_addr, bo->size);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static const struct drm_gem_object_funcs tessai_gem_funcs = {
	.free		= tessai_gem_free,
	.mmap		= tessai_gem_obj_mmap,
	.vm_ops		= &tessai_gem_vm_ops,
	.vmap		= tessai_gem_vmap,
	.vunmap		= tessai_gem_vunmap,
	.get_sg_table	= tessai_gem_get_sg_table,
	.export		= drm_gem_prime_export,
};

/* --------------------------------------------------------------- */
/* dma-buf import path                                             */
/* --------------------------------------------------------------- */

static struct drm_gem_object *
tessai_gem_prime_import_sg_table(struct drm_device *ddev,
			       struct dma_buf_attachment *attach,
			       struct sg_table *sgt)
{
	struct tessai_dev *tdev = to_tessai_dev(ddev);
	struct tessai_bank *bank;
	struct tessai_gem *bo;
	int ret;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	bank = tessai_bank_by_region(tdev, TESSAI_BANK_DEV);
	if (!bank) {
		kfree(bo);
		return ERR_PTR(-ENODEV);
	}

	bo->base.funcs = &tessai_gem_funcs;
	bo->bank = bank;
	bo->size = attach->dmabuf->size;
	bo->sgt  = sgt;
	bo->dma_addr = sg_dma_address(sgt->sgl);

	ret = drm_gem_object_init(ddev, &bo->base, bo->size);
	if (ret) {
		kfree(bo);
		return ERR_PTR(ret);
	}
	return &bo->base;
}

/* --------------------------------------------------------------- */
/* Buffer ioctls                                                   */
/* --------------------------------------------------------------- */

static int tessai_buf_alloc(struct drm_device *ddev, struct tessai_buf_alloc *args,
			  struct drm_file *file)
{
	struct tessai_dev *tdev = to_tessai_dev(ddev);
	struct tessai_bank *bank;
	struct tessai_gem *bo;
	u32 handle;
	int ret;

	if (args->flags || args->reserved0)
		return -EINVAL;
	if (!args->size || args->size > SZ_2G)
		return -EINVAL;

	bank = tessai_bank_memory_type(tdev, args->memory_type);
	if (!bank)
		return -ENODEV;

	if (args->memory_type == TESSAI_MEMORY_RPU && !bank->has_reserved_mem) {
		dev_err(tdev->dev,
			"TESSAI_MEMORY_RPU needs a memory-region phandle on bank@0\n");
		return -ENXIO;
	}

	bo = tessai_gem_alloc(tdev, bank, args->size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	bo->kvaddr = dma_alloc_coherent(bank->dev, bo->size,
					&bo->dma_addr, GFP_KERNEL);
	if (!bo->kvaddr) {
		ret = -ENOMEM;
		goto err_release;
	}

	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret)
		goto err_free_dma;

	ret = drm_gem_handle_create(file, &bo->base, &handle);
	drm_gem_object_put(&bo->base);
	if (ret)
		return ret;

	args->drm_handle = handle;
	return 0;

err_free_dma:
	dma_free_coherent(bank->dev, bo->size, bo->kvaddr, bo->dma_addr);
	bo->kvaddr = NULL;
err_release:
	drm_gem_object_put(&bo->base);
	return ret;
}

/* --------------------------------------------------------------- */
/* Custom ioctl dispatch                                           */
/* --------------------------------------------------------------- */

/*
 * tessai_drm_ioctl() handles our 'T'-magic ioctls and delegates everything
 * else to drm_ioctl(), which in turn handles DRM core, syncobj, and
 * PRIME ioctls.
 */
static long tessai_drm_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct drm_file *file = filp->private_data;
	struct drm_device *ddev = file->minor->dev;
	void __user *uarg = (void __user *)arg;
	int ret;

	if (_IOC_TYPE(cmd) != TESSAI_IOC_MAGIC)
		return drm_ioctl(filp, cmd, arg);

	if (drm_dev_is_unplugged(ddev))
		return -ENODEV;

	switch (cmd) {
	case TESSAI_BUF_ALLOC: {
		struct tessai_buf_alloc args;

		if (copy_from_user(&args, uarg, sizeof(args)))
			return -EFAULT;
		ret = tessai_buf_alloc(ddev, &args, file);
		if (!ret && copy_to_user(uarg, &args, sizeof(args)))
			ret = -EFAULT;
		return ret;
	}
	case TESSAI_CMD_SUBMIT:
		return tessai_ioctl_cmd_submit(ddev, uarg, file);

	default:
		return -ENOTTY;
	}
}

/* --------------------------------------------------------------- */
/* DRM driver wiring                                               */
/* --------------------------------------------------------------- */

/*
 * drm_mode_mmap_dumb_ioctl() gates on dumb_create != NULL even when the only
 * goal is MODE_MAP_DUMB for a generic GEM handle (e.g. from TESSAI_BUF_ALLOC).
 * We do not implement legacy KMS dumb allocation; handles come from IOCTLs
 * and PRIME.  Mapping uses the core drm_gem_dumb_map_offset() helper.
 */
static int tessai_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	return -EOPNOTSUPP;
}

static const struct file_operations tessai_drm_fops = {
	.fop_flags	= FOP_UNSIGNED_OFFSET,
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= tessai_drm_ioctl,
	.compat_ioctl	= tessai_drm_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.llseek		= noop_llseek,
	.mmap		= drm_gem_mmap,
};

static void tessai_drm_postclose(struct drm_device *ddev, struct drm_file *file)
{
	struct tessai_dev *tdev = to_tessai_dev(ddev);

	tessai_cmd_release_owner(tdev, file);
}

const struct drm_driver tessai_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.fops			= &tessai_drm_fops,
	.postclose		= tessai_drm_postclose,

	.dumb_create		= tessai_dumb_create,
	.dumb_map_offset	= drm_gem_dumb_map_offset,

	.gem_prime_import_sg_table = tessai_gem_prime_import_sg_table,

	.name			= TESSAI_DRV_NAME,
	.desc			= "AMD tessai RPU management driver",
	.date			= "20260430",
	.major			= 1,
	.minor			= 0,
};

/*
 * The drm_device is allocated in the platform probe via
 * devm_drm_dev_alloc() so its lifetime is tied to the parent device.
 * tessai_drm_register() therefore only needs to publish the device.
 */
int tessai_drm_register(struct tessai_dev *tdev)
{
	return drm_dev_register(&tdev->drm, 0);
}

void tessai_drm_unregister(struct tessai_dev *tdev)
{
	drm_dev_unregister(&tdev->drm);
}
