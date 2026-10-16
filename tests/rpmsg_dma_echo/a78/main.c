/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * A78 device-peer entry point for the rpmsg-lite DMA echo test.
 * Usage: echo_device [config.ini] [max_echoes]
 *   config.ini  shared memory-map config (default ./echo_config.ini)
 *   max_echoes  stop after N echoes (default 0 = run forever)
 */
#define _POSIX_C_SOURCE 200809L

#include "echo_config.h"
#include "echo_device.h"
#include "mem_devmem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *cfg_path = argc > 1 ? argv[1] : "echo_config.ini";
	unsigned long max_echoes = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
	struct echo_config cfg;
	struct mem_devmem mem;
	struct rpmsg_mem_ops ops;
	int ret;

	ret = echo_config_load(&cfg, cfg_path);
	if (ret) {
		fprintf(stderr, "config load '%s': %s\n", cfg_path,
			strerror(-ret));
		return 1;
	}
	echo_config_dump(&cfg);

	if (!cfg.shm_size) {
		fprintf(stderr, "shm_size must be set for /dev/mem mapping\n");
		return 1;
	}
	ret = mem_devmem_open(&mem, &ops, cfg.dev_base, cfg.a78_phys_base,
			      cfg.shm_size);
	if (ret) {
		fprintf(stderr,
			"/dev/mem map of phys 0x%llx (%zu B): %s\n"
			"(run as root; OCM region should be reserved no-map)\n",
			(unsigned long long)cfg.a78_phys_base, cfg.shm_size,
			strerror(-ret));
		return 1;
	}

	ret = device_echo_run(&ops, &cfg, max_echoes);
	if (ret)
		fprintf(stderr, "device_echo_run: %s\n", strerror(-ret));

	mem_devmem_close(&mem);
	return ret ? 1 : 0;
}
