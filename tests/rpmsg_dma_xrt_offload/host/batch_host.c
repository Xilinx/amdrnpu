/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * x86 host for RPURuntime tests/xrt_batches, over the MDB5 PCIe-DMA
 * rpmsg-lite path (wire protocol: lib/examples/apu_xrt_batch_ops.h).
 *
 *   1. CREATE:  sync the ELF and the shared kernel args (e.g. weights) to the
 *               device once, send APU_XRT_BATCH_CREATE; RPU_APP parses the
 *               ELF, creates the hw_context/kernel/run, binds the shared args
 *               and returns the context id in the result.
 *   2. RUN x B: per batch, sync every entry's inputs (and zero its spill),
 *               send APU_XRT_BATCH_RUN with N entries against the context id
 *               (no ELF parse on the RPU), wait, read back every output.
 *   3. DESTROY: send APU_XRT_BATCH_DESTROY for the context.
 *
 * Kernel args, in kernel-arg order (index = position):
 *   --shared-in FILE     sent at CREATE, shared by every entry (weights)
 *   --in FILE[,FILE..]   per-entry input: one file for all entries, or one per
 *                        entry (comma-separated, exactly --entries of them)
 *   --in-0 SIZE          per-entry input of SIZE zero bytes (e.g. spill)
 *   --out SIZE           per-entry output of SIZE bytes
 *
 * Usage (resnet50-int8-t20, 4-col):
 *   batch_host --ini offload_config.ini --model aie_control-4cols.elf \
 *       --entries 3 --batches 100 --shared-in wts32-4cols.bin \
 *       --in inputSubBO_vaiml_par_0_id_0_0_0_trim.bin --in-0 2408448 --out 1000
 *
 * Every phase is timed on the host (CLOCK_MONOTONIC) and RPU_APP's own
 * per-command timings come back in the result struct (profiling builds).
 * Quiet by default; --verbose prints the bring-up, hexdumps and checks.
 */
#define _POSIX_C_SOURCE 200809L

#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_mdb5.h"
#include "amdrnpu_wire.h"
#include "apu_xrt_batch_ops.h"

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

#define DEFAULT_ENTRIES    3u
#define DEFAULT_BATCHES    1u
#define DEFAULT_TIMEOUT_S  60u
#define POOL_ALIGN         0x1000u     /* page-align every pool allocation */
#define RESULT_POISON      0xFFFFFFFFu /* result.status sentinel = "not written yet" */
/* After completion is seen via the DDR result word, keep polling this long for
 * the rpmsg reply, so the RPU's reply goes out before the host moves on or
 * deinits (which would clear DRIVER_OK and fail the RPU's rpmsg_send). */
#define REPLY_WAIT_MS      500u
#define ENTRIES_MAX        APU_XRT_BATCH_MAX_ARGS

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
	char          *list;                 /* strdup of the --in/--shared-in value */
	char          *files[ENTRIES_MAX];   /* pointers into list */
	uint32_t       n_files;
	uint64_t       size;                 /* KARG_IN_ZERO / KARG_OUT */
};

struct opts {
	char             ini[PATH_MAX];
	char             elf_file[PATH_MAX];
	uint32_t         entries;
	uint32_t         batches;
	struct karg_spec kargs[APU_XRT_BATCH_MAX_KARGS];
	uint32_t         n_kargs;
	uint32_t         timeout_s;
	uint32_t         poll_us;
	char             save_out[PATH_MAX];
	bool             want_result;     /* always true: the result carries the ctx id */
	bool             zero_out;
	bool             trace_dma;
};

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s --ini INI --model ELF [--entries N] [--batches B] KARG... [OPTIONS]\n"
		"\n"
		"Kernel args, in kernel-arg order (index = position, up to %u):\n"
		"  --shared-in FILE    sent once at CREATE, shared by all entries (e.g. weights)\n"
		"  --in FILE[,FILE..]  per-entry input: one file for all entries, or one per entry\n"
		"  --in-0 SIZE         per-entry input of SIZE zero bytes (e.g. spill)\n"
		"  --out SIZE          per-entry output of SIZE bytes\n"
		"\n"
		"Options:\n"
		"  --ini PATH          offload config INI (vrings + pools + MDB5 nodes)\n"
		"  --model PATH        AIE control ELF (zlib or raw; required)\n"
		"  --entries N         entries per RUN batch (default %u)\n"
		"  --batches B         RUN requests to send against the context (default %u)\n"
		"  --timeout-s N       per-command completion timeout seconds (default %u)\n"
		"  --poll-us N         sleep between completion checks, us (default 0 = busy poll)\n"
		"  --no-zero-out       do not zero output BOs before each batch (untimed step)\n"
		"  --save-out PREFIX   write outputs to PREFIX_b<batch>_e<entry>_arg<idx>.bin\n"
		"  --trace-dma         print dev_addr + len on every MDB5 DMA call\n"
		"  --verbose           debug output: link bring-up, layout, hexdumps, checks\n"
		"\n"
		"A request is one rpmsg buffer: RUN needs 4 + K + 2*N*K <= %u args (K = per-entry kargs).\n",
		p, APU_XRT_BATCH_MAX_KARGS, DEFAULT_ENTRIES, DEFAULT_BATCHES,
		DEFAULT_TIMEOUT_S, APU_XRT_BATCH_MAX_ARGS);
}

static struct karg_spec *new_karg(struct opts *o)
{
	if (o->n_kargs >= APU_XRT_BATCH_MAX_KARGS) {
		fprintf(stderr, "too many kernel args (max %u)\n", APU_XRT_BATCH_MAX_KARGS);
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

static int parse_files(const char *opt, const char *v, struct karg_spec *k)
{
	char *save = NULL;

	k->list = strdup(v);
	if (!k->list)
		return -ENOMEM;
	for (char *t = strtok_r(k->list, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		if (k->n_files >= ENTRIES_MAX) {
			fprintf(stderr, "%s: too many files\n", opt);
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

static uint32_t count_kargs(const struct opts *o, bool shared)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < o->n_kargs; i++)
		if ((o->kargs[i].kind == KARG_SHARED_IN) == shared)
			n++;
	return n;
}

static int parse_cli(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->entries = DEFAULT_ENTRIES;
	o->batches = DEFAULT_BATCHES;
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
		} else if (!strcmp(a, "--entries") && has_val) {
			o->entries = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--batches") && has_val) {
			o->batches = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--timeout-s") && has_val) {
			o->timeout_s = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--poll-us") && has_val) {
			o->poll_us = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(a, "--save-out") && has_val) {
			snprintf(o->save_out, sizeof(o->save_out), "%s", argv[++i]);
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
	if (!o->entries || !o->batches) {
		fprintf(stderr, "error: --entries and --batches must be >= 1\n");
		return -EINVAL;
	}
	const uint32_t nfix = count_kargs(o, true), kpe = count_kargs(o, false);
	if (APU_XRT_BATCH_CREATE_NUM_ARGS(nfix) > APU_XRT_BATCH_MAX_ARGS ||
	    APU_XRT_BATCH_RUN_NUM_ARGS(o->entries, kpe) > APU_XRT_BATCH_MAX_ARGS) {
		fprintf(stderr, "error: RUN with %u entries x %u per-entry kargs needs %u args "
			"(> %u, one rpmsg buffer)\n", o->entries, kpe,
			APU_XRT_BATCH_RUN_NUM_ARGS(o->entries, kpe), APU_XRT_BATCH_MAX_ARGS);
		return -EINVAL;
	}
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		const struct karg_spec *k = &o->kargs[i];
		if (k->kind == KARG_IN_FILE && k->n_files != 1 && k->n_files != o->entries) {
			fprintf(stderr, "error: karg %u: --in has %u files; give 1 (same for all "
				"entries) or %u (one per entry)\n", i, k->n_files, o->entries);
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

/* ---------------------------------------------------------------------------
 * Buffers: set up once. Shared kargs have one device buffer (pa[0]); per-entry
 * kargs have one per entry. data[e] is the host copy of a file input (aliases
 * data[0] when one file feeds every entry); out[e] the readback for outputs.
 * ------------------------------------------------------------------------- */
struct karg_bufs {
	uint64_t pa[ENTRIES_MAX];
	uint64_t size[ENTRIES_MAX];
	uint8_t *data[ENTRIES_MAX];
	uint8_t *out[ENTRIES_MAX];
};

struct bufs {
	uint8_t         *elf;
	uint64_t         elf_sz, elf_pa;
	struct karg_bufs k[APU_XRT_BATCH_MAX_KARGS];
};

static void free_bufs(const struct opts *o, struct bufs *b)
{
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		struct karg_bufs *kb = &b->k[i];
		bool one_file = o->kargs[i].n_files == 1;
		for (uint32_t e = 0; e < ENTRIES_MAX; e++) {
			if (!one_file || e == 0)
				free(kb->data[e]);
			free(kb->out[e]);
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
		const uint32_t n_dev = (k->kind == KARG_SHARED_IN) ? 1u : o->entries;

		for (uint32_t e = 0; e < n_dev; e++) {
			if (k->kind == KARG_SHARED_IN || k->kind == KARG_IN_FILE) {
				if (k->n_files == 1 && e > 0) {
					kb->data[e] = kb->data[0];
					kb->size[e] = kb->size[0];
				} else {
					rc = slurp(k->files[e], &kb->data[e], &kb->size[e]);
					if (rc) return rc;
				}
			} else {
				kb->size[e] = k->size;
			}
			if (k->kind == KARG_OUT) {
				kb->out[e] = malloc(kb->size[e]);
				if (!kb->out[e]) return -ENOMEM;
			}
			rc = pool_alloc(l->dev_pool, kb->size[e], &kb->pa[e]);
			if (rc) return rc;
		}
	}

	VPRINTF("\nelf: %s PA=0x%" PRIx64 " (%" PRIu64 " B)\n", o->elf_file, b->elf_pa, b->elf_sz);
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		const struct karg_spec *k = &o->kargs[i];
		const uint32_t n_dev = (k->kind == KARG_SHARED_IN) ? 1u : o->entries;
		for (uint32_t e = 0; e < n_dev; e++)
			VPRINTF("  karg[%2u] %s%u PA=0x%010" PRIx64 " size=%" PRIu64 "\n", i,
				k->kind == KARG_SHARED_IN ? "shared " : "entry ", e,
				b->k[i].pa[e], b->k[i].size[e]);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * One command: fresh result slot (pre-filled with 0xFF), send, wait for the
 * reply or the result's status word, collect the reply, read the result.
 * Returns 0 when the RPU reported APU_XRT_BATCH_OK.
 * ------------------------------------------------------------------------- */
struct cmd_stat {
	uint64_t exec_ns;    /* send -> completion seen */
	bool     via_rsp;
	struct apu_xrt_batch_result res;
};

static uint32_t g_seq;

static const char *op_name(uint32_t op)
{
	switch (op) {
	case APU_XRT_BATCH_CREATE:  return "CREATE";
	case APU_XRT_BATCH_RUN:     return "RUN";
	case APU_XRT_BATCH_DESTROY: return "DESTROY";
	default:                    return "?";
	}
}

/* args[] must leave the result-PA slot (res_idx) for this function to fill. */
static int send_cmd(const struct link *l, const struct opts *o, uint32_t op,
		    uint64_t *args, uint32_t nargs, uint32_t res_idx, struct cmd_stat *cs)
{
	uint8_t cmdbuf[sizeof(struct amdrnpu_wire_cmd) + APU_XRT_BATCH_MAX_ARGS * sizeof(uint64_t)];
	struct amdrnpu_wire_cmd *cmd = (struct amdrnpu_wire_cmd *)cmdbuf;
	const size_t cmd_bytes = sizeof(*cmd) + (size_t)nargs * sizeof(uint64_t);
	uint8_t poison[APU_XRT_BATCH_RESULT_SIZE];
	uint64_t result_pa, t;
	int rc;

	memset(cs, 0, sizeof(*cs));
	/* Never reuse a result word the host already polled. */
	rc = pool_alloc(l->rpu_pool, APU_XRT_BATCH_RESULT_SIZE, &result_pa);
	if (rc) return rc;
	memset(poison, 0xFF, sizeof(poison));
	rc = pool_write(l->rpu_pool, result_pa, poison, sizeof(poison));
	if (rc) { fprintf(stderr, "%s: result init: %s\n", op_name(op), strerror(-rc)); return rc; }
	args[res_idx] = result_pa;

	cmd->seq = ++g_seq;
	cmd->command_op = op;
	cmd->flags = 0;
	cmd->num_args = nargs;
	memcpy(cmd->args, args, (size_t)nargs * sizeof(uint64_t));
	if (g_verbose)
		hexdump(op_name(op), cmdbuf, cmd_bytes);

	l->rsp->expect_seq = cmd->seq;
	l->rsp->got = 0;
	t = now_ns();
	rc = rpmsg_lite_dma_sendto(l->ctx, l->ept, cmdbuf, cmd_bytes, l->peer_dst);
	if (rc) { fprintf(stderr, "%s: sendto: %s\n", op_name(op), strerror(-rc)); return rc; }
	rc = wait_done(l, o, result_pa, &cs->via_rsp);
	cs->exec_ns = now_ns() - t;
	if (rc) return rc;

	/* Collect the reply too (untimed) so the RPU's send succeeds and the
	 * rings stay in step. */
	bool got_reply = cs->via_rsp || wait_reply(l);
	if (!got_reply)
		fprintf(stderr, "%s: no rpmsg reply within %u ms after DDR completion\n",
			op_name(op), REPLY_WAIT_MS);

	rc = l->mem->read(l->mem->priv, &cs->res, result_pa, sizeof(cs->res));
	if (rc) { fprintf(stderr, "%s: read result: %s\n", op_name(op), strerror(-rc)); return rc; }
	if (cs->res.status != APU_XRT_BATCH_OK) {
		fprintf(stderr, "%s: RPU status %u (ctx %u, entries_done %u)\n", op_name(op),
			cs->res.status, cs->res.ctx_id, cs->res.entries_done);
		return -EIO;
	}
	if (got_reply && l->rsp->rsp_type != AMDRNPU_WIRE_RSP_COMPLETE) {
		fprintf(stderr, "%s: RPU replied %s\n", op_name(op), rsp_str(l->rsp->rsp_type));
		return -EIO;
	}
	return 0;
}

/* ---- CREATE: sync ELF + shared kargs, create the context ---------------- */
static int do_create(const struct link *l, const struct opts *o, struct bufs *b,
		     uint64_t *sync_ns, struct cmd_stat *cs, uint32_t *ctx_id)
{
	uint64_t args[APU_XRT_BATCH_MAX_ARGS] = { 0 };
	uint32_t nfix = 0;
	uint64_t t = now_ns();
	int rc;

	rc = pool_write(l->rpu_pool, b->elf_pa, b->elf, b->elf_sz);
	if (rc) { fprintf(stderr, "sync elf: %s\n", strerror(-rc)); return rc; }
	for (uint32_t i = 0; i < o->n_kargs; i++) {
		if (o->kargs[i].kind != KARG_SHARED_IN)
			continue;
		rc = pool_write(l->dev_pool, b->k[i].pa[0], b->k[i].data[0], b->k[i].size[0]);
		if (rc) { fprintf(stderr, "sync shared arg %u: %s\n", i, strerror(-rc)); return rc; }
		const uint32_t base = APU_XRT_BATCH_CREATE_FIXED_IDX(nfix);
		args[base] = i;
		args[base + 1] = b->k[i].pa[0];
		args[base + 2] = b->k[i].size[0];
		nfix++;
	}
	*sync_ns = now_ns() - t;

	args[0] = b->elf_pa;
	args[1] = b->elf_sz;
	args[3] = nfix;
	rc = send_cmd(l, o, APU_XRT_BATCH_CREATE, args, APU_XRT_BATCH_CREATE_NUM_ARGS(nfix), 2, cs);
	if (rc) return rc;
	*ctx_id = cs->res.ctx_id;
	return 0;
}

/* ---- RUN: sync every entry's inputs, run the batch, read outputs ------- */
struct batch_stat {
	uint64_t h2d_ns, d2h_ns;
	struct cmd_stat cs;
};

static int do_run(const struct link *l, const struct opts *o, struct bufs *b,
		  uint32_t ctx_id, uint32_t batch, struct batch_stat *bs)
{
	uint64_t args[APU_XRT_BATCH_MAX_ARGS] = { 0 };
	const uint32_t N = o->entries;
	uint32_t kpe = 0, kidx[APU_XRT_BATCH_MAX_KARGS];
	uint64_t t;
	int rc;

	memset(bs, 0, sizeof(*bs));
	for (uint32_t i = 0; i < o->n_kargs; i++)
		if (o->kargs[i].kind != KARG_SHARED_IN)
			kidx[kpe++] = i;

	/* untimed: zero the outputs */
	for (uint32_t e = 0; e < N && o->zero_out; e++)
		for (uint32_t k = 0; k < kpe; k++) {
			const uint32_t i = kidx[k];
			if (o->kargs[i].kind != KARG_OUT)
				continue;
			rc = pool_zero(l->dev_pool, b->k[i].pa[e], b->k[i].size[e]);
			if (rc) { fprintf(stderr, "zero out e%u arg %u: %s\n", e, i, strerror(-rc)); return rc; }
		}

	/* sync to device: every entry's inputs (in-0 zeroed) */
	t = now_ns();
	for (uint32_t e = 0; e < N; e++)
		for (uint32_t k = 0; k < kpe; k++) {
			const uint32_t i = kidx[k];
			const struct karg_bufs *kb = &b->k[i];
			if (o->kargs[i].kind == KARG_IN_FILE)
				rc = pool_write(l->dev_pool, kb->pa[e], kb->data[e], kb->size[e]);
			else if (o->kargs[i].kind == KARG_IN_ZERO)
				rc = pool_zero(l->dev_pool, kb->pa[e], kb->size[e]);
			else
				continue;
			if (rc) { fprintf(stderr, "sync e%u arg %u: %s\n", e, i, strerror(-rc)); return rc; }
		}
	bs->h2d_ns = now_ns() - t;

	/* RUN request */
	args[0] = ctx_id;
	args[2] = N;
	args[3] = kpe;
	for (uint32_t k = 0; k < kpe; k++)
		args[APU_XRT_BATCH_RUN_KIDX_IDX(k)] = kidx[k];
	for (uint32_t e = 0; e < N; e++)
		for (uint32_t k = 0; k < kpe; k++) {
			const uint32_t idx = APU_XRT_BATCH_RUN_ENTRY_IDX(e, k, kpe);
			args[idx] = b->k[kidx[k]].pa[e];
			args[idx + 1] = b->k[kidx[k]].size[e];
		}
	rc = send_cmd(l, o, APU_XRT_BATCH_RUN, args, APU_XRT_BATCH_RUN_NUM_ARGS(N, kpe), 1, &bs->cs);
	if (rc) return rc;

	/* sync from device: every entry's outputs */
	t = now_ns();
	for (uint32_t e = 0; e < N; e++)
		for (uint32_t k = 0; k < kpe; k++) {
			const uint32_t i = kidx[k];
			struct karg_bufs *kb = &b->k[i];
			if (o->kargs[i].kind != KARG_OUT)
				continue;
			rc = l->mem->read(l->mem->priv, kb->out[e], kb->pa[e], kb->size[e]);
			if (rc) { fprintf(stderr, "read e%u arg %u: %s\n", e, i, strerror(-rc)); return rc; }
		}
	bs->d2h_ns = now_ns() - t;

	if (o->save_out[0]) {
		for (uint32_t e = 0; e < N; e++)
			for (uint32_t k = 0; k < kpe; k++) {
				const uint32_t i = kidx[k];
				char path[PATH_MAX + 64];
				if (o->kargs[i].kind != KARG_OUT)
					continue;
				snprintf(path, sizeof(path), "%s_b%u_e%u_arg%u.bin", o->save_out, batch, e, i);
				rc = write_file(path, b->k[i].out[e], b->k[i].size[e]);
				if (rc) return rc;
			}
	}
	return 0;
}

/* ---- DESTROY ------------------------------------------------------------ */
static int do_destroy(const struct link *l, const struct opts *o, uint32_t ctx_id,
		      struct cmd_stat *cs)
{
	uint64_t args[APU_XRT_BATCH_DESTROY_NUM_ARGS] = { ctx_id, 0 };
	return send_cmd(l, o, APU_XRT_BATCH_DESTROY, args, APU_XRT_BATCH_DESTROY_NUM_ARGS, 1, cs);
}

/* ---------------------------------------------------------------------------
 * Report
 * ------------------------------------------------------------------------- */
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
	printf("  %-26s %12.3f %12.3f %12.3f %6u  %s\n", name, a->sum / a->n / 1e3,
	       (double)a->min / 1e3, (double)a->max / 1e3, a->n, note);
}

static void one(const char *name, uint64_t ns, const char *note)
{
	struct agg a = { 0 };
	agg_add(&a, ns);
	agg_print(name, &a, note);
}

static void print_report(const struct opts *o, uint64_t create_sync_ns,
			 const struct cmd_stat *cr, const struct batch_stat *bs,
			 uint32_t n_done, const struct cmd_stat *ds, bool destroyed)
{
	const bool prof = cr->res.prof_magic == APU_XRT_BATCH_PROF_MAGIC;
	struct agg h2d = {0}, exec = {0}, d2h = {0}, rx = {0}, handler = {0}, map = {0};
	struct agg bind = {0}, rexec = {0}, rexec_max = {0}, send = {0}, gap = {0};

	for (uint32_t i = 0; i < n_done; i++) {
		const struct apu_xrt_batch_result *r = &bs[i].cs.res;
		agg_add(&h2d, bs[i].h2d_ns);
		agg_add(&exec, bs[i].cs.exec_ns);
		agg_add(&d2h, bs[i].d2h_ns);
		if (r->prof_magic != APU_XRT_BATCH_PROF_MAGIC)
			continue;
		agg_add(&rx, r->rx_to_start_ns);
		agg_add(&handler, r->handler_ns);
		agg_add(&map, r->bo_map_ns);
		agg_add(&bind, r->bind_ns);
		agg_add(&rexec, r->exec_ns);
		agg_add(&rexec_max, r->exec_max_ns);
		if (r->prev_send_ns)
			agg_add(&send, r->prev_send_ns);
		uint64_t rpu = (uint64_t)r->rx_to_start_ns + r->handler_ns;
		if (bs[i].cs.exec_ns > rpu)
			agg_add(&gap, bs[i].cs.exec_ns - rpu);
	}

	printf("\nProfile: 1 CREATE, %u RUN batch(es) x %u entries, %s DESTROY; times in us\n",
	       n_done, o->entries, destroyed ? "1" : "no");
	printf("  %-26s %12s %12s %12s %6s\n", "phase", "avg", "min", "max", "n");
	printf("  CREATE:\n");
	one("host sync elf + shared", create_sync_ns, "to device, once");
	one("host exec (send -> done)", cr->exec_ns, "");
	if (prof) {
		one("RPU ELF parse", cr->res.elf_parse_ns, "once per context");
		one("RPU hw_context+kernel+run", cr->res.hwctx_ns, "");
		one("RPU bo map (shared)", cr->res.bo_map_ns, "set_arg is per entry in RUN");
		one("RPU handler total", cr->res.handler_ns, "");
	}
	printf("  RUN (per batch):\n");
	agg_print("host sync to device", &h2d, "every entry's inputs");
	agg_print("host exec (send -> done)", &exec, "");
	agg_print("host sync from device", &d2h, "every entry's outputs");
	agg_print("RPU rx -> start", &rx, "");
	agg_print("RPU bo map", &map, "new buffers only (cached)");
	agg_print("RPU set_arg", &bind, "every karg, every entry");
	agg_print("RPU execution", &rexec, "start -> wait, sum over entries");
	agg_print("RPU execution (longest)", &rexec_max, "single entry");
	agg_print("RPU handler total", &handler, "");
	agg_print("RPU reply send + IPI", &send, "previous command");
	agg_print("not covered by RPU", &gap, "host exec - (rx->start + handler)");
	if (destroyed) {
		printf("  DESTROY:\n");
		one("host exec (send -> done)", ds->exec_ns, "");
		if (prof)
			one("RPU teardown", ds->res.teardown_ns, "");
	}
	if (!prof)
		printf("  (no RPU timings: RPU_APP built with APU_BATCH_PROFILE=OFF)\n");
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
	struct batch_stat *bstats = NULL;
	struct bufs *bufs = NULL;
	struct cmd_stat cr = { 0 }, ds = { 0 };
	uint64_t create_sync_ns = 0;
	uint8_t *scratch = NULL;
	uint32_t n_done = 0, ctx_id = 0;
	bool created = false, destroyed = false;
	int ret = 1, rc;

	rc = parse_cli(argc, argv, &opts);
	if (rc > 0) { free_opts(&opts); return 0; }
	if (rc < 0) { usage(argv[0]); free_opts(&opts); return 2; }

	rc = config_load(&cfg, opts.ini[0] ? opts.ini : NULL);
	if (rc) { fprintf(stderr, "config load '%s': %s\n", opts.ini, strerror(-rc)); goto out_opts; }
	if (sizeof(struct amdrnpu_wire_cmd) + APU_XRT_BATCH_MAX_ARGS * sizeof(uint64_t) +
	    sizeof(struct rpmsg_hdr) > cfg.buf_size) {
		fprintf(stderr, "error: rpmsg buf_size %u too small for %u-arg requests\n",
			cfg.buf_size, APU_XRT_BATCH_MAX_ARGS);
		goto out_opts;
	}

	printf("ini: %s  entries/batch: %u  batches: %u  kargs: %u (%u shared)  peer_dst: %u\n",
	       opts.ini[0] ? opts.ini : "(defaults)", opts.entries, opts.batches, opts.n_kargs,
	       count_kargs(&opts, true), cfg.peer_dst);

	bstats = calloc(opts.batches, sizeof(*bstats));
	bufs = calloc(1, sizeof(*bufs));
	scratch = malloc(cfg.buf_size);
	if (!bstats || !bufs || !scratch) { fprintf(stderr, "out of memory\n"); goto out_opts; }

	rc = rpmsg_mem_mdb5_open(&mdb5, &mdb5_ops, cfg.mdb5_write, cfg.mdb5_read);
	if (rc) { fprintf(stderr, "mdb5 open (%s / %s): %s\n", cfg.mdb5_write, cfg.mdb5_read, strerror(-rc)); goto out_opts; }

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
	}

	rc = rpmsg_lite_dma_init(&ctx, &dcfg);
	if (rc) { fprintf(stderr, "rpmsg_lite_dma_init: %s\n", strerror(-rc)); goto out_mdb5; }
	rc = rpmsg_lite_dma_create_ept(&ctx, &ept, cfg.ept_name, cfg.host_ept_addr, on_rsp, &rsp);
	if (rc) { fprintf(stderr, "create_ept: %s\n", strerror(-rc)); goto out_dma; }

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

	rc = prepare(&link, &opts, bufs);
	if (rc) { fprintf(stderr, "prepare failed: %s\n", strerror(-rc)); goto out_dma; }

	/* 1. CREATE */
	rc = do_create(&link, &opts, bufs, &create_sync_ns, &cr, &ctx_id);
	if (rc) { fprintf(stderr, "CREATE failed: %s\n", strerror(-rc)); goto out_dma; }
	created = true;
	printf("CREATE done: ctx %u, exec %.3f ms\n", ctx_id, (double)cr.exec_ns / 1e6);

	/* 2. RUN x batches */
	ret = 0;
	for (uint32_t i = 0; i < opts.batches && !g_stop; i++) {
		rc = do_run(&link, &opts, bufs, ctx_id, i, &bstats[i]);
		if (rc) {
			fprintf(stderr, "RUN batch %u failed: %s\n", i, strerror(-rc));
			ret = 1;
			break;
		}
		n_done++;
		if (!g_verbose && (i < 3 || (i + 1) % 100 == 0 || i + 1 == opts.batches)) {
			printf("RUN batch %u done: exec %.3f ms\n", i, (double)bstats[i].cs.exec_ns / 1e6);
			fflush(stdout);
		}
	}

	/* 3. DESTROY (also after a failed or interrupted RUN) */
	g_stop = 0;
	rc = do_destroy(&link, &opts, ctx_id, &ds);
	if (rc) {
		fprintf(stderr, "DESTROY failed: %s\n", strerror(-rc));
		ret = 1;
	} else {
		destroyed = true;
		printf("DESTROY done: exec %.3f ms\n", (double)ds.exec_ns / 1e6);
	}
	signal(SIGINT, SIG_DFL);

	print_report(&opts, create_sync_ns, &cr, bstats, n_done, &ds, destroyed);
	if (n_done != opts.batches)
		ret = 1;

out_dma:
	rpmsg_lite_dma_deinit(&ctx);
out_mdb5:
	rpmsg_mem_mdb5_close(&mdb5);
out_opts:
	if (!created)
		ret = 1;
	if (bufs)
		free_bufs(&opts, bufs);
	free(bufs);
	free(scratch);
	free(bstats);
	free_opts(&opts);
	printf("%s\n", ret == 0 ? "PASS" : "FAIL");
	return ret;
}
