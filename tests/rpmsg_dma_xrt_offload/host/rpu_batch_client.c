/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 */
/*
 * rpu_batch_client - see rpu_batch_client.h. Transport pieces (INI parser,
 * DMA word-align / trace shims, completion polling) as in batch_host.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "rpu_batch_client.h"

#include "rpmsg_lite_dma.h"
#include "rpmsg_mem_mdb5.h"
#include "amdrnpu_wire.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <zlib.h>

#define POOL_ALIGN         0x1000u     /* page-align every pool allocation */
#define RESULT_POISON      0xFFFFFFFFu /* result.status sentinel = "not written yet" */
/* After completion is seen via the DDR result word, keep polling this long for
 * the rpmsg reply, so the RPU's reply goes out before the host moves on or
 * deinits (which would clear DRIVER_OK and fail the RPU's rpmsg_send). */
#define REPLY_WAIT_MS      500u
#define DEFAULT_TIMEOUT_S  60u

/* ---------------------------------------------------------------------------
 * Config (inline INI parser). Same keys as xrt_offload_host.
 * ------------------------------------------------------------------------- */
static void config_defaults(struct rbc_config *c)
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

static int config_apply(struct rbc_config *c, const char *key, const char *val)
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

int rbc_config_load(struct rbc_config *c, const char *path)
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
 * File / ELF loading (transparently inflates a zlib-compressed control ELF).
 * ------------------------------------------------------------------------- */
int rbc_load_file(const char *path, uint8_t **data, uint64_t *size)
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

int rbc_load_elf(const char *path, uint8_t **data, uint64_t *size)
{
	uint8_t *raw = NULL, *elf = NULL;
	uint64_t raw_len = 0, elf_len = 0;
	int rc = rbc_load_file(path, &raw, &raw_len);
	if (rc) return rc;
	if (!is_zlib(raw, raw_len)) { *data = raw; *size = raw_len; return 0; }
	rc = zlib_inflate(raw, raw_len, &elf, &elf_len);
	free(raw);
	if (rc) { fprintf(stderr, "zlib inflate(%s) failed\n", path); return rc; }
	if (elf_len < 4 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
		fprintf(stderr, "%s: inflated data is not an ELF\n", path);
		free(elf); return -EINVAL;
	}
	*data = elf; *size = elf_len;
	return 0;
}

/* ---------------------------------------------------------------------------
 * DMA tracing shim (trace_dma): prints the first access to each
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
 * Client
 * ------------------------------------------------------------------------- */
struct pool {
	uint64_t base;
	uint64_t size;
	uint64_t off;
};

struct rbc_client {
	struct rbc_config          cfg;
	struct rbc_open_opts       opts;
	volatile sig_atomic_t      stop;

	struct rpmsg_mem_mdb5      mdb5;
	struct rpmsg_mem_ops       mdb5_ops;
	struct dma_trace           trace;
	struct rpmsg_mem_ops       trace_ops;
	struct dma_align           align;
	struct rpmsg_mem_ops       align_ops;
	const struct rpmsg_mem_ops *mem;

	struct rpmsg_lite_dma      ctx;
	struct rpmsg_lite_dma_ept  ept;
	bool                       dma_up;
	uint8_t                   *scratch;
	uint64_t                   vr0_used_addr;  /* verbose completion trace */

	struct pool                pools[2];       /* enum rbc_pool */

	/* reply capture for the in-flight command */
	uint32_t                   seq;
	uint32_t                   expect_seq;
	volatile int               got;
	uint32_t                   rsp_type;
};

#define VPRINTF(c, ...) do { if ((c)->opts.verbose) printf(__VA_ARGS__); } while (0)

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

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_us(const struct rbc_client *c, uint32_t us)
{
	struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L };
	while (nanosleep(&ts, &ts) && errno == EINTR && !c->stop)
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

const char *rbc_status_str(uint32_t status)
{
	switch (status) {
	case APU_XRT_BATCH_OK:        return "OK";
	case APU_XRT_BATCH_ERR_ARGS:  return "ERR_ARGS";
	case APU_XRT_BATCH_ERR_BUSY:  return "ERR_BUSY";
	case APU_XRT_BATCH_ERR_NOCTX: return "ERR_NOCTX";
	case APU_XRT_BATCH_ERR_XRT:   return "ERR_XRT";
	case APU_XRT_BATCH_ERR_RUN:   return "ERR_RUN";
	default:                      return "UNKNOWN";
	}
}

static void on_rsp(struct rpmsg_lite_dma_ept *ept, const void *data, size_t len, void *priv)
{
	struct rbc_client *c = priv;
	const struct amdrnpu_wire_rsp *w = data;
	(void)ept;
	if (c->opts.verbose)
		hexdump("rpmsg RX (response)", data, len);
	if (len < sizeof(*w))
		return;
	if (w->seq != c->expect_seq) {
		/* Late reply to an earlier command (its completion was taken
		 * from the DDR result word); ignore it. */
		VPRINTF(c, "rpmsg RX: stale reply seq=%u (waiting for %u), ignored\n",
			w->seq, c->expect_seq);
		return;
	}
	c->rsp_type = w->command_rsp_type;
	c->got = 1;
}

void rbc_request_stop(struct rbc_client *c) { c->stop = 1; }
void rbc_clear_stop(struct rbc_client *c)   { c->stop = 0; }

int rbc_open(struct rbc_client **out, const struct rbc_config *cfg,
	     const struct rbc_open_opts *o)
{
	struct rbc_client *c;
	int rc;

	*out = NULL;
	if (sizeof(struct amdrnpu_wire_cmd) + APU_XRT_BATCH_MAX_ARGS * sizeof(uint64_t) +
	    sizeof(struct rpmsg_hdr) > cfg->buf_size) {
		fprintf(stderr, "rbc: rpmsg buf_size %u too small for %u-arg requests\n",
			cfg->buf_size, APU_XRT_BATCH_MAX_ARGS);
		return -EINVAL;
	}
	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->cfg = *cfg;
	if (o)
		c->opts = *o;
	if (!c->opts.timeout_s)
		c->opts.timeout_s = DEFAULT_TIMEOUT_S;
	c->scratch = malloc(cfg->buf_size);
	if (!c->scratch) { free(c); return -ENOMEM; }

	rc = rpmsg_mem_mdb5_open(&c->mdb5, &c->mdb5_ops, cfg->mdb5_write, cfg->mdb5_read);
	if (rc) {
		fprintf(stderr, "rbc: mdb5 open (%s / %s): %s\n", cfg->mdb5_write,
			cfg->mdb5_read, strerror(-rc));
		free(c->scratch); free(c);
		return rc;
	}

	/* align shim closest to rpmsg_lite_dma; trace (if any) below it so the
	 * log shows the actual word-aligned MDB5 transfers. */
	const struct rpmsg_mem_ops *base = &c->mdb5_ops;
	if (c->opts.trace_dma) {
		c->trace.inner = &c->mdb5_ops;
		c->trace_ops = (struct rpmsg_mem_ops){ .write = trace_write, .read = trace_read, .priv = &c->trace };
		base = &c->trace_ops;
	}
	c->align.inner = base;
	c->align_ops = (struct rpmsg_mem_ops){ .write = align_write, .read = align_read, .priv = &c->align };
	c->mem = &c->align_ops;

	c->pools[RBC_POOL_RPU] = (struct pool){ .base = cfg->rpu_pool_base, .size = cfg->rpu_pool_size };
	c->pools[RBC_POOL_DEV] = (struct pool){ .base = cfg->dev_pool_base, .size = cfg->dev_pool_size };

	struct rpmsg_lite_dma_cfg dcfg = {
		.mem = c->mem,
		.shm_phys = cfg->dev_base,
		.status_offset = cfg->status_offset,
		.vring_tx_offset = cfg->vring_tx_offset,
		.vring_rx_offset = cfg->vring_rx_offset,
		.vring_num = cfg->vring_num,
		.vring_align = cfg->vring_align,
		.buf_offset = cfg->buf_offset,
		.buf_size = cfg->buf_size,
		.buf_count = cfg->buf_count,
		.notify = NULL,
		.priv = NULL,
		.scratch = c->scratch,
		.scratch_size = cfg->buf_size,
		.shm_size = cfg->shm_size,
	};
	/* The RPU firmware reads vring0/1 .da once, right after DRIVER_OK, so
	 * fill them (resource-table offsets +0x30 / +0x44) before
	 * rpmsg_lite_dma_init sets DRIVER_OK. */
	{
		uint32_t da0 = (uint32_t)(cfg->dev_base + cfg->vring_rx_offset);
		uint32_t da1 = (uint32_t)(cfg->dev_base + cfg->vring_tx_offset);
		rc = c->mem->write(c->mem->priv, cfg->dev_base + 0x30, &da0, sizeof(da0));
		if (!rc) rc = c->mem->write(c->mem->priv, cfg->dev_base + 0x44, &da1, sizeof(da1));
		if (rc) { fprintf(stderr, "rbc: write vring da: %s\n", strerror(-rc)); goto err; }
		VPRINTF(c, "vring da:   vring0.da=0x%08x vring1.da=0x%08x (before DRIVER_OK)\n", da0, da1);
	}

	rc = rpmsg_lite_dma_init(&c->ctx, &dcfg);
	if (rc) { fprintf(stderr, "rbc: rpmsg_lite_dma_init: %s\n", strerror(-rc)); goto err; }
	c->dma_up = true;
	rc = rpmsg_lite_dma_create_ept(&c->ctx, &c->ept, cfg->ept_name, cfg->host_ept_addr, on_rsp, c);
	if (rc) { fprintf(stderr, "rbc: create_ept: %s\n", strerror(-rc)); goto err; }
	VPRINTF(c, "[host-ept] '%s' local_addr=%u -> peer_dst=%u\n", cfg->ept_name,
		c->ept.local_addr, cfg->peer_dst);

	uint64_t used_off = (uint64_t)cfg->vring_num * 16u + 6u + 2u * cfg->vring_num;
	used_off = (used_off + cfg->vring_align - 1u) & ~((uint64_t)cfg->vring_align - 1u);
	c->vr0_used_addr = cfg->dev_base + cfg->vring_rx_offset + used_off;

	*out = c;
	return 0;
err:
	rbc_close(c);
	return rc;
}

void rbc_close(struct rbc_client *c)
{
	if (!c)
		return;
	if (c->dma_up)
		rpmsg_lite_dma_deinit(&c->ctx);
	rpmsg_mem_mdb5_close(&c->mdb5);
	free(c->scratch);
	free(c);
}

/* ---------------------------------------------------------------------------
 * Device memory: bump allocator per pool, never reused, so no buffer (in
 * particular a result word) shares a line the host already read.
 * ------------------------------------------------------------------------- */
int rbc_alloc(struct rbc_client *c, enum rbc_pool which, uint64_t len, uint64_t *pa)
{
	struct pool *p = &c->pools[which];
	uint64_t aligned = (len + POOL_ALIGN - 1) & ~(uint64_t)(POOL_ALIGN - 1);
	if (p->off + aligned > p->size) {
		fprintf(stderr, "rbc: pool @0x%" PRIx64 " overflow: need %" PRIu64
			" B, %" PRIu64 " B free\n", p->base, aligned, p->size - p->off);
		return -ENOMEM;
	}
	*pa = p->base + p->off;
	p->off += aligned;
	return 0;
}

int rbc_write(struct rbc_client *c, uint64_t pa, const void *src, uint64_t len)
{
	return c->mem->write(c->mem->priv, pa, src, (size_t)len);
}

int rbc_zero(struct rbc_client *c, uint64_t pa, uint64_t len)
{
	/* Not const: MDB5 pins user pages writable (get_user_pages_fast), which
	 * fails with EFAULT on read-only .rodata. */
	static uint8_t zeros[65536];
	uint64_t done = 0;
	while (done < len) {
		uint64_t n = len - done;
		if (n > sizeof(zeros)) n = sizeof(zeros);
		int rc = c->mem->write(c->mem->priv, pa + done, zeros, (size_t)n);
		if (rc) return rc;
		done += n;
	}
	return 0;
}

int rbc_read(struct rbc_client *c, void *dst, uint64_t pa, uint64_t len)
{
	return c->mem->read(c->mem->priv, dst, pa, (size_t)len);
}

/* ---------------------------------------------------------------------------
 * Commands
 * ------------------------------------------------------------------------- */

/* Wait until the rpmsg reply for this command arrives or the RPU writes the
 * DDR result word. Returns 0 on completion, negative errno on error/timeout. */
static int wait_done(struct rbc_client *c, uint64_t result_pa, bool *via_rsp)
{
	uint64_t t0 = now_ns();
	uint64_t deadline = t0 + (uint64_t)c->opts.timeout_s * 1000000000ull;
	uint32_t last_used_w = 0, last_w0 = 0;
	bool first = true;

	for (unsigned long chk = 0;; chk++) {
		int rc = rpmsg_lite_dma_poll(&c->ctx);
		if (rc < 0) { fprintf(stderr, "rbc: poll: %s\n", strerror(-rc)); return rc; }
		if (c->got) { *via_rsp = true; return 0; }

		uint32_t w0 = RESULT_POISON;
		rc = c->mem->read(c->mem->priv, &w0, result_pa, sizeof(w0));
		if (rc) {
			fprintf(stderr, "rbc: result read @0x%" PRIx64 ": %s\n", result_pa, strerror(-rc));
			return rc;
		}
		if (c->opts.verbose) {
			uint32_t used_w = 0;
			c->mem->read(c->mem->priv, &used_w, c->vr0_used_addr, sizeof(used_w));
			if (first || used_w != last_used_w || w0 != last_w0)
				printf("  [chk %lu t+%.3fms] vring0.used=0x%08x (idx=%u) result=0x%08x%s\n",
				       chk, (double)(now_ns() - t0) / 1e6, used_w, used_w >> 16, w0,
				       w0 == RESULT_POISON ? " (poison)" : "");
			last_used_w = used_w;
			first = false;
		}
		last_w0 = w0;
		if (w0 != RESULT_POISON) { *via_rsp = false; return 0; }

		if (c->stop) { fprintf(stderr, "rbc: interrupted\n"); return -EINTR; }
		if (now_ns() > deadline) {
			fprintf(stderr, "rbc: TIMEOUT after %us / %lu checks: no rpmsg reply and no "
				"DDR result (last result=0x%08x)\n", c->opts.timeout_s, chk + 1, last_w0);
			return -ETIMEDOUT;
		}
		if (c->opts.poll_us)
			sleep_us(c, c->opts.poll_us);
	}
}

/* Poll for the rpmsg reply of the in-flight command for up to REPLY_WAIT_MS.
 * Returns true if it arrived. */
static bool wait_reply(struct rbc_client *c)
{
	uint64_t deadline = now_ns() + (uint64_t)REPLY_WAIT_MS * 1000000ull;

	while (!c->got && !c->stop && now_ns() < deadline) {
		if (rpmsg_lite_dma_poll(&c->ctx) < 0)
			break;
		if (!c->got)
			sleep_us(c, 1000);
	}
	return c->got != 0;
}

static const char *op_name(uint32_t op)
{
	switch (op) {
	case APU_XRT_BATCH_CREATE:  return "CREATE";
	case APU_XRT_BATCH_RUN:     return "RUN";
	case APU_XRT_BATCH_DESTROY: return "DESTROY";
	default:                    return "?";
	}
}

/* One command: fresh result slot (pre-filled with 0xFF), send, wait for the
 * reply or the result's status word, collect the reply, read the result.
 * args[res_idx] is filled with the result PA here. */
static int send_cmd(struct rbc_client *c, uint32_t op, uint64_t *args, uint32_t nargs,
		    uint32_t res_idx, struct rbc_cmd_stat *st)
{
	uint8_t cmdbuf[sizeof(struct amdrnpu_wire_cmd) + APU_XRT_BATCH_MAX_ARGS * sizeof(uint64_t)];
	struct amdrnpu_wire_cmd *cmd = (struct amdrnpu_wire_cmd *)cmdbuf;
	const size_t cmd_bytes = sizeof(*cmd) + (size_t)nargs * sizeof(uint64_t);
	uint8_t poison[APU_XRT_BATCH_RESULT_SIZE];
	uint64_t result_pa, t;
	int rc;

	memset(st, 0, sizeof(*st));
	st->res.status = RESULT_POISON;
	if (nargs > APU_XRT_BATCH_MAX_ARGS) {
		fprintf(stderr, "rbc: %s: %u args > %u (one rpmsg buffer)\n", op_name(op),
			nargs, APU_XRT_BATCH_MAX_ARGS);
		return -E2BIG;
	}
	/* Never reuse a result word the host already polled. */
	rc = rbc_alloc(c, RBC_POOL_RPU, APU_XRT_BATCH_RESULT_SIZE, &result_pa);
	if (rc) return rc;
	memset(poison, 0xFF, sizeof(poison));
	rc = rbc_write(c, result_pa, poison, sizeof(poison));
	if (rc) { fprintf(stderr, "rbc: %s: result init: %s\n", op_name(op), strerror(-rc)); return rc; }
	args[res_idx] = result_pa;

	cmd->seq = ++c->seq;
	cmd->command_op = op;
	cmd->flags = 0;
	cmd->num_args = nargs;
	memcpy(cmd->args, args, (size_t)nargs * sizeof(uint64_t));
	if (c->opts.verbose)
		hexdump(op_name(op), cmdbuf, cmd_bytes);

	c->expect_seq = cmd->seq;
	c->got = 0;
	t = now_ns();
	rc = rpmsg_lite_dma_sendto(&c->ctx, &c->ept, cmdbuf, cmd_bytes, c->cfg.peer_dst);
	if (rc) { fprintf(stderr, "rbc: %s: sendto: %s\n", op_name(op), strerror(-rc)); return rc; }
	rc = wait_done(c, result_pa, &st->via_rsp);
	st->exec_ns = now_ns() - t;
	if (rc) return rc;

	/* Collect the reply too (untimed) so the RPU's send succeeds and the
	 * rings stay in step. */
	bool got_reply = st->via_rsp || wait_reply(c);
	if (!got_reply)
		fprintf(stderr, "rbc: %s: no rpmsg reply within %u ms after DDR completion\n",
			op_name(op), REPLY_WAIT_MS);

	rc = rbc_read(c, &st->res, result_pa, sizeof(st->res));
	if (rc) { fprintf(stderr, "rbc: %s: read result: %s\n", op_name(op), strerror(-rc)); return rc; }
	if (st->res.status != APU_XRT_BATCH_OK) {
		fprintf(stderr, "rbc: %s: RPU status %s (%u), ctx %u, entries_done %u\n", op_name(op),
			rbc_status_str(st->res.status), st->res.status, st->res.ctx_id,
			st->res.entries_done);
		return -EIO;
	}
	if (got_reply && c->rsp_type != AMDRNPU_WIRE_RSP_COMPLETE) {
		fprintf(stderr, "rbc: %s: RPU replied %s\n", op_name(op), rsp_str(c->rsp_type));
		return -EIO;
	}
	return 0;
}

int rbc_create(struct rbc_client *c, uint64_t elf_pa, uint64_t elf_size,
	       const struct rbc_karg *fixed, uint32_t n_fixed,
	       uint32_t *ctx_id, struct rbc_cmd_stat *st)
{
	uint64_t args[APU_XRT_BATCH_MAX_ARGS] = { 0 };
	const uint32_t nargs = APU_XRT_BATCH_CREATE_NUM_ARGS(n_fixed);
	int rc;

	if (n_fixed > APU_XRT_BATCH_MAX_KARGS || nargs > APU_XRT_BATCH_MAX_ARGS)
		return -E2BIG;
	args[0] = elf_pa;
	args[1] = elf_size;
	args[3] = n_fixed;
	for (uint32_t i = 0; i < n_fixed; i++) {
		const uint32_t b = APU_XRT_BATCH_CREATE_FIXED_IDX(i);
		args[b]     = fixed[i].kidx;
		args[b + 1] = fixed[i].pa;
		args[b + 2] = fixed[i].size;
	}
	rc = send_cmd(c, APU_XRT_BATCH_CREATE, args, nargs, 2, st);
	if (!rc)
		*ctx_id = st->res.ctx_id;
	return rc;
}

int rbc_run(struct rbc_client *c, uint32_t ctx_id, uint32_t n_entries,
	    uint32_t kpe, const uint32_t *kidx, const struct rbc_karg *entries,
	    struct rbc_cmd_stat *st)
{
	uint64_t args[APU_XRT_BATCH_MAX_ARGS] = { 0 };

	if (!n_entries || kpe > APU_XRT_BATCH_MAX_KARGS ||
	    APU_XRT_BATCH_RUN_NUM_ARGS(n_entries, kpe) > APU_XRT_BATCH_MAX_ARGS) {
		fprintf(stderr, "rbc: RUN: %u entries x %u kargs needs %u args (max %u)\n",
			n_entries, kpe, APU_XRT_BATCH_RUN_NUM_ARGS(n_entries, kpe),
			APU_XRT_BATCH_MAX_ARGS);
		return -E2BIG;
	}
	args[0] = ctx_id;
	args[2] = n_entries;
	args[3] = kpe;
	for (uint32_t k = 0; k < kpe; k++)
		args[APU_XRT_BATCH_RUN_KIDX_IDX(k)] = kidx[k];
	for (uint32_t e = 0; e < n_entries; e++)
		for (uint32_t k = 0; k < kpe; k++) {
			const uint32_t i = APU_XRT_BATCH_RUN_ENTRY_IDX(e, k, kpe);
			args[i]     = entries[e * kpe + k].pa;
			args[i + 1] = entries[e * kpe + k].size;
		}
	return send_cmd(c, APU_XRT_BATCH_RUN, args, APU_XRT_BATCH_RUN_NUM_ARGS(n_entries, kpe), 1, st);
}

int rbc_destroy(struct rbc_client *c, uint32_t ctx_id, struct rbc_cmd_stat *st)
{
	uint64_t args[APU_XRT_BATCH_DESTROY_NUM_ARGS] = { ctx_id, 0 };
	return send_cmd(c, APU_XRT_BATCH_DESTROY, args, APU_XRT_BATCH_DESTROY_NUM_ARGS, 1, st);
}
