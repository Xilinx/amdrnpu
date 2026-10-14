/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * amdrnpu_xrt_offload_example - offload an XRT/AIE workload from the APU to the
 * RPU_APP over the userspace amdrnpu rpmsg-lite stack (no kernel driver, no
 * APU-side XRT). Models its buffer/submit flow on amdrnpu_copy_loopback_example,
 * but talks to a REAL RPU_APP instead of the built-in loopback peer.
 *
 * Flow (control side, public amdrnpu API only):
 *   1. amdrnpu_buf_alloc() the AIE control ELF into an RPU buffer; buf_write it.
 *   2. amdrnpu_buf_alloc() each kernel-arg buffer (IFM/WTS from files, OFM as a
 *      zeroed output); amdrnpu_buf_get_phys() to learn each one's PA.
 *   3. Build an apu_xrt_cmd_desc descriptor (1 run entry, N kargs = those PAs)
 *      into an RPU buffer.
 *   4. amdrnpu_buf_alloc() a result buffer.
 *   5. amdrnpu_cmd_submit(APU_XRT_TEST) with the 6-arg wire layout:
 *        args[0]=iterations, args[1]=ELF BO, args[2]=ELF size,
 *        args[3]=descriptor BO, args[4]=descriptor size, args[5]=result BO.
 *      The library resolves BO handles to physical addresses on the wire.
 *   6. amdrnpu_syncobj_wait_complete() (polls the vring for the RPU response).
 *   7. amdrnpu_buf_read() the result struct back and print it.
 *
 * Config comes from an INI (default /etc/amdrnpu.ini; override with --ini or
 * $AMDRNPU_INI). Mapping the shared memory / pools typically needs root.
 *
 * Usage:
 *   amdrnpu_xrt_offload_example --elf-file aie_control_full.elf \
 *       --arg ifm32.bin --arg-zero 114688 --arg wts32.bin --iterations 1
 *
 *   --ini PATH          amdrnpu config INI (else $AMDRNPU_INI or /etc/amdrnpu.ini)
 *   --elf-file PATH     AIE control ELF (required)
 *   --arg PATH          kernel-arg buffer loaded from a file (repeat, in order)
 *   --arg-zero SIZE     kernel-arg output buffer, zero-filled, SIZE bytes (repeat)
 *   --iterations N      run count (default 1)
 *   --no-result         skip the result writeback buffer
 *   --timeout-s N       wait timeout in seconds (default 60)
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

#include "apu_ops.h"

#define DEFAULT_ITERATIONS 1u
#define DEFAULT_TIMEOUT_S  60u

/* One kernel arg: file-backed input, or a zero-filled output of 'size' bytes. */
struct karg_spec {
	char     path[PATH_MAX];  /* non-empty => load this file */
	uint64_t size;            /* used for --arg-zero (output) */
	bool     is_output;       /* true => zero-filled output buffer */
};

struct cli_opts {
	char             ini[PATH_MAX];
	char             elf_file[PATH_MAX];
	uint32_t         iterations;
	uint32_t         timeout_s;
	struct karg_spec kargs[APU_XRT_TEST_MAX_KARGS];
	uint32_t         n_kargs;
	bool             want_result;
};

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s --elf-file ELF [OPTIONS]\n"
		"  --ini PATH          config INI (else $AMDRNPU_INI or /etc/amdrnpu.ini)\n"
		"  --elf-file PATH     AIE control ELF (required)\n"
		"  --arg PATH          kernel-arg buffer from file (repeat, up to %u)\n"
		"  --arg-zero SIZE     zero-filled output kernel-arg, SIZE bytes (repeat)\n"
		"  --iterations N      run count (default %u)\n"
		"  --no-result         skip result writeback\n"
		"  --timeout-s N       wait timeout seconds (default %u)\n",
		p, APU_XRT_TEST_MAX_KARGS, DEFAULT_ITERATIONS, DEFAULT_TIMEOUT_S);
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

static int parse_cli(int argc, char **argv, struct cli_opts *o)
{
	int i = 1;

	memset(o, 0, sizeof(*o));
	o->iterations = DEFAULT_ITERATIONS;
	o->timeout_s = DEFAULT_TIMEOUT_S;
	o->want_result = true;

	while (i < argc) {
		if (!strcmp(argv[i], "--ini") && i + 1 < argc) {
			snprintf(o->ini, sizeof(o->ini), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--elf-file") && i + 1 < argc) {
			snprintf(o->elf_file, sizeof(o->elf_file), "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
			o->iterations = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--timeout-s") && i + 1 < argc) {
			o->timeout_s = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--no-result")) {
			o->want_result = false;
		} else if (!strcmp(argv[i], "--arg") && i + 1 < argc) {
			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) {
				fprintf(stderr, "too many kargs (max %u)\n",
					APU_XRT_TEST_MAX_KARGS);
				return -EINVAL;
			}
			snprintf(o->kargs[o->n_kargs].path,
				 sizeof(o->kargs[o->n_kargs].path), "%s", argv[++i]);
			o->n_kargs++;
		} else if (!strcmp(argv[i], "--arg-zero") && i + 1 < argc) {
			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) {
				fprintf(stderr, "too many kargs (max %u)\n",
					APU_XRT_TEST_MAX_KARGS);
				return -EINVAL;
			}
			if (parse_u64(argv[++i], &o->kargs[o->n_kargs].size) ||
			    o->kargs[o->n_kargs].size == 0) {
				fprintf(stderr, "--arg-zero needs a non-zero SIZE\n");
				return -EINVAL;
			}
			o->kargs[o->n_kargs].is_output = true;
			o->n_kargs++;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 1;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return -EINVAL;
		}
		i++;
	}

	if (!o->elf_file[0]) {
		fprintf(stderr, "error: --elf-file is required\n");
		return -EINVAL;
	}
	return 0;
}

static const char *config_path(const struct cli_opts *o)
{
	const char *p;

	if (o->ini[0])
		return o->ini;
	p = getenv("AMDRNPU_INI");
	return (p && *p) ? p : "/etc/amdrnpu.ini";
}

/* Read a whole file into a malloc'd buffer. Caller frees. */
static int slurp(const char *path, uint8_t **data, uint64_t *size)
{
	struct stat st;
	FILE *fp;
	uint8_t *buf;

	if (stat(path, &st) < 0) {
		fprintf(stderr, "stat(%s): %s\n", path, strerror(errno));
		return -errno;
	}
	if (st.st_size <= 0) {
		fprintf(stderr, "%s: empty\n", path);
		return -EINVAL;
	}
	buf = malloc((size_t)st.st_size);
	if (!buf)
		return -ENOMEM;
	fp = fopen(path, "rb");
	if (!fp) {
		free(buf);
		return -errno;
	}
	if (fread(buf, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) {
		fclose(fp);
		free(buf);
		return -EIO;
	}
	fclose(fp);
	*data = buf;
	*size = (uint64_t)st.st_size;
	return 0;
}

int main(int argc, char **argv)
{
	struct cli_opts opts;
	int fd = -1, rc;

	/* Allocated handles, tracked for cleanup. */
	uint32_t elf_h = 0, desc_h = 0, result_h = 0;
	uint32_t karg_h[APU_XRT_TEST_MAX_KARGS] = {0};
	uint64_t karg_pa[APU_XRT_TEST_MAX_KARGS] = {0};
	uint64_t karg_sz[APU_XRT_TEST_MAX_KARGS] = {0};
	uint8_t *desc = NULL, *elf_data = NULL;
	int ret = 1;

	rc = parse_cli(argc, argv, &opts);
	if (rc > 0)
		return 0;
	if (rc < 0)
		return 2;

	/* The library reads its INI from $AMDRNPU_INI (else /etc/amdrnpu.ini);
	 * point it at --ini so the CLI flag selects the config amdrnpu_open()
	 * loads. */
	setenv("AMDRNPU_INI", config_path(&opts), 1);
	printf("ini:        %s\n", config_path(&opts));
	printf("opcode:     APU_XRT_TEST (0x%08x)\n", APU_XRT_TEST);
	printf("iterations: %u\n", opts.iterations);

	/* Bring up Link A: maps the shared window + pools, fills the remote's
	 * vring descriptors, and completes the rpmsg name-service bind. */
	fd = amdrnpu_open(0);
	if (fd < 0) {
		fprintf(stderr, "amdrnpu_open: %s\n", strerror(-fd));
		return 1;
	}

	/* ---- ELF into an RPU buffer -------------------------------------- */
	uint64_t elf_sz = 0;
	rc = slurp(opts.elf_file, &elf_data, &elf_sz);
	if (rc)
		goto out;
	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, elf_sz, &elf_h);
	if (rc) { fprintf(stderr, "buf_alloc(elf): %s\n", strerror(-rc)); goto out; }
	rc = amdrnpu_buf_write(fd, elf_h, elf_data, 0, elf_sz);
	if (rc) { fprintf(stderr, "buf_write(elf): %s\n", strerror(-rc)); goto out; }
	printf("elf-file:   %s -> BO=%u size=%" PRIu64 "\n",
	       opts.elf_file, elf_h, elf_sz);

	/* ---- kernel-arg buffers (DEVICE memory, AIE-reachable) ----------- */
	for (uint32_t i = 0; i < opts.n_kargs; i++) {
		struct karg_spec *k = &opts.kargs[i];
		uint64_t pa = 0, sz = 0;

		if (k->is_output) {
			rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE,
					       k->size, &karg_h[i]);
			if (rc) { fprintf(stderr, "buf_alloc(out[%u]): %s\n", i, strerror(-rc)); goto out; }
			karg_sz[i] = k->size;
			printf("arg[%u]:     zeroed BO=%u size=%" PRIu64 " (output)\n",
			       i, karg_h[i], k->size);
		} else {
			uint8_t *d = NULL;
			uint64_t fsz = 0;
			rc = slurp(k->path, &d, &fsz);
			if (rc) goto out;
			rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, fsz, &karg_h[i]);
			if (rc) { free(d); fprintf(stderr, "buf_alloc(arg[%u]): %s\n", i, strerror(-rc)); goto out; }
			rc = amdrnpu_buf_write(fd, karg_h[i], d, 0, fsz);
			free(d);
			if (rc) { fprintf(stderr, "buf_write(arg[%u]): %s\n", i, strerror(-rc)); goto out; }
			karg_sz[i] = fsz;
			printf("arg[%u]:     %s -> BO=%u size=%" PRIu64 "\n",
			       i, k->path, karg_h[i], fsz);
		}
		rc = amdrnpu_buf_get_phys(fd, karg_h[i], &pa, &sz);
		if (rc) { fprintf(stderr, "buf_get_phys(arg[%u]): %s\n", i, strerror(-rc)); goto out; }
		karg_pa[i] = pa;
	}

	/* ---- Build the descriptor: 1 run entry with N kargs -------------- */
	uint32_t desc_content = APU_XRT_CMD_DESC_HDR_SIZE +
				APU_XRT_RUN_ENTRY_SIZE(opts.n_kargs);
	desc = calloc(1, desc_content);
	if (!desc) { rc = -ENOMEM; goto out; }

	struct apu_xrt_cmd_desc *cd = (struct apu_xrt_cmd_desc *)desc;
	cd->n_entries = 1;
	cd->reserved  = 0;

	struct apu_xrt_run_entry *re =
		(struct apu_xrt_run_entry *)(desc + APU_XRT_CMD_DESC_HDR_SIZE);
	re->iterations      = opts.iterations;
	re->n_kargs         = opts.n_kargs;
	re->kernel_name_off = 0;   /* auto-discover kernel from ELF */
	re->kernel_name_len = 0;
	re->result_bo_off   = 0;   /* top-level result via args[5] instead */
	re->reserved        = 0;
	for (uint32_t i = 0; i < opts.n_kargs; i++) {
		re->kargs[i].pa   = karg_pa[i];   /* absolute PA of the karg buffer */
		re->kargs[i].size = karg_sz[i];   /* >0 => BO arg (set_arg(i)=bo) */
	}

	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, desc_content, &desc_h);
	if (rc) { fprintf(stderr, "buf_alloc(desc): %s\n", strerror(-rc)); goto out; }
	rc = amdrnpu_buf_write(fd, desc_h, desc, 0, desc_content);
	if (rc) { fprintf(stderr, "buf_write(desc): %s\n", strerror(-rc)); goto out; }
	printf("descriptor: BO=%u size=%u (1 entry, %u kargs)\n",
	       desc_h, desc_content, opts.n_kargs);

	/* ---- Result buffer ----------------------------------------------- */
	if (opts.want_result) {
		rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU,
				       APU_XRT_TEST_RESULT_SIZE, &result_h);
		if (rc) { fprintf(stderr, "buf_alloc(result): %s\n", strerror(-rc)); goto out; }
	}

	/* ---- Wire args (6, descriptor protocol) -------------------------- */
	struct amdrnpu_arg args[APU_XRT_TEST_NUM_ARGS];
	memset(args, 0, sizeof(args));
	args[0].size = 0;        args[0].value = opts.iterations;   /* iterations  */
	args[1].size = elf_sz;   args[1].bo.buffer_handle = (int32_t)elf_h;    /* ELF BO     */
	args[1].bo.offset = 0;
	args[2].size = 0;        args[2].value = elf_sz;            /* ELF size    */
	args[3].size = desc_content; args[3].bo.buffer_handle = (int32_t)desc_h; /* descriptor */
	args[3].bo.offset = 0;
	args[4].size = 0;        args[4].value = desc_content;      /* desc size   */
	if (opts.want_result) {
		args[APU_XRT_TEST_RESULT_ARG_IDX].size = APU_XRT_TEST_RESULT_SIZE;
		args[APU_XRT_TEST_RESULT_ARG_IDX].bo.buffer_handle = (int32_t)result_h;
		args[APU_XRT_TEST_RESULT_ARG_IDX].bo.offset = 0;
	} else {
		args[APU_XRT_TEST_RESULT_ARG_IDX].size = 0;
		args[APU_XRT_TEST_RESULT_ARG_IDX].value = 0;   /* 0 = skip writeback */
	}

	/* ---- Submit + wait ----------------------------------------------- */
	struct amdrnpu_cmd_submit hdr;
	memset(&hdr, 0, sizeof(hdr));
	printf("submitting APU_XRT_TEST (%u args) ...\n", APU_XRT_TEST_NUM_ARGS);
	rc = amdrnpu_cmd_submit(fd, APU_XRT_TEST, args, APU_XRT_TEST_NUM_ARGS,
				&hdr, sizeof(hdr));
	if (rc) { fprintf(stderr, "cmd_submit: %s\n", strerror(-rc)); goto out; }
	printf("sync_handle=%d\n", hdr.sync_handle);

	int32_t wait_status = 0;
	int64_t timeout_ns = (int64_t)opts.timeout_s * INT64_C(1000000000);
	rc = amdrnpu_syncobj_wait_complete(fd, (uint32_t)hdr.sync_handle,
					   timeout_ns, &wait_status);
	if (rc) { fprintf(stderr, "syncobj_wait: %s\n", strerror(-rc)); goto out; }
	printf("wait_status=%" PRId32 " (%s)\n", wait_status,
	       wait_status > 0 ? "COMPLETE" : wait_status == 0 ? "PENDING" : "ERROR");

	if (hdr.sync_handle >= 0)
		close(hdr.sync_handle);

	if (wait_status <= 0) {
		ret = 1;
		goto out;
	}

	/* ---- Read back the result ---------------------------------------- */
	if (opts.want_result) {
		struct apu_xrt_test_result res;
		memset(&res, 0, sizeof(res));
		rc = amdrnpu_buf_read(fd, result_h, &res, 0, sizeof(res));
		if (rc) { fprintf(stderr, "buf_read(result): %s\n", strerror(-rc)); goto out; }
		printf("result: passed=%u iterations_done=%u avg_us=%u first_us=%u\n",
		       res.passed, res.iterations_done,
		       res.latency_avg_us, res.latency_first_us);
		ret = res.passed ? 0 : 1;
	} else {
		ret = 0;
	}

out:
	for (uint32_t i = 0; i < opts.n_kargs; i++)
		if (karg_h[i])
			amdrnpu_buf_close(fd, karg_h[i]);
	if (result_h) amdrnpu_buf_close(fd, result_h);
	if (desc_h)   amdrnpu_buf_close(fd, desc_h);
	if (elf_h)    amdrnpu_buf_close(fd, elf_h);
	free(desc);
	free(elf_data);
	/* fd is an opaque key from amdrnpu_open(), not a real descriptor —
	 * nothing to close(2). */

	printf("%s\n", ret == 0 ? "PASS" : "FAIL");
	return ret;
}
