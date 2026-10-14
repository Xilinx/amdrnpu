/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
#include "amdrnpu_ini.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMDRNPU_INI_DEFAULT_PATH "/etc/amdrnpu.ini"
#define AMDRNPU_INI_LINE_MAX 256

/* A single settable key: which 64-bit slot it fills and whether it's 32-bit. */
struct ini_key {
	const char *section;
	const char *name;
	size_t offset;	/* byte offset into struct amdrnpu_config */
	int is_u32;	/* width of the destination field */
};

#define K64(sec, nm, field) \
	{ sec, nm, offsetof(struct amdrnpu_config, field), 0 }
#define K32(sec, nm, field) \
	{ sec, nm, offsetof(struct amdrnpu_config, field), 1 }

static const struct ini_key amdrnpu_ini_keys[] = {
	K64("pool.device", "base", pool_device.base),
	K64("pool.device", "size", pool_device.size),
	K64("pool.rpu", "base", pool_rpu.base),
	K64("pool.rpu", "size", pool_rpu.size),
	K64("rpmsg", "shm_phys", rpmsg.shm_phys),
	K64("rpmsg", "shm_size", rpmsg.shm_size),
	K64("rpmsg", "status_offset", rpmsg.status_offset),
	K64("rpmsg", "vring_rx_offset", rpmsg.vring_rx_offset),
	K64("rpmsg", "vring_tx_offset", rpmsg.vring_tx_offset),
	K32("rpmsg", "vring_num", rpmsg.vring_num),
	K32("rpmsg", "vring_align", rpmsg.vring_align),
	K64("rpmsg", "buf_offset", rpmsg.buf_offset),
	K32("rpmsg", "buf_size", rpmsg.buf_size),
	K32("rpmsg", "buf_count", rpmsg.buf_count),
};

#define AMDRNPU_INI_NKEYS \
	(sizeof(amdrnpu_ini_keys) / sizeof(amdrnpu_ini_keys[0]))

/* Trim leading/trailing ASCII whitespace in place; returns the start pointer. */
static char *ini_trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	end = s + strlen(s);
	while (end > s) {
		char c = end[-1];

		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		*--end = '\0';
	}
	return s;
}

/* Apply "section/key = value". Returns 1 if a known key was set, else 0. */
static int ini_set(struct amdrnpu_config *cfg, uint64_t *seen,
		   const char *section, const char *key, const char *value)
{
	size_t i;

	for (i = 0; i < AMDRNPU_INI_NKEYS; i++) {
		const struct ini_key *k = &amdrnpu_ini_keys[i];
		char *endp;
		unsigned long long v;

		if (strcmp(section, k->section) || strcmp(key, k->name))
			continue;

		errno = 0;
		v = strtoull(value, &endp, 0);
		if (errno || endp == value || *endp)
			return -EINVAL;
		if (k->is_u32) {
			if (v > 0xffffffffull)
				return -EINVAL;
			*(uint32_t *)((char *)cfg + k->offset) = (uint32_t)v;
		} else {
			*(uint64_t *)((char *)cfg + k->offset) = (uint64_t)v;
		}
		*seen |= (uint64_t)1 << i;
		return 1;
	}
	return 0;
}

int amdrnpu_config_load(struct amdrnpu_config *cfg, const char *path)
{
	char section[64] = "";
	char line[AMDRNPU_INI_LINE_MAX];
	uint64_t seen = 0;
	FILE *f;
	int ret = 0;

	if (!cfg)
		return -EINVAL;
	if (!path)
		path = getenv("AMDRNPU_INI");
	if (!path || !*path)
		path = AMDRNPU_INI_DEFAULT_PATH;

	f = fopen(path, "re");
	if (!f)
		return -errno;

	memset(cfg, 0, sizeof(*cfg));

	while (fgets(line, sizeof(line), f)) {
		char *s = line;
		char *eq, *key, *val;

		/* Strip inline comments (';' or '#') and surrounding space. */
		s[strcspn(s, ";#")] = '\0';
		s = ini_trim(s);
		if (!*s)
			continue;

		if (*s == '[') {
			char *close = strchr(s, ']');

			if (!close) {
				ret = -EINVAL;
				goto out;
			}
			*close = '\0';
			s = ini_trim(s + 1);
			if (strlen(s) >= sizeof(section)) {
				ret = -EINVAL;
				goto out;
			}
			snprintf(section, sizeof(section), "%s", s);
			continue;
		}

		eq = strchr(s, '=');
		if (!eq) {
			ret = -EINVAL;
			goto out;
		}
		*eq = '\0';
		key = ini_trim(s);
		val = ini_trim(eq + 1);

		ret = ini_set(cfg, &seen, section, key, val);
		if (ret < 0)
			goto out;
		ret = 0; /* unknown keys (ret == 0) are ignored */
	}

	if (ferror(f)) {
		ret = -EIO;
		goto out;
	}

	/* Every declared key is mandatory. */
	if (seen != (((uint64_t)1 << AMDRNPU_INI_NKEYS) - 1))
		ret = -EINVAL;

out:
	fclose(f);
	return ret;
}
