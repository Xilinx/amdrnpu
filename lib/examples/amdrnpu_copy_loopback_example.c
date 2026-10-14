/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_copy_loopback_example - end-to-end ifm -> ofm copy over the userspace
 * amdrnpu stack, with a built-in "response" server standing in for the RPU.
 *
 * Two threads share the configured rpmsg shared memory and device pools:
 *
 *   CONTROL side (control_app, uses only the public amdrnpu API):
 *     1. amdrnpu_buf_alloc() two DEVICE buffers (ifm, ofm) from the pool
 *     2. posix_memalign() aligned host input/output buffers
 *     3. amdrnpu_buf_write() the input pattern into ifm
 *     4. amdrnpu_cmd_submit() a copy command (goes over rpmsg_lite)
 *     5. amdrnpu_syncobj_wait_complete() for completion (polls the vring)
 *     6. amdrnpu_buf_read() ofm back and verify it matches the input
 *
 *   RESPONSE side (response_server, the vring slave / "RPU"):
 *     - polls the virtio status until DRIVER_OK, announces the "amdrnpu" channel
 *     - polls the leader's TX vring for the command
 *     - copies ifm -> ofm in shared physical memory
 *     - posts an rpmsg response back to the control side
 *
 * Both sides are polling-only (no interrupts): the control side polls through
 * the library, the response side polls the status byte and the rings directly.
 *
 * Running it:
 *
 *   On the board (real no-map pools + /dev/mem, RPU firmware NOT required --
 *   the response thread is the loopback peer):
 *       amdrnpu_copy_loopback_example            # uses /etc/amdrnpu.ini
 *       AMDRNPU_INI=/path/to.ini amdrnpu_copy_loopback_example
 *   Needs permission to mmap /dev/mem (typically root).
 *
 *   Hardware-free self-test (no root, backing file instead of /dev/mem):
 *       amdrnpu_copy_loopback_example --self-test
 *   This synthesizes a small ini + backing file in /tmp, points $AMDRNPU_INI
 *   and $AMDRNPU_DEVMEM at them, and runs the same flow.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

#include "amdrnpu_ini.h"	/* struct amdrnpu_config, amdrnpu_config_load() */
#include "amdrnpu_wire.h"	/* amdrnpu_wire_cmd / amdrnpu_wire_rsp */
#include "rpmsg_lite.h"		/* rpmsg_hdr, rpmsg_ns_msg, NS + status consts */
#include "vring_lite.h"		/* vring_desc / vring_avail / vring_used */

#define CHECK(expr)                                                            \
	do {                                                                   \
		if (!(expr)) {                                                  \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #expr);                              \
			exit(EXIT_FAILURE);                                    \
		}                                                              \
	} while (0)

#define DEV_ADDR	100u		/* the response server's rpmsg address */
#define COPY_OP		0x00000001u	/* opcode (ignored by the loopback peer) */
#define XFER_LEN	2048u		/* bytes copied ifm -> ofm */

static void nap(void)
{
	struct timespec ts = { 0, 50000 }; /* 50 us */

	nanosleep(&ts, NULL);
}

/* ================================================================== */
/* RESPONSE side: vring slave ("RPU" loopback)                         */
/* ================================================================== */

struct response_server {
	int mem_fd;			/* /dev/mem or $AMDRNPU_DEVMEM file */
	uint8_t *shm;			/* mapped [shm_phys, shm_phys+shm_size) */
	struct amdrnpu_config cfg;
	long page;
	uint16_t tx_cursor;		/* read cursor over leader TX avail ring */
	uint16_t rx_cursor;		/* read cursor over leader RX avail ring */
};

/* vring sub-structure offsets, matching vring_lite's internal layout. */
static size_t vq_used_off(const struct amdrnpu_config *cfg)
{
	uint32_t num = cfg->rpmsg.vring_num, align = cfg->rpmsg.vring_align;
	size_t end = 16u * num + 6u + 2u * num;

	return (end + align - 1u) & ~((size_t)align - 1u);
}

static volatile struct vring_desc *vq_desc(struct response_server *s, size_t off)
{
	return (volatile struct vring_desc *)(s->shm + off);
}

static volatile struct vring_avail *vq_avail(struct response_server *s, size_t off)
{
	return (volatile struct vring_avail *)(s->shm + off +
					       16u * s->cfg.rpmsg.vring_num);
}

static volatile struct vring_used *vq_used(struct response_server *s, size_t off)
{
	return (volatile struct vring_used *)(s->shm + off + vq_used_off(&s->cfg));
}

/* Translate a device/physical buffer address into the shm mapping. */
static uint8_t *shm_ptr(struct response_server *s, uint64_t dev_addr)
{
	return s->shm + (size_t)(dev_addr - s->cfg.rpmsg.shm_phys);
}

/* mmap a physical range (e.g. a pool buffer) for the copy; page-aligned. */
static uint8_t *map_phys(struct response_server *s, uint64_t phys, uint64_t len,
			 void **map_base, size_t *map_len)
{
	uint64_t pageoff = phys % (uint64_t)s->page;
	uint64_t base = phys - pageoff;
	size_t mlen = (size_t)(pageoff + len);
	void *m;

	m = mmap(NULL, mlen, PROT_READ | PROT_WRITE, MAP_SHARED, s->mem_fd,
		 (off_t)base);
	if (m == MAP_FAILED)
		return NULL;
	*map_base = m;
	*map_len = mlen;
	return (uint8_t *)m + pageoff;
}

/* Device -> leader: fill a leader-posted RX buffer and complete it. */
static void resp_send(struct response_server *s, uint32_t src, uint32_t dst,
		      const void *data, uint16_t len)
{
	size_t off = s->cfg.rpmsg.vring_rx_offset;
	volatile struct vring_avail *av = vq_avail(s, off);
	volatile struct vring_desc *de = vq_desc(s, off);
	volatile struct vring_used *us = vq_used(s, off);
	uint32_t num = s->cfg.rpmsg.vring_num;
	struct rpmsg_hdr hdr = { src, dst, 0, len, 0 };
	unsigned int spins = 0;
	uint16_t id, uidx;
	uint8_t *buf;

	while ((uint16_t)(av->idx - s->rx_cursor) == 0) {
		CHECK(++spins < 2000000u);
		nap();
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	id = av->ring[s->rx_cursor++ & (num - 1u)];
	CHECK(id < num);
	buf = shm_ptr(s, de[id].addr);
	memcpy(buf, &hdr, sizeof(hdr));
	if (data && len)
		memcpy(buf + sizeof(hdr), data, len);
	uidx = us->idx;
	us->ring[uidx & (num - 1u)].id = id;
	us->ring[uidx & (num - 1u)].len = sizeof(hdr) + len;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	us->idx = (uint16_t)(uidx + 1u);
}

/* Leader -> device: consume one TX message if present. Returns 1 if consumed. */
static int resp_recv(struct response_server *s, struct rpmsg_hdr *hdr_out,
		     uint8_t *payload, size_t payload_cap, size_t *plen)
{
	size_t off = s->cfg.rpmsg.vring_tx_offset;
	volatile struct vring_avail *av = vq_avail(s, off);
	volatile struct vring_desc *de = vq_desc(s, off);
	volatile struct vring_used *us = vq_used(s, off);
	uint32_t num = s->cfg.rpmsg.vring_num;
	uint16_t id, uidx;
	uint32_t dlen;
	uint8_t *buf;
	size_t n;

	if ((uint16_t)(av->idx - s->tx_cursor) == 0)
		return 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	id = av->ring[s->tx_cursor++ & (num - 1u)];
	CHECK(id < num);
	dlen = de[id].len;
	buf = shm_ptr(s, de[id].addr);
	memcpy(hdr_out, buf, sizeof(*hdr_out));
	n = hdr_out->len;
	if (n > dlen - sizeof(*hdr_out))
		n = dlen - sizeof(*hdr_out);
	if (n > payload_cap)
		n = payload_cap;
	memcpy(payload, buf + sizeof(*hdr_out), n);
	*plen = n;
	uidx = us->idx;
	us->ring[uidx & (num - 1u)].id = id;
	us->ring[uidx & (num - 1u)].len = dlen;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	us->idx = (uint16_t)(uidx + 1u);
	return 1;
}

/* Perform the firmware's job: copy @len bytes from ifm phys to ofm phys. */
static void resp_do_copy(struct response_server *s, uint64_t ifm, uint64_t ofm,
			 uint64_t len)
{
	void *sbase, *dbase;
	size_t slen, dlen;
	uint8_t *src, *dst;

	src = map_phys(s, ifm, len, &sbase, &slen);
	CHECK(src != NULL);
	dst = map_phys(s, ofm, len, &dbase, &dlen);
	CHECK(dst != NULL);
	memcpy(dst, src, (size_t)len);
	munmap(sbase, slen);
	munmap(dbase, dlen);
}

static void *response_server(void *arg)
{
	struct response_server *s = arg;
	size_t status_off = (size_t)s->cfg.rpmsg.status_offset;
	struct rpmsg_ns_msg ns;
	unsigned int spins;
	int done = 0;

	/* Poll the virtio state until the leader publishes DRIVER_OK. */
	for (spins = 0; !(s->shm[status_off] & VIRTIO_CONFIG_STATUS_DRIVER_OK);
	     spins++) {
		CHECK(spins < 2000000u);
		nap();
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Announce the "amdrnpu" channel so the control-side endpoint binds. */
	memset(&ns, 0, sizeof(ns));
	snprintf(ns.name, sizeof(ns.name), "%s", AMDRNPU_WIRE_CHANNEL);
	ns.addr = DEV_ADDR;
	ns.flags = RPMSG_NS_CREATE;
	resp_send(s, DEV_ADDR, RPMSG_NS_ADDR, &ns, sizeof(ns));

	for (spins = 0; !done && spins < 2000000u; spins++) {
		_Alignas(8) uint8_t pl[512];
		struct rpmsg_hdr hdr;
		size_t plen;

		if (!resp_recv(s, &hdr, pl, sizeof(pl), &plen)) {
			nap();
			continue;
		}
		/* Ignore the control side's own NS announce; act on a command. */
		if (hdr.dst != DEV_ADDR ||
		    plen < sizeof(struct amdrnpu_wire_cmd))
			continue;

		struct amdrnpu_wire_cmd cmd;
		uint64_t a[3];
		struct amdrnpu_wire_rsp rsp;

		memcpy(&cmd, pl, sizeof(cmd));
		CHECK(le32toh(cmd.num_args) == 3);
		CHECK(plen >= sizeof(cmd) + sizeof(a));
		memcpy(a, pl + sizeof(cmd), sizeof(a));

		resp_do_copy(s, le64toh(a[0]), le64toh(a[1]), le64toh(a[2]));

		rsp.seq = cmd.seq; /* echo the wire seq (already little-endian) */
		rsp.command_rsp_type = htole32(AMDRNPU_WIRE_RSP_COMPLETE);
		resp_send(s, DEV_ADDR, hdr.src, &rsp, sizeof(rsp));
		done = 1;
	}

	CHECK(done);
	return NULL;
}

static int response_server_init(struct response_server *s)
{
	const char *mem = getenv("AMDRNPU_DEVMEM");
	int ret;

	memset(s, 0, sizeof(*s));
	s->page = sysconf(_SC_PAGESIZE);
	CHECK(s->page > 0);

	ret = amdrnpu_config_load(&s->cfg, NULL);
	if (ret) {
		fprintf(stderr, "config load: %s\n", strerror(-ret));
		return ret;
	}

	if (mem && *mem)
		s->mem_fd = open(mem, O_RDWR | O_CLOEXEC);
	else
		s->mem_fd = open("/dev/mem", O_RDWR | O_CLOEXEC | O_SYNC);
	if (s->mem_fd < 0) {
		perror("open devmem (response)");
		return -errno;
	}

	s->shm = mmap(NULL, s->cfg.rpmsg.shm_size, PROT_READ | PROT_WRITE,
		      MAP_SHARED, s->mem_fd, (off_t)s->cfg.rpmsg.shm_phys);
	if (s->shm == MAP_FAILED) {
		perror("mmap shm (response)");
		close(s->mem_fd);
		return -errno;
	}
	return 0;
}

static void response_server_fini(struct response_server *s)
{
	if (s->shm && s->shm != MAP_FAILED)
		munmap(s->shm, s->cfg.rpmsg.shm_size);
	if (s->mem_fd >= 0)
		close(s->mem_fd);
}

/* ================================================================== */
/* CONTROL side: public amdrnpu API only                               */
/* ================================================================== */

static int control_app(int fd)
{
	struct amdrnpu_cmd_submit hdr;
	struct amdrnpu_arg args[3];
	uint32_t ifm = 0, ofm = 0;
	uint64_t pa = 0, sz = 0;
	int32_t wait_status = 0;
	uint8_t *in, *out;
	unsigned int i;
	int rc;

	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, XFER_LEN, &ifm);
	if (rc) { fprintf(stderr, "buf_alloc ifm: %s\n", strerror(-rc)); return rc; }
	rc = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, XFER_LEN, &ofm);
	if (rc) { fprintf(stderr, "buf_alloc ofm: %s\n", strerror(-rc)); return rc; }

	CHECK(posix_memalign((void **)&in, 4096, XFER_LEN) == 0);
	CHECK(posix_memalign((void **)&out, 4096, XFER_LEN) == 0);
	for (i = 0; i < XFER_LEN; i++)
		in[i] = (uint8_t)(i * 7u + 1u);
	memset(out, 0, XFER_LEN);

	rc = amdrnpu_buf_write(fd, ifm, in, 0, XFER_LEN);
	CHECK(rc == 0);

	memset(args, 0, sizeof(args));
	args[0].size = XFER_LEN;			/* buffer arg -> ifm phys */
	args[0].bo.buffer_handle = (int32_t)ifm;
	args[1].size = XFER_LEN;			/* buffer arg -> ofm phys */
	args[1].bo.buffer_handle = (int32_t)ofm;
	args[2].size = 0;				/* scalar: copy length */
	args[2].value = XFER_LEN;

	rc = amdrnpu_cmd_submit(fd, COPY_OP, args, 3, &hdr, sizeof(hdr));
	CHECK(rc == 0);
	printf("control: submitted copy, sync_handle=%d\n", hdr.sync_handle);

	rc = amdrnpu_syncobj_wait_complete(fd, (uint32_t)hdr.sync_handle,
					   INT64_C(5000000000), &wait_status);
	CHECK(rc == 0);
	CHECK(wait_status == 1); /* completed without error */

	rc = amdrnpu_buf_read(fd, ofm, out, 0, XFER_LEN);
	CHECK(rc == 0);
	CHECK(memcmp(in, out, XFER_LEN) == 0);

	CHECK(amdrnpu_buf_get_phys(fd, ifm, &pa, &sz) == 0);
	printf("control: ifm pa=0x%llx size=%llu; verify OK: output matches input\n",
	       (unsigned long long)pa, (unsigned long long)sz);

	close(hdr.sync_handle);
	amdrnpu_buf_close(fd, ifm);
	amdrnpu_buf_close(fd, ofm);
	free(in);
	free(out);
	return 0;
}

/* ================================================================== */
/* Self-test environment (hardware-free)                               */
/* ================================================================== */

/* Small file-offset "physical" layout used only by --self-test. */
#define ST_SHM_SIZE	0x10000u
#define ST_VRING_TX	0x1000u
#define ST_VRING_RX	0x2000u
#define ST_BUF_OFF	0x3000u
#define ST_VRING_NUM	8u
#define ST_VRING_ALIGN	16u
#define ST_BUF_SIZE	512u
#define ST_BUF_COUNT	8u
#define ST_POOL_DEV	0x10000ull
#define ST_POOL_DEV_SZ	0x40000ull
#define ST_POOL_RPU	0x50000ull
#define ST_POOL_RPU_SZ	0x10000ull
#define ST_FILE_SIZE	0x60000u

static void make_file(const char *tmpl, size_t size, char *out, size_t cap)
{
	int fd;

	snprintf(out, cap, "%s", tmpl);
	fd = mkstemp(out);
	CHECK(fd >= 0);
	CHECK(ftruncate(fd, (off_t)size) == 0);
	close(fd);
}

static void selftest_setup(char *ini_out, char *mem_out, size_t cap)
{
	FILE *ini;

	make_file("/tmp/amdrnpu_lb_mem_XXXXXX", ST_FILE_SIZE, mem_out, cap);
	make_file("/tmp/amdrnpu_lb_ini_XXXXXX", 0, ini_out, cap);

	ini = fopen(ini_out, "w");
	CHECK(ini != NULL);
	fprintf(ini,
		"[pool.device]\nbase = 0x%llx\nsize = 0x%llx\n"
		"[pool.rpu]\nbase = 0x%llx\nsize = 0x%llx\n"
		"[rpmsg]\n"
		"shm_phys = 0\nshm_size = 0x%x\nstatus_offset = 0\n"
		"vring_rx_offset = 0x%x\nvring_tx_offset = 0x%x\n"
		"vring_num = %u\nvring_align = %u\n"
		"buf_offset = 0x%x\nbuf_size = %u\nbuf_count = %u\n",
		(unsigned long long)ST_POOL_DEV, (unsigned long long)ST_POOL_DEV_SZ,
		(unsigned long long)ST_POOL_RPU, (unsigned long long)ST_POOL_RPU_SZ,
		ST_SHM_SIZE, ST_VRING_RX, ST_VRING_TX, ST_VRING_NUM,
		ST_VRING_ALIGN, ST_BUF_OFF, ST_BUF_SIZE, ST_BUF_COUNT);
	fclose(ini);

	CHECK(setenv("AMDRNPU_INI", ini_out, 1) == 0);
	CHECK(setenv("AMDRNPU_DEVMEM", mem_out, 1) == 0);
}

static const char *config_path(void)
{
	const char *p = getenv("AMDRNPU_INI");

	return (p && *p) ? p : "/etc/amdrnpu.ini";
}

int main(int argc, char **argv)
{
	char ini_path[64] = "", mem_path[64] = "";
	int self_test = 0;
	struct response_server srv;
	pthread_t tid;
	int fd, rc, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--self-test"))
			self_test = 1;
		else {
			fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
			return 2;
		}
	}
	if (getenv("AMDRNPU_SELFTEST"))
		self_test = 1;

	/* A hung transport should fail loudly, not stall. */
	alarm(60);

	if (self_test)
		selftest_setup(ini_path, mem_path, sizeof(ini_path));

	/* Start the response server (vring slave) first so it is ready to bind. */
	rc = response_server_init(&srv);
	if (rc)
		return 1;
	CHECK(pthread_create(&tid, NULL, response_server, &srv) == 0);

	/* The control app's fd is only an opaque key; open the config file. */
	fd = open(config_path(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(config_path());
		return 1;
	}

	rc = control_app(fd);
	CHECK(pthread_join(tid, NULL) == 0);
	close(fd);
	response_server_fini(&srv);

	if (self_test) {
		unlink(mem_path);
		unlink(ini_path);
	}
	if (rc)
		return 1;

	puts("amdrnpu copy loopback: PASS");
	return 0;
}
