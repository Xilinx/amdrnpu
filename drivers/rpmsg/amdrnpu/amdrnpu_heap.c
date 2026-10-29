// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#include "amdrnpu_internal.h"

void amdrnpu_dev_heap_init(struct amdrnpu_dev *rnpu)
{
	mutex_init(&rnpu->heap_lock);
}

/* Called once every buffer allocated from the heaps has been freed. */
void amdrnpu_dev_heap_fini(struct amdrnpu_dev *rnpu)
{
	struct amdrnpu_heap *heap;
	unsigned int i;

	for (i = 0; i < AMDRNPU_NUM_MEMORY_TYPES; i++) {
		heap = &rnpu->heaps[i];
		if (!heap->pdev)
			continue;
		if (heap->rmem->ops->device_release)
			heap->rmem->ops->device_release(heap->rmem,
							&heap->pdev->dev);
		platform_device_unregister(heap->pdev);
		heap->pdev = NULL;
		amdrnpu_region_put(i);
	}
	mutex_destroy(&rnpu->heap_lock);
}

static bool amdrnpu_region_name_match(struct device_node *np, const char *name)
{
	if (strchr(name, '@'))
		return !strcmp(np->full_name, name);
	return of_node_name_eq(np, name);
}

/* The /reserved-memory child named @name, or an ERR_PTR. */
static struct device_node *amdrnpu_find_region_node(struct amdrnpu_dev *rnpu,
						    const char *name)
{
	struct device_node *parent, *child, *np = NULL;

	parent = of_find_node_by_path("/reserved-memory");
	if (!parent) {
		RNPU_ERR(rnpu, "no /reserved-memory node\n");
		return ERR_PTR(-ENODEV);
	}

	for_each_available_child_of_node(parent, child) {
		if (!amdrnpu_region_name_match(child, name))
			continue;
		if (np) {
			RNPU_ERR(rnpu, "\"%s\" matches %pOF and %pOF; use the full node name\n",
				 name, np, child);
			of_node_put(child);
			of_node_put(np);
			np = ERR_PTR(-EINVAL);
			break;
		}
		np = of_node_get(child);
	}
	of_node_put(parent);

	if (!np) {
		RNPU_ERR(rnpu, "no reserved-memory node \"%s\"\n", name);
		return ERR_PTR(-ENODEV);
	}
	return np;
}

static int amdrnpu_heap_register(struct amdrnpu_dev *rnpu,
				 struct amdrnpu_heap *heap, u32 memory_type)
{
	char name[AMDRNPU_REGION_NAME_LEN];
	struct platform_device *pdev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct device *dev;
	int ret;

	if (memory_type == AMDRNPU_MEMORY_RPU && !rnpu->rproc_dev)
		return -ENODEV;

	amdrnpu_region_get(memory_type, name);

	np = amdrnpu_find_region_node(rnpu, name);
	if (IS_ERR(np)) {
		ret = PTR_ERR(np);
		goto err_put_region;
	}
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem || !rmem->ops || !rmem->ops->device_init) {
		RNPU_ERR(rnpu, "\"%s\" is not an initialised shared-dma-pool\n",
			 name);
		ret = -ENODEV;
		goto err_put_region;
	}

	pdev = platform_device_alloc("amdrnpu-heap", PLATFORM_DEVID_AUTO);
	if (!pdev) {
		ret = -ENOMEM;
		goto err_put_region;
	}
	ret = platform_device_add(pdev);
	if (ret) {
		platform_device_put(pdev);
		goto err_put_region;
	}
	dev = &pdev->dev;

	/*
	 * RPU buffers take the remoteproc's DMA configuration (dma-ranges,
	 * iommus, coherency) so the address is the one the RPU sees, and are
	 * kept below 4 GiB for the RPU's 32-bit address space.
	 */
	if (memory_type == AMDRNPU_MEMORY_RPU) {
		ret = of_dma_configure(dev, rnpu->rproc_dev->of_node, true);
		if (!ret)
			ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	} else {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
	}
	if (ret) {
		RNPU_ERR(rnpu, "\"%s\": DMA configuration failed (%d)\n", name, ret);
		goto err_unregister;
	}

	ret = rmem->ops->device_init(rmem, dev);
	if (ret) {
		RNPU_ERR(rnpu, "\"%s\": cannot bind %s (%d)\n", name, rmem->name, ret);
		goto err_unregister;
	}

	heap->pdev = pdev;
	heap->rmem = rmem;
	RNPU_INFO(rnpu, "%s memory from %s\n",
		  memory_type == AMDRNPU_MEMORY_RPU ? "RPU" : "device",
		  rmem->name);
	return 0;

err_unregister:
	platform_device_unregister(pdev);
err_put_region:
	amdrnpu_region_put(memory_type);
	return ret;
}

/*
 * Return the heap serving @memory_type, registering it on first use.  Heaps
 * stay registered until the device is released, so the result is valid for as
 * long as the caller holds a reference on @rnpu.
 */
struct amdrnpu_heap *amdrnpu_heap_get(struct amdrnpu_dev *rnpu, u32 memory_type)
{
	struct amdrnpu_heap *heap;
	int ret = 0;

	if (memory_type >= AMDRNPU_NUM_MEMORY_TYPES)
		return ERR_PTR(-EINVAL);
	heap = &rnpu->heaps[memory_type];

	mutex_lock(&rnpu->heap_lock);
	if (!heap->pdev)
		ret = amdrnpu_heap_register(rnpu, heap, memory_type);
	mutex_unlock(&rnpu->heap_lock);

	return ret ? ERR_PTR(ret) : heap;
}
