/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * x86 host side of the XRT/AIE offload, over the MDB5 PCIe-DMA rpmsg-lite path.
 *
 * Same APU_XRT_TEST descriptor/submit flow as amdrnpu_xrt_offload_example.c,
 * but WITHOUT the amdrnpu library / kernel driver / /dev/mem. The x86 host
 * cannot mmap the Versal DDR, so every device-memory access is an MDB5 DMA:
 *   - the Link A rpmsg vrings + buffer pool live in device memory, driven by
 *     rpmsg_lite_dma over the MDB5 backend (rpmsg_mem_mdb5: pwrite/pread);
 *   - the ELF / descriptor / result / kernel-arg buffers live in a hardcoded
 *     device-memory pool reached by the SAME MDB5 channels (direct DMA), and
 *     the descriptor carries their ABSOLUTE device physical addresses.
 *
 * The RPU_APP dereferences elf_pa/desc_pa directly and the AIE DMAs the kargs
 * by PA, so the pools must be RPU/AIE-reachable device DDR (rpu_gem_cma).
 * Halt the APU (Linux) first so it does not touch the pool range.
 *
 * Poll-only: no IPI/doorbell. The RPU_APP must be running first.
 *
 * Usage:
 *   xrt_offload_host --ini offload_config.ini --elf-file aie_control.elf \
 *       --arg wts32.bin --arg ifm.bin --arg-zero 14956544 --arg-zero 2000
 */
#define _POSIX_C_SOURCE 200809L

#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_mdb5.h"
#include "amdrnpu_wire.h"
#include "apu_ops.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <zlib.h>

#define DEFAULT_ITERATIONS 1u
#define DEFAULT_TIMEOUT_S  60u
#define POOL_ALIGN         0x1000u   /* page-align every pool allocation */

/* ---------------------------------------------------------------------------
 * Config (inline INI parser). Covers the Link A vring geometry + the two
 * device-memory pools (RPU-reachable: ELF/desc/result; AIE-reachable: kargs).
 * ------------------------------------------------------------------------- */
struct config {
	/* MDB5 DMA channel device nodes. */
	char     mdb5_write[PATH_MAX];
	char     mdb5_read[PATH_MAX];

	/* Link A rpmsg vring region (device addresses). */
	uint64_t dev_base;       /* rpmsg_lite_dma shm_phys (apu_rsctbl base). */
	uint64_t shm_size;
	uint64_t status_offset;
	uint64_t vring_tx_offset; /* leader TX -> RPU_APP RX. */
	uint64_t vring_rx_offset; /* leader RX <- RPU_APP TX. */
	uint64_t buf_offset;
	uint32_t vring_num;
	uint32_t vring_align;
	uint32_t buf_size;
	uint32_t buf_count;

	/* rpmsg endpoint. */
	char     ept_name[RPMSG_LITE_NAME_SIZE];
	uint32_t host_ept_addr;  /* our local ept address. */
	uint32_t peer_dst;       /* RPU_APP "amdrnpu" ept address. */

	/* Device-memory pools (absolute device PAs). */
	uint64_t rpu_pool_base;  /* ELF / descriptor / result (RPU-reachable). */
	uint64_t rpu_pool_size;
	uint64_t dev_pool_base;  /* kargs: IFM/OFM/WTS/spill (AIE-reachable).  */
	uint64_t dev_pool_size;
};

static void config_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->mdb5_write, sizeof(c->mdb5_write), "%s", "/dev/mdb5_write00");
	snprintf(c->mdb5_read, sizeof(c->mdb5_read), "%s", "/dev/mdb5_read00");

	/* Link A = apu_rsctbl in OCM bank 1 (RPURuntime reserved.dtsi). */
	c->dev_base        = UINT64_C(0xBBF80000);
	c->shm_size        = 0x49000;
	c->status_offset   = 0x2C;
	c->vring_rx_offset = 0x1000;   /* vring0: RPU_APP produces */
	c->vring_tx_offset = 0x5000;   /* vring1: leader produces  */
	c->buf_offset      = 0x9000;
	c->vring_num       = 8;
	c->vring_align     = 16;
	c->buf_size        = 512;
	c->buf_count       = 16;

	snprintf(c->ept_name, sizeof(c->ept_name), "%s", AMDRNPU_WIRE_CHANNEL);
	c->host_ept_addr   = 1024;
	c->peer_dst        = 0;        /* MUST match RPU_APP amdrnpu ept addr. */

	/* rpu_gem_cma sub-pools (match amdrnpu-xrt-offload.ini). */
	c->rpu_pool_base   = UINT64_C(0x20000000);
	c->rpu_pool_size   = 0x08000000;   /* 128 MiB */
	c->dev_pool_base   = UINT64_C(0x28000000);
	c->dev_pool_size   = 0x08000000;   /* 128 MiB */
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
		/* Strip an inline '#' or ';' comment from the value. */
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
 * Device-memory pool: bump allocator backed by MDB5 DMA.
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

/* DMA host bytes into the pool at @pa. */
static int pool_write(struct pool *p, uint64_t pa, const void *src, uint64_t len)
{
	return p->mem->write(p->mem->priv, pa, src, len);
}

/* DMA-zero @len bytes at @pa in chunks. */
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
 * ELF loader (transparently inflates a zlib-compressed control ELF).
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
	printf("elf-file:   %s zlib %" PRIu64 " B -> raw ELF %" PRIu64 " B\n", path, raw_len, elf_len);
	*data = elf; *size = elf_len;
	return 0;
}

/* ---------------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------------- */
struct karg_spec {
	char     path[PATH_MAX];
	uint64_t size;
	bool     is_output;
};

struct opts {
	char             ini[PATH_MAX];
	char             elf_file[PATH_MAX];
	uint32_t         iterations;
	uint32_t         timeout_s;
	struct karg_spec kargs[APU_XRT_TEST_MAX_KARGS];
	uint32_t         n_kargs;
	bool             want_result;
	bool             dump_args;
	bool             trace_dma;
};

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage: %s --ini INI --elf-file ELF [OPTIONS]\n"
		"  --ini PATH          offload config INI (vrings + pools + MDB5 nodes)\n"
		"  --elf-file PATH     AIE control ELF (zlib or raw; required)\n"
		"  --arg PATH          kernel-arg buffer from file (repeat, up to %u)\n"
		"  --arg-zero SIZE     zero-filled output kernel-arg, SIZE bytes (repeat)\n"
		"  --iterations N      run count (default %u)\n"
		"  --no-result         skip result writeback\n"
		"  --dump-args         print the descriptor kargs + wire args, then submit\n"
		"  --trace-dma         print dev_addr + len on every MDB5 DMA call\n"
		"  --timeout-s N       response wait timeout seconds (default %u)\n",
		p, APU_XRT_TEST_MAX_KARGS, DEFAULT_ITERATIONS, DEFAULT_TIMEOUT_S);
}

static int parse_cli(int argc, char **argv, struct opts *o)
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
		} else if (!strcmp(argv[i], "--dump-args")) {
			o->dump_args = true;
		} else if (!strcmp(argv[i], "--trace-dma")) {
			o->trace_dma = true;
		} else if (!strcmp(argv[i], "--arg") && i + 1 < argc) {
			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) { fprintf(stderr, "too many kargs\n"); return -EINVAL; }
			snprintf(o->kargs[o->n_kargs].path, sizeof(o->kargs[o->n_kargs].path), "%s", argv[++i]);
			o->n_kargs++;
		} else if (!strcmp(argv[i], "--arg-zero") && i + 1 < argc) {
			if (o->n_kargs >= APU_XRT_TEST_MAX_KARGS) { fprintf(stderr, "too many kargs\n"); return -EINVAL; }
			if (parse_u64(argv[++i], &o->kargs[o->n_kargs].size) || o->kargs[o->n_kargs].size == 0) {
				fprintf(stderr, "--arg-zero needs a non-zero SIZE\n"); return -EINVAL;
			}
			o->kargs[o->n_kargs].is_output = true;
			o->n_kargs++;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]); return 1;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]); return -EINVAL;
		}
		i++;
	}
	if (!o->elf_file[0]) { fprintf(stderr, "error: --elf-file is required\n"); return -EINVAL; }
	return 0;
}

/* Hex-dump an rpmsg payload to stderr. */
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

/* ---------------------------------------------------------------------------
 * rpmsg response capture
 * ------------------------------------------------------------------------- */
struct rsp_ctx {
	volatile int got;
	uint32_t     seq;
	uint32_t     rsp_type;
};

static void on_rsp(struct rpmsg_lite_dma_ept *ept, const void *data, size_t len, void *priv)
{
	struct rsp_ctx *r = priv;
	(void)ept;
	hexdump("rpmsg RX (response)", data, len);
	if (len >= sizeof(struct amdrnpu_wire_rsp)) {
		const struct amdrnpu_wire_rsp *w = data;
		r->seq = w->seq;
		r->rsp_type = w->command_rsp_type;
	}
	r->got = 1;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
 * DMA tracing shim: prints dev_addr + len on every MDB5 transfer, forwards to
 * the real backend. Wraps rpmsg_mem_ops so it covers BOTH the pool DMAs and
 * the rpmsg_lite_dma ring/field accesses (one ring field = one DMA).
 * ------------------------------------------------------------------------- */
struct dma_trace {
	const struct rpmsg_mem_ops *inner;
	unsigned long seq;
};

static int trace_write(void *priv, uint64_t dev_addr, const void *src, size_t len)
{
	struct dma_trace *t = priv;
	fprintf(stderr, "[dma %6lu] WR dev_addr=0x%010" PRIx64 " len=%zu\n",
		t->seq++, dev_addr, len);
	return t->inner->write(t->inner->priv, dev_addr, src, len);
}

static int trace_read(void *priv, void *dst, uint64_t dev_addr, size_t len)
{
	struct dma_trace *t = priv;
	fprintf(stderr, "[dma %6lu] RD dev_addr=0x%010" PRIx64 " len=%zu\n",
		t->seq++, dev_addr, len);
	return t->inner->read(t->inner->priv, dst, dev_addr, len);
}

int main(int argc, char **argv)
{
	struct opts opts;
	struct config cfg;
	struct rpmsg_mem_mdb5 mdb5;
	struct rpmsg_mem_ops mdb5_ops;
	struct dma_trace trace;
	struct rpmsg_mem_ops trace_ops;
	const struct rpmsg_mem_ops *mem;   /* active ops (traced or raw) */
	struct rpmsg_lite_dma ctx;
	struct rpmsg_lite_dma_ept ept;
	struct rsp_ctx rsp = { 0 };
	uint8_t *scratch = NULL, *elf_data = NULL, *desc = NULL;
	struct pool rpu_pool, dev_pool;
	uint64_t karg_pa[APU_XRT_TEST_MAX_KARGS] = { 0 };
	uint64_t karg_sz[APU_XRT_TEST_MAX_KARGS] = { 0 };
	int ret = 1, rc;

	rc = parse_cli(argc, argv, &opts);
	if (rc > 0) return 0;
	if (rc < 0) return 2;

	rc = config_load(&cfg, opts.ini[0] ? opts.ini : NULL);
	if (rc) { fprintf(stderr, "config load '%s': %s\n", opts.ini, strerror(-rc)); return 1; }

	printf("ini:        %s\n", opts.ini[0] ? opts.ini : "(defaults)");
	printf("mdb5:       wr=%s rd=%s\n", cfg.mdb5_write, cfg.mdb5_read);
	printf("vring:      base=0x%" PRIx64 " size=0x%" PRIx64 " ept='%s' peer_dst=%u\n",
	       cfg.dev_base, cfg.shm_size, cfg.ept_name, cfg.peer_dst);
	printf("pools:      rpu=0x%" PRIx64 "/0x%" PRIx64 "  device=0x%" PRIx64 "/0x%" PRIx64 "\n",
	       cfg.rpu_pool_base, cfg.rpu_pool_size, cfg.dev_pool_base, cfg.dev_pool_size);
	printf("opcode:     APU_XRT_TEST (0x%08x)  iterations=%u\n", APU_XRT_TEST, opts.iterations);

	scratch = malloc(cfg.buf_size);
	if (!scratch) { fprintf(stderr, "scratch alloc failed\n"); return 1; }

	rc = rpmsg_mem_mdb5_open(&mdb5, &mdb5_ops, cfg.mdb5_write, cfg.mdb5_read);
	if (rc) { fprintf(stderr, "mdb5 open (%s / %s): %s\n", cfg.mdb5_write, cfg.mdb5_read, strerror(-rc)); goto out_scratch; }

	/* Route every DMA through the tracing shim when --trace-dma is set. */
	if (opts.trace_dma) {
		trace = (struct dma_trace){ .inner = &mdb5_ops, .seq = 0 };
		trace_ops = (struct rpmsg_mem_ops){ .write = trace_write, .read = trace_read, .priv = &trace };
		mem = &trace_ops;
		printf("dma trace:  ON (dev_addr + len printed per MDB5 transfer)\n");
	} else {
		mem = &mdb5_ops;
	}

	rpu_pool = (struct pool){ .mem = mem, .base = cfg.rpu_pool_base, .size = cfg.rpu_pool_size, .off = 0 };
	dev_pool = (struct pool){ .mem = mem, .base = cfg.dev_pool_base, .size = cfg.dev_pool_size, .off = 0 };

	/* Bring up Link A rings in device memory (resets the vring region). */
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
	rc = rpmsg_lite_dma_init(&ctx, &dcfg);
	if (rc) { fprintf(stderr, "rpmsg_lite_dma_init: %s\n", strerror(-rc)); goto out_mdb5; }

	/* Confirm the rpmsg-lite DMA transport drove the shared region: init writes
	 * vdev status = ACK|DRIVER|DRIVER_OK (0x07) at shm_phys+status_offset. Read
	 * it back over the SAME MDB5 channel. If this shows 0x07 but XSDB at the
	 * device shows 0x00, the DMA is not landing at the right physical address.
	 * vring0/1 .da offsets (0x30/0x44) are this resource table's layout. */
	{
		uint8_t  st = 0;
		uint32_t da0 = 0, da1 = 0;
		mem->read(mem->priv, &st,  cfg.dev_base + cfg.status_offset, 1);
		mem->read(mem->priv, &da0, cfg.dev_base + 0x30, 4);
		mem->read(mem->priv, &da1, cfg.dev_base + 0x44, 4);
		printf("rpmsg-lite DMA: init OK (MDB5 backend, shm_phys=0x%" PRIx64 ")\n", cfg.dev_base);
		printf("  vdev status @0x%" PRIx64 " = 0x%02x  DRIVER_OK(0x04) = %s\n",
		       cfg.dev_base + cfg.status_offset, st, (st & 0x04u) ? "YES" : "NO");
		printf("  vring0.da = 0x%08x  vring1.da = 0x%08x\n", da0, da1);
	}

	rc = rpmsg_lite_dma_create_ept(&ctx, &ept, cfg.ept_name, cfg.host_ept_addr, on_rsp, &rsp);
	if (rc) { fprintf(stderr, "create_ept: %s\n", strerror(-rc)); goto out_dma; }
	printf("ept '%s' local_addr=%u -> peer_dst=%u\n\n", cfg.ept_name, ept.local_addr, cfg.peer_dst);

	/* ---- ELF into the RPU pool ---------------------------------------- */
	uint64_t elf_sz = 0, elf_pa = 0;
	rc = load_elf(opts.elf_file, &elf_data, &elf_sz);
	if (rc) goto out_dma;
	if (pool_alloc(&rpu_pool, elf_sz, &elf_pa)) goto out_dma;
	rc = pool_write(&rpu_pool, elf_pa, elf_data, elf_sz);
	if (rc) { fprintf(stderr, "DMA ELF: %s\n", strerror(-rc)); goto out_dma; }
	printf("elf:        PA=0x%" PRIx64 " size=%" PRIu64 "\n", elf_pa, elf_sz);

	/* ---- kernel-arg buffers into the DEVICE pool ---------------------- */
	for (uint32_t i = 0; i < opts.n_kargs; i++) {
		struct karg_spec *k = &opts.kargs[i];
		uint64_t pa = 0;
		if (k->is_output) {
			if (pool_alloc(&dev_pool, k->size, &pa)) goto out_dma;
			rc = pool_zero(&dev_pool, pa, k->size);
			if (rc) { fprintf(stderr, "DMA zero arg[%u]: %s\n", i, strerror(-rc)); goto out_dma; }
			karg_sz[i] = k->size;
			printf("arg[%u]:     zeroed PA=0x%" PRIx64 " size=%" PRIu64 " (output)\n", i, pa, k->size);
		} else {
			uint8_t *d = NULL; uint64_t fsz = 0;
			rc = slurp(k->path, &d, &fsz);
			if (rc) goto out_dma;
			if (pool_alloc(&dev_pool, fsz, &pa)) { free(d); goto out_dma; }
			rc = pool_write(&dev_pool, pa, d, fsz);
			free(d);
			if (rc) { fprintf(stderr, "DMA arg[%u]: %s\n", i, strerror(-rc)); goto out_dma; }
			karg_sz[i] = fsz;
			printf("arg[%u]:     %s -> PA=0x%" PRIx64 " size=%" PRIu64 "\n", i, k->path, pa, fsz);
		}
		karg_pa[i] = pa;
	}

	/* ---- descriptor into the RPU pool --------------------------------- */
	uint32_t desc_sz = APU_XRT_CMD_DESC_HDR_SIZE + APU_XRT_RUN_ENTRY_SIZE(opts.n_kargs);
	desc = calloc(1, desc_sz);
	if (!desc) { fprintf(stderr, "desc alloc failed\n"); goto out_dma; }
	struct apu_xrt_cmd_desc *cd = (struct apu_xrt_cmd_desc *)desc;
	cd->n_entries = 1;
	struct apu_xrt_run_entry *re = (struct apu_xrt_run_entry *)(desc + APU_XRT_CMD_DESC_HDR_SIZE);
	re->iterations = opts.iterations;
	re->n_kargs = opts.n_kargs;
	for (uint32_t i = 0; i < opts.n_kargs; i++) { re->kargs[i].pa = karg_pa[i]; re->kargs[i].size = karg_sz[i]; }
	uint64_t desc_pa = 0;
	if (pool_alloc(&rpu_pool, desc_sz, &desc_pa)) goto out_dma;
	rc = pool_write(&rpu_pool, desc_pa, desc, desc_sz);
	if (rc) { fprintf(stderr, "DMA desc: %s\n", strerror(-rc)); goto out_dma; }
	printf("descriptor: PA=0x%" PRIx64 " size=%u (1 entry, %u kargs)\n", desc_pa, desc_sz, opts.n_kargs);

	/* ---- result into the RPU pool ------------------------------------- */
	uint64_t result_pa = 0;
	if (opts.want_result) {
		if (pool_alloc(&rpu_pool, APU_XRT_TEST_RESULT_SIZE, &result_pa)) goto out_dma;
		rc = pool_zero(&rpu_pool, result_pa, APU_XRT_TEST_RESULT_SIZE);
		if (rc) { fprintf(stderr, "DMA zero result: %s\n", strerror(-rc)); goto out_dma; }
	}

	/* ---- build + send the APU_XRT_TEST wire command ------------------- */
	uint32_t seq = 1;
	uint8_t cmdbuf[sizeof(struct amdrnpu_wire_cmd) + APU_XRT_TEST_NUM_ARGS * sizeof(uint64_t)];
	struct amdrnpu_wire_cmd *cmd = (struct amdrnpu_wire_cmd *)cmdbuf;
	cmd->seq = seq;
	cmd->command_op = APU_XRT_TEST;
	cmd->flags = 0;
	cmd->num_args = APU_XRT_TEST_NUM_ARGS;
	cmd->args[0] = opts.iterations;
	cmd->args[1] = elf_pa;
	cmd->args[2] = elf_sz;
	cmd->args[3] = desc_pa;
	cmd->args[4] = desc_sz;
	cmd->args[APU_XRT_TEST_RESULT_ARG_IDX] = opts.want_result ? result_pa : 0;

	if (opts.dump_args) {
		printf("\n-- descriptor kargs (n=%u) --\n", opts.n_kargs);
		for (uint32_t i = 0; i < opts.n_kargs; i++)
			printf("   karg[%2u] PA=0x%010" PRIx64 " size=%-10" PRIu64 " %s\n",
			       i, re->kargs[i].pa, re->kargs[i].size,
			       opts.kargs[i].is_output ? "(output, zeroed)" : opts.kargs[i].path);
		printf("-- wire args (APU_XRT_TEST, %u) --\n", APU_XRT_TEST_NUM_ARGS);
		printf("   args[0] iterations = %" PRIu64 "\n", cmd->args[0]);
		printf("   args[1] elf_pa     = 0x%010" PRIx64 "\n", cmd->args[1]);
		printf("   args[2] elf_size   = %" PRIu64 "\n", cmd->args[2]);
		printf("   args[3] desc_pa    = 0x%010" PRIx64 "\n", cmd->args[3]);
		printf("   args[4] desc_size  = %" PRIu64 "\n", cmd->args[4]);
		printf("   args[5] result_pa  = 0x%010" PRIx64 "\n", cmd->args[APU_XRT_TEST_RESULT_ARG_IDX]);
	}

	printf("\nsubmitting APU_XRT_TEST (%u args) to peer_dst=%u ...\n", APU_XRT_TEST_NUM_ARGS, cfg.peer_dst);
	hexdump("rpmsg TX (APU_XRT_TEST)", cmdbuf, sizeof(cmdbuf));
	rsp.got = 0;
	rc = rpmsg_lite_dma_sendto(&ctx, &ept, cmdbuf, sizeof(cmdbuf), cfg.peer_dst);
	if (rc) { fprintf(stderr, "sendto: %s\n", strerror(-rc)); goto out_dma; }

	/* ---- poll for the RPU response ------------------------------------ */
	uint64_t deadline = now_ns() + (uint64_t)opts.timeout_s * 1000000000ull;
	while (!rsp.got) {
		rc = rpmsg_lite_dma_poll(&ctx);
		if (rc < 0) { fprintf(stderr, "poll: %s\n", strerror(-rc)); goto out_dma; }
		if (now_ns() > deadline) {
			fprintf(stderr, "TIMEOUT waiting for RPU response (is RPU_APP running?)\n");
			goto out_dma;
		}
	}
	printf("response:   seq=%u type=%u (%s)\n", rsp.seq, rsp.rsp_type, rsp_str(rsp.rsp_type));
	if (rsp.rsp_type != AMDRNPU_WIRE_RSP_COMPLETE) { ret = 1; goto out_dma; }

	/* ---- DMA-read the result struct back ------------------------------ */
	if (opts.want_result) {
		struct apu_xrt_test_result res;
		memset(&res, 0, sizeof(res));
		rc = mem->read(mem->priv, &res, result_pa, sizeof(res));
		if (rc) { fprintf(stderr, "DMA read result: %s\n", strerror(-rc)); goto out_dma; }
		printf("result:     passed=%u iterations_done=%u avg_us=%u first_us=%u\n",
		       res.passed, res.iterations_done, res.latency_avg_us, res.latency_first_us);
		ret = res.passed ? 0 : 1;
	} else {
		ret = 0;
	}

out_dma:
	rpmsg_lite_dma_deinit(&ctx);
out_mdb5:
	rpmsg_mem_mdb5_close(&mdb5);
out_scratch:
	free(desc);
	free(elf_data);
	free(scratch);
	printf("%s\n", ret == 0 ? "PASS" : "FAIL");
	return ret;
}
