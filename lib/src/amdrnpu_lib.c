/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * Userspace amdrnpu backend.
 *
 * The library no longer depends on the amdrnpu kernel module.  Device/RPU
 * memory is carved from fixed physical address pools (no-map in the device
 * tree) with a 4 KiB-granular bitmap allocator and accessed through /dev/mem;
 * commands are submitted over the userspace rpmsg_lite leader.  All layout --
 * pool base/size and the rpmsg shared-memory geometry -- is read once from
 * /etc/amdrnpu.ini (override with $AMDRNPU_INI) and cached in a lazily built
 * process-wide context.  The public API signatures are unchanged; the `int fd`
 * argument is now only an opaque key the caller passes in.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* le32toh()/htole64() live behind __USE_MISC (glibc <endian.h>). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu_lib.h>

#include "amdrnpu_ini.h"
#include "amdrnpu_pool.h"
#include "amdrnpu_trace.h"
#include "amdrnpu_wire.h"
#include "rpmsg_lite.h"

#define AMDRNPU_NPOOLS 2 /* indexed by enum amdrnpu_memory_type (RPU=0, DEVICE=1) */
#define AMDRNPU_LIB_MAX_ARGS 64
#define AMDRNPU_BIND_POLLS 200000  /* NS-bind poll budget at init */
#define AMDRNPU_CMD_POLLS 2000000  /* send/response poll budget per submit */

/* Device-memory-safe pool copies (see definitions below buf_read). */
static void amdrnpu_memcpy_todev(volatile uint8_t *d, const uint8_t *s,
				 size_t n);
static void amdrnpu_memcpy_fromdev(uint8_t *d, const volatile uint8_t *s,
				   size_t n);

/* ------------------------------------------------------------------ */
/* Process-wide context                                               */
/* ------------------------------------------------------------------ */

struct amdrnpu_ctx {
	int initialized;
	int init_err;			/* cached fatal init failure */
	int mem_fd;			/* /dev/mem */
	struct amdrnpu_config cfg;
	struct amdrnpu_pool pool[AMDRNPU_NPOOLS];
	void *shm_map;			/* rpmsg shared region mapping */
	size_t shm_len;
	struct rpmsg_lite rl;
	struct rpmsg_lite_ept ept;
	uint32_t next_seq;		/* wire command id cursor */
	/* response capture (one command outstanding at a time under g_lock) */
	int rsp_seen;
	uint32_t rsp_seq;
	uint32_t rsp_type;
};

static struct amdrnpu_ctx g_ctx;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Buffer handle registry (guarded by g_lock)                         */
/* ------------------------------------------------------------------ */

struct amdrnpu_buf_entry {
	int used;
	uint32_t handle;
	uint32_t pool_id;
	size_t page_index;
	size_t npages;
	uint64_t phys;
	uint64_t size;
};

static struct amdrnpu_buf_entry *amdrnpu_reg;
static size_t amdrnpu_reg_cap;
static uint32_t amdrnpu_next_handle = 1;

/* Insert a new entry and return its handle in @out_handle. Caller holds g_lock. */
static int reg_add(uint32_t *out_handle, uint32_t pool_id, size_t page_index,
		   size_t npages, uint64_t phys, uint64_t size)
{
	size_t i, slot = (size_t)-1;

	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (!amdrnpu_reg[i].used) {
			slot = i;
			break;
		}
	}
	if (slot == (size_t)-1) {
		size_t ncap = amdrnpu_reg_cap ? amdrnpu_reg_cap * 2 : 8;
		struct amdrnpu_buf_entry *n =
			realloc(amdrnpu_reg, ncap * sizeof(*n));

		if (!n)
			return -ENOMEM;
		memset(n + amdrnpu_reg_cap, 0,
		       (ncap - amdrnpu_reg_cap) * sizeof(*n));
		slot = amdrnpu_reg_cap;
		amdrnpu_reg = n;
		amdrnpu_reg_cap = ncap;
	}

	amdrnpu_reg[slot].used = 1;
	amdrnpu_reg[slot].handle = amdrnpu_next_handle++;
	if (amdrnpu_next_handle == 0)
		amdrnpu_next_handle = 1;
	amdrnpu_reg[slot].pool_id = pool_id;
	amdrnpu_reg[slot].page_index = page_index;
	amdrnpu_reg[slot].npages = npages;
	amdrnpu_reg[slot].phys = phys;
	amdrnpu_reg[slot].size = size;
	*out_handle = amdrnpu_reg[slot].handle;
	return 0;
}

/* Copy the entry for @handle into @out. Returns 1 if found. Caller holds g_lock. */
static int reg_lookup(uint32_t handle, struct amdrnpu_buf_entry *out)
{
	size_t i;

	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (amdrnpu_reg[i].used && amdrnpu_reg[i].handle == handle) {
			*out = amdrnpu_reg[i];
			return 1;
		}
	}
	return 0;
}

/* Copy and remove the entry for @handle. Returns 1 if found. Caller holds g_lock. */
static int reg_remove(uint32_t handle, struct amdrnpu_buf_entry *out)
{
	size_t i;

	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (amdrnpu_reg[i].used && amdrnpu_reg[i].handle == handle) {
			*out = amdrnpu_reg[i];
			amdrnpu_reg[i].used = 0;
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Completion table: sync_handle (eventfd) -> captured status          */
/* ------------------------------------------------------------------ */

struct amdrnpu_cmpl_entry {
	int used;
	int fd;
	int32_t status;
};

static struct amdrnpu_cmpl_entry *amdrnpu_cmpl;
static size_t amdrnpu_cmpl_cap;

/* Store (or overwrite) the completion status for @fd. Self-locking. */
static int cmpl_store(int fd, int32_t status)
{
	size_t i, slot = (size_t)-1;
	int ret = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < amdrnpu_cmpl_cap; i++) {
		if (amdrnpu_cmpl[i].used && amdrnpu_cmpl[i].fd == fd) {
			slot = i;
			break;
		}
		if (!amdrnpu_cmpl[i].used && slot == (size_t)-1)
			slot = i;
	}
	if (slot == (size_t)-1) {
		size_t ncap = amdrnpu_cmpl_cap ? amdrnpu_cmpl_cap * 2 : 8;
		struct amdrnpu_cmpl_entry *n =
			realloc(amdrnpu_cmpl, ncap * sizeof(*n));

		if (!n) {
			ret = -ENOMEM;
			goto out;
		}
		memset(n + amdrnpu_cmpl_cap, 0,
		       (ncap - amdrnpu_cmpl_cap) * sizeof(*n));
		slot = amdrnpu_cmpl_cap;
		amdrnpu_cmpl = n;
		amdrnpu_cmpl_cap = ncap;
	}
	amdrnpu_cmpl[slot].used = 1;
	amdrnpu_cmpl[slot].fd = fd;
	amdrnpu_cmpl[slot].status = status;
out:
	pthread_mutex_unlock(&g_lock);
	return ret;
}

/* Fetch the status for @fd. Returns 1 if found. Self-locking. */
static int cmpl_lookup(int fd, int32_t *out_status)
{
	size_t i;
	int found = 0;

	pthread_mutex_lock(&g_lock);
	for (i = 0; i < amdrnpu_cmpl_cap; i++) {
		if (amdrnpu_cmpl[i].used && amdrnpu_cmpl[i].fd == fd) {
			*out_status = amdrnpu_cmpl[i].status;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return found;
}

/* ------------------------------------------------------------------ */
/* Context initialization                                             */
/* ------------------------------------------------------------------ */

static void amdrnpu_nap(void)
{
	struct timespec ts = { 0, 50000 }; /* 50 us */

	nanosleep(&ts, NULL);
}

static void amdrnpu_rsp_cb(struct rpmsg_lite_ept *ept, const void *data,
			   size_t len, void *priv)
{
	struct amdrnpu_ctx *c = priv;
	struct amdrnpu_wire_rsp rsp;

	(void)ept;
	if (len < sizeof(rsp))
		return;
	memcpy(&rsp, data, sizeof(rsp));
	c->rsp_seq = le32toh(rsp.seq);
	c->rsp_type = le32toh(rsp.command_rsp_type);
	c->rsp_seen = 1;
}

static int amdrnpu_ctx_rpmsg_init(struct amdrnpu_ctx *c)
{
	const struct amdrnpu_rpmsg_cfg *r = &c->cfg.rpmsg;
	struct rpmsg_lite_cfg cfg;
	void *map;
	int ret, i;

	if (!r->shm_size)
		return -EINVAL;

	map = mmap(NULL, r->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   c->mem_fd, (off_t)r->shm_phys);
	if (map == MAP_FAILED)
		return -errno;
	c->shm_map = map;
	c->shm_len = r->shm_size;

	/*
	 * Master duty (remoteproc-style vring allocation).
	 *
	 * The OpenAMP remote (RPU_APP) ships a resource table whose vring
	 * descriptors carry da = -1 ("allocated by the master") and only reads
	 * the real addresses after we raise DRIVER_OK.  Linux remoteproc fills
	 * those da fields before DRIVER_OK; rpmsg-lite as leader does not (it
	 * only writes the virtio status byte).  So fill them here, before
	 * rpmsg_lite_init() raises DRIVER_OK, or the remote dereferences a NULL
	 * vring and faults in virtqueue_disable_cb.
	 *
	 * The fw_rsc_vdev / fw_rsc_vdev_vring layout is the stable
	 * virtio/remoteproc ABI, so derive the da offsets from status_offset
	 * (both shift together if the firmware enables CONFIG_RAM_CONSOLE):
	 *   fw_rsc_vdev.status      @ status_offset
	 *   fw_rsc_vdev.vring[0].da @ status_offset - 0x18 (offsetof status)
	 *                                           + 0x1C (offsetof vring[0])
	 *                            = status_offset + 0x04
	 *   fw_rsc_vdev.vring[1].da = vring[0].da + 0x14 (sizeof fw_rsc_vdev_vring)
	 *                            = status_offset + 0x18
	 * vring0 = firmware TX (leader RX) = shm_phys + vring_rx_offset
	 * vring1 = firmware RX (leader TX) = shm_phys + vring_tx_offset
	 * Both the APU and the R52 are little-endian, so a native u32 store
	 * matches what the remote reads.
	 */
	if (r->status_offset >= 0x18 &&
	    (r->status_offset + 0x1C) <= r->shm_size) {
		uint8_t *rsc = (uint8_t *)map;
		uint32_t da0 = (uint32_t)(r->shm_phys + r->vring_rx_offset);
		uint32_t da1 = (uint32_t)(r->shm_phys + r->vring_tx_offset);

		memcpy(rsc + r->status_offset + 0x04, &da0, sizeof(da0));
		memcpy(rsc + r->status_offset + 0x18, &da1, sizeof(da1));
	}

	/*
	 * Master duty (virtio feature negotiation).
	 *
	 * OpenAMP's device computes support_ns = dfeatures & gfeatures.  The
	 * remote advertises dfeatures (VIRTIO_RPMSG_F_NS) but leaves gfeatures = 0
	 * until the driver accepts features.  rpmsg-lite as leader never touches
	 * gfeatures, so the device sees support_ns = 0 and SKIPS its name-service
	 * announcement — rpmsg_create_ept() still returns success, but no NS ever
	 * lands in the leader's RX vring, so the bind times out.  Accept the
	 * device's offered features here (gfeatures = dfeatures), before DRIVER_OK,
	 * exactly as remoteproc's negotiate_features does.
	 *
	 * In fw_rsc_vdev: dfeatures @ status - 0x0C, gfeatures @ status - 0x08.
	 */
	if (r->status_offset >= 0x0C) {
		uint8_t *rsc = (uint8_t *)map;
		uint32_t dfeatures;

		memcpy(&dfeatures, rsc + r->status_offset - 0x0C, sizeof(dfeatures));
		memcpy(rsc + r->status_offset - 0x08, &dfeatures, sizeof(dfeatures));
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.shm_base = map;
	cfg.shm_phys = r->shm_phys;
	cfg.status_offset = r->status_offset;
	cfg.vring_tx_offset = r->vring_tx_offset;
	cfg.vring_rx_offset = r->vring_rx_offset;
	cfg.vring_num = r->vring_num;
	cfg.vring_align = r->vring_align;
	cfg.buf_offset = r->buf_offset;
	cfg.buf_size = r->buf_size;
	cfg.buf_count = r->buf_count;
	cfg.notify = NULL; /* polling */
	cfg.priv = NULL;
	cfg.shm_size = r->shm_size;

	ret = rpmsg_lite_init(&c->rl, &cfg);
	if (ret)
		goto err_unmap;

	ret = rpmsg_lite_create_ept(&c->rl, &c->ept, AMDRNPU_WIRE_CHANNEL,
				    RPMSG_ADDR_ANY, amdrnpu_rsp_cb, c);
	if (ret)
		goto err_deinit;

	ret = rpmsg_lite_ns_announce(&c->rl, &c->ept);
	if (ret)
		goto err_destroy;

	/* Poll until the firmware announces the "amdrnpu" channel. */
	for (i = 0; i < AMDRNPU_BIND_POLLS; i++) {
		if (c->ept.dest_addr != RPMSG_ADDR_ANY)
			return 0;
		ret = rpmsg_lite_poll(&c->rl);
		if (ret < 0)
			goto err_destroy;
		amdrnpu_nap();
	}
	ret = -ETIMEDOUT;

err_destroy:
	rpmsg_lite_destroy_ept(&c->rl, &c->ept);
err_deinit:
	rpmsg_lite_deinit(&c->rl);
err_unmap:
	munmap(map, r->shm_size);
	c->shm_map = NULL;
	return ret;
}

/*
 * Open the physical-memory device.  Normally /dev/mem (O_SYNC for an
 * uncached mapping of the no-map pools and rpmsg shm).  $AMDRNPU_DEVMEM
 * redirects this to a regular file whose offsets stand in for physical
 * addresses, which lets hardware-free tests back the pools and vrings with an
 * ordinary file.
 */
static int amdrnpu_open_devmem(void)
{
	const char *path = getenv("AMDRNPU_DEVMEM");

	if (path && *path)
		return open(path, O_RDWR | O_CLOEXEC);
	return open("/dev/mem", O_RDWR | O_CLOEXEC | O_SYNC);
}

/* Build the context from the ini. Caller holds g_lock. */
static int amdrnpu_ctx_init(struct amdrnpu_ctx *c)
{
	int ret;

	ret = amdrnpu_config_load(&c->cfg, NULL);
	if (ret)
		return ret;

	c->mem_fd = amdrnpu_open_devmem();
	if (c->mem_fd < 0)
		return -errno;

	ret = amdrnpu_pool_init(&c->pool[AMDRNPU_MEMORY_RPU],
				c->cfg.pool_rpu.base, c->cfg.pool_rpu.size);
	if (ret)
		goto err_close;
	ret = amdrnpu_pool_init(&c->pool[AMDRNPU_MEMORY_DEVICE],
				c->cfg.pool_device.base, c->cfg.pool_device.size);
	if (ret)
		goto err_pool_rpu;

	ret = amdrnpu_ctx_rpmsg_init(c);
	if (ret)
		goto err_pool_dev;

	return 0;

err_pool_dev:
	amdrnpu_pool_fini(&c->pool[AMDRNPU_MEMORY_DEVICE]);
err_pool_rpu:
	amdrnpu_pool_fini(&c->pool[AMDRNPU_MEMORY_RPU]);
err_close:
	close(c->mem_fd);
	c->mem_fd = -1;
	return ret;
}

static int amdrnpu_ctx_ensure(struct amdrnpu_ctx **out)
{
	int err;

	pthread_mutex_lock(&g_lock);
	if (!g_ctx.initialized && !g_ctx.init_err) {
		int ret = amdrnpu_ctx_init(&g_ctx);

		if (ret)
			g_ctx.init_err = ret;
		else
			g_ctx.initialized = 1;
	}
	err = g_ctx.init_err;
	pthread_mutex_unlock(&g_lock);
	*out = &g_ctx;
	return err;
}

/* ------------------------------------------------------------------ */
/* Command submission helpers                                         */
/* ------------------------------------------------------------------ */

static int32_t amdrnpu_rsp_status(uint32_t type)
{
	switch (type) {
	case AMDRNPU_WIRE_RSP_COMPLETE:
		return 0;
	case AMDRNPU_WIRE_RSP_REMOTEIO:
		return -EREMOTEIO;
	case AMDRNPU_WIRE_RSP_NOTSUPPORTED:
		return -EOPNOTSUPP;
	case AMDRNPU_WIRE_RSP_CANCELLED:
		return -ECANCELED;
	default:
		return -EREMOTEIO;
	}
}

/* Resolve scalar/buffer args to little-endian wire words. Caller holds g_lock. */
static int amdrnpu_resolve_args(const struct amdrnpu_arg *args, uint32_t num_args,
				uint64_t *out_words)
{
	uint32_t i;

	for (i = 0; i < num_args; i++) {
		const struct amdrnpu_arg *a = &args[i];
		struct amdrnpu_buf_entry e;

		if (a->size == 0) {
			out_words[i] = htole64(a->value);
			continue;
		}
		if (a->bo.buffer_handle <= 0)
			return -EINVAL;
		if (!reg_lookup((uint32_t)a->bo.buffer_handle, &e))
			return -ENOENT;
		if (a->bo.offset > e.size || a->size > e.size - a->bo.offset)
			return -EINVAL;
		out_words[i] = htole64(e.phys + a->bo.offset);
	}
	return 0;
}

/* Send @msg and poll rpmsg_lite until the matching response arrives. Caller holds g_lock. */
static int amdrnpu_send_and_wait(struct amdrnpu_ctx *c,
				 struct amdrnpu_wire_cmd *msg, size_t len,
				 uint32_t seq, int32_t *out_status)
{
	int ret, i;

	c->rsp_seen = 0;

	/* Transmit, polling to reclaim TX if every buffer is momentarily busy. */
	for (i = 0;; i++) {
		ret = rpmsg_lite_send(&c->rl, &c->ept, msg, len);
		if (ret == 0)
			break;
		if (ret != -EAGAIN)
			return ret;
		if (i >= AMDRNPU_CMD_POLLS)
			return -ETIMEDOUT;
		ret = rpmsg_lite_poll(&c->rl);
		if (ret < 0)
			return ret;
		amdrnpu_nap();
	}

	/* Await the response for our seq; drop any non-matching message. */
	for (i = 0; i < AMDRNPU_CMD_POLLS; i++) {
		ret = rpmsg_lite_poll(&c->rl);
		if (ret < 0)
			return ret;
		if (c->rsp_seen) {
			if (c->rsp_seq == seq) {
				*out_status = amdrnpu_rsp_status(c->rsp_type);
				return 0;
			}
			c->rsp_seen = 0;
		}
		amdrnpu_nap();
	}
	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int amdrnpu_open(uint32_t device_id)
{
	struct amdrnpu_ctx *c;
	int ret;

	/*
	 * Single Link A device for now; device_id is accepted for forward
	 * compatibility (multi-RPU / multi-link selection) but ignored.  Eagerly
	 * bring up the process-wide context here — mapping the shared window,
	 * filling the remote's vring descriptors, and completing the rpmsg-lite
	 * name-service bind — so the first buffer/command call does not pay the
	 * handshake latency and connection errors surface at open() time.
	 */
	(void)device_id;
	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		return ret;

	/* The rest of the API takes an opaque key; the lib keys off g_ctx. */
	return 0;
}

int amdrnpu_buf_alloc(int fd, uint32_t memory_type, uint64_t size,
		      uint32_t *out_handle)
{
	struct amdrnpu_ctx *c;
	size_t npages;
	long idx;
	uint32_t handle = 0;
	int ret;

	(void)fd;
	AMDRNPU_TRACE2(buf_alloc_enter, memory_type, (int)size);
	if (!out_handle || size == 0)
		AMDRNPU_TRACE_RET(buf_alloc_exit, -EINVAL);
	if (memory_type != AMDRNPU_MEMORY_RPU &&
	    memory_type != AMDRNPU_MEMORY_DEVICE)
		AMDRNPU_TRACE_RET(buf_alloc_exit, -EINVAL);

	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		AMDRNPU_TRACE_RET(buf_alloc_exit, ret);

	npages = amdrnpu_pool_pages_for(size);

	pthread_mutex_lock(&g_lock);
	idx = amdrnpu_pool_alloc(&c->pool[memory_type], npages);
	if (idx < 0) {
		pthread_mutex_unlock(&g_lock);
		AMDRNPU_TRACE_RET(buf_alloc_exit, (int)idx);
	}
	ret = reg_add(&handle, memory_type, (size_t)idx, npages,
		      amdrnpu_pool_addr(&c->pool[memory_type], (size_t)idx),
		      size);
	if (ret) {
		amdrnpu_pool_free(&c->pool[memory_type], (size_t)idx, npages);
		pthread_mutex_unlock(&g_lock);
		AMDRNPU_TRACE_RET(buf_alloc_exit, ret);
	}
	pthread_mutex_unlock(&g_lock);

	*out_handle = handle;
	AMDRNPU_TRACE_RET(buf_alloc_exit, 0);
}

int amdrnpu_buf_close(int fd, uint32_t handle)
{
	struct amdrnpu_ctx *c;
	struct amdrnpu_buf_entry e;
	int ret, found;

	(void)fd;
	AMDRNPU_TRACE1(buf_close_enter, handle);
	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		AMDRNPU_TRACE_RET(buf_close_exit, ret);

	pthread_mutex_lock(&g_lock);
	found = reg_remove(handle, &e);
	if (found)
		amdrnpu_pool_free(&c->pool[e.pool_id], e.page_index, e.npages);
	pthread_mutex_unlock(&g_lock);

	AMDRNPU_TRACE_RET(buf_close_exit, found ? 0 : -ENOENT);
}

int amdrnpu_buf_get_phys(int fd, uint32_t handle, uint64_t *out_pa,
			 uint64_t *out_size)
{
	struct amdrnpu_ctx *c;
	struct amdrnpu_buf_entry e;
	int ret, found;

	(void)fd;
	if (!out_pa)
		return -EINVAL;
	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		return ret;

	pthread_mutex_lock(&g_lock);
	found = reg_lookup(handle, &e);
	pthread_mutex_unlock(&g_lock);
	if (!found)
		return -ENOENT;

	*out_pa = e.phys;
	if (out_size)
		*out_size = e.size;
	return 0;
}

int amdrnpu_buf_mmap(int fd, uint32_t handle, uint64_t buf_size,
		     uint64_t offset, uint64_t length, void **out_map)
{
	struct amdrnpu_ctx *c;
	struct amdrnpu_buf_entry e;
	size_t maplen;
	void *map;
	int ret, found;

	(void)buf_size; /* reserved: size is tracked internally */
	(void)fd;

	AMDRNPU_TRACE1(buf_mmap_enter, handle);
	if (!out_map)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);
	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		AMDRNPU_TRACE_RET(buf_mmap_exit, ret);

	pthread_mutex_lock(&g_lock);
	found = reg_lookup(handle, &e);
	pthread_mutex_unlock(&g_lock);
	if (!found)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -ENOENT);
	if (offset > e.size)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	maplen = length ? (size_t)length : (size_t)(e.size - offset);
	if (maplen == 0 || offset + maplen > e.size)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	/* Pool bases are page-aligned; the API requires a page-aligned offset,
	 * so phys + offset is a valid /dev/mem mmap offset.
	 */
	map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED,
		   c->mem_fd, (off_t)(e.phys + offset));
	if (map == MAP_FAILED)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -errno);

	*out_map = map;
	AMDRNPU_TRACE_RET(buf_mmap_exit, 0);
}

void amdrnpu_buf_munmap(void *addr, uint64_t length)
{
	AMDRNPU_TRACE0(buf_munmap_enter);
	if (!addr || !length) {
		AMDRNPU_TRACE0(buf_munmap_exit);
		return;
	}
	(void)munmap(addr, (size_t)length);
	AMDRNPU_TRACE0(buf_munmap_exit);
}

/*
 * Map [@offset, @offset + @length).  mmap(2) requires a page-aligned offset,
 * so the mapped base may precede @offset; @out_ptr points at @offset.
 */
static int amdrnpu_buf_map_section(int fd, uint32_t handle, uint64_t offset,
				   uint64_t length, void **out_base,
				   uint64_t *out_map_len, void **out_ptr)
{
	uint64_t page_off, map_off, map_len;
	long page;
	void *map;
	int ret;

	if (length == 0 || length > SIZE_MAX || offset > UINT64_MAX - length)
		return -EINVAL;

	page = sysconf(_SC_PAGESIZE);
	if (page <= 0)
		return -EINVAL;

	page_off = offset % (uint64_t)page;
	map_off = offset - page_off;
	map_len = page_off + length;
	if (map_len > SIZE_MAX)
		return -EINVAL;

	ret = amdrnpu_buf_mmap(fd, handle, 0, map_off, map_len, &map);
	if (ret)
		return ret;

	*out_base = map;
	*out_map_len = map_len;
	*out_ptr = (unsigned char *)map + page_off;
	return 0;
}

int amdrnpu_buf_write(int fd, uint32_t handle, const void *src,
		      uint64_t offset, uint64_t length)
{
	uint64_t map_len;
	void *map, *dst;
	int ret;

	if (length == 0)
		return 0;
	if (!src)
		return -EINVAL;

	ret = amdrnpu_buf_map_section(fd, handle, offset, length,
				      &map, &map_len, &dst);
	if (ret)
		return ret;

	amdrnpu_memcpy_todev(dst, src, (size_t)length);
	amdrnpu_buf_munmap(map, map_len);
	return 0;
}

/*
 * Device-memory-safe copies.
 *
 * Pool buffers are no-map reserved memory mapped through /dev/mem (O_SYNC).
 * On arm64 the kernel maps such non-RAM pfns as Device-nGnRnE memory, which
 * FAULTS (SIGBUS) on unaligned accesses — and glibc memcpy() freely uses
 * unaligned loads/stores.  Copy with naturally-aligned accesses on the
 * device-memory side instead (byte head/tail to reach 8-byte alignment, then
 * aligned 64-bit transfers), mirroring the kernel's memcpy_toio/fromio.  The
 * host-memory side may be freely unaligned (it is ordinary cacheable RAM).
 */
static void amdrnpu_memcpy_todev(volatile uint8_t *d, const uint8_t *s,
				 size_t n)
{
	while (n && ((uintptr_t)d & 7u)) {
		*d++ = *s++;
		n--;
	}
	while (n >= 8) {
		uint64_t v;

		memcpy(&v, s, sizeof(v));
		*(volatile uint64_t *)d = v;
		d += 8;
		s += 8;
		n -= 8;
	}
	while (n--)
		*d++ = *s++;
}

static void amdrnpu_memcpy_fromdev(uint8_t *d, const volatile uint8_t *s,
				   size_t n)
{
	while (n && ((uintptr_t)s & 7u)) {
		*d++ = *s++;
		n--;
	}
	while (n >= 8) {
		uint64_t v = *(const volatile uint64_t *)s;

		memcpy(d, &v, sizeof(v));
		d += 8;
		s += 8;
		n -= 8;
	}
	while (n--)
		*d++ = *s++;
}

int amdrnpu_buf_read(int fd, uint32_t handle, void *dst, uint64_t offset,
		     uint64_t length)
{
	uint64_t map_len;
	void *map, *src;
	int ret;

	if (length == 0)
		return 0;
	if (!dst)
		return -EINVAL;

	ret = amdrnpu_buf_map_section(fd, handle, offset, length,
				      &map, &map_len, &src);
	if (ret)
		return ret;

	amdrnpu_memcpy_fromdev(dst, src, (size_t)length);
	amdrnpu_buf_munmap(map, map_len);
	return 0;
}

/*
 * Pool memory is no-map and mapped uncached/coherent through /dev/mem, so there
 * is no CPU cache to clean or invalidate.  Validate the handle/direction and
 * return success.
 */
int amdrnpu_buf_sync(int fd, uint32_t handle, uint32_t direction,
		     uint64_t offset, uint64_t length)
{
	struct amdrnpu_ctx *c;
	struct amdrnpu_buf_entry e;
	int ret, found;

	(void)fd;
	(void)offset;
	(void)length;

	AMDRNPU_TRACE2(buf_sync_enter, handle, (int)direction);
	if (direction != AMDRNPU_BUF_SYNC_TO_DEVICE &&
	    direction != AMDRNPU_BUF_SYNC_FROM_DEVICE)
		AMDRNPU_TRACE_RET(buf_sync_exit, -EINVAL);

	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		AMDRNPU_TRACE_RET(buf_sync_exit, ret);

	pthread_mutex_lock(&g_lock);
	found = reg_lookup(handle, &e);
	pthread_mutex_unlock(&g_lock);

	AMDRNPU_TRACE_RET(buf_sync_exit, found ? 0 : -ENOENT);
}

int amdrnpu_cmd_submit(int fd, uint32_t command_op,
		       const struct amdrnpu_arg *args, uint32_t num_args,
		       struct amdrnpu_cmd_submit *hdr_out, size_t hdr_out_sz)
{
	size_t fix = offsetof(struct amdrnpu_cmd_submit, args);
	struct amdrnpu_ctx *c;
	struct amdrnpu_wire_cmd *msg;
	size_t msg_len;
	uint32_t seq;
	int32_t status = 0;
	int ret, evfd;

	(void)fd;
	AMDRNPU_TRACE2(cmd_submit_enter, command_op, num_args);
	if (!hdr_out || hdr_out_sz < fix)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -EINVAL);
	if (num_args && !args)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -EINVAL);
	if (num_args > AMDRNPU_LIB_MAX_ARGS)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -E2BIG);

	ret = amdrnpu_ctx_ensure(&c);
	if (ret)
		AMDRNPU_TRACE_RET(cmd_submit_exit, ret);

	msg_len = sizeof(*msg) + (size_t)num_args * sizeof(uint64_t);
	msg = calloc(1, msg_len);
	if (!msg)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -ENOMEM);

	pthread_mutex_lock(&g_lock);
	ret = amdrnpu_resolve_args(args, num_args, msg->args);
	if (ret) {
		pthread_mutex_unlock(&g_lock);
		free(msg);
		AMDRNPU_TRACE_RET(cmd_submit_exit, ret);
	}

	seq = c->next_seq++;
	msg->seq = htole32(seq);
	msg->command_op = htole32(command_op);
	msg->flags = 0;
	msg->num_args = htole32(num_args);

	ret = amdrnpu_send_and_wait(c, msg, msg_len, seq, &status);
	pthread_mutex_unlock(&g_lock);
	free(msg);
	if (ret)
		AMDRNPU_TRACE_RET(cmd_submit_exit, ret);

	/* Represent completion as an already-signaled, pollable eventfd. */
	evfd = eventfd(1, EFD_CLOEXEC);
	if (evfd < 0)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -errno);
	ret = cmpl_store(evfd, status);
	if (ret) {
		close(evfd);
		AMDRNPU_TRACE_RET(cmd_submit_exit, ret);
	}

	hdr_out->sync_handle = evfd;
	hdr_out->command_op = command_op;
	hdr_out->num_args = num_args;
	AMDRNPU_TRACE_RET(cmd_submit_exit, 0);
}

int amdrnpu_syncobj_fence_status(int fd, uint32_t syncobj_handle,
				 int32_t *out_fence_status)
{
	int32_t status;

	(void)fd;
	AMDRNPU_TRACE1(fence_status_enter, (int)syncobj_handle);
	if (!out_fence_status)
		AMDRNPU_TRACE_RET(fence_status_exit, -EINVAL);
	if (!cmpl_lookup((int)syncobj_handle, &status))
		AMDRNPU_TRACE_RET(fence_status_exit, -EINVAL);

	/* Synchronous model: always complete. >0 success, <0 fence errno. */
	*out_fence_status = status ? status : 1;
	AMDRNPU_TRACE_RET(fence_status_exit, 0);
}

int amdrnpu_syncobj_wait_complete(int fd, uint32_t syncobj_handle,
				  int64_t timeout_ns, int32_t *out_wait_status)
{
	int32_t status;

	(void)fd;
	AMDRNPU_TRACE2(wait_complete_enter, (int)syncobj_handle, (int)timeout_ns);
	if (!out_wait_status || timeout_ns < 0)
		AMDRNPU_TRACE_RET(wait_complete_exit, -EINVAL);
	if (!cmpl_lookup((int)syncobj_handle, &status))
		AMDRNPU_TRACE_RET(wait_complete_exit, -EINVAL);

	/* 0 not signalled, 1 completed OK, or a negative fence errno. */
	*out_wait_status = status ? status : 1;
	AMDRNPU_TRACE_RET(wait_complete_exit, 0);
}

int amdrnpu_syncobj_wait_busy_poll(int fd, uint32_t syncobj_handle,
				   int64_t max_wall_ns, uint64_t interval_ns,
				   int32_t *out_fence_status)
{
	(void)interval_ns; /* reserved: completion is already synchronous */

	AMDRNPU_TRACE2(busy_poll_enter, (int)syncobj_handle, (int)interval_ns);
	AMDRNPU_TRACE_RET(busy_poll_exit,
			  amdrnpu_syncobj_wait_complete(fd, syncobj_handle,
							max_wall_ns,
							out_fence_status));
}

int amdrnpu_syncobj_to_fence_fd(int fd, uint32_t syncobj_handle,
				int *out_fence_fd)
{
	int nf;

	(void)fd;
	AMDRNPU_TRACE1(to_fence_fd_enter, (int)syncobj_handle);
	if (!out_fence_fd)
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -EINVAL);

	/* The completion handle is an already-signaled eventfd; a dup is a
	 * poll(2)-able fence fd that is immediately readable.
	 */
	nf = fcntl((int)syncobj_handle, F_DUPFD_CLOEXEC, 0);
	if (nf < 0)
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -errno);

	*out_fence_fd = nf;
	AMDRNPU_TRACE_RET(to_fence_fd_exit, 0);
}
