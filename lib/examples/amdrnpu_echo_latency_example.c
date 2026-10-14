/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_echo_latency_example - measure APU <-> RPU_APP echo-message
 * round-trip latency over Link A (RPMsg).
 *
 * Each iteration submits a no-arg echo command (AMDRNPU_FW_ECHO_OP) and waits
 * for the firmware's completion fence. With num_args=0 there is no buffer DMA,
 * so the sample isolates the pure message path: submit ioctl -> RPMsg to
 * RPU_APP -> opcode dispatch -> response -> kernel fence signal -> wait wakeup.
 *
 * The RPU_APP test firmware (xrt_apu_rpu_test) answers AMDRNPU_FW_ECHO_OP with
 * AMDRNPU_RSP_COMPLETE, so the fence reaches a success terminal state.
 *
 * Usage:
 *   amdrnpu_echo_latency_example [options]
 *     --device /dev/amdrnpu.N   target node (default: scan /dev/amdrnpu.*)
 *     --iterations N            measured round-trips (default 1000)
 *     --busy-poll               poll the fence instead of a blocking wait
 *     --timeout S               per-op wait deadline in seconds (default 5)
 *     --op 0xVAL                command opcode (default 0x42424242 = echo)
 *
 * Prints a summary block (min/avg/max/stddev + throughput) and a lone "PASS"
 * line on success (exit 0), or "FAIL" (non-zero exit) on any submit/wait error.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

/* Firmware-side magic op for the Zephyr/RPU_APP echo path (AMDRNPU_FW_ECHO_OP). */
#define AMDRNPU_FW_ECHO_OP	0x42424242u

#define DEFAULT_ITERATIONS	1000u
#define DEFAULT_TIMEOUT_SEC	5u
/* Busy-poll cadence: yield-only tight loop (interval_ns == 0). */
#define BUSY_POLL_INTERVAL_NS	0ull

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static inline uint64_t ts_delta_ns(struct timespec a, struct timespec b)
{
	return (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll +
			  (b.tv_nsec - a.tv_nsec));
}

struct echo_sample {
	struct timespec t0;
	struct timespec t1;
};

/*
 * Same-signature wait wrappers so the blocking-vs-busy-poll choice is resolved
 * ONCE (before the measured loop) into a function pointer, instead of branching
 * on a loop-invariant inside every timed iteration.
 */
static int echo_wait_blocking(int fd, uint32_t handle, int64_t timeout_ns,
			      int32_t *status)
{
	return amdrnpu_syncobj_wait_complete(fd, handle, timeout_ns,
						  status);
}

static int echo_wait_busy_poll(int fd, uint32_t handle, int64_t timeout_ns,
			       int32_t *status)
{
	return amdrnpu_syncobj_wait_busy_poll(fd, handle, timeout_ns,
						   BUSY_POLL_INTERVAL_NS,
						   status);
}

/* ------------------------------------------------------------------ */
/* device discovery (mirrors amdrnpu_test.c)                           */
/* ------------------------------------------------------------------ */

/*
 * The library is configured from an INI file rather than a kernel node; the fd
 * is only an opaque API key.  Open the user-supplied config path, or the
 * default INI ($AMDRNPU_INI or /etc/amdrnpu.ini) when path is NULL.
 */
static int open_amdrnpu_device(const char *path, char *opened_path, size_t sz)
{
	int fd;

	if (!path || !path[0]) {
		path = getenv("AMDRNPU_INI");
		if (!path || !*path)
			path = "/etc/amdrnpu.ini";
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
		return -1;
	}
	if (opened_path && sz > 0)
		snprintf(opened_path, sz, "%s", path);
	return fd;
}

/* ------------------------------------------------------------------ */
/* echo round-trip for # iterations                                   */
/* ------------------------------------------------------------------ */

static int echo_roundtrip(int fd, uint32_t op, int64_t timeout_ns,
			  int busy_poll, unsigned int iterations,
			  struct echo_sample *samples)
{
	int (*wait_fn)(int, uint32_t, int64_t, int32_t *) =
		busy_poll ? echo_wait_busy_poll : echo_wait_blocking;
	struct amdrnpu_cmd_submit hdr;

	memset(&hdr, 0, sizeof(hdr));

	for (unsigned int i = 0; i < iterations; ++i) {
		int32_t status = 0;
		int rc;

		clock_gettime(CLOCK_MONOTONIC, &samples[i].t0);
		rc = amdrnpu_cmd_submit(fd, op, NULL, 0, &hdr,
					     sizeof(hdr));
		if (rc < 0) {
			fprintf(stderr, "iter %u cmd_submit: %s (rc=%d)\n",
				i, strerror(-rc), rc);
			return rc;
		}

		rc = wait_fn(fd, (uint32_t)hdr.sync_handle,
			     timeout_ns, &status);
		clock_gettime(CLOCK_MONOTONIC, &samples[i].t1);
		if (rc < 0) {
			fprintf(stderr, "iter %u syncobj_wait: %s (rc=%d)\n",
				i, strerror(-rc), rc);
			return rc;
		}
		if (status <= 0) {
			fprintf(stderr, "iter %u fence status=%" PRId32
				" (0=pending/timeout, negative=fence errno)\n",
				i, status);
			if (status < 0 && -status < 4096)
				fprintf(stderr, "  fence errno: %s\n",
					strerror(-status));
			return status == 0 ? -ETIMEDOUT : status;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --device /dev/amdrnpu.N   target node (default: scan /dev/amdrnpu.*)\n"
		"  --iterations N            measured round-trips (default %u)\n"
		"  --busy-poll               poll the fence instead of blocking wait\n"
		"  --timeout S               per-op wait deadline, seconds (default %u)\n"
		"  --op 0xVAL                command opcode (default 0x%08x)\n",
		prog, DEFAULT_ITERATIONS,
		DEFAULT_TIMEOUT_SEC, AMDRNPU_FW_ECHO_OP);
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	char opened_path[PATH_MAX] = {0};
	unsigned int iterations = DEFAULT_ITERATIONS;
	unsigned int timeout_sec = DEFAULT_TIMEOUT_SEC;
	uint32_t op = AMDRNPU_FW_ECHO_OP;
	int busy_poll = 0;
	int64_t timeout_ns;
	struct echo_sample *raw = NULL;
	uint64_t *samples = NULL;
	int fd = -1;
	int ret = 1;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			path = argv[++i];
		} else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
			iterations = (unsigned int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
			timeout_sec = (unsigned int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--op") && i + 1 < argc) {
			op = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--busy-poll")) {
			busy_poll = 1;
		} else {
			fprintf(stderr, "unknown/incomplete arg: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (iterations == 0) {
		fprintf(stderr, "--iterations must be > 0\n");
		return 2;
	}
	timeout_ns = (int64_t)timeout_sec * 1000000000ll;

	fd = open_amdrnpu_device(path, opened_path, sizeof(opened_path));
	if (fd < 0)
		goto out;

	raw = calloc(iterations, sizeof(*raw));
	samples = calloc(iterations, sizeof(*samples));
	if (!raw || !samples) {
		fprintf(stderr, "  [echo-lat] OOM allocating %u samples\n",
			iterations);
		goto out;
	}

	printf("  [echo-lat] device=%s op=0x%08x iters=%u mode=%s\n",
	       opened_path, op, iterations,
	       busy_poll ? "busy-poll" : "blocking");
	fflush(stdout);

	if (echo_roundtrip(fd, op, timeout_ns, busy_poll, iterations,
			   raw) < 0) {
		fprintf(stderr, "  [echo-lat] echo round-trips failed\n");
		goto out;
	}

	/* Convert the raw timestamps to per-iteration nanoseconds */
	for (unsigned int i = 0; i < iterations; ++i)
		samples[i] = ts_delta_ns(raw[i].t0, raw[i].t1);
	{
		uint64_t sum_ns = 0, min_ns = UINT64_MAX, max_ns = 0;
		double avg_us, var = 0.0, stddev_us;
		double total_s;

		for (unsigned int i = 0; i < iterations; ++i) {
			uint64_t s = samples[i];

			sum_ns += s;
			if (s < min_ns)
				min_ns = s;
			if (s > max_ns)
				max_ns = s;
		}
		avg_us = ((double)sum_ns / (double)iterations) / 1000.0;
		for (unsigned int i = 0; i < iterations; ++i) {
			double d = (double)samples[i] / 1000.0 - avg_us;

			var += d * d;
		}
		stddev_us = sqrt(var / (double)iterations);
		total_s = (double)sum_ns / 1e9;

		printf("  [echo-lat] avg=%.1fus min=%.1fus max=%.1fus stddev=%.1fus\n",
		       avg_us,
		       (double)min_ns / 1000.0,
		       (double)max_ns / 1000.0,
		       stddev_us);
		printf("  [echo-lat] throughput=%.0f msg/s\n",
		       total_s > 0.0 ? (double)iterations / total_s : 0.0);
	}

	ret = 0;

out:
	free(raw);
	free(samples);
	if (fd >= 0)
		close(fd);
	printf(ret == 0 ? "PASS\n" : "FAIL\n");
	return ret;
}
