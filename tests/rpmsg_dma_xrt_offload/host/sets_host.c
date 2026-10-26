/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * x86 host for RPURuntime tests/xrt_apu_rpu_sets_test (APU_XRT_SETS_TEST),
 * over the MDB5 PCIe-DMA rpmsg-lite path.
 *
 * One AIE control ELF, one kernel, N kernel-arg sets per request ("batches",
 * --sets, default 3). Each set is the kernel-arg list for one RPU run; the
 * RPU creates the hw_context once per request and runs set 0..N-1 in turn
 * (wire layout: lib/examples/apu_xrt_sets_ops.h).
 *
 * Kernel args, in kernel-arg order:
 *   --shared-in FILE     one input BO used by every set (e.g. weights)
 *   --in FILE[,FILE..]   per-set input BO: one file for all sets, or one
 *                        file per set (comma-separated, exactly --sets of them)
 *   --in-0 SIZE          per-set input BO of SIZE zero bytes (e.g. spill)
 *   --out SIZE           per-set output BO of SIZE bytes
 *
 * Files are loaded and device buffers allocated once. Then --rounds R
 * requests are sent one at a time; every round does:
 *   1. sync to device:   ELF, shared inputs, every set's inputs (in-0 zeroed)
 *   2. execute:          send APU_XRT_SETS_TEST, wait for the rpmsg reply or
 *                        the DDR result word
 *   3. sync from device: every set's output BOs
 * and the three phases are timed separately (CLOCK_MONOTONIC on the host).
 * The first request asks RPU_APP to reset its profile totals and the last one
 * to print its avg/min/max per-phase summary on the RPU_APP console after
 * replying (APU_XRT_SETS_FLAG_PROF_*).
 *
 * Usage (bevformer-t20, 3 batches sharing the weights):
 *   sets_host --ini offload_config.ini --model aie_control_james.elf --sets 3 \
 *       --shared-in wts32.bin --in ifm_a.bin,ifm_b.bin,ifm_c.bin \
 *       --in-0 4403200 --out 18000 --out 18000
 *
 * Quiet by default; --verbose prints the rpmsg/vring bring-up, buffer layout,
 * hexdumps of every rpmsg message and each completion check.
 */
#define _POSIX_C_SOURCE 200809L

#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_mdb5.h"
#include "amdrnpu_wire.h"
#include "apu_xrt_sets_ops.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <zlib.h>

#define DEFAULT_SETS       3u
#define DEFAULT_ROUNDS     1u
#define DEFAULT_TIMEOUT_S  60u
#define POOL_ALIGN         0x1000u     /* page-align every pool allocation */
#define RESULT_POISON      0xFFFFFFFFu /* result.passed sentinel = "not written yet" */
/* After completion is seen via the DDR result word, keep polling this long for
 * the rpmsg reply. The RPU writes the result before it sends the reply, so
 * without this the host can deinit (clearing DRIVER_OK) before the reply goes
 * out and the RPU's rpmsg_send fails with RPMSG_ERR_NO_BUFF (-2002). */
#define REPLY_WAIT_MS      500u
/* Wire limit: 6 header + 2*sets*kargs + 1 result args must fit one rpmsg
 * buffer (see apu_xrt_sets_ops.h), so sets*kargs <= 26. */
#define SETS_MAX           26u

static bool g_verbose;

#define VPRINTF(...) do { if (g_verbose) printf(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------------------
 * Config (inline INI parser). Same keys as xrt_offload_host.
 * ------------------------------------------------------------------------- */
struct config {
	char     mdb5_write[PATH_MAX];
	char     mdb5_read[PATH_MAX];

	uint64_t dev_base;
	uint64_t shm_size;
	uint64_t status_offset;
	uint64_t vring_tx_offset;
	uint64_t vring_rx_offset;
	uint64_t buf_offset;
	uint32_t vring_num;
	uint32_t vring_align;
	uint32_t buf_size;
	uint32_t buf_count;

	char     ept_name[RPMSG_LITE_NAME_SIZE];
	uint32_t host_ept_addr;
	uint32_t peer_dst;

	uint64_t rpu_pool_base;  /* ELF / descriptor / result (RPU-reachable). */
	uint64_t rpu_pool_size;
	uint64_t dev_pool_base;  /* kernel-arg BOs (AIE-reachable).            */
	uint64_t dev_pool_size;
};

static void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->mdb5_write, sizeof(c->mdb5_write), "%s", "/dev/mdb5_write00");
	snprintf(c->mdb5_read, sizeof(c->mdb5_read), "%s", "/dev/mdb5_read00");

	c->dev_base        = UINT64_C(0xBBF80000);
	c->shm_size        = 0x49000;
	c->status_offset   = 0x2C;
	c->vring_rx_offset = 0x1000;
	c->vring_tx_offset = 0x5000;
	c->buf_offset      = 0x9000;
	c->vring_num       = 8;
	c->vring_align     = 16;
	c->buf_size        = 512;
	c->buf_count       = 16;

	snprintf(c->ept_name, sizeof(c->ept_name), "%s", AMDRNPU_WIRE_CHANNEL);
	c->host_ept_addr   = 1024;
	c->peer_dst        = 0;

	c->rpu_pool_base   = UINT64_C(0x20000000);
	c->rpu_pool_size   = 0x08000000;
	c->dev_pool_base   = UINT64_C(0x28000000);
	c->dev_pool_size   = 0x08000000;
}

static char *trim(char *s)
{
	char *end;
	while (*s == ' ' || *s == '\t') ++s;
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

static int config_apply(struct config *c, const char *key, const char *val)
{
	uint64_t n;
#define NUM(field) do { if (parse_u64(val, &n)) return -EINVAL; \
	c->field = (__typeof__(c->field))n; return 0; } while (0)
	if (!strcmp(key, "dev_base"))        NUM(dev_base);
	if (!strcmp(key, "shm_size"))        NUM(shm_size);
	if (!strcmp(key, "status_offset"))   NUM(status_offset);
	if (!strcmp(key, "vring_tx_offset")) NUM(vring_tx_offset);
	if (!strcmp(key, "vring_rx_offset")) NUM(vring_rx_offset);
	if (!strcmp(key, "buf_offset"))      NUM(buf_offset);
	if (!strcmp(key, "vring_num"))       NUM(vring_num);
	if (!strcmp(key, "vring_align"))     NUM(vring_align);
	if (!strcmp(key, "buf_size"))        NUM(buf_size);
	if (!strcmp(key, "buf_count"))       NUM(buf_count);
	if (!strcmp(key, "host_ept_addr"))   NUM(host_ept_addr);
	if (!strcmp(key, "peer_dst"))        NUM(peer_dst);
	if (!strcmp(key, "rpu_pool_base"))   NUM(rpu_pool_base);
	if (!strcmp(key, "rpu_pool_size"))   NUM(rpu_pool_size);
	if (!strcmp(key, "dev_pool_base"))   NUM(dev_pool_base);
	if (!strcmp(key, "dev_pool_size"))   NUM(dev_pool_size);
#undef NUM
	if (!strcmp(key, "ept_name"))   { snprintf(c->ept_name, sizeof(c->ept_name), "%s", val); return 0; }
	if (!strcmp(key, "mdb5_write")) { snprintf(c->mdb5_write, sizeof(c->mdb5_write), "%s", val); return 0; }
	if (!strcmp(key, "mdb5_read"))  { snprintf(c->mdb5_read, sizeof(c->mdb5_read), "%s", val); return 0; }
	return -ENOENT;
}

static int config_load(struct config *c, const char *path)
{
	FILE *f;
	char line[512];
	int lineno = 0;

	config_defaults(c);
	if (!path)
		return 0;
	f = fopen(path, "re");
	if (!f)
		return -errno;
	while (fgets(line, sizeof(line), f)) {
		char *s, *eq, *key, *val;
		++lineno;
		s = trim(line);
		if (!*s || *s == '#' || *s == ';' || *s == '[')
			continue;
		eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = trim(s);
		val = eq + 1;
		for (char *q = val; *q; ++q) {
			if (*q == '#' || *q == ';') { *q = '\0'; break; }
		}
		val = trim(val);
		if (config_apply(c, key, val) == -EINVAL) {
			fprintf(stderr, "config: bad value for '%s' (line %d)\n", key, lineno);
			fclose(f);
			return -EINVAL;
		}
	}
	fclose(f);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Device-memory pool: bump allocator backed by MDB5 DMA. Allocations are never
 * reused across models, so no model's buffers (in particular its result word)
 * share a line the host already read for an earlier model.
 * ------------------------------------------------------------------------- */
struct pool {
	const struct rpmsg_mem_ops *mem;
	uint64_t base;
	uint64_t size;
	uint64_t off;
};

static int pool_alloc(struct pool *p, uint64_t len, uint64_t *out_pa)
{
	uint64_t aligned = (len + POOL_ALIGN - 1) & ~(uint64_t)(POOL_ALIGN - 1);
	if (p->off + aligned > p->size) {
		fprintf(stderr, "pool @0x%" PRIx64 " overflow: need %" PRIu64
			" B, %" PRIu64 " B free\n", p->base, aligned, p->size - p->off);
		return -ENOMEM;
	}
	*out_pa = p->base + p->off;
	p->off += aligned;
	return 0;
}

static int pool_write(struct pool *p, uint64_t pa, const void *src, uint64_t len)
{
	return p->mem->write(p->mem->priv, pa, src, len);
}

static int pool_zero(struct pool *p, uint64_t pa, uint64_t len)
{
	static uint8_t zeros[65536];
	uint64_t done = 0;
	while (done < len) {
		uint64_t n = len - done;
		if (n > sizeof(zeros)) n = sizeof(zeros);
		int rc = p->mem->write(p->mem->priv, pa + done, zeros, (size_t)n);
		if (rc) return rc;
		done += n;
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * File / ELF loading (transparently inflates a zlib-compressed control ELF).
 * ------------------------------------------------------------------------- */
static int slurp(const char *path, uint8_t **data, uint64_t *size)
{
	struct stat st;
	FILE *fp;
	uint8_t *buf;
	if (stat(path, &st) < 0) { fprintf(stderr, "stat(%s): %s\n", path, strerror(errno)); return -errno; }
	if (st.st_size <= 0) { fprintf(stderr, "%s: empty\n", path); return -EINVAL; }
	buf = malloc((size_t)st.st_size);
	if (!buf) return -ENOMEM;
	fp = fopen(path, "rb");
	if (!fp) { free(buf); return -errno; }
	if (fread(buf, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) { fclose(fp); free(buf); return -EIO; }
	fclose(fp);
	*data = buf;
	*size = (uint64_t)st.st_size;
	return 0;
}

static bool is_zlib(const uint8_t *d, uint64_t n)
{
	return n >= 2 && d[0] == 0x78 && (d[1] == 0x01 || d[1] == 0x9c || d[1] == 0xda);
}

static int zlib_inflate(const uint8_t *src, uint64_t src_len, uint8_t **out, uint64_t *out_len)
{
	z_stream zs;
	size_t cap = (size_t)src_len * 6u + 65536u;
	uint8_t *buf = malloc(cap);
	int zrc;
	if (!buf) return -ENOMEM;
	memset(&zs, 0, sizeof(zs));
	if (inflateInit(&zs) != Z_OK) { free(buf); return -EIO; }
	zs.next_in = (Bytef *)src; zs.avail_in = (uInt)src_len;
	zs.next_out = buf; zs.avail_out = (uInt)cap;
	for (;;) {
		zrc = inflate(&zs, Z_NO_FLUSH);
		if (zrc == Z_STREAM_END) break;
		if (zrc != Z_OK) { inflateEnd(&zs); free(buf); return -EIO; }
		if (zs.avail_out == 0) {
			size_t used = cap; uint8_t *nb;
			cap *= 2u; nb = realloc(buf, cap);
			if (!nb) { inflateEnd(&zs); free(buf); return -ENOMEM; }
			buf = nb; zs.next_out = buf + used; zs.avail_out = (uInt)(cap - used);
		}
	}
	*out_len = (uint64_t)(cap - zs.avail_out);
	*out = buf;
	inflateEnd(&zs);
	return 0;
}

static int load_elf(const char *path, uint8_t **data, uint64_t *size)
{
	uint8_t *raw = NULL, *elf = NULL;
	uint64_t raw_len = 0, elf_len = 0;
	int rc = slurp(path, &raw, &raw_len);
	if (rc) return rc;
	if (!is_zlib(raw, raw_len)) { *data = raw; *size = raw_len; return 0; }
	rc = zlib_inflate(raw, raw_len, &elf, &elf_len);
	free(raw);
	if (rc) { fprintf(stderr, "zlib inflate(%s) failed\n", path); return rc; }
	if (elf_len < 4 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
		fprintf(stderr, "%s: inflated data is not an ELF\n", path);
		free(elf); return -EINVAL;
	}
	VPRINTF("elf-file:   %s zlib %" PRIu64 " B -> raw ELF %" PRIu64 " B\n", path, raw_len, elf_len);
	*data = elf; *size = elf_len;
	return 0;
}

static int write_file(const char *path, const void *data, uint64_t len)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) { fprintf(stderr, "open(%s): %s\n", path, strerror(errno)); return -errno; }
	if (fwrite(data, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); return -EIO; }
	fclose(fp);
	return 0;
}

/* ---------------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------------- */
enum karg_kind { KARG_SHARED_IN, KARG_IN_FILE, KARG_IN_ZERO, KARG_OUT };

struct karg_spec {
	enum karg_kind kind;
	char          *list;              /* strdup of the --in/--shared-in value */
	char          *files[SETS_MAX];   /* pointers into list */
	uint32_t       n_files;
	uint64_t       size;              /* KARG_IN_ZERO / KARG_OUT */
};

struct opts {
	char             ini[PATH_MAX];
	char             elf_file[PATH_MAX];
	uint32_t         num_sets;
	uint32_t         rounds;
	uint32_t         rpu_iterations;  /* wire args[0]; 0 = num_sets */
	struct karg_spec kargs[APU_XRT_SETS_TEST_MAX_KARGS];
	uint32_t         n_kargs;
	uint32_t         timeout_s;
	uint32_t         poll_us;
	char             save_out[PATH_MAX];
	bool             want_result;
	bool             zero_out;
	bool             trace_dma;
};

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s --ini INI --model ELF [--sets N] [--rounds R] KARG... [OPTIONS]\n"
		"\n"
		"Kernel args, in kernel-arg order (up to %u):\n"
		"  --shared-in FILE    one input BO used by every set (e.g. weights)\n"
		"  --in FILE[,FILE..]  per-set input BO: one file for all sets, or one per set\n"
		"  --in-0 SIZE         per-set input BO of SIZE zero bytes (e.g. spill)\n"
		"  --out SIZE          per-set output BO of SIZE bytes\n"
		"\n"
		"Options:\n"
		"  --ini PATH          offload config INI (vrings + pools + MDB5 nodes)\n"
		"  --model PATH        AIE control ELF (zlib or raw; required)\n"
		"  --sets N            kernel-arg sets (batches) per request (default %u)\n"
		"  --rounds R          requests to send, one at a time (default %u)\n"
		"  --rpu-iterations N  RPU runs per request, set = run %% N (default 0 = one per set)\n"
		"  --timeout-s N       per-request completion timeout seconds (default %u)\n"
		"  --poll-us N         sleep between completion checks, us (default 0 = busy poll)\n"
		"  --no-result         wait for the rpmsg reply only (no DDR result word)\n"
		"  --no-zero-out       do not zero output BOs before each round (untimed step)\n"
		"  --save-out PREFIX   write outputs to PREFIX_r<round>_s<set>_arg<idx>.bin\n"
		"  --trace-dma         print dev_addr + len on every MDB5 DMA call\n"
		"  --verbose           debug output: link bring-up, layout, hexdumps, checks\n"
		"\n"
		"sets x kargs must be <= %u (one rpmsg buffer per request).\n",
		p, APU_XRT_SETS_TEST_MAX_KARGS, DEFAULT_SETS, DEFAULT_ROUNDS,
		DEFAULT_TIMEOUT_S, SETS_MAX);
}

static struct karg_spec *new_karg(struct opts *o)
{
	if (o->n_kargs >= APU_XRT_SETS_TEST_MAX_KARGS) {
		fprintf(stderr, "too many kernel args (max %u)\n", APU_XRT_SETS_TEST_MAX_KARGS);
		return NULL;
	}
	return &o->kargs[o->n_kargs++];
}

static int parse_size(const char *opt, const char *v, uint64_t *out)
{
	if (parse_u64(v, out) || *out == 0) {
		fprintf(stderr, "%s needs a non-zero SIZE, got '%s'\n", opt, v);
		return -EINVAL;
	}
	return 0;
}

/* Split a comma-separated file list into k->files (pointers into k->list). */
static int parse_files(const char *opt, const char *v, struct karg_spec *k)
{
	char *save = NULL;

	k->list = strdup(v);
	if (!k->list)
		return -ENOMEM;
	for (char *t = strtok_r(k->list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		if (k->n_files >= SETS_MAX) {
			fprintf(stderr, "%s: more than %u files\n", opt, SETS_MAX);
			return -EINVAL;
		}
		k->files[k->n_files++] = t;
	}
	if (!k->n_files) {
		fprintf(stderr, "%s needs a FILE\n", opt);
		return -EINVAL;
	}
	return 0;
}

static void free_opts(struct opts *o)
{
	for (uint32_t i = 0; i < o->n_kargs; i++)
		free(o->kargs[i].list);
}

static int parse_cli(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->num_sets = DEFAULT_SETS;
	o->rounds = DEFAULT_ROUNDS;
	o->timeout_s = DEFAULT_TIMEOUT_S;
	o->want_result = true;
	o->zero_out = true;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		bool has_val = i + 1 < argc;
		struct karg_spec *k;

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]); return 1;
		} else if (!strcmp(a, "--ini") && has_val) {
			snprintf(o->ini, sizeof(o->ini), "%s", argv[++i]);
		} else if (!strcmp(a, "--model") && has_val) {
			snprintf(o->elf_file, sizeof(o->elf_file), "%s", argv[++i]);
		} else if (!strcmp(a, "--sets") && has_val) {
			o->num_sets = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--rounds") && has_val) {
			o->rounds = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--rpu-iterations") && has_val) {
			o->rpu_iterations = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--timeout-s") && has_val) {
			o->timeout_s = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--poll-us") && has_val) {
			o->poll_us = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--save-out") && has_val) {
			snprintf(o->save_out, sizeof(o->save_out), "%s", argv[++i]);
		} else if (!strcmp(a, "--no-result")) {
			o->want_result = false;
		} else if (!strcmp(a, "--no-zero-out")) {
			o->zero_out = false;
		} else if (!strcmp(a, "--trace-dma")) {
			o->trace_dma = true;
		} else if (!strcmp(a, "--verbose")) {
			g_verbose = true;
		} else if (!strcmp(a, "--shared-in") && has_val) {
			if (!(k = new_karg(o))) return -EINVAL;
			k->kind = KARG_SHARED_IN;
			if (parse_files(a, argv[++i], k)) return -EINVAL;
			if (k->n_files != 1) {
				fprintf(stderr, "--shared-in takes exactly one FILE\n");
				return -EINVAL;
			}
		} else if (!strcmp(a, "--in") && has_val) {
			if (!(k = new_karg(o))) return -EINVAL;
			k->kind = KARG_IN_FILE;
			if (parse_files(a, argv[++i], k)) return -EINVAL;
		} else if (!strcmp(a, "--in-0") && has_val) {
			if (!(k = new_karg(o))) return -EINVAL;
			k->kind = KARG_IN_ZERO;
			if (parse_size(a, argv[++i], &k->size)) return -EINVAL;
		} else if (!strcmp(a, "--out") && has_val) {
			if (!(k = new_karg(o))) return -EINVAL;
			k->kind = KARG_OUT;
			if (parse_size(a, argv[++i], &k->size)) return -EINVAL;
		} else {
			fprintf(stderr, "unknown option or missing value: %s\n", a);
			return -EINVAL;
		}
	}

	if (!o->elf_file[0]) { fprintf(stderr, "error: --model is required\n"); return -EINVAL; }
	if (!o->n_kargs) { fprintf(stderr, "error: no kernel args given\n"); return -EINVAL; }
	if (!o->num_sets || !o->rounds) {
		fprintf(stderr, "error: --sets and --rounds must be >= 1\n");
		return -EINVAL;
	}
	if (o->num_sets * o->n_kargs > SETS_MAX) {
		fprintf(stderr, "error: %u sets x %u kargs = %u > %u (one rpmsg buffer per request)\n",
			o->num_sets, o->n_kargs, o->num_sets * o->n_kargs, SETS_MAX);
		return -EINVAL;
	}
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		const struct karg_spec *k = &o->kargs[i];
		if (k->kind == KARG_IN_FILE && k->n_files != 1 && k->n_files != o->num_sets) {
			fprintf(stderr, "error: karg %u: --in has %u files; give 1 (same for all "
				"sets) or %u (one per set)\n", i, k->n_files, o->num_sets);
			return -EINVAL;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * rpmsg response capture
 * ------------------------------------------------------------------------- */
static void hexdump(const char *tag, const void *p, size_t len)
{
	const uint8_t *b = p;
	fprintf(stderr, "%s (%zu B):\n", tag, len);
	for (size_t i = 0; i < len; i += 16) {
		fprintf(stderr, "  %04zx:", i);
		for (size_t j = i; j < i + 16 && j < len; j++)
			fprintf(stderr, " %02x", b[j]);
		fprintf(stderr, "\n");
	}
}

struct rsp_ctx {
	uint32_t     expect_seq;   /* reply for this seq completes the command */
	volatile int got;
	uint32_t     rsp_type;
};

static void on_rsp(struct rpmsg_lite_dma_ept *ept, const void *data, size_t len, void *priv)
{
	struct rsp_ctx *r = priv;
	const struct amdrnpu_wire_rsp *w = data;
	(void)ept;
	if (g_verbose)
		hexdump("rpmsg RX (response)", data, len);
	if (len < sizeof(*w))
		return;
	if (w->seq != r->expect_seq) {
		/* Late reply to an earlier model (its completion was taken from
		 * the DDR result word); ignore it. */
		VPRINTF("rpmsg RX: stale reply seq=%u (waiting for %u), ignored\n",
			w->seq, r->expect_seq);
		return;
	}
	r->rsp_type = w->command_rsp_type;
	r->got = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void sleep_us(uint32_t us)
{
	struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L };
	while (nanosleep(&ts, &ts) && errno == EINTR && !g_stop)
		;
}

static const char *rsp_str(uint32_t t)
{
	switch (t) {
	case AMDRNPU_WIRE_RSP_COMPLETE:     return "COMPLETE";
	case AMDRNPU_WIRE_RSP_REMOTEIO:     return "REMOTEIO";
	case AMDRNPU_WIRE_RSP_NOTSUPPORTED: return "NOTSUPPORTED";
	case AMDRNPU_WIRE_RSP_CANCELLED:    return "CANCELLED";
	default:                            return "UNKNOWN";
	}
}

/* ---------------------------------------------------------------------------
 * DMA tracing shim (--trace-dma): prints the first access to each
 * (dir, addr, len) so a busy poll loop does not flood stderr.
 * ------------------------------------------------------------------------- */
#define DMA_TRACE_SEEN_MAX 1024
struct dma_trace {
	const struct rpmsg_mem_ops *inner;
	unsigned long seq;
	struct { uint64_t addr; size_t len; int write; } seen[DMA_TRACE_SEEN_MAX];
	size_t n_seen;
};

static int dma_trace_first(struct dma_trace *t, int write, uint64_t addr, size_t len)
{
	for (size_t i = 0; i < t->n_seen; i++)
		if (t->seen[i].write == write && t->seen[i].addr == addr &&
		    t->seen[i].len == len)
			return 0;
	if (t->n_seen < DMA_TRACE_SEEN_MAX) {
		t->seen[t->n_seen].addr  = addr;
		t->seen[t->n_seen].len   = len;
		t->seen[t->n_seen].write = write;
		t->n_seen++;
	}
	return 1;
}

static int trace_write(void *priv, uint64_t dev_addr, const void *src, size_t len)
{
	struct dma_trace *t = priv;
	if (dma_trace_first(t, 1, dev_addr, len))
		fprintf(stderr, "[dma %6lu] WR dev_addr=0x%010" PRIx64 " len=%zu (first)\n",
			t->seq, dev_addr, len);
	t->seq++;
	return t->inner->write(t->inner->priv, dev_addr, src, len);
}

static int trace_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct dma_trace *t = priv;
	if (dma_trace_first(t, 0, dev_addr, len))
		fprintf(stderr, "[dma %6lu] RD dev_addr=0x%010" PRIx64 " len=%zu (first)\n",
			t->seq, dev_addr, len);
	t->seq++;
	return t->inner->read(t->inner->priv, dst, dev_addr, len);
}

/* ---------------------------------------------------------------------------
 * Word-aligning shim: the MDB5 DMA engine only moves 4-byte aligned words, so
 * expand every transfer to 4-byte granularity (RMW for partial head/tail).
 * ------------------------------------------------------------------------- */
#define DMA_WORD 4u
struct dma_align { const struct rpmsg_mem_ops *inner; };

static int align_rmw_word(const struct rpmsg_mem_ops *in, uint64_t wbase,
			  unsigned off, const uint8_t *src, unsigned n)
{
	uint8_t w[DMA_WORD];
	int rc = in->read(in->priv, w, wbase, DMA_WORD);
	if (rc) return rc;
	memcpy(w + off, src, n);
	return in->write(in->priv, wbase, w, DMA_WORD);
}

static int align_write(void *priv, uint64_t dev_addr, const void *src, size_t len)
{
	const struct rpmsg_mem_ops *in = ((struct dma_align *)priv)->inner;
	const uint8_t *p = src;
	uint64_t addr = dev_addr;

	if (addr & (DMA_WORD - 1)) {
		uint64_t wbase = addr & ~(uint64_t)(DMA_WORD - 1);
		unsigned off = (unsigned)(addr - wbase);
		unsigned n = DMA_WORD - off;
		if (n > len) n = (unsigned)len;
		int rc = align_rmw_word(in, wbase, off, p, n);
		if (rc) return rc;
		addr += n; p += n; len -= n;
	}
	size_t full = len & ~(size_t)(DMA_WORD - 1);
	if (full) {
		int rc = in->write(in->priv, addr, p, full);
		if (rc) return rc;
		addr += full; p += full; len -= full;
	}
	if (len) {
		int rc = align_rmw_word(in, addr, 0, p, (unsigned)len);
		if (rc) return rc;
	}
	return 0;
}

static int align_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	const struct rpmsg_mem_ops *in = ((struct dma_align *)priv)->inner;
	uint8_t *p = dst;
	uint64_t addr = dev_addr;
	uint8_t w[DMA_WORD];

	if (addr & (DMA_WORD - 1)) {
		uint64_t wbase = addr & ~(uint64_t)(DMA_WORD - 1);
		unsigned off = (unsigned)(addr - wbase);
		unsigned n = DMA_WORD - off;
		if (n > len) n = (unsigned)len;
		int rc = in->read(in->priv, w, wbase, DMA_WORD);
		if (rc) return rc;
		memcpy(p, w + off, n);
		addr += n; p += n; len -= n;
	}
	size_t full = len & ~(size_t)(DMA_WORD - 1);
	if (full) {
		int rc = in->read(in->priv, p, addr, full);
		if (rc) return rc;
		addr += full; p += full; len -= full;
	}
	if (len) {
		int rc = in->read(in->priv, w, addr, DMA_WORD);
		if (rc) return rc;
		memcpy(p, w, len);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Per-round run
 * ------------------------------------------------------------------------- */
struct link {
	struct rpmsg_lite_dma      *ctx;
	struct rpmsg_lite_dma_ept  *ept;
	struct rsp_ctx             *rsp;
	const struct rpmsg_mem_ops *mem;
	struct pool                *rpu_pool;
	struct pool                *dev_pool;
	uint32_t                    peer_dst;
	uint64_t                    vr0_used_addr;  /* verbose completion trace */
};


/* Wait until the rpmsg reply for this command arrives or the RPU writes the
 * DDR result word. Returns 0 on completion, negative errno on error/timeout. */
static int wait_done(const struct link *l, const struct opts *o, uint64_t result_pa,
		     bool *via_rsp)
{
	uint64_t t0 = now_ns();
	uint64_t deadline = t0 + (uint64_t)o->timeout_s * 1000000000ull;
	uint32_t last_used_w = 0, last_w0 = 0;
	bool first = true;

	for (unsigned long chk = 0;; chk++) {
		int rc = rpmsg_lite_dma_poll(l->ctx);
		if (rc < 0) { fprintf(stderr, "poll: %s\n", strerror(-rc)); return rc; }
		if (l->rsp->got) { *via_rsp = true; return 0; }

		uint32_t w0 = RESULT_POISON;
		if (o->want_result) {
			rc = l->mem->read(l->mem->priv, &w0, result_pa, sizeof(w0));
			if (rc) {
				fprintf(stderr, "result read @0x%" PRIx64 ": %s\n", result_pa, strerror(-rc));
				return rc;
			}
		}
		if (g_verbose) {
			uint32_t used_w = 0;
			l->mem->read(l->mem->priv, &used_w, l->vr0_used_addr, sizeof(used_w));
			if (first || used_w != last_used_w || w0 != last_w0)
				printf("  [chk %lu t+%.3fms] vring0.used=0x%08x (idx=%u) result=0x%08x%s\n",
				       chk, (double)(now_ns() - t0) / 1e6, used_w, used_w >> 16, w0,
				       w0 == RESULT_POISON ? " (poison)" : "");
			last_used_w = used_w;
			first = false;
		}
		last_w0 = w0;
		if (o->want_result && w0 != RESULT_POISON) { *via_rsp = false; return 0; }

		if (g_stop) { fprintf(stderr, "interrupted\n"); return -EINTR; }
		if (now_ns() > deadline) {
			fprintf(stderr, "TIMEOUT after %us / %lu checks: no rpmsg reply and no DDR "
				"result (last result=0x%08x)\n", o->timeout_s, chk + 1, last_w0);
			return -ETIMEDOUT;
		}
		if (o->poll_us)
			sleep_us(o->poll_us);
	}
}

/* Poll for the rpmsg reply of the in-flight command for up to REPLY_WAIT_MS.
 * Returns true if it arrived. */
static bool wait_reply(const struct link *l)
{
	uint64_t deadline = now_ns() + (uint64_t)REPLY_WAIT_MS * 1000000ull;

	while (!l->rsp->got && !g_stop && now_ns() < deadline) {
		if (rpmsg_lite_dma_poll(l->ctx) < 0)
			break;
		if (!l->rsp->got)
			sleep_us(1000);
	}
	return l->rsp->got != 0;
}

struct round_stat {
	uint64_t elf_ns, shared_ns, sets_ns, init_ns;  /* sync to device, split   */
	uint64_t exec_ns;                              /* send -> completion seen */
	uint64_t out_ns;                               /* sync from device        */
	uint64_t to_bytes, out_bytes;
	uint32_t iterations_done;                      /* RPU result struct       */
	uint32_t rpu_avg_us, rpu_first_us;
	bool     via_rsp;                              /* completion by rpmsg reply */
	bool     have_result;
	bool     have_prof;                            /* prof_magic matched      */
	bool     passed;
	struct apu_xrt_sets_test_result res;           /* raw RPU result + profile */
};

/* Host + device buffers, set up once and reused by every round. A shared
 * karg has one device buffer (pa[0], used by every set); per-set kargs have
 * one per set. data[s] is the host copy for file inputs (aliases data[0]
 * when one file feeds every set), out[s] the readback buffer for outputs. */
struct karg_bufs {
	uint64_t pa[SETS_MAX];
	uint64_t size[SETS_MAX];
	uint8_t *data[SETS_MAX];
	uint8_t *out[SETS_MAX];
};

struct bufs {
	uint8_t         *elf;
	uint64_t         elf_sz, elf_pa;
	struct karg_bufs k[APU_XRT_SETS_TEST_MAX_KARGS];
};

static void free_bufs(const struct opts *o, struct bufs *b)
{
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		struct karg_bufs *kb = &b->k[i];
		bool one_file = o->kargs[i].n_files == 1;
		for (uint32_t s = 0; s < SETS_MAX; s++) {
			if (!one_file || s == 0)
				free(kb->data[s]);
			free(kb->out[s]);
		}
	}
	free(b->elf);
	memset(b, 0, sizeof(*b));
}

/* Untimed, once: host file I/O and pool allocation. */
static int prepare(const struct link *l, const struct opts *o, struct bufs *b)
{
	int rc;

	memset(b, 0, sizeof(*b));
	rc = load_elf(o->elf_file, &b->elf, &b->elf_sz);
	if (rc) return rc;
	rc = pool_alloc(l->rpu_pool, b->elf_sz, &b->elf_pa);
	if (rc) return rc;

	for (uint32_t i = 0; i < o->n_kargs; i++) {
		const struct karg_spec *k = &o->kargs[i];
		struct karg_bufs *kb = &b->k[i];
		uint32_t n_dev = (k->kind == KARG_SHARED_IN) ? 1u : o->num_sets;

		for (uint32_t s = 0; s < n_dev; s++) {
			if (k->kind == KARG_SHARED_IN || k->kind == KARG_IN_FILE) {
				if (k->n_files == 1 && s > 0) {
					kb->data[s] = kb->data[0];
					kb->size[s] = kb->size[0];
				} else {
					rc = slurp(k->files[s], &kb->data[s], &kb->size[s]);
					if (rc) return rc;
				}
			} else {
				kb->size[s] = k->size;
			}
			if (k->kind == KARG_OUT) {
				kb->out[s] = malloc(kb->size[s]);
				if (!kb->out[s]) return -ENOMEM;
			}
			rc = pool_alloc(l->dev_pool, kb->size[s], &kb->pa[s]);
			if (rc) return rc;
		}
		if (k->kind == KARG_SHARED_IN) {
			for (uint32_t s = 1; s < o->num_sets; s++) {
				kb->pa[s] = kb->pa[0];
				kb->size[s] = kb->size[0];
			}
		}
	}

	VPRINTF("\nelf: %s PA=0x%" PRIx64 " (%" PRIu64 " B)\n", o->elf_file, b->elf_pa, b->elf_sz);
	for (uint32_t s = 0; s < o->num_sets; s++)
		for (uint32_t i = 0; i < o->n_kargs; i++) {
			const struct karg_spec *k = &o->kargs[i];
			VPRINTF("  set[%u] karg[%2u] PA=0x%010" PRIx64 " size=%-10" PRIu64 " %s %s\n",
				s, i, b->k[i].pa[s], b->k[i].size[s],
				k->kind == KARG_SHARED_IN ? "shared-in" :
				k->kind == KARG_IN_FILE   ? "in       " :
				k->kind == KARG_IN_ZERO   ? "in-0     " : "out      ",
				(k->kind == KARG_SHARED_IN || k->kind == KARG_IN_FILE)
					? k->files[k->n_files == 1 ? 0 : s] : "");
		}
	return 0;
}

/* One request: sync to device, submit + wait, sync from device. */
static int run_round(const struct link *l, const struct opts *o, uint32_t round,
		     struct bufs *b, struct round_stat *st)
{
	const uint32_t S = o->num_sets, K = o->n_kargs;
	const uint32_t num_args = APU_XRT_SETS_TEST_NUM_ARGS(S, K);
	uint64_t result_pa = 0, t;
	uint32_t seq = round + 1;
	int rc;

	memset(st, 0, sizeof(*st));

	/* ---- untimed: fresh result slot per round, zero the output BOs ------ */
	if (o->want_result) {
		/* Never reuse a result word the host already polled. */
		rc = pool_alloc(l->rpu_pool, APU_XRT_SETS_TEST_RESULT_SIZE, &result_pa);
		if (rc) return rc;
	}
	for (uint32_t i = 0; i < K && o->zero_out; i++) {
		if (o->kargs[i].kind != KARG_OUT)
			continue;
		for (uint32_t s = 0; s < S; s++) {
			rc = pool_zero(l->dev_pool, b->k[i].pa[s], b->k[i].size[s]);
			if (rc) { fprintf(stderr, "zero out set %u arg %u: %s\n", s, i, strerror(-rc)); return rc; }
		}
	}

	/* ---- 1. sync to device --------------------------------------------- */
	t = now_ns();
	rc = pool_write(l->rpu_pool, b->elf_pa, b->elf, b->elf_sz);
	if (rc) { fprintf(stderr, "sync elf: %s\n", strerror(-rc)); return rc; }
	st->elf_ns = now_ns() - t;
	st->to_bytes += b->elf_sz;

	t = now_ns();
	for (uint32_t i = 0; i < K; i++) {
		if (o->kargs[i].kind != KARG_SHARED_IN)
			continue;
		rc = pool_write(l->dev_pool, b->k[i].pa[0], b->k[i].data[0], b->k[i].size[0]);
		if (rc) { fprintf(stderr, "sync shared arg %u: %s\n", i, strerror(-rc)); return rc; }
		st->to_bytes += b->k[i].size[0];
	}
	st->shared_ns = now_ns() - t;

	t = now_ns();
	for (uint32_t s = 0; s < S; s++) {
		for (uint32_t i = 0; i < K; i++) {
			const struct karg_bufs *kb = &b->k[i];
			if (o->kargs[i].kind == KARG_IN_FILE)
				rc = pool_write(l->dev_pool, kb->pa[s], kb->data[s], kb->size[s]);
			else if (o->kargs[i].kind == KARG_IN_ZERO)
				rc = pool_zero(l->dev_pool, kb->pa[s], kb->size[s]);
			else
				continue;
			if (rc) { fprintf(stderr, "sync set %u arg %u: %s\n", s, i, strerror(-rc)); return rc; }
			st->to_bytes += kb->size[s];
		}
	}
	st->sets_ns = now_ns() - t;

	t = now_ns();
	if (o->want_result) {
		uint8_t poison[APU_XRT_SETS_TEST_RESULT_SIZE];
		memset(poison, 0xFF, sizeof(poison));   /* RPU overwrites with real status */
		rc = pool_write(l->rpu_pool, result_pa, poison, sizeof(poison));
		if (rc) { fprintf(stderr, "sync result init: %s\n", strerror(-rc)); return rc; }
	}
	st->init_ns = now_ns() - t;

	/* ---- 2. execute: submit APU_XRT_SETS_TEST and wait for completion --- */
	size_t cmd_bytes = sizeof(struct amdrnpu_wire_cmd) + (size_t)num_args * sizeof(uint64_t);
	uint8_t cmdbuf[sizeof(struct amdrnpu_wire_cmd) +
		       APU_XRT_SETS_TEST_NUM_ARGS(SETS_MAX, 1) * sizeof(uint64_t)];
	struct amdrnpu_wire_cmd *cmd = (struct amdrnpu_wire_cmd *)cmdbuf;
	cmd->seq = seq;
	cmd->command_op = APU_XRT_SETS_TEST;
	cmd->flags = 0;
	cmd->num_args = num_args;
	cmd->args[0] = o->rpu_iterations;
	cmd->args[1] = b->elf_pa;
	cmd->args[2] = b->elf_sz;
	cmd->args[3] = S;
	cmd->args[4] = K;
	/* RPU_APP profile: start fresh on the first round, print the summary on
	 * the RPU_APP console after the last round's reply. */
	cmd->args[APU_XRT_SETS_TEST_FLAGS_ARG_IDX] =
		(round == 0 ? APU_XRT_SETS_FLAG_PROF_RESET : 0u) |
		(round + 1 == o->rounds ? APU_XRT_SETS_FLAG_PROF_REPORT : 0u);
	for (uint32_t s = 0; s < S; s++)
		for (uint32_t i = 0; i < K; i++) {
			uint32_t idx = APU_XRT_SETS_TEST_KARG_ARG_IDX(s, i, K);
			cmd->args[idx] = b->k[i].pa[s];
			cmd->args[idx + 1] = b->k[i].size[s];
		}
	cmd->args[APU_XRT_SETS_TEST_RESULT_ARG_IDX(S, K)] = result_pa;
	if (g_verbose)
		hexdump("rpmsg TX (APU_XRT_SETS_TEST)", cmdbuf, cmd_bytes);

	l->rsp->expect_seq = seq;
	l->rsp->got = 0;
	t = now_ns();
	rc = rpmsg_lite_dma_sendto(l->ctx, l->ept, cmdbuf, cmd_bytes, l->peer_dst);
	if (rc) { fprintf(stderr, "sendto: %s\n", strerror(-rc)); return rc; }
	rc = wait_done(l, o, result_pa, &st->via_rsp);
	st->exec_ns = now_ns() - t;
	if (rc) return rc;

	/* ---- 3. sync from device: every set's output BOs ------------------- */
	t = now_ns();
	for (uint32_t s = 0; s < S; s++)
		for (uint32_t i = 0; i < K; i++) {
			struct karg_bufs *kb = &b->k[i];
			if (o->kargs[i].kind != KARG_OUT)
				continue;
			rc = l->mem->read(l->mem->priv, kb->out[s], kb->pa[s], kb->size[s]);
			if (rc) { fprintf(stderr, "sync out set %u arg %u: %s\n", s, i, strerror(-rc)); return rc; }
			st->out_bytes += kb->size[s];
		}
	st->out_ns = now_ns() - t;

	/* Completion came from the DDR result word: collect the rpmsg reply too
	 * (untimed) so the RPU's send succeeds and the rings stay in step. */
	bool got_reply = st->via_rsp || wait_reply(l);
	if (!got_reply)
		fprintf(stderr, "round %u: no rpmsg reply within %u ms after DDR completion\n",
			round, REPLY_WAIT_MS);

	/* ---- status: rpmsg reply type and/or the RPU result struct ---------- */
	st->passed = true;
	if (got_reply && l->rsp->rsp_type != AMDRNPU_WIRE_RSP_COMPLETE) {
		fprintf(stderr, "round %u: RPU replied %s\n", round, rsp_str(l->rsp->rsp_type));
		st->passed = false;
	}
	if (o->want_result) {
		struct apu_xrt_sets_test_result res;
		uint32_t expect = o->rpu_iterations ? o->rpu_iterations : S;
		memset(&res, 0, sizeof(res));
		rc = l->mem->read(l->mem->priv, &res, result_pa, sizeof(res));
		if (rc) { fprintf(stderr, "read result: %s\n", strerror(-rc)); return rc; }
		if (res.passed != RESULT_POISON) {
			st->have_result = true;
			st->res = res;
			st->have_prof = (res.prof_magic == APU_XRT_SETS_PROF_MAGIC);
			st->iterations_done = res.iterations_done;
			st->rpu_avg_us = res.latency_avg_us;
			st->rpu_first_us = res.latency_first_us;
			if (!res.passed || res.iterations_done != expect)
				st->passed = false;
		}
		VPRINTF("  result: passed=%u iterations_done=%u/%u avg_us=%u first_us=%u\n",
			res.passed, res.iterations_done, expect, res.latency_avg_us,
			res.latency_first_us);
	}

	if (o->save_out[0]) {
		for (uint32_t s = 0; s < S; s++)
			for (uint32_t i = 0; i < K; i++) {
				char path[PATH_MAX + 64];
				if (o->kargs[i].kind != KARG_OUT)
					continue;
				snprintf(path, sizeof(path), "%s_r%u_s%u_arg%u.bin", o->save_out, round, s, i);
				rc = write_file(path, b->k[i].out[s], b->k[i].size[s]);
				if (rc) return rc;
				VPRINTF("  saved %s (%" PRIu64 " B)\n", path, b->k[i].size[s]);
			}
	}
	return 0;
}

static double ms(uint64_t ns) { return (double)ns / 1e6; }

/* avg / min / max over rounds, in ns. */
struct agg {
	double   sum;
	uint64_t min, max;
	uint32_t n;
};

static void agg_add(struct agg *a, uint64_t v)
{
	if (!a->n || v < a->min) a->min = v;
	if (!a->n || v > a->max) a->max = v;
	a->sum += (double)v;
	a->n++;
}

static void agg_print(const char *name, const struct agg *a, const char *note)
{
	if (!a->n) {
		printf("  %-26s %12s %12s %12s %6s  %s\n", name, "-", "-", "-", "0", note);
		return;
	}
	printf("  %-26s %12.3f %12.3f %12.3f %6u  %s\n", name,
	       a->sum / a->n / 1e3, (double)a->min / 1e3, (double)a->max / 1e3, a->n, note);
}

/* RPU-reported value usable for stats: not the 0xFF placeholder. */
static bool prof_ok(uint32_t v) { return v != RESULT_POISON; }

static void print_profile(const struct opts *o, const struct round_stat *st, uint32_t n_done)
{
	struct agg h_to = {0}, h_exec = {0}, h_from = {0}, h_gap = {0};
	struct agg rx = {0}, parse = {0}, hwctx = {0}, bomap = {0}, bind = {0};
	struct agg exec = {0}, exec_max = {0}, teardown = {0}, handler = {0};
	struct agg fill = {0}, send = {0};
	uint32_t n_prof = 0;

	for (uint32_t r = 0; r < n_done; r++) {
		const struct round_stat *s = &st[r];
		const struct apu_xrt_sets_test_result *p = &s->res;

		agg_add(&h_to, s->elf_ns + s->shared_ns + s->sets_ns + s->init_ns);
		agg_add(&h_exec, s->exec_ns);
		agg_add(&h_from, s->out_ns);
		if (!s->have_prof)
			continue;
		n_prof++;
		agg_add(&rx, p->rx_to_start_ns);
		agg_add(&parse, p->elf_parse_ns);
		agg_add(&hwctx, p->hwctx_ns);
		agg_add(&bomap, p->bo_map_ns);
		agg_add(&bind, p->bind_ns);
		agg_add(&exec, p->exec_ns);
		agg_add(&exec_max, p->exec_max_ns);
		agg_add(&teardown, p->teardown_ns);
		agg_add(&handler, p->handler_ns);
		if (prof_ok(p->result_fill_ns))
			agg_add(&fill, p->result_fill_ns);
		if (p->prev_send_ns && prof_ok(p->prev_send_ns))
			agg_add(&send, p->prev_send_ns);
		/* Host send->completion minus what the RPU accounted for: vring
		 * poll wait (0-1 ms), host completion polling, DMA reads. */
		uint64_t rpu = (uint64_t)p->rx_to_start_ns + p->handler_ns;
		if (s->exec_ns > rpu)
			agg_add(&h_gap, s->exec_ns - rpu);
	}

	printf("\nProfile over %u round(s), %u with an RPU profile (%u set(s)/request); times in us\n",
	       n_done, n_prof, o->num_sets);
	printf("  %-26s %12s %12s %12s %6s\n", "phase", "avg", "min", "max", "n");
	printf("  host:\n");
	agg_print("sync to device", &h_to, "elf + shared + sets + result init");
	agg_print("exec (send -> done)", &h_exec, "host-observed command time");
	agg_print("sync from device", &h_from, "every set's outputs");
	printf("  RPU APP:\n");
	agg_print("rx -> start processing", &rx, "RPMsg RX callback -> handler start");
	agg_print("ELF parse", &parse, "xrt::elf + kernel lookup");
	agg_print("hw_context + kernel", &hwctx, "via RPU_FW (Link B)");
	agg_print("bo map + run create", &bomap, "");
	agg_print("set_arg (bind)", &bind, "sum over sets");
	agg_print("execution", &exec, "run start -> wait, sum over sets");
	agg_print("execution (longest run)", &exec_max, "per command");
	agg_print("teardown", &teardown, "run/bo/kernel/hw_context destroy");
	agg_print("handler total", &handler, "handler start -> before result fill");
	agg_print("result fill", &fill, "write result struct");
	agg_print("reply send + IPI kick", &send, "rpmsg_send (previous command)");
	agg_print("not covered by RPU", &h_gap, "host exec - (rx->start + handler)");
	if (!n_prof && n_done)
		printf("  (no RPU profile: RPU_APP predates the profiling result layout)\n");
}

static void print_stats(const struct opts *o, const struct round_stat *st, uint32_t n_done)
{
	uint64_t sum_to = 0, sum_exec = 0, sum_from = 0;

	printf("\n%-5s %10s %9s %9s %9s %9s %10s %11s %8s %10s %-4s\n",
	       "round", "to_dev_ms", "(elf", "shared", "sets", "init)", "exec_ms",
	       "from_dev_ms", "rpu_runs", "rpu_avg_us", "ok");
	for (uint32_t r = 0; r < n_done; r++) {
		const struct round_stat *s = &st[r];
		uint64_t to = s->elf_ns + s->shared_ns + s->sets_ns + s->init_ns;
		char runs[16], rpu[16];

		if (s->have_result) {
			snprintf(runs, sizeof(runs), "%u", s->iterations_done);
			snprintf(rpu, sizeof(rpu), "%u", s->rpu_avg_us);
		} else {
			snprintf(runs, sizeof(runs), "-");
			snprintf(rpu, sizeof(rpu), "-");
		}
		printf("%-5u %10.3f %9.3f %9.3f %9.3f %9.3f %10.3f %11.3f %8s %10s %-4s%s\n",
		       r, ms(to), ms(s->elf_ns), ms(s->shared_ns), ms(s->sets_ns), ms(s->init_ns),
		       ms(s->exec_ns), ms(s->out_ns), runs, rpu, s->passed ? "yes" : "NO",
		       s->via_rsp ? "" : " (ddr)");
		sum_to += to;
		sum_exec += s->exec_ns;
		sum_from += s->out_ns;
	}
	if (!n_done)
		return;
	printf("%-5s %10.3f %39s %10.3f %11.3f\n", "avg",
	       ms(sum_to) / n_done, "", ms(sum_exec) / n_done, ms(sum_from) / n_done);
	printf("%-5s %10.3f %39s %10.3f %11.3f\n", "total",
	       ms(sum_to), "", ms(sum_exec), ms(sum_from));
	printf("\n%u set(s) per request. to_dev = elf + shared inputs + every set's inputs\n"
	       "(in-0 zeroed) + result init; exec = rpmsg send -> completion seen (\"(ddr)\" =\n"
	       "from the DDR result word); from_dev = every set's outputs; rpu_avg_us = AIE\n"
	       "run time per set reported by the RPU.\n", o->num_sets);
}

int main(int argc, char **argv)
{
	struct opts opts;
	struct config cfg;
	struct rpmsg_mem_mdb5 mdb5;
	struct rpmsg_mem_ops mdb5_ops;
	struct dma_trace trace;
	struct rpmsg_mem_ops trace_ops;
	struct dma_align align;
	struct rpmsg_mem_ops align_ops;
	const struct rpmsg_mem_ops *mem;
	struct rpmsg_lite_dma ctx;
	struct rpmsg_lite_dma_ept ept;
	struct rsp_ctx rsp = { 0 };
	struct pool rpu_pool, dev_pool;
	struct round_stat *stats = NULL;
	struct bufs *bufs = NULL;
	uint8_t *scratch = NULL;
	uint32_t n_done = 0;
	int ret = 1, rc;

	rc = parse_cli(argc, argv, &opts);
	if (rc > 0) { free_opts(&opts); return 0; }
	if (rc < 0) { usage(argv[0]); free_opts(&opts); return 2; }

	rc = config_load(&cfg, opts.ini[0] ? opts.ini : NULL);
	if (rc) { fprintf(stderr, "config load '%s': %s\n", opts.ini, strerror(-rc)); goto out_opts; }

	/* The whole request must fit one rpmsg buffer (16 B rpmsg header). */
	size_t cmd_bytes = sizeof(struct amdrnpu_wire_cmd) +
		(size_t)APU_XRT_SETS_TEST_NUM_ARGS(opts.num_sets, opts.n_kargs) * sizeof(uint64_t);
	if (cmd_bytes + sizeof(struct rpmsg_hdr) > cfg.buf_size) {
		fprintf(stderr, "error: request is %zu B, exceeds rpmsg buffer payload %zu B\n",
			cmd_bytes, (size_t)cfg.buf_size - sizeof(struct rpmsg_hdr));
		goto out_opts;
	}

	printf("ini: %s  sets: %u  kargs: %u  rounds: %u  peer_dst: %u\n",
	       opts.ini[0] ? opts.ini : "(defaults)", opts.num_sets, opts.n_kargs,
	       opts.rounds, cfg.peer_dst);
	VPRINTF("mdb5:       wr=%s rd=%s\n", cfg.mdb5_write, cfg.mdb5_read);
	VPRINTF("vring:      base=0x%" PRIx64 " size=0x%" PRIx64 " ept='%s'\n",
		cfg.dev_base, cfg.shm_size, cfg.ept_name);
	VPRINTF("pools:      rpu=0x%" PRIx64 "/0x%" PRIx64 "  device=0x%" PRIx64 "/0x%" PRIx64 "\n",
		cfg.rpu_pool_base, cfg.rpu_pool_size, cfg.dev_pool_base, cfg.dev_pool_size);

	stats = calloc(opts.rounds, sizeof(*stats));
	bufs = calloc(1, sizeof(*bufs));
	scratch = malloc(cfg.buf_size);
	if (!stats || !bufs || !scratch) { fprintf(stderr, "out of memory\n"); goto out_opts; }

	rc = rpmsg_mem_mdb5_open(&mdb5, &mdb5_ops, cfg.mdb5_write, cfg.mdb5_read);
	if (rc) { fprintf(stderr, "mdb5 open (%s / %s): %s\n", cfg.mdb5_write, cfg.mdb5_read, strerror(-rc)); goto out_opts; }

	/* align shim closest to rpmsg_lite_dma; trace (if any) below it so the log
	 * shows the actual word-aligned MDB5 transfers. */
	const struct rpmsg_mem_ops *base = &mdb5_ops;
	if (opts.trace_dma) {
		trace = (struct dma_trace){ .inner = &mdb5_ops, .seq = 0 };
		trace_ops = (struct rpmsg_mem_ops){ .write = trace_write, .read = trace_read, .priv = &trace };
		base = &trace_ops;
	}
	align = (struct dma_align){ .inner = base };
	align_ops = (struct rpmsg_mem_ops){ .write = align_write, .read = align_read, .priv = &align };
	mem = &align_ops;

	rpu_pool = (struct pool){ .mem = mem, .base = cfg.rpu_pool_base, .size = cfg.rpu_pool_size, .off = 0 };
	dev_pool = (struct pool){ .mem = mem, .base = cfg.dev_pool_base, .size = cfg.dev_pool_size, .off = 0 };

	struct rpmsg_lite_dma_cfg dcfg = {
		.mem = mem,
		.shm_phys = cfg.dev_base,
		.status_offset = cfg.status_offset,
		.vring_tx_offset = cfg.vring_tx_offset,
		.vring_rx_offset = cfg.vring_rx_offset,
		.vring_num = cfg.vring_num,
		.vring_align = cfg.vring_align,
		.buf_offset = cfg.buf_offset,
		.buf_size = cfg.buf_size,
		.buf_count = cfg.buf_count,
		.notify = NULL,
		.priv = NULL,
		.scratch = scratch,
		.scratch_size = cfg.buf_size,
		.shm_size = cfg.shm_size,
	};
	/* The RPU firmware reads vring0/1 .da once, right after DRIVER_OK, so fill
	 * them (resource-table offsets +0x30 / +0x44) before rpmsg_lite_dma_init
	 * sets DRIVER_OK. */
	{
		uint32_t da0 = (uint32_t)(cfg.dev_base + cfg.vring_rx_offset);
		uint32_t da1 = (uint32_t)(cfg.dev_base + cfg.vring_tx_offset);
		rc = mem->write(mem->priv, cfg.dev_base + 0x30, &da0, sizeof(da0));
		if (!rc) rc = mem->write(mem->priv, cfg.dev_base + 0x44, &da1, sizeof(da1));
		if (rc) { fprintf(stderr, "write vring da: %s\n", strerror(-rc)); goto out_mdb5; }
		VPRINTF("vring da:   vring0.da=0x%08x vring1.da=0x%08x (before DRIVER_OK)\n", da0, da1);
	}

	rc = rpmsg_lite_dma_init(&ctx, &dcfg);
	if (rc) { fprintf(stderr, "rpmsg_lite_dma_init: %s\n", strerror(-rc)); goto out_mdb5; }

	if (g_verbose) {
		uint8_t st = 0;
		mem->read(mem->priv, &st, cfg.dev_base + cfg.status_offset, 1);
		printf("rpmsg-lite DMA: init OK, vdev status @0x%" PRIx64 " = 0x%02x  DRIVER_OK=%s\n",
		       cfg.dev_base + cfg.status_offset, st, (st & 0x04u) ? "YES" : "NO");
	}

	rc = rpmsg_lite_dma_create_ept(&ctx, &ept, cfg.ept_name, cfg.host_ept_addr, on_rsp, &rsp);
	if (rc) { fprintf(stderr, "create_ept: %s\n", strerror(-rc)); goto out_dma; }
	VPRINTF("[host-ept] '%s' local_addr=%u -> peer_dst=%u\n", cfg.ept_name, ept.local_addr, cfg.peer_dst);

	uint64_t used_off = (uint64_t)cfg.vring_num * 16u + 6u + 2u * cfg.vring_num;
	used_off = (used_off + cfg.vring_align - 1u) & ~((uint64_t)cfg.vring_align - 1u);

	struct link link = {
		.ctx = &ctx, .ept = &ept, .rsp = &rsp, .mem = mem,
		.rpu_pool = &rpu_pool, .dev_pool = &dev_pool, .peer_dst = cfg.peer_dst,
		.vr0_used_addr = cfg.dev_base + cfg.vring_rx_offset + used_off,
	};

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/* Load files and allocate device buffers once (untimed). */
	rc = prepare(&link, &opts, bufs);
	if (rc) {
		fprintf(stderr, "prepare failed: %s\n", strerror(-rc));
		goto out_dma;
	}

	/* Each round: sync to device, submit + wait, sync from device. */
	ret = 0;
	for (uint32_t r = 0; r < opts.rounds && !g_stop; r++) {
		rc = run_round(&link, &opts, r, bufs, &stats[r]);
		if (rc) {
			fprintf(stderr, "round %u failed: %s\n", r, strerror(-rc));
			ret = 1;
			break;
		}
		n_done++;
		if (!stats[r].passed)
			ret = 1;
		if (!g_verbose && (r < 3 || (r + 1) % 100 == 0 || r + 1 == opts.rounds)) {
			printf("round %u done: exec %.3f ms\n", r, ms(stats[r].exec_ns));
			fflush(stdout);
		}
	}
	signal(SIGINT, SIG_DFL);

	if (n_done <= 20 || g_verbose)
		print_stats(&opts, stats, n_done);
	print_profile(&opts, stats, n_done);
	if (n_done != opts.rounds)
		ret = 1;

out_dma:
	rpmsg_lite_dma_deinit(&ctx);
out_mdb5:
	rpmsg_mem_mdb5_close(&mdb5);
out_opts:
	if (bufs)
		free_bufs(&opts, bufs);
	free(bufs);
	free(scratch);
	free(stats);
	free_opts(&opts);
	printf("%s\n", ret == 0 ? "PASS" : "FAIL");
	return ret;
}
