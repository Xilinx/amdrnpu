/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_test - one-binary CLI exercising every amdrnpu UAPI op.
 *
 * Each subcommand prints a short "Sending ..." banner and a one-line result.
 *
 * Usage:
 *   amdrnpu_test [--device /dev/amdrnpu.N] <command> [args...]
 *
 * If --device is omitted the CLI scans /dev/amdrnpu.* and picks the
 * lowest-numbered node.  The driver is a miscdevice, so the node name is the
 * identity -- there is no name/version ioctl to probe.
 *
 * Commands:
 *   device                                    # print which node was opened
 *   buf_alloc <rpu|device> <size>             # alloc a buffer then close
 *   cmd_submit <op_hex> [scalar:<u64> |
 *              buf:<handle>:<offset>]...      # one-shot AMDRNPU_CMD_SUBMIT
 *   syncobj_wait <handle> [timeout_ns=2e9]    # wait + report fence status
 *   fw_echo <v1_hex> <v2_hex>                 # full RPU-firmware echo flow
 *   lifecycle                                 # alloc + no-op submit + wait
 *
 * Note about handle scoping: buffer handles and completion (sync_file) fds are
 * scoped to the device fd that created them.  cmd_submit and syncobj_wait that
 * are given handles from a prior invocation will fail because the handles are
 * not valid in the new fd.  Use fw_echo or lifecycle for in-process flows.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

/* Firmware-side magic op for the Zephyr amdrnpu_ept echo path. */
#define AMDRNPU_FW_ECHO_OP	0x42424242u
/* Must match AMDRNPU_TEXT_MAX in firmware amdrnpu_ept.c. */
#define AMDRNPU_FW_RSP_TEXT_MAX	400u
/* Response BO sized for one page (also covers the ASCII reply header). */
#define AMDRNPU_FW_RSP_BO_SIZE	4096u
/* Optional last-arg device-bank GEM (no host mmap; RPU may consume the dma_addr). */
#define AMDRNPU_FW_EXTRA_BO_SIZE	256u
/* Default syncobj wait deadline for the smoke flows: 2 seconds. */
#define AMDRNPU_DEFAULT_WAIT_NS	(INT64_C(2) * INT64_C(1000000000))

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int report(const char *what, int rc)
{
	if (rc == 0)
		return 0;
	fprintf(stderr, "%s: %s (rc=%d)\n", what, strerror(-rc), rc);
	return 1;
}

static uint32_t parse_u32(const char *s)
{
	return (uint32_t)strtoul(s, NULL, 0);
}

static uint64_t parse_u64(const char *s)
{
	return (uint64_t)strtoull(s, NULL, 0);
}

static int parse_memory_type(const char *s, uint32_t *out)
{
	if (!strcmp(s, "rpu") || !strcmp(s, "RPU")) {
		*out = AMDRNPU_MEMORY_RPU;
		return 0;
	}
	if (!strcmp(s, "device") || !strcmp(s, "dev") || !strcmp(s, "DEVICE")) {
		*out = AMDRNPU_MEMORY_DEVICE;
		return 0;
	}
	fprintf(stderr, "unknown memory type \"%s\" (want rpu|device)\n", s);
	return -EINVAL;
}

static const char *memory_type_str(uint32_t t)
{
	switch (t) {
	case AMDRNPU_MEMORY_RPU:    return "rpu";
	case AMDRNPU_MEMORY_DEVICE: return "device";
	default:                   return "?";
	}
}

/*
 * Parse one cmd_submit positional argument:
 *   "scalar:<u64>"          -> amdrnpu_arg{ .size = 0,    .value = u64 }
 *   "buf:<handle>:<offset>" -> amdrnpu_arg{ .size = ~0u,  .bo = {handle, offset} }
 *
 * The "size" of buffer args is set by the caller to the slice length the
 * firmware should treat the buffer as. For raw cmd_submit testing we use
 * the offset value as a stand-in: it does not matter to the kernel because
 * the kernel only needs the handle to resolve dma_addr; the firmware-side
 * interpretation depends on the op. Most users will want fw_echo instead.
 */
static int parse_arg_token(const char *tok, struct amdrnpu_arg *out)
{
	memset(out, 0, sizeof(*out));
	if (!strncmp(tok, "scalar:", 7)) {
		out->size = 0;
		out->value = parse_u64(tok + 7);
		return 0;
	}
	if (!strncmp(tok, "buf:", 4)) {
		const char *p = tok + 4;
		const char *colon = strchr(p, ':');
		uint32_t handle = 0, offset = 0;

		if (!colon) {
			fprintf(stderr,
				"buf arg needs handle:offset (e.g. buf:7:0)\n");
			return -EINVAL;
		}
		handle = parse_u32(p);
		offset = parse_u32(colon + 1);
		/* Non-zero size flags this as a buffer arg to the kernel.
		 * The kernel only inspects the handle/offset; size is opaque
		 * to it. Use 1 to mean "minimum valid buffer slice".
		 */
		out->size = 1;
		out->bo.buffer_handle = (int32_t)handle;
		out->bo.offset = offset;
		return 0;
	}
	fprintf(stderr, "unknown arg \"%s\" (want scalar:U64 or buf:H:OFFSET)\n", tok);
	return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* device discovery                                                    */
/* ------------------------------------------------------------------ */

/*
 * Scan /dev/amdrnpu.* and pick the lowest-numbered node. The driver is a
 * miscdevice, so the node name IS the identity -- there is no name/version
 * ioctl to probe. If @want is non-NULL, only the node it names
 * ("/dev/amdrnpu.N") is opened; the path is rebuilt from the directory entry so
 * --device can only ever reach an amdrnpu node. Returns fd >= 0 on success,
 * -1 if no match. Optionally writes the path into @out_path (must be at least
 * PATH_MAX bytes).
 */
static int open_amdrnpu_scan(const char *want, char *out_path,
			     size_t out_sz)
{
	DIR *dir;
	struct dirent *e;
	long best = LONG_MAX;
	int best_fd = -1;
	char best_path[PATH_MAX] = {0};

	if (want && strncmp(want, "/dev/", 5) != 0)
		return -1;
	dir = opendir("/dev");
	if (!dir)
		return -1;

	while ((e = readdir(dir)) != NULL) {
		long n;
		int device_fd;
		char p[PATH_MAX];

		if (strncmp(e->d_name, "amdrnpu.", 8) != 0)
			continue;
		if (!isdigit((unsigned char)e->d_name[8]))
			continue;
		if (want && strcmp(e->d_name, want + 5) != 0)
			continue;
		n = strtol(e->d_name + 8, NULL, 10);
		if (n < 0 || n >= best)
			continue;
		snprintf(p, sizeof(p), "/dev/%s", e->d_name);
		device_fd = open(p, O_RDWR | O_CLOEXEC);
		if (device_fd < 0)
			continue;
		if (best_fd >= 0)
			close(best_fd);
		best = n;
		best_fd = device_fd;
		snprintf(best_path, sizeof(best_path), "%s", p);
	}
	closedir(dir);

	if (best_fd >= 0 && out_path && out_sz > 0)
		snprintf(out_path, out_sz, "%s", best_path);
	return best_fd;
}

/*
 * Open the user-supplied device path, or fall back to a /dev/amdrnpu.* scan
 * when path is NULL.
 */
static int open_amdrnpu_device(const char *path, char *opened_path, size_t sz)
{
	int device_fd;

	if (path && path[0]) {
		device_fd = open_amdrnpu_scan(path, opened_path, sz);
		if (device_fd < 0)
			fprintf(stderr, "%s: not an accessible /dev/amdrnpu.N node\n",
				path);
		return device_fd;
	}
	device_fd = open_amdrnpu_scan(NULL, opened_path, sz);
	if (device_fd < 0)
		fprintf(stderr,
			"no /dev/amdrnpu.* node found; pass --device explicitly\n");
	return device_fd;
}

/* ------------------------------------------------------------------ */
/* per-command handlers                                                */
/* ------------------------------------------------------------------ */

static int do_device(const char *opened_path)
{
	printf("amdrnpu device: %s\n", opened_path);
	return 0;
}

static int do_buf_alloc(int device_fd, int argc, char **argv)
{
	uint32_t mtype, handle = 0;
	uint64_t size;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "Usage: buf_alloc <rpu|device> <size>\n");
		return 2;
	}
	if (parse_memory_type(argv[0], &mtype))
		return 2;
	size = parse_u64(argv[1]);

	printf("buf_alloc type=%s size=%" PRIu64 "\n",
	       memory_type_str(mtype), size);
	rc = amdrnpu_buf_alloc(device_fd, mtype, size, &handle);
	if (rc)
		return report("buf_alloc", rc);
	printf("  -> buffer_handle=%u\n", handle);
	rc = amdrnpu_buf_close(device_fd, handle);
	if (rc)
		return report("buf_close (cleanup)", rc);
	printf("  -> closed\n");
	return 0;
}

static int do_cmd_submit(int device_fd, int argc, char **argv)
{
	struct amdrnpu_arg args[16];
	struct amdrnpu_cmd_submit hdr;
	uint32_t op;
	int n, i, rc;

	if (argc < 1) {
		fprintf(stderr,
			"Usage: cmd_submit <op_hex> [scalar:U64 | buf:H:OFFSET]...\n");
		return 2;
	}
	op = parse_u32(argv[0]);
	n = argc - 1;
	if (n > (int)(sizeof(args) / sizeof(args[0]))) {
		fprintf(stderr, "too many args (max %zu)\n",
			sizeof(args) / sizeof(args[0]));
		return 2;
	}
	for (i = 0; i < n; i++) {
		if (parse_arg_token(argv[1 + i], &args[i]))
			return 2;
	}

	memset(&hdr, 0, sizeof(hdr));
	printf("Sending AMDRNPU_CMD_SUBMIT op=0x%08x num_args=%d\n", op, n);
	rc = amdrnpu_cmd_submit(device_fd, op, args, (uint32_t)n,
				     &hdr, sizeof(hdr));
	if (rc)
		return report("cmd_submit", rc);
	printf("  -> syncobj_handle=%d\n", hdr.sync_handle);
	printf("  (handle valid only within this process; chain via fw_echo or lifecycle)\n");
	return 0;
}

static int do_syncobj_wait(int device_fd, int argc, char **argv)
{
	uint32_t handle;
	int64_t timeout_ns = AMDRNPU_DEFAULT_WAIT_NS;
	int32_t status = 0;
	int rc;

	if (argc < 1) {
		fprintf(stderr, "Usage: syncobj_wait <handle> [timeout_ns]\n");
		return 2;
	}
	handle = parse_u32(argv[0]);
	if (argc >= 2)
		timeout_ns = (int64_t)strtoll(argv[1], NULL, 0);

	printf("Waiting on completion handle=%u timeout_ns=%" PRId64 "\n",
	       handle, timeout_ns);
	rc = amdrnpu_syncobj_wait_complete(device_fd, handle, timeout_ns, &status);
	if (rc)
		return report("syncobj_wait", rc);
	printf("  -> status=%" PRId32
	       " (0=pending, 1=ok, negative=fence errno)\n", status);
	if (status < 0 && -status > 0 && -status < 4096)
		printf("     fence errno: %s\n", strerror(-status));
	return 0;
}

/*
 * Compose the firmware echo flow exactly the way the upstream/reference
 * amdrnpu_fw_echo example does: a 400-byte response BO on the RPU
 * carveout (so the R5F can NUL-terminate ASCII into it), two scalar
 * payload qwords, and a small device-bank "extra" BO so the kernel
 * exercises both bank dma_addr resolution paths.
 *
 * Cleanup is best-effort: we tear down both BOs even on failure so we
 * don't leak buffer handles between runs.
 */
static int do_fw_echo(int device_fd, int argc, char **argv)
{
	uint32_t rsp_bo = 0, extra_bo = 0;
	void *map = MAP_FAILED;
	struct amdrnpu_arg args[4];
	struct amdrnpu_cmd_submit hdr;
	uint64_t v1, v2;
	int32_t fence_st = 0;
	int rc, ret = 0;
	unsigned int rpu_cid = 0, rpu_nargs = 0;

	if (argc < 2) {
		fprintf(stderr, "Usage: fw_echo <v1_hex> <v2_hex>\n");
		return 2;
	}
	v1 = parse_u64(argv[0]);
	v2 = parse_u64(argv[1]);

	printf("=== fw_echo: payload=0x%016" PRIx64 ", 0x%016" PRIx64 " ===\n",
	       v1, v2);

	printf("[1/8] buf_alloc rpu rsp size=%u\n", AMDRNPU_FW_RSP_BO_SIZE);
	rc = amdrnpu_buf_alloc(device_fd, AMDRNPU_MEMORY_RPU,
				    AMDRNPU_FW_RSP_BO_SIZE, &rsp_bo);
	if (rc) { ret = report("buf_alloc(rpu)", rc); goto out; }
	printf("      -> rsp_bo=%u\n", rsp_bo);

	printf("[2/8] buf_alloc device extra size=%u\n", AMDRNPU_FW_EXTRA_BO_SIZE);
	rc = amdrnpu_buf_alloc(device_fd, AMDRNPU_MEMORY_DEVICE,
				    AMDRNPU_FW_EXTRA_BO_SIZE, &extra_bo);
	if (rc) { ret = report("buf_alloc(device)", rc); goto out; }
	printf("      -> extra_bo=%u\n", extra_bo);

	/* CPU-map the response BO via the library (mmap on the buffer's
	 * dma-buf).  offset=0, length=0 maps the whole allocation.
	 */
	rc = amdrnpu_buf_mmap(device_fd, rsp_bo, 0, 0, 0, &map);
	if (rc) {
		ret = report("buf_mmap(rsp_bo)", rc);
		map = MAP_FAILED;
		goto out;
	}
	memset(map, 0x0a, AMDRNPU_FW_RSP_BO_SIZE);
	printf("[3/8] mmap rsp_bo + canary (0x0a)\n");

	/*
	 * Buffers from amdrnpu_buf_alloc() are coherent, so this sync is a
	 * no-op for them, but a buffer imported from a cacheable exporter
	 * needs it: any range the device will read must be flushed from CPU
	 * caches first (DMA_BUF_IOCTL_SYNC via amdrnpu_buf_sync_to_device),
	 * otherwise the RPU could observe stale (un-flushed) bytes when it
	 * inspects the buffer.  Syncing keeps this a valid template for both.
	 *
	 * rsp_bo: flush the 0x0a canary so the RPU sees it before it
	 *         overwrites the buffer with the ASCII reply.
	 *
	 * extra_bo: the current FW_ECHO opcode does not actually inspect
	 *         this buffer, but we sync it anyway so this example
	 *         stays a faithful template for ops that DO read their
	 *         "extra" / scratch BOs.  The sync is a no-op on contents
	 *         but exercises the contract.
	 */
	printf("[4/8] buf_sync TO_DEVICE (rsp_bo canary + extra_bo)\n");
	rc = amdrnpu_buf_sync_to_device(device_fd, rsp_bo, 0, 0);
	if (rc) { ret = report("buf_sync(rsp_bo, to_device)", rc); goto out; }
	rc = amdrnpu_buf_sync_to_device(device_fd, extra_bo, 0, 0);
	if (rc) { ret = report("buf_sync(extra_bo, to_device)", rc); goto out; }

	memset(args, 0, sizeof(args));
	args[0].size = AMDRNPU_FW_RSP_TEXT_MAX;
	args[0].bo.buffer_handle = (int32_t)rsp_bo;
	args[0].bo.offset = 0;
	args[1].size = 0;
	args[1].value = v1;
	args[2].size = 0;
	args[2].value = v2;
	args[3].size = AMDRNPU_FW_EXTRA_BO_SIZE;
	args[3].bo.buffer_handle = (int32_t)extra_bo;
	args[3].bo.offset = 0;

	memset(&hdr, 0, sizeof(hdr));
	printf("[5/8] cmd_submit op=0x%08x num_args=4\n", AMDRNPU_FW_ECHO_OP);
	rc = amdrnpu_cmd_submit(device_fd, AMDRNPU_FW_ECHO_OP, args, 4,
				     &hdr, sizeof(hdr));
	if (rc) { ret = report("cmd_submit", rc); goto out; }
	printf("      -> syncobj_handle=%d\n", hdr.sync_handle);

	printf("[6/8] syncobj_wait (timeout=%" PRId64 "ns)\n",
	       AMDRNPU_DEFAULT_WAIT_NS);
	rc = amdrnpu_syncobj_wait_complete(
		device_fd, (uint32_t)hdr.sync_handle,
		AMDRNPU_DEFAULT_WAIT_NS, &fence_st);
	if (rc) { ret = report("syncobj_wait", rc); goto out; }
	printf("      -> fence_status=%" PRId32 "\n", fence_st);
	if (fence_st < 0) {
		if (-fence_st > 0 && -fence_st < 4096)
			fprintf(stderr, "      fence errno: %s\n",
				strerror(-fence_st));
		ret = 1;
		goto out;
	}
	if (fence_st == 0) {
		fprintf(stderr, "      fence still pending after timeout\n");
		ret = 1;
		goto out;
	}

	/*
	 * RPU has signalled COMPLETE, which means its writes into the
	 * carveout are observable on the bus.  But the APU CPU may
	 * still have stale (canary) cache lines for this BO; invalidate
	 * before sscanf so we read the freshly-DMA'd ASCII reply.
	 */
	printf("[7/8] buf_sync FROM_DEVICE (invalidate CPU view)\n");
	rc = amdrnpu_buf_sync_from_device(device_fd, rsp_bo, 0, 0);
	if (rc) { ret = report("buf_sync(from_device)", rc); goto out; }

	printf("[8/8] parse RPU reply\n");
	if (sscanf(map, "rsp: command id: %u, num_args: %u",
		   &rpu_cid, &rpu_nargs) != 2) {
		fprintf(stderr,
			"      unexpected RPU banner; first 256B:\n%.256s\n",
			(char *)map);
		ret = 1;
		goto out;
	}
	printf("      RPU reply: command_id=%u num_args=%u\n",
	       rpu_cid, rpu_nargs);
	printf("      RPU text : %.*s\n",
	       (int)strnlen(map, AMDRNPU_FW_RSP_TEXT_MAX), (char *)map);
	if (rpu_nargs != 4) {
		fprintf(stderr, "      verify FAIL: expected num_args=4, got %u\n",
			rpu_nargs);
		ret = 1;
		goto out;
	}

out:
	if (map != MAP_FAILED)
		amdrnpu_buf_munmap(map, AMDRNPU_FW_RSP_BO_SIZE);
	if (extra_bo)
		(void)amdrnpu_buf_close(device_fd, extra_bo);
	if (rsp_bo)
		(void)amdrnpu_buf_close(device_fd, rsp_bo);
	if (ret == 0)
		printf("=== fw_echo: PASSED ===\n");
	else
		printf("=== fw_echo: FAILED (rc=%d) ===\n", ret);
	return ret;
}

/*
 * lifecycle: the smallest end-to-end exercise that touches both UAPI
 * ioctls plus the syncobj path -- alloc on the device bank, submit a
 * no-op command (op=0, no args), wait for completion, free. Useful as a
 * "is the driver alive?" smoke test that doesn't require firmware
 * cooperation beyond signaling completion.
 */
static int do_lifecycle(int device_fd)
{
	uint32_t bo = 0;
	struct amdrnpu_cmd_submit hdr;
	int32_t fence_st = 0;
	int rc, ret = 0;

	printf("=== lifecycle: alloc + no-op submit + wait + close ===\n");

	printf("[1/4] buf_alloc device size=4096\n");
	rc = amdrnpu_buf_alloc(device_fd, AMDRNPU_MEMORY_DEVICE, 4096, &bo);
	if (rc) { ret = report("buf_alloc", rc); goto out; }
	printf("      -> bo=%u\n", bo);

	memset(&hdr, 0, sizeof(hdr));
	printf("[2/4] cmd_submit op=0 num_args=0 (no-op)\n");
	rc = amdrnpu_cmd_submit(device_fd, 0, NULL, 0, &hdr, sizeof(hdr));
	if (rc) { ret = report("cmd_submit", rc); goto out; }
	printf("      -> syncobj_handle=%d\n", hdr.sync_handle);

	printf("[3/4] syncobj_wait (timeout=%" PRId64 "ns)\n",
	       AMDRNPU_DEFAULT_WAIT_NS);
	rc = amdrnpu_syncobj_wait_complete(
		device_fd, (uint32_t)hdr.sync_handle,
		AMDRNPU_DEFAULT_WAIT_NS, &fence_st);
	if (rc) {
		/* A no-op submit may not be honored by every firmware build;
		 * report rather than fail-hard so this stays useful as a
		 * driver-only smoke test.
		 */
		(void)report("syncobj_wait (non-fatal)", rc);
	} else {
		printf("      -> fence_status=%" PRId32 "\n", fence_st);
	}

out:
	if (bo) {
		printf("[4/4] buf_close bo=%u\n", bo);
		int rc2 = amdrnpu_buf_close(device_fd, bo);
		if (rc2)
			(void)report("buf_close (cleanup)", rc2);
	}
	if (ret == 0)
		printf("=== lifecycle: PASSED ===\n");
	else
		printf("=== lifecycle: FAILED (rc=%d) ===\n", ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--device /dev/amdrnpu.N] <command> [args...]\n"
		"\n"
		"Commands:\n"
		"  device                                       print which node was opened\n"
		"  buf_alloc <rpu|device> <size>                alloc a buffer then close\n"
		"  cmd_submit <op_hex> [scalar:U64 | buf:H:O]   one-shot AMDRNPU_CMD_SUBMIT\n"
		"  syncobj_wait <handle> [timeout_ns=2e9]       wait on completion + fence status\n"
		"  fw_echo <v1_hex> <v2_hex>                    full RPU-firmware echo flow\n"
		"  lifecycle                                    alloc + no-op submit + wait\n"
		"\n"
		"If --device is omitted the CLI scans /dev/amdrnpu.* and opens the\n"
		"lowest-numbered node.\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	char opened_path[PATH_MAX] = {0};
	int idx = 1;
	int device_fd, rc;
	const char *cmd;
	char **rest;
	int nrest;

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--help") == 0 ||
		    strcmp(argv[i], "-h")     == 0 ||
		    strcmp(argv[i], "help")   == 0) {
			usage(argv[0]);
			return 0;
		}
	}

	if (idx < argc && strcmp(argv[idx], "--device") == 0) {
		if (idx + 1 >= argc) {
			usage(argv[0]);
			return 2;
		}
		path = argv[idx + 1];
		idx += 2;
	}

	if (idx >= argc) {
		usage(argv[0]);
		return 2;
	}

	device_fd = open_amdrnpu_device(path, opened_path, sizeof(opened_path));
	if (device_fd < 0)
		return 1;

	cmd = argv[idx];
	rest = argv + idx + 1;
	nrest = argc - idx - 1;

	if      (strcmp(cmd, "device")       == 0) rc = do_device(opened_path);
	else if (strcmp(cmd, "buf_alloc")    == 0) rc = do_buf_alloc(device_fd, nrest, rest);
	else if (strcmp(cmd, "cmd_submit")   == 0) rc = do_cmd_submit(device_fd, nrest, rest);
	else if (strcmp(cmd, "syncobj_wait") == 0) rc = do_syncobj_wait(device_fd, nrest, rest);
	else if (strcmp(cmd, "fw_echo")      == 0) rc = do_fw_echo(device_fd, nrest, rest);
	else if (strcmp(cmd, "lifecycle")    == 0) rc = do_lifecycle(device_fd);
	else {
		fprintf(stderr, "Unknown command: %s\n", cmd);
		usage(argv[0]);
		rc = 2;
	}

	close(device_fd);
	return rc;
}
