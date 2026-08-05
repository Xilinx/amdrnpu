/* SPDX-License-Identifier: MIT */
/*
 * amdrnpu_inout_example - end-to-end in/out buffer test for amdrnpu.
 *
 * Flow:
 *   1. Allocate input and output RPU BOs (AMDRNPU_MEMORY_RPU / bank@0 carveout).
 *   2. mmap + fill input, sync to device.
 *   3. mmap + zero output, sync to device.
 *   4. Submit AMDRNPU_OP_COPY_IN_OUT (firmware copies in -> out).
 *   5. Wait on syncobj, sync output from device, verify bytes match input.
 *
 * Prerequisites on VEK385 (system-top-rpu-enlarged.dtb + rebuilt rpu_fw):
 *   - RPU firmware registers RPMsg channel "amdrnpu" (amdrnpu_ept.c).
 *   - OpenAMP rsctbl/vrings match DT: 0x9800000 / 0x9860000 / 0x9864000 / 0x9868000.
 *   - amdrnpu driver loaded; DT node amd,amdrnpu with bank@0
 *     memory-region bound (by numeric phandle 0x500 in the official
 *     Versal-net layout) to rpu_gem_cma@20000000 (0x20000000, 512 MiB
 *     reusable shared-dma-pool + linux,cma-default).
 *   - Open /dev/dri/cardN for amdrnpu (MODE_MAP_DUMB requires card, not renderD*).
 *
 * Usage:
 *   amdrnpu_inout_example [--device /dev/dri/cardN]
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
#include <unistd.h>

#include <xf86drm.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

#define AMDRNPU_OP_COPY_IN_OUT	0x00000001u
#define INPUT_BO_TAG		1ull
#define OUTPUT_BO_TAG		2ull
#define TEST_BUF_SIZE		4096u
#define DEFAULT_WAIT_NS		(INT64_C(5) * INT64_C(1000000000))

static int g_fail;

static void fail(const char *what, int rc)
{
	fprintf(stderr, "%s: %s (rc=%d)\n", what, strerror(rc < 0 ? -rc : rc), rc);
	g_fail = 1;
}

static int drm_is_amdrnpu(int fd)
{
	drmVersionPtr v = drmGetVersion(fd);
	int ok = 0;

	if (!v)
		return 0;
	ok = (strcmp(v->name, AMDRNPU_DRIVER_NAME) == 0);
	drmFreeVersion(v);
	return ok;
}

static int open_amdrnpu_card(char *path_out, size_t path_sz)
{
	DIR *dir = opendir("/dev/dri");
	struct dirent *e;
	long best = LONG_MAX;
	int best_fd = -1;
	char best_path[PATH_MAX];

	if (!dir)
		return -1;

	while ((e = readdir(dir)) != NULL) {
		long m;
		int fd;
		char p[PATH_MAX];

		if (strncmp(e->d_name, "card", 4) != 0)
			continue;
		m = strtol(e->d_name + 4, NULL, 10);
		if (m < 0 || m >= best)
			continue;
		snprintf(p, sizeof(p), "/dev/dri/%s", e->d_name);
		fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0 || !drm_is_amdrnpu(fd)) {
			if (fd >= 0)
				close(fd);
			continue;
		}
		if (m < best) {
			if (best_fd >= 0)
				close(best_fd);
			best = m;
			best_fd = fd;
			snprintf(best_path, sizeof(best_path), "%s", p);
		} else {
			close(fd);
		}
	}
	closedir(dir);

	if (best_fd < 0)
		return -1;
	if (path_out && path_sz)
		snprintf(path_out, path_sz, "%s", best_path);
	return best_fd;
}

static int parse_device_arg(int argc, char **argv, char *path, size_t path_sz)
{
	int i = 1;

	if (i < argc && !strcmp(argv[i], "--device")) {
		if (i + 1 >= argc) {
			fprintf(stderr, "--device needs a path\n");
			return -EINVAL;
		}
		snprintf(path, path_sz, "%s", argv[i + 1]);
		return 0;
	}
	path[0] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	char dev_path[PATH_MAX] = {0};
	int fd = -1;
	uint32_t in_hd = 0, out_hd = 0;
	void *in_map = NULL, *out_map = NULL;
	struct amdrnpu_arg args[6];
	struct amdrnpu_cmd_submit hdr;
	int32_t fence_st = 0;
	int rc;

	if (parse_device_arg(argc, argv, dev_path, sizeof(dev_path))) {
		return 2;
	}

	if (dev_path[0]) {
		fd = open(dev_path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			perror(dev_path);
			return 1;
		}
	} else {
		fd = open_amdrnpu_card(dev_path, sizeof(dev_path));
		if (fd < 0) {
			fprintf(stderr, "no amdrnpu card under /dev/dri/card*\n");
			return 1;
		}
	}

	printf("device: %s\n", dev_path[0] ? dev_path : "(auto)");
	printf("buffer size: %u bytes per RPU BO\n", TEST_BUF_SIZE);

	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, TEST_BUF_SIZE, &in_hd);
	if (rc) {
		fail("buf_alloc(in RPU)", rc);
		goto out;
	}
	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_RPU, TEST_BUF_SIZE, &out_hd);
	if (rc) {
		fail("buf_alloc(out RPU)", rc);
		goto out;
	}
	printf("RPU BO handles: in=%u out=%u\n", in_hd, out_hd);

	rc = amdrnpu_buf_mmap(fd, in_hd, TEST_BUF_SIZE, 0, 0, &in_map);
	if (rc) {
		fail("buf_mmap(in)", rc);
		goto out;
	}
	rc = amdrnpu_buf_mmap(fd, out_hd, TEST_BUF_SIZE, 0, 0, &out_map);
	if (rc) {
		fail("buf_mmap(out)", rc);
		goto out;
	}

	for (uint32_t i = 0; i < TEST_BUF_SIZE; i++)
		((uint8_t *)in_map)[i] = (uint8_t)(0xa5 ^ (i & 0xff));
	memset(out_map, 0x5a, TEST_BUF_SIZE);

	rc = amdrnpu_buf_sync_to_device(fd, in_hd, 0, TEST_BUF_SIZE);
	if (rc) {
		fail("buf_sync_to_device(in)", rc);
		goto out;
	}
	rc = amdrnpu_buf_sync_to_device(fd, out_hd, 0, TEST_BUF_SIZE);
	if (rc) {
		fail("buf_sync_to_device(out)", rc);
		goto out;
	}
	printf("filled input, primed output, synced both to device\n");

	memset(args, 0, sizeof(args));
	args[0].size = 0;
	args[0].value = INPUT_BO_TAG;
	args[1].size = 0;
	args[1].value = TEST_BUF_SIZE;
	args[2].size = TEST_BUF_SIZE;
	args[2].bo.drm_buffer_handle = (int32_t)in_hd;
	args[2].bo.offset = 0;
	args[3].size = 0;
	args[3].value = OUTPUT_BO_TAG;
	args[4].size = 0;
	args[4].value = TEST_BUF_SIZE;
	args[5].size = TEST_BUF_SIZE;
	args[5].bo.drm_buffer_handle = (int32_t)out_hd;
	args[5].bo.offset = 0;

	memset(&hdr, 0, sizeof(hdr));
	rc = amdrnpu_cmd_submit(fd, AMDRNPU_OP_COPY_IN_OUT, args, 6,
				     &hdr, sizeof(hdr));
	if (rc) {
		fail("cmd_submit", rc);
		goto out;
	}
	printf("submitted COPY op, syncobj=%d\n", hdr.sync_handle);

	rc = amdrnpu_syncobj_wait_complete(fd,
						(uint32_t)hdr.sync_handle,
						DEFAULT_WAIT_NS, &fence_st);
	if (rc) {
		fail("syncobj_wait", rc);
		goto out;
	}
	if (fence_st <= 0) {
		fprintf(stderr, "command did not complete successfully (fence_st=%" PRId32 ")\n",
			fence_st);
		g_fail = 1;
		goto out;
	}
	printf("command completed (fence_st=%" PRId32 ")\n", fence_st);

	rc = amdrnpu_buf_sync_from_device(fd, out_hd, 0, TEST_BUF_SIZE);
	if (rc) {
		fail("buf_sync_from_device(out)", rc);
		goto out;
	}

	if (memcmp(in_map, out_map, TEST_BUF_SIZE) != 0) {
		fprintf(stderr, "verify FAIL: output does not match input\n");
		g_fail = 1;
	} else {
		printf("verify OK: output matches input (%u bytes)\n", TEST_BUF_SIZE);
	}

out:
	if (in_map)
		amdrnpu_buf_munmap(in_map, TEST_BUF_SIZE);
	if (out_map)
		amdrnpu_buf_munmap(out_map, TEST_BUF_SIZE);
	if (in_hd)
		amdrnpu_buf_close(fd, in_hd);
	if (out_hd)
		amdrnpu_buf_close(fd, out_hd);
	if (fd >= 0)
		close(fd);

	return g_fail ? 1 : 0;
}
