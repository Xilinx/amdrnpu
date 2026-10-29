// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/configfs.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "amdrnpu_internal.h"

/* Configured region names and how many devices have each one bound. */
static DEFINE_MUTEX(amdrnpu_region_lock);
static char amdrnpu_region_names[AMDRNPU_NUM_MEMORY_TYPES][AMDRNPU_REGION_NAME_LEN] = {
	[AMDRNPU_MEMORY_RPU]	= "rpu_gem_cma",
	[AMDRNPU_MEMORY_DEVICE]	= "buffer",
};

static unsigned int amdrnpu_region_users[AMDRNPU_NUM_MEMORY_TYPES];

/*
 * Take a use of @memory_type's region and copy its name into @name
 * (AMDRNPU_REGION_NAME_LEN bytes).  The name cannot be changed until every use
 * is dropped with amdrnpu_region_put().
 */
void amdrnpu_region_get(u32 memory_type, char *name)
{
	mutex_lock(&amdrnpu_region_lock);
	strscpy(name, amdrnpu_region_names[memory_type], AMDRNPU_REGION_NAME_LEN);
	amdrnpu_region_users[memory_type]++;
	mutex_unlock(&amdrnpu_region_lock);
}

void amdrnpu_region_put(u32 memory_type)
{
	mutex_lock(&amdrnpu_region_lock);
	amdrnpu_region_users[memory_type]--;
	mutex_unlock(&amdrnpu_region_lock);
}

static ssize_t amdrnpu_region_show(u32 memory_type, char *page)
{
	ssize_t len;

	mutex_lock(&amdrnpu_region_lock);
	len = scnprintf(page, PAGE_SIZE, "%s\n", amdrnpu_region_names[memory_type]);
	mutex_unlock(&amdrnpu_region_lock);
	return len;
}

static ssize_t amdrnpu_region_store(u32 memory_type, const char *page,
				    size_t count)
{
	size_t len = strcspn(page, "\n");
	ssize_t ret = count;

	if (!len || len >= AMDRNPU_REGION_NAME_LEN)
		return -EINVAL;

	mutex_lock(&amdrnpu_region_lock);
	if (amdrnpu_region_users[memory_type])
		ret = -EBUSY;
	else
		strscpy(amdrnpu_region_names[memory_type], page, len + 1);
	mutex_unlock(&amdrnpu_region_lock);
	return ret;
}

static ssize_t amdrnpu_cfg_rpu_region_show(struct config_item *item, char *page)
{
	return amdrnpu_region_show(AMDRNPU_MEMORY_RPU, page);
}

static ssize_t amdrnpu_cfg_rpu_region_store(struct config_item *item,
					    const char *page, size_t count)
{
	return amdrnpu_region_store(AMDRNPU_MEMORY_RPU, page, count);
}

static ssize_t amdrnpu_cfg_device_region_show(struct config_item *item,
					      char *page)
{
	return amdrnpu_region_show(AMDRNPU_MEMORY_DEVICE, page);
}

static ssize_t amdrnpu_cfg_device_region_store(struct config_item *item,
					       const char *page, size_t count)
{
	return amdrnpu_region_store(AMDRNPU_MEMORY_DEVICE, page, count);
}

CONFIGFS_ATTR(amdrnpu_cfg_, rpu_region);
CONFIGFS_ATTR(amdrnpu_cfg_, device_region);

static struct configfs_attribute *amdrnpu_cfg_attrs[] = {
	&amdrnpu_cfg_attr_rpu_region,
	&amdrnpu_cfg_attr_device_region,
	NULL,
};

static const struct config_item_type amdrnpu_cfg_type = {
	.ct_attrs	= amdrnpu_cfg_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem amdrnpu_cfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf	= AMDRNPU_DRIVER_NAME,
			.ci_type	= &amdrnpu_cfg_type,
		},
	},
};

int amdrnpu_configfs_register(void)
{
	config_group_init(&amdrnpu_cfg_subsys.su_group);
	mutex_init(&amdrnpu_cfg_subsys.su_mutex);
	return configfs_register_subsystem(&amdrnpu_cfg_subsys);
}

void amdrnpu_configfs_unregister(void)
{
	configfs_unregister_subsystem(&amdrnpu_cfg_subsys);
}
