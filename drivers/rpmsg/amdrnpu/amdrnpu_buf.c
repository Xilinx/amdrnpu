// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include "amdrnpu_internal.h"

MODULE_INFO(import_ns, "DMA_BUF");

static int amdrnpu_sgt_dma_contig(struct sg_table *sgt, dma_addr_t *base,
				  size_t *len)
{
	struct scatterlist *sg;
	dma_addr_t expect;
	size_t total = 0;
	unsigned int i;

	if (!sgt->nents)
		return -EINVAL;

	*base = sg_dma_address(sgt->sgl);
	expect = *base;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_dma_address(sg) != expect)
			return -EINVAL;
		expect += sg_dma_len(sg);
		total  += sg_dma_len(sg);
	}
	*len = total;
	return 0;
}

static void amdrnpu_buf_release(struct kref *ref)
{
	struct amdrnpu_buf *buf = container_of(ref, struct amdrnpu_buf, ref);

	if (buf->attach) {
		dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
						  DMA_BIDIRECTIONAL);
		dma_buf_detach(buf->dmabuf, buf->attach);
	}
	dma_buf_put(buf->dmabuf);
	kfree(buf);
}

/*
 * Look up @handle in @client's table and return the buffer with an extra
 * reference, or NULL.  The table's own reference keeps the buffer alive under
 * xa_lock, so a plain kref_get is safe here.
 */
struct amdrnpu_buf *amdrnpu_buf_get(struct amdrnpu_client *client, u32 handle)
{
	struct amdrnpu_buf *buf;

	xa_lock(&client->buf_xa);
	buf = xa_load(&client->buf_xa, handle);
	if (buf)
		kref_get(&buf->ref);
	xa_unlock(&client->buf_xa);
	return buf;
}

void amdrnpu_buf_put(struct amdrnpu_buf *buf)
{
	kref_put(&buf->ref, amdrnpu_buf_release);
}

/* Release every buffer still held by @client (called on file close). */
void amdrnpu_client_buf_release(struct amdrnpu_client *client)
{
	struct amdrnpu_buf *buf;
	unsigned long idx;

	xa_for_each(&client->buf_xa, idx, buf) {
		xa_erase(&client->buf_xa, idx);
		amdrnpu_buf_put(buf);
	}
}

int amdrnpu_ioctl_buf_import(struct amdrnpu_client *client, void __user *uarg)
{
	struct amdrnpu_dev *rnpu = client->rnpu;
	struct amdrnpu_buf_import arg;
	struct device *attach_dev;
	struct amdrnpu_buf *buf;
	struct dma_buf *dmabuf;
	dma_addr_t base;
	size_t len;
	u32 handle;
	int ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags & ~AMDRNPU_BUF_IMPORT_FLAGS_ALL)
		return -EINVAL;
	if (arg.handle || arg.pad)
		return -EINVAL;
	if (arg.fd < 0)
		return -EINVAL;

	if (arg.flags & AMDRNPU_BUF_IMPORT_FW) {
		attach_dev = rnpu->rproc_dev;
		if (!attach_dev)
			return -ENODEV;
	} else {
		attach_dev = rnpu->dev;
	}

	dmabuf = dma_buf_get(arg.fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_put_dmabuf;
	}
	buf->dmabuf = dmabuf;
	kref_init(&buf->ref);

	buf->attach = dma_buf_attach(dmabuf, attach_dev);
	if (IS_ERR(buf->attach)) {
		ret = PTR_ERR(buf->attach);
		goto err_free_buf;
	}

	buf->sgt = dma_buf_map_attachment_unlocked(buf->attach,
						   DMA_BIDIRECTIONAL);
	if (IS_ERR(buf->sgt)) {
		ret = PTR_ERR(buf->sgt);
		goto err_detach;
	}

	ret = amdrnpu_sgt_dma_contig(buf->sgt, &base, &len);
	if (ret) {
		RNPU_ERR(rnpu, "buf import: buffer is not physically contiguous\n");
		goto err_unmap;
	}
	buf->dma_addr = base;
	buf->size = len;

	ret = xa_alloc(&client->buf_xa, &handle, buf, xa_limit_31b, GFP_KERNEL);
	if (ret < 0)
		goto err_unmap;

	arg.handle = handle;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		xa_erase(&client->buf_xa, handle);
		amdrnpu_buf_put(buf);
		return -EFAULT;
	}
	return 0;

err_unmap:
	dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
					  DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dmabuf, buf->attach);
err_free_buf:
	kfree(buf);
err_put_dmabuf:
	dma_buf_put(dmabuf);
	return ret;
}

int amdrnpu_ioctl_buf_destroy(struct amdrnpu_client *client, void __user *uarg)
{
	struct amdrnpu_buf_destroy arg;
	struct amdrnpu_buf *buf;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.pad)
		return -EINVAL;

	buf = xa_erase(&client->buf_xa, arg.handle);
	if (!buf)
		return -ENOENT;

	amdrnpu_buf_put(buf);
	return 0;
}

int amdrnpu_ioctl_buf_query(struct amdrnpu_client *client, void __user *uarg)
{
	struct amdrnpu_buf_query arg;
	struct amdrnpu_buf *buf;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.pad || arg.dma_addr || arg.size)
		return -EINVAL;

	buf = amdrnpu_buf_get(client, arg.handle);
	if (!buf)
		return -ENOENT;

	arg.dma_addr = (u64)buf->dma_addr;
	arg.size = (u64)buf->size;
	amdrnpu_buf_put(buf);

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;
	return 0;
}

/* Exported as a single scatterlist segment. */
#define AMDRNPU_BUF_ALLOC_MAX	((u64)(UINT_MAX & PAGE_MASK))

/*
 * Backing store of an AMDRNPU_BUF_ALLOC buffer, owned by the exported dma-buf
 * (its priv).  Holds a device reference so the heap device the pages came from
 * outlives every reference to the dma-buf, including ones held by userspace or
 * other importers after the client has gone.
 */
struct amdrnpu_bo {
	struct amdrnpu_dev	*rnpu;
	struct device		*dev;		/* heap device */
	void			*vaddr;		/* dma_alloc_coherent() */
	dma_addr_t		dma_addr;
	size_t			size;
};

static struct sg_table *amdrnpu_bo_map_dma_buf(struct dma_buf_attachment *attach,
					       enum dma_data_direction dir)
{
	struct amdrnpu_bo *bo = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(bo->dev, sgt, bo->vaddr, bo->dma_addr, bo->size);
	if (ret)
		goto err_free;

	/*
	 * The memory is coherent, and a no-map pool has no kernel linear
	 * mapping to perform CPU cache maintenance on.
	 */
	ret = dma_map_sgtable(attach->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		goto err_free_table;
	return sgt;

err_free_table:
	sg_free_table(sgt);
err_free:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void amdrnpu_bo_unmap_dma_buf(struct dma_buf_attachment *attach,
				     struct sg_table *sgt,
				     enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static void amdrnpu_bo_free(struct amdrnpu_bo *bo)
{
	dma_free_coherent(bo->dev, bo->size, bo->vaddr, bo->dma_addr);
	amdrnpu_dev_put(bo->rnpu);
	kfree(bo);
}

static void amdrnpu_bo_release(struct dma_buf *dmabuf)
{
	amdrnpu_bo_free(dmabuf->priv);
}

static int amdrnpu_bo_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct amdrnpu_bo *bo = dmabuf->priv;

	return dma_mmap_coherent(bo->dev, vma, bo->vaddr, bo->dma_addr, bo->size);
}

static const struct dma_buf_ops amdrnpu_bo_dmabuf_ops = {
	.map_dma_buf	= amdrnpu_bo_map_dma_buf,
	.unmap_dma_buf	= amdrnpu_bo_unmap_dma_buf,
	.release	= amdrnpu_bo_release,
	.mmap		= amdrnpu_bo_mmap,
};

int amdrnpu_ioctl_buf_alloc(struct amdrnpu_client *client, void __user *uarg)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct amdrnpu_dev *rnpu = client->rnpu;
	struct amdrnpu_buf_alloc arg;
	struct amdrnpu_heap *heap;
	struct amdrnpu_buf *buf;
	struct dma_buf *dmabuf;
	struct amdrnpu_bo *bo;
	u32 handle;
	int fd, ret;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || arg.handle || arg.fd)
		return -EINVAL;
	if (!arg.size || arg.size > AMDRNPU_BUF_ALLOC_MAX)
		return -EINVAL;

	heap = amdrnpu_heap_get(rnpu, arg.memory_type);
	if (IS_ERR(heap))
		return PTR_ERR(heap);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->dev = &heap->pdev->dev;
	bo->size = PAGE_ALIGN(arg.size);

	bo->vaddr = dma_alloc_coherent(bo->dev, bo->size, &bo->dma_addr,
				       GFP_KERNEL);
	if (!bo->vaddr) {
		RNPU_ERR(rnpu, "memory type %u: cannot allocate %zu bytes\n",
			 arg.memory_type, bo->size);
		kfree(bo);
		return -ENOMEM;
	}
	amdrnpu_dev_get(rnpu);
	bo->rnpu = rnpu;

	exp_info.ops = &amdrnpu_bo_dmabuf_ops;
	exp_info.size = bo->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = bo;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		amdrnpu_bo_free(bo);
		return PTR_ERR(dmabuf);
	}

	/* From here @bo is freed by the dma-buf's release. */
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_put_dmabuf;
	}
	get_dma_buf(dmabuf);
	buf->dmabuf = dmabuf;
	buf->dma_addr = bo->dma_addr;
	buf->size = bo->size;
	kref_init(&buf->ref);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_put_buf;
	}

	ret = xa_alloc(&client->buf_xa, &handle, buf, xa_limit_31b, GFP_KERNEL);
	if (ret < 0)
		goto err_put_fd;

	arg.handle = handle;
	arg.fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		/* A racing AMDRNPU_BUF_DESTROY may already have dropped it. */
		if (xa_cmpxchg(&client->buf_xa, handle, buf, NULL,
			       GFP_KERNEL) != buf)
			buf = NULL;
		ret = -EFAULT;
		goto err_put_fd;
	}

	fd_install(fd, dmabuf->file);
	return 0;

err_put_fd:
	put_unused_fd(fd);
err_put_buf:
	if (buf)
		amdrnpu_buf_put(buf);
err_put_dmabuf:
	dma_buf_put(dmabuf);
	return ret;
}
