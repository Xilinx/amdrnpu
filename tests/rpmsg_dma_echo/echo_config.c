/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
#define _POSIX_C_SOURCE 200809L

#include "echo_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_defaults(struct echo_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	/* Dedicated free OCM bank (candidate); confirm with the address-equality
	 * bring-up step before trusting it.
	 */
	cfg->dev_base = UINT64_C(0xBBF00000);
	cfg->a78_phys_base = UINT64_C(0xBBF00000);
	cfg->shm_size = 0x80000; /* 512 KiB bank. */

	cfg->status_offset = 0;
	cfg->vring_tx_offset = 0x1000;
	cfg->vring_rx_offset = 0x2000;
	cfg->buf_offset = 0x4000;

	cfg->vring_num = 4;
	cfg->vring_align = 64;
	cfg->buf_count = 8;
	cfg->buf_size = 512;

	cfg->peer_dst = 42;
	cfg->host_ept_addr = 1024;
	cfg->iters = 16;
	snprintf(cfg->ept_name, sizeof(cfg->ept_name), "%s", "echo");
	snprintf(cfg->mdb5_write, sizeof(cfg->mdb5_write), "%s",
		 "/dev/mdb5_write00");
	snprintf(cfg->mdb5_read, sizeof(cfg->mdb5_read), "%s",
		 "/dev/mdb5_read00");
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		++s;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\n' || end[-1] == '\r'))
		*--end = '\0';
	return s;
}

static int parse_u64(const char *v, uint64_t *out)
{
	char *endp;
	unsigned long long n;

	errno = 0;
	n = strtoull(v, &endp, 0);
	if (errno || endp == v || *endp)
		return -EINVAL;
	*out = (uint64_t)n;
	return 0;
}

static int apply_kv(struct echo_config *cfg, const char *key, const char *val)
{
	uint64_t n;

#define NUM(field)                               \
	do {                                     \
		if (parse_u64(val, &n))          \
			return -EINVAL;          \
		cfg->field = (__typeof__(cfg->field))n; \
		return 0;                        \
	} while (0)

	if (!strcmp(key, "dev_base"))
		NUM(dev_base);
	if (!strcmp(key, "a78_phys_base"))
		NUM(a78_phys_base);
	if (!strcmp(key, "shm_size"))
		NUM(shm_size);
	if (!strcmp(key, "status_offset"))
		NUM(status_offset);
	if (!strcmp(key, "vring_tx_offset"))
		NUM(vring_tx_offset);
	if (!strcmp(key, "vring_rx_offset"))
		NUM(vring_rx_offset);
	if (!strcmp(key, "buf_offset"))
		NUM(buf_offset);
	if (!strcmp(key, "vring_num"))
		NUM(vring_num);
	if (!strcmp(key, "vring_align"))
		NUM(vring_align);
	if (!strcmp(key, "buf_count"))
		NUM(buf_count);
	if (!strcmp(key, "buf_size"))
		NUM(buf_size);
	if (!strcmp(key, "peer_dst"))
		NUM(peer_dst);
	if (!strcmp(key, "host_ept_addr"))
		NUM(host_ept_addr);
	if (!strcmp(key, "iters"))
		NUM(iters);
#undef NUM

	if (!strcmp(key, "ept_name")) {
		snprintf(cfg->ept_name, sizeof(cfg->ept_name), "%s", val);
		return 0;
	}
	if (!strcmp(key, "mdb5_write")) {
		snprintf(cfg->mdb5_write, sizeof(cfg->mdb5_write), "%s", val);
		return 0;
	}
	if (!strcmp(key, "mdb5_read")) {
		snprintf(cfg->mdb5_read, sizeof(cfg->mdb5_read), "%s", val);
		return 0;
	}
	return -ENOENT; /* Unknown key. */
}

int echo_config_load(struct echo_config *cfg, const char *path)
{
	FILE *f;
	char line[512];
	int lineno = 0;

	set_defaults(cfg);
	if (!path)
		return 0;
	f = fopen(path, "re");
	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		char *s, *eq, *key, *val;

		++lineno;
		s = line;
		/* Strip trailing comment. */
		char *hash = strchr(s, '#');

		if (hash)
			*hash = '\0';
		s = trim(s);
		if (!*s)
			continue;
		eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "%s:%d: no '=' in '%s'\n", path, lineno,
				s);
			fclose(f);
			return -EINVAL;
		}
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		if (apply_kv(cfg, key, val)) {
			fprintf(stderr, "%s:%d: bad key/value '%s'\n", path,
				lineno, key);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	return 0;
}

void echo_config_dump(const struct echo_config *cfg)
{
	fprintf(stderr,
		"echo_config:\n"
		"  dev_base       = 0x%llx\n"
		"  a78_phys_base  = 0x%llx\n"
		"  shm_size       = 0x%zx\n"
		"  status_offset  = 0x%zx\n"
		"  vring_tx_offset= 0x%zx\n"
		"  vring_rx_offset= 0x%zx\n"
		"  buf_offset     = 0x%zx\n"
		"  vring_num      = %u\n"
		"  vring_align    = %u\n"
		"  buf_count      = %u\n"
		"  buf_size       = %u\n"
		"  peer_dst       = %u\n"
		"  host_ept_addr  = %u\n"
		"  iters          = %u\n"
		"  ept_name       = %s\n"
		"  mdb5_write     = %s\n"
		"  mdb5_read      = %s\n",
		(unsigned long long)cfg->dev_base,
		(unsigned long long)cfg->a78_phys_base, cfg->shm_size,
		cfg->status_offset, cfg->vring_tx_offset, cfg->vring_rx_offset,
		cfg->buf_offset, cfg->vring_num, cfg->vring_align,
		cfg->buf_count, cfg->buf_size, cfg->peer_dst,
		cfg->host_ept_addr, cfg->iters, cfg->ept_name, cfg->mdb5_write,
		cfg->mdb5_read);
}
