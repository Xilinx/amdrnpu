// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/xarray.h>

#include "amdrnpu_internal.h"

/*
 * v6.13 (cdd30ebb1b9f) switched MODULE_IMPORT_NS to take a string literal;
 * on <=v6.12 the macro still __stringify()s a bare token. Pass the form the
 * running kernel's macro expects so modpost records the namespace as
 * DMA_BUF (not "DMA_BUF") on both.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif

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

	dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
					  DMA_BIDIRECTIONAL);
	dma_buf_detach(buf->dmabuf, buf->attach);
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
