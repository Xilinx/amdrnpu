/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_xrt_test_example - APU-side driver for APU_XRT_TEST.
 *
 * Wire protocol matches RPURuntime tests/xrt_apu_rpu_test/src/apu_xrt_ops.h:
 *   args[0] = iterations
 *   args[1] = ELF BO / PA
 *   args[2] = ELF size
 *   args[3] = descriptor BO / PA
 *   args[4] = descriptor content size
 *   args[5] = result BO / PA (optional)
 *
 * The descriptor holds one (or more) apu_xrt_run_entry structs with kernel
 * arg PA+size pairs.  Kernel-arg physical addresses come from
 * amdrnpu_buf_get_phys() (AMDRNPU_BUF_QUERY) — dma-buf mmaps are not
 * reliably present in /proc/self/pagemap.
 *
 * Usage:
 *   amdrnpu_xrt_test_example --elf-file ELF --arg PATH... [OPTIONS]
 */

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

#include "apu_ops.h"

#define DEFAULT_ITERATIONS   1u
#define DEFAULT_TIMEOUT_S    60u

static int g_fail;

static void fail(const char *what, int rc)
{
	fprintf(stderr, "%s: %s (rc=%d)\n", what,
		strerror(rc < 0 ? -rc : rc), rc);
	g_fail = 1;
}

static int open_amdrnpu_node(char *path_out, size_t path_sz)
{
	DIR *dir = opendir("/dev");
	struct dirent *e;
	long best = LONG_MAX;
	int best_fd = -1;
	char best_path[PATH_MAX];

	if (!dir)
		return -1;

	while ((e = readdir(dir)) != NULL) {
		long n;
		int fd;
		char p[PATH_MAX];

		if (strncmp(e->d_name, "amdrnpu.", 8) != 0)
			continue;
		if (e->d_name[8] < '0' || e->d_name[8] > '9')
			continue;
		n = strtol(e->d_name + 8, NULL, 10);
		if (n < 0 || n >= best)
			continue;
		snprintf(p, sizeof(p), "/dev/%s", e->d_name);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (best_fd >= 0)
			close(best_fd);
		best = n;
		best_fd = fd;
		snprintf(best_path, sizeof(best_path), "%s", p);
	}
	closedir(dir);

	if (best_fd < 0)
		return -1;
	if (path_out && path_sz)
		snprintf(path_out, path_sz, "%s", best_path);
	return best_fd;
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || !end || *end != '\0')
		return -EINVAL;
	*out = (uint64_t)v;
	return 0;
}

/* Absolute PA for a BO handle (fills descriptor karg entries). */
static int buf_phys(int fd, uint32_t handle, uint64_t *pa_out)
{
	return amdrnpu_buf_get_phys(fd, handle, pa_out, NULL);
}

struct karg_spec {
	char     path[PATH_MAX];
	uint64_t pa;
	uint64_t size;
};

struct cli_opts {
	char             device[PATH_MAX];
	uint32_t         iterations;
	uint32_t         timeout_s;
	char             elf_file[PATH_MAX];
	uint64_t         elf_pa;
	uint64_t         elf_size;
	struct karg_spec kargs[APU_XRT_TEST_MAX_KARGS];
	uint32_t         n_kargs;
	bool             want_result;
};

static void print_usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"  --device PATH        /dev/amdrnpu.N node (auto-detected)\n"
		"  --iterations N       Inference iterations (default %u)\n"
		"  --elf-file PATH      ELF from file into RPU BO\n"
		"  --elf-pa PA          Pre-loaded ELF physical address\n"
		"  --elf-size SZ        ELF size bytes (required with --elf-pa)\n"
		"  --arg PATH           Kernel arg file (repeat up to %u times)\n"
		"  --arg-pa PA:SIZE     Kernel arg pre-loaded PA:size (hex)\n"
		"  --no-result          Skip result writeback\n"
		"  --timeout-s N        Wait timeout seconds (default %u)\n",
		prog, DEFAULT_ITERATIONS, APU_XRT_TEST_MAX_KARGS,
		DEFAULT_TIMEOUT_S);
}

static int parse_cli(int argc, char **argv, struct cli_opts *o)
{
	int i = 1;

	memset(o, 0, sizeof(*o));
	o->iterations  = DEFAULT_ITERATIONS;
	o->timeout_s   = DEFAULT_TIMEOUT_S;
	o->want_result = true;

	while (i < argc) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			snprintf(o->device, sizeof(o->device), "%s", argv[i + 1]);
			i += 2;
		} else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
			o->iterations = (uint32_t)strtoul(argv[i + 1], NULL, 0);
			i += 2;
		} else if (!strcmp(argv[i], "--elf-file") && i + 1 < argc) {
			snprintf(o->elf_file, sizeof(o->elf_file),
				 "%s", argv[i + 1]);
			i += 2;
		} else if (!strcmp(argv[i], "--elf-pa") && i + 1 < argc) {
			if (parse_u64(argv[i + 1], &o->elf_pa))
				return -EINVAL;
			i += 2;
		} else if (!strcmp(argv[i], "--elf-size") && i + 1 < argc) {
			if (parse_u64(argv[i + 1], &o->elf_size))
				return -EINVAL;
			i += 2;
		} else if (!strcmp(argv[i], "--arg") && i + 1 < argc) {
			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) {
				fprintf(stderr,
					"error: max %u --arg entries\n",
					APU_XRT_TEST_MAX_KARGS);
				return -EINVAL;
			}
			snprintf(o->kargs[o->n_kargs].path,
				 sizeof(o->kargs[o->n_kargs].path),
				 "%s", argv[i + 1]);
			o->n_kargs++;
			i += 2;
		} else if (!strcmp(argv[i], "--arg-pa") && i + 1 < argc) {
			char *colon;

			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) {
				fprintf(stderr,
					"error: max %u --arg-pa entries\n",
					APU_XRT_TEST_MAX_KARGS);
				return -EINVAL;
			}
			colon = strchr(argv[i + 1], ':');
			if (!colon) {
				fprintf(stderr,
					"error: --arg-pa needs PA:SIZE\n");
				return -EINVAL;
			}
			*colon = '\0';
			if (parse_u64(argv[i + 1],
				      &o->kargs[o->n_kargs].pa) ||
			    parse_u64(colon + 1,
				      &o->kargs[o->n_kargs].size)) {
				return -EINVAL;
			}
			o->n_kargs++;
			i += 2;
		} else if (!strcmp(argv[i], "--no-result")) {
			o->want_result = false;
			i += 1;
		} else if (!strcmp(argv[i], "--timeout-s") && i + 1 < argc) {
			o->timeout_s = (uint32_t)strtoul(argv[i + 1], NULL, 0);
			i += 2;
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			print_usage(argv[0]);
			return 1;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (o->elf_file[0] && (o->elf_pa || o->elf_size)) {
		fprintf(stderr,
			"error: --elf-file and --elf-pa/--elf-size are mutually exclusive\n");
		return -EINVAL;
	}
	if (o->elf_pa && !o->elf_size) {
		fprintf(stderr, "error: --elf-pa requires --elf-size\n");
		return -EINVAL;
	}

	return 0;
}

static int load_file_to_rpu_bo(int fd, const char *path,
				uint32_t *out_bo, void **out_map,
				uint64_t *out_file_sz, uint64_t *out_alloc_sz)
{
	struct stat st;
	uint64_t file_sz, alloc_sz;
	uint32_t bo = 0;
	void *map = NULL;
	FILE *fp = NULL;
	int rc;

	if (stat(path, &st) < 0) {
		rc = -errno;
		fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
		return rc;
	}
	if (!S_ISREG(st.st_mode) || st.st_size == 0) {
		fprintf(stderr, "%s: not a regular file or empty\n", path);
		return -EINVAL;
	}
	file_sz  = (uint64_t)st.st_size;
	alloc_sz = (file_sz + 4095ULL) & ~4095ULL;

	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, alloc_sz, &bo);
	if (rc) {
		fprintf(stderr, "buf_alloc(%s, %llu): %s\n", path,
			(unsigned long long)alloc_sz, strerror(-rc));
		return rc;
	}

	rc = amdrnpu_buf_mmap(fd, bo, alloc_sz, 0, alloc_sz, &map);
	if (rc) {
		fprintf(stderr, "buf_mmap(%s): %s\n", path, strerror(-rc));
		goto err;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		rc = -errno;
		fprintf(stderr, "fopen(%s): %s\n", path, strerror(errno));
		goto err_unmap;
	}
	if (fread(map, 1, file_sz, fp) != file_sz) {
		fprintf(stderr, "fread(%s): short read\n", path);
		rc = -EIO;
		fclose(fp);
		goto err_unmap;
	}
	fclose(fp);

	if (alloc_sz > file_sz)
		memset((char *)map + file_sz, 0, alloc_sz - file_sz);

	rc = amdrnpu_buf_sync_to_device(fd, bo, 0, 0);
	if (rc) {
		fprintf(stderr, "buf_sync_to_device(%s): %s\n", path,
			strerror(-rc));
		goto err_unmap;
	}

	*out_bo       = bo;
	*out_map      = map;
	*out_file_sz  = file_sz;
	*out_alloc_sz = alloc_sz;
	return 0;

err_unmap:
	amdrnpu_buf_munmap(map, alloc_sz);
err:
	amdrnpu_buf_close(fd, bo);
	return rc;
}

static const char *rsp_status_str(int32_t st)
{
	if (st > 0)    return "COMPLETE";
	if (st == 0)   return "PENDING";
	switch (-st) {
	case EREMOTEIO:  return "REMOTEIO";
	case ECANCELED:  return "CANCELLED";
	case EOPNOTSUPP: return "NOTSUPPORTED";
	default:         return "ERROR";
	}
}

struct karg_bo {
	uint32_t handle;
	void    *map;
	uint64_t alloc_sz;
	uint64_t file_sz;
	uint64_t pa;
};

int main(int argc, char **argv)
{
	struct cli_opts opts;
	char opened_path[PATH_MAX];
	int fd = -1;
	int rc = 0;

	uint32_t elf_bo = 0;
	void    *elf_map = NULL;
	uint64_t elf_file_sz = 0;
	uint64_t elf_alloc_sz = 0;

	struct karg_bo kbo[APU_XRT_TEST_MAX_KARGS];
	memset(kbo, 0, sizeof(kbo));

	uint32_t desc_bo = 0;
	void    *desc_map = NULL;
	uint64_t desc_alloc_sz = 0;
	uint32_t desc_content_sz = 0;

	uint32_t result_bo = 0;
	void    *result_map = NULL;

	struct amdrnpu_arg args[APU_XRT_TEST_NUM_ARGS];
	memset(args, 0, sizeof(args));

	struct amdrnpu_cmd_submit hdr;
	int32_t fence_st = 0;
	uint32_t i;

	rc = parse_cli(argc, argv, &opts);
	if (rc > 0) return 0;
	if (rc < 0) return 2;

	if (opts.device[0]) {
		fd = open(opts.device, O_RDWR | O_CLOEXEC);
		if (fd < 0) { perror(opts.device); return 1; }
		snprintf(opened_path, sizeof(opened_path), "%s", opts.device);
	} else {
		fd = open_amdrnpu_node(opened_path, sizeof(opened_path));
		if (fd < 0) {
			fprintf(stderr,
				"no amdrnpu node under /dev/amdrnpu.*\n");
			return 1;
		}
	}

	printf("device:     %s\n", opened_path);
	printf("opcode:     APU_XRT_TEST (0x%08x)\n", APU_XRT_TEST);
	printf("iterations: %u\n", opts.iterations);
	printf("timeout:    %u s\n", opts.timeout_s);

	/* args[0]: iteration count */
	args[0].size  = 0;
	args[0].value = opts.iterations;

	/* args[1]/[2]: ELF */
	if (opts.elf_file[0]) {
		rc = load_file_to_rpu_bo(fd, opts.elf_file,
					 &elf_bo, &elf_map,
					 &elf_file_sz, &elf_alloc_sz);
		if (rc) { fail("load ELF", rc); goto out; }
		printf("elf-file:   %s → BO=%u size=%" PRIu64 "\n",
		       opts.elf_file, elf_bo, elf_file_sz);
		args[1].size                 = elf_alloc_sz;
		args[1].bo.buffer_handle     = (int32_t)elf_bo;
		args[1].bo.offset            = 0;
		args[2].size  = 0;
		args[2].value = elf_file_sz;
	} else if (opts.elf_pa) {
		printf("elf-pa:     0x%08" PRIx64 " size=%" PRIu64 "\n",
		       opts.elf_pa, opts.elf_size);
		args[1].size  = 0;
		args[1].value = opts.elf_pa;
		args[2].size  = 0;
		args[2].value = opts.elf_size;
	} else {
		fprintf(stderr,
			"error: provide --elf-file or --elf-pa/--elf-size\n");
		rc = -EINVAL;
		goto out;
	}

	/* Load kernel-arg BOs / resolve PAs for the descriptor. */
	for (i = 0; i < opts.n_kargs; ++i) {
		struct karg_spec *ks = &opts.kargs[i];

		if (ks->path[0]) {
			rc = load_file_to_rpu_bo(fd, ks->path,
						 &kbo[i].handle, &kbo[i].map,
						 &kbo[i].file_sz,
						 &kbo[i].alloc_sz);
			if (rc) {
				fprintf(stderr, "load arg[%u] %s: %s\n",
					i, ks->path, strerror(-rc));
				g_fail = 1;
				goto out;
			}
			rc = buf_phys(fd, kbo[i].handle, &kbo[i].pa);
			if (rc) {
				fprintf(stderr,
					"buf_get_phys(arg[%u]): %s\n",
					i, strerror(-rc));
				g_fail = 1;
				goto out;
			}
			printf("arg[%u]:     %s → BO=%u size=%" PRIu64
			       " PA=0x%08" PRIx64 "\n",
			       i, ks->path, kbo[i].handle, kbo[i].file_sz,
			       kbo[i].pa);
		} else {
			kbo[i].pa = ks->pa;
			kbo[i].file_sz = ks->size;
			kbo[i].alloc_sz = ks->size;
			printf("arg[%u]:     PA=0x%08" PRIx64 " size=%" PRIu64 "\n",
			       i, ks->pa, ks->size);
		}
	}

	/* Build descriptor: 1 entry, n_kargs kernel args. */
	desc_content_sz = APU_XRT_CMD_DESC_HDR_SIZE +
			  APU_XRT_RUN_ENTRY_SIZE(opts.n_kargs);
	desc_alloc_sz = (desc_content_sz + 4095ULL) & ~4095ULL;
	if (desc_alloc_sz < 4096)
		desc_alloc_sz = 4096;

	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, desc_alloc_sz, &desc_bo);
	if (rc) { fail("buf_alloc(desc)", rc); goto out; }
	rc = amdrnpu_buf_mmap(fd, desc_bo, desc_alloc_sz, 0, desc_alloc_sz,
			      &desc_map);
	if (rc) { fail("buf_mmap(desc)", rc); goto out; }

	{
		uint8_t *base = desc_map;
		struct apu_xrt_cmd_desc *desc =
			(struct apu_xrt_cmd_desc *)base;
		struct apu_xrt_run_entry *entry =
			(struct apu_xrt_run_entry *)(base +
						     APU_XRT_CMD_DESC_HDR_SIZE);

		memset(base, 0, desc_alloc_sz);
		desc->n_entries = 1;
		desc->reserved  = 0;
		entry->iterations      = opts.iterations;
		entry->n_kargs         = opts.n_kargs;
		entry->kernel_name_off = 0;
		entry->kernel_name_len = 0;
		entry->result_bo_off   = 0;
		entry->reserved        = 0;
		for (i = 0; i < opts.n_kargs; ++i) {
			entry->kargs[i].pa = kbo[i].pa;
			/* Use file size for BO mapping (not padded alloc). */
			entry->kargs[i].size = kbo[i].file_sz ?
				kbo[i].file_sz : kbo[i].alloc_sz;
		}
	}

	rc = amdrnpu_buf_sync_to_device(fd, desc_bo, 0, 0);
	if (rc) { fail("buf_sync(desc)", rc); goto out; }
	printf("desc BO:    handle=%u content=%u B (1 entry, %u kargs)\n",
	       desc_bo, desc_content_sz, opts.n_kargs);

	args[3].size             = desc_alloc_sz;
	args[3].bo.buffer_handle = (int32_t)desc_bo;
	args[3].bo.offset        = 0;
	args[4].size  = 0;
	args[4].value = desc_content_sz;

	/* args[5]: result writeback */
	if (opts.want_result) {
		rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU,
					    APU_XRT_TEST_RESULT_SIZE,
					    &result_bo);
		if (rc) { fail("buf_alloc(result)", rc); goto out; }

		rc = amdrnpu_buf_mmap(fd, result_bo, 0, 0, 0, &result_map);
		if (rc) {
			fail("buf_mmap(result)", rc);
			goto out;
		}
		memset(result_map, 0, APU_XRT_TEST_RESULT_SIZE);
		rc = amdrnpu_buf_sync_to_device(fd, result_bo, 0, 0);
		if (rc) { fail("buf_sync(result)", rc); goto out; }

		args[5].size             = APU_XRT_TEST_RESULT_SIZE;
		args[5].bo.buffer_handle = (int32_t)result_bo;
		args[5].bo.offset        = 0;
		printf("result BO:  handle=%u (%zu bytes)\n",
		       result_bo, APU_XRT_TEST_RESULT_SIZE);
	} else {
		args[5].size  = 0;
		args[5].value = 0;
	}

	printf("submitting APU_XRT_TEST (%u args) ...\n", APU_XRT_TEST_NUM_ARGS);
	rc = amdrnpu_cmd_submit(fd, APU_XRT_TEST, args, APU_XRT_TEST_NUM_ARGS,
				&hdr, sizeof(hdr));
	if (rc) {
		fail("cmd_submit", rc);
		goto out;
	}
	printf("syncobj_handle=%d\n", hdr.sync_handle);

	rc = amdrnpu_syncobj_wait_complete(
		fd, (uint32_t)hdr.sync_handle,
		(int64_t)opts.timeout_s * 1000000000LL, &fence_st);
	if (rc) {
		fail("syncobj_wait", rc);
		close(hdr.sync_handle);
		goto out;
	}
	close(hdr.sync_handle);
	printf("fence_st=%d (%s)\n", fence_st, rsp_status_str(fence_st));

	if (opts.want_result && result_map) {
		struct apu_xrt_test_result res;

		rc = amdrnpu_buf_sync_from_device(fd, result_bo, 0, 0);
		if (rc) { fail("buf_sync_from(result)", rc); goto out; }
		memcpy(&res, result_map, sizeof(res));
		printf("result: passed=%u iterations_done=%u "
		       "avg_us=%u first_us=%u\n",
		       res.passed, res.iterations_done,
		       res.latency_avg_us, res.latency_first_us);
		if (!res.passed)
			g_fail = 1;
	} else if (fence_st <= 0) {
		g_fail = 1;
	}

out:
	if (result_map)
		amdrnpu_buf_munmap(result_map, APU_XRT_TEST_RESULT_SIZE);
	if (result_bo)
		amdrnpu_buf_close(fd, result_bo);
	if (desc_map)
		amdrnpu_buf_munmap(desc_map, desc_alloc_sz);
	if (desc_bo)
		amdrnpu_buf_close(fd, desc_bo);
	for (i = 0; i < APU_XRT_TEST_MAX_KARGS; ++i) {
		if (kbo[i].map)
			amdrnpu_buf_munmap(kbo[i].map, kbo[i].alloc_sz);
		if (kbo[i].handle)
			amdrnpu_buf_close(fd, kbo[i].handle);
	}
	if (elf_map)
		amdrnpu_buf_munmap(elf_map, elf_alloc_sz);
	if (elf_bo)
		amdrnpu_buf_close(fd, elf_bo);
	if (fd >= 0)
		close(fd);

	if (g_fail || rc) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
