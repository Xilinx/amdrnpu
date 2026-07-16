// SPDX-License-Identifier: GPL-2.0
/*
 * amdrnpu - per-bank platform device handling.
 *
 * The "amd,amdrnpu" parent node has exactly two DT children, both with the
 * same compatible. The bank role is encoded by the logical-index "reg" cell:
 *
 *   bank@0   compatible = "amd,amdrnpu-bank"; reg = <0>;   (RPU bank)
 *   bank@1   compatible = "amd,amdrnpu-bank"; reg = <1>;   (dev bank)
 *
 * The "reg" value is a logical bank index, not an MMIO base. It matches both
 * the kernel-internal enum (AMDRNPU_BANK_RPU / AMDRNPU_BANK_DEV) and the UAPI
 * AMDRNPU_MEMORY_RPU / AMDRNPU_MEMORY_DEVICE values, so DT, kernel and userspace
 * share one identifier scheme.
 *
 * The parent does not walk children directly; it calls
 * devm_of_platform_populate(), which triggers this bank platform_driver to
 * probe each child synchronously.
 *
 * The bank probe gives each bank its own struct device (so reserved-memory
 * heap, optional iommus, and dma-mask are owned per-bank), then registers
 * itself into the parent's amdrnpu_dev->banks[] table.
 *
 * Banks are not marked dma-coherent in DT.  Native GEM buffers use
 * dma_alloc_noncoherent() (contiguous, cacheable CPU mappings); userspace
 * must call AMDRNPU_BUF_SYNC before the device reads CPU-written data and
 * after the device writes data the CPU will read.
 */

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "amdrnpu_internal.h"

static const char *amdrnpu_bank_label(u32 bank_index)
{
	switch (bank_index) {
	case AMDRNPU_BANK_RPU:	return "rpu";
	case AMDRNPU_BANK_DEV:	return "dev";
	default:		return "?";
	}
}

static int amdrnpu_bank_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct amdrnpu_dev *tdev;
	struct amdrnpu_bank *slot;
	u32 bank_index;
	const char *label;
	int ret;

	if (!parent) {
		dev_err(dev, "bank has no parent device\n");
		return -ENODEV;
	}
	tdev = dev_get_drvdata(parent);
	if (!tdev) {
		/*
		 * Parent's amdrnpu_platform_probe() must have published drvdata
		 * before calling devm_of_platform_populate().
		 */
		dev_err(dev, "parent amdrnpu_dev not available yet\n");
		return -EPROBE_DEFER;
	}

	/*
	 * Read 'reg' as the logical bank index. Schema bounds it to
	 * [0, AMDRNPU_NUM_BANKS - 1]; the driver re-checks defensively.
	 */
	ret = of_property_read_u32(dev->of_node, "reg", &bank_index);
	if (ret) {
		dev_err(dev, "bank: missing reg (logical bank index)\n");
		return ret;
	}
	if (bank_index >= AMDRNPU_NUM_BANKS) {
		dev_err(dev, "bank: reg=%u out of range (max %u)\n",
			bank_index, AMDRNPU_NUM_BANKS - 1);
		return -EINVAL;
	}
	label = amdrnpu_bank_label(bank_index);

	slot = &tdev->banks[bank_index];
	if (slot->dev) {
		dev_err(dev, "duplicate bank@%u (%s)\n", bank_index, label);
		return -EEXIST;
	}

	/*
	 * of_platform / dev_pm_domain machinery already ran of_dma_configure()
	 * and of_iommu_configure() (when 'iommus' is present) on this device.
	 */

	ret = of_reserved_mem_device_init_by_idx(dev, dev->of_node, 0);
	if (ret == -ENODEV) {
		ret = 0;
	} else if (ret) {
		dev_err(dev, "bank@%u (%s): reserved-mem init failed (%d)\n",
			bank_index, label, ret);
		return ret;
	}

	if (bank_index == AMDRNPU_BANK_RPU) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(dev,
				"bank@%u (%s): cannot set 32-bit dma-mask (%d)\n",
				bank_index, label, ret);
			goto err_relmem;
		}
	} else {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(64));
		if (ret) {
			dev_warn(dev,
				 "bank@%u (%s): 64-bit dma-mask rejected (%d), falling back to 32-bit\n",
				 bank_index, label, ret);
			ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
			if (ret)
				goto err_relmem;
		}
	}

	slot->dev = dev;
	slot->pdev = pdev;
	slot->np = of_node_get(dev->of_node);
	slot->region_id = bank_index;
	slot->label = label;
	/*
	 * no-map shared-dma-pool -> dev->dma_mem (rmem_dma_ops).
	 * reusable shared-dma-pool -> dev->cma_area (rmem_cma_ops).
	 */
	slot->has_reserved_mem = dev->dma_mem || dev->cma_area;

	platform_set_drvdata(pdev, slot);

	dev_info(dev, "bank@%u (%s): mask=0x%llx%s\n",
		 bank_index, label, dma_get_mask(dev),
		 slot->has_reserved_mem ? "" : ", no reserved-memory (default DMA heap)");
	return 0;

err_relmem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void amdrnpu_bank_remove(struct platform_device *pdev)
{
	struct amdrnpu_bank *slot = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	if (slot && slot->has_reserved_mem)
		of_reserved_mem_device_release(dev);

	if (slot) {
		of_node_put(slot->np);
		slot->dev = NULL;
		slot->pdev = NULL;
		slot->np = NULL;
		slot->has_reserved_mem = false;
	}
}

static const struct of_device_id amdrnpu_bank_of_match[] = {
	{ .compatible = "amd,amdrnpu-bank" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amdrnpu_bank_of_match);

static struct platform_driver amdrnpu_bank_driver = {
	.probe = amdrnpu_bank_probe,
	.remove = amdrnpu_bank_remove,
	.driver = {
		.name = "amdrnpu-bank",
		.of_match_table = amdrnpu_bank_of_match,
	},
};

int amdrnpu_bank_driver_register(void)
{
	return platform_driver_register(&amdrnpu_bank_driver);
}

void amdrnpu_bank_driver_unregister(void)
{
	platform_driver_unregister(&amdrnpu_bank_driver);
}

int amdrnpu_bank_check_all_present(struct amdrnpu_dev *tdev)
{
	if (!tdev->banks[AMDRNPU_BANK_RPU].dev ||
	    !tdev->banks[AMDRNPU_BANK_DEV].dev) {
		dev_err(tdev->dev,
			"missing bank: expect bank@0 and bank@1 children with compatible \"amd,amdrnpu-bank\"; rpu=%s dev=%s\n",
			tdev->banks[AMDRNPU_BANK_RPU].dev ? "ok" : "missing",
			tdev->banks[AMDRNPU_BANK_DEV].dev ? "ok" : "missing");
		return -ENODEV;
	}
	return 0;
}

struct amdrnpu_bank *amdrnpu_bank_by_region(struct amdrnpu_dev *tdev, u32 region_id)
{
	if (region_id >= AMDRNPU_NUM_BANKS)
		return NULL;
	if (!tdev->banks[region_id].dev)
		return NULL;
	return &tdev->banks[region_id];
}

struct amdrnpu_bank *amdrnpu_bank_memory_type(struct amdrnpu_dev *tdev, u32 mem_type)
{
	/*
	 * UAPI AMDRNPU_MEMORY_* values are deliberately the same as
	 * AMDRNPU_BANK_* (rpu=0, device=1). AMDRNPU_MEMORY_DEFAULT is normalised
	 * to AMDRNPU_MEMORY_DEVICE here so callers can pass it directly.
	 */
	if (mem_type == AMDRNPU_MEMORY_DEFAULT)
		mem_type = AMDRNPU_MEMORY_DEVICE;

	switch (mem_type) {
	case AMDRNPU_MEMORY_RPU:
		return amdrnpu_bank_by_region(tdev, AMDRNPU_BANK_RPU);
	case AMDRNPU_MEMORY_DEVICE:
		return amdrnpu_bank_by_region(tdev, AMDRNPU_BANK_DEV);
	default:
		return NULL;
	}
}
