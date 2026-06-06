/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Link A (APU<->RPURuntime) OpenAMP transport — RPURuntime SLAVE side.
 *
 *   APU (tessai-agent driver, kernel remoteproc framework) = VirtIO DRIVER.
 *   RPURuntime (this file, R52 cluster 0 core 1)            = VirtIO DEVICE.
 *
 * Mirrors rpu-fw/rpu_fw/app/src/transport/openamp_platform.c but uses the
 * link A reserved-memory carveouts (apu_rsctbl @ 0x0A000000,
 * apu_rpmsg_shmem @ 0x0A100000) and the link A IPI mailbox
 * (ipi_2_to_ipi_3) instead of the rpu-fw link B layout. Compiled when
 * CONFIG_TESSAI_APU_RPMSG_SLAVE=y. This is the only OpenAMP transport
 * in the rpu_app snapshot: the historical Link B master-side
 * transport (rpmsg_platform.c) plus the shmem alternative
 * (shmem_platform.c) were both dropped because nothing in the Link A
 * path dispatches Link B traffic to rpu-fw (see xrt/src/driver/
 * CMakeLists.txt header for the full Link-A-only rationale).
 *
 * Init sequence:
 *   1. tessai_metal_init_once()  (shared with link B master, no double-init)
 *   2. metal_io_init() for SHM (vrings + buffers) and rsctbl
 *   3. MBOX RX callback on the link A IPI
 *   4. rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, ...) using the
 *      compile-time resource table placed at apu_rsctbl by the standard
 *      CONFIG_OPENAMP_RSC_TABLE linker fragment (resource_table.ld).
 *   5. rproc_virtio_wait_remote_ready() — block until APU sets DRIVER_OK
 *      after kernel remoteproc parses our ELF's .resource_table section.
 *   6. rproc_virtio_init_vring() x2 (reading vring.da from the rsc table
 *      that the kernel just populated).
 *   7. rpmsg_init_vdev() with name-service callback that announces the
 *      "tessai-agent" endpoint (tessai_agent_endpoint.c registers a real
 *      handler in phase1_tessai_endpoint_port).
 *   8. Spawn RX task on a coop priority.
 *
 * Exposes apu_openamp_get_rpmsg_device() so tessai_agent_endpoint.c can
 * register its endpoint (same pattern as the upstream rpu-fw
 * openamp_platform_get_rpmsg_device()).
 *
 * Vring-index convention (subtle - keep in sync with apu_rpmsg_task):
 *
 *   resource_table.h labels vrings as
 *     VRING0 = "master to remote" (Linux -> RPU = slave RX direction)
 *     VRING1 = "remote to master" (RPU -> Linux = slave TX direction)
 *   but OpenAMP's rpmsg_init_vdev() for a DEVICE-role vdev assigns
 *     rdev->tx_vq = vrings_info[0]
 *     rdev->rx_vq = vrings_info[1]
 *   so the slave's RX queue lives at vring INDEX 1, not 0. Linux's
 *   "kicking vq index: 1" in dmesg (= Linux TX = slave RX) lines up
 *   with that: notifyid 1 arrives at the slave, and the matching
 *   vring is the one OpenAMP set up at index 1 as our rx_vq.
 *   apu_rpmsg_task therefore calls rproc_virtio_notified with
 *   RSC_NOTIFY_ID_ANY so it drains regardless of which vring the
 *   master kicked - see the long comment in the task itself.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>

#include <openamp/open_amp.h>
#include <metal/sys.h>
#include <metal/io.h>
#include <resource_table.h>     /* rsc_table_get / VDEV_ID / VRING*_ID */

#include "metal_init_once.h"

LOG_MODULE_REGISTER(apu_rpmsg_platform, CONFIG_LOG_DEFAULT_LEVEL);

/* Verbose/error printk wrappers (CONFIG_TESSAI_VERBOSE gates DBG). */
#ifndef CONFIG_TESSAI_VERBOSE
#define CONFIG_TESSAI_VERBOSE 0
#endif

#define TESSAI_DBG(fmt, ...)                           \
	do {                                           \
		if (CONFIG_TESSAI_VERBOSE)             \
			printk(fmt, ##__VA_ARGS__);    \
	} while (0)

#define TESSAI_ERR(fmt, ...) printk(fmt, ##__VA_ARGS__)

/* ---------------------------------------------------------------------------
 * DT bindings (link A)
 * --------------------------------------------------------------------------- */

#define SHM_NODE     DT_NODELABEL(apu_rpmsg_shmem)
#define RSC_NODE     DT_NODELABEL(apu_rsctbl)

/* Link A IPI mailbox.
 *
 * The Versal Gen 2 IPI TRIG / OBS / ISR registers are 32-bit bitmaps;
 * each peer IPI agent occupies one bit position, and `xlnx,ipi-id` in
 * the DT is literally that bit number (see Versal Gen 2 TRM, IPI
 * controller register map):
 *
 *   bit  field        xlnx,ipi-id  design-doc role
 *   ---  -----        -----------  ---------------------
 *   0    ASU          0            ASU
 *   1    PMC          1            PMC
 *   2    IPI0         2            (unused on Versal-NET)
 *   3    IPI1         3            RPU-FW   (logical IPI_1)
 *   4    IPI2         4            RPU-APP  (logical IPI_2)  <- us (R5-1)
 *   5    IPI3         5            APU      (logical IPI_3)
 *   6    IPI4         6            APU      (logical IPI_4)
 *   7    IPI5         7            APU      (logical IPI_5)  <- target for OpenAMP
 *
 * So when this slave calls mbox_send() on its `ipi_2_to_ipi_5` child,
 * the AMD Zephyr xlnx_ipi_mailbox driver writes (1 << 7) into R5-1's
 * own IPI2 TRIG register (the parent `ipi2` block at 0xEB350000).
 * Hardware then fires the IPI5 agent, which on the Linux side is
 * mailbox@eb380000 (xlnx,ipi-id=7, SPI 62). The kernel mailbox driver
 * reads its ISR, sees bit 4 set (= RPU-APP IPI_2 source), and routes
 * the notification to r52f@1's rproc-virtio. Same physical IPI5 agent
 * is shared with rpu-fw on r52f@0 (its bit-3 = RPU-FW IPI_1 source),
 * which matches the platform design doc:
 *   "In Linux, IPI is shared across mgmt mailbox response from RPU-FW
 *    [and] from each RPU-APP"
 *
 * Decoding the slave-side child labels under `ipi2` in the lopper-
 * emitted SoC DTS: the label suffix uses the design-doc logical APU
 * IPI_X numbering (NOT the Vivado xlnx,ipi-id), so
 *   ipi_2_to_ipi_3 -> APU logical IPI_3 -> xlnx,ipi-id=5  (bit 5)
 *   ipi_2_to_ipi_5 -> APU logical IPI_5 -> xlnx,ipi-id=7  (bit 7)  <-- THIS ONE
 *
 * Counterpart wiring on the Linux kernel-DT side: r52f@1.mboxes =
 * <0x17d 0x00 0x17d 0x01> (phandle 0x17d = /axi/mailbox@eb380000/
 * child@eb350000, target xlnx,ipi-id=4 = RPU-APP IPI_2 = us). See
 * apply_tessai_patch.py R52_CORE1_INJECT.
 */
#define MBOX_NODE    DT_NODELABEL(ipi_2_to_ipi_5)

/* DT_NODE_HAS_STATUS(N, okay) is the form available in Zephyr 3.7
 * (xlnx_rel_v2025.2). DT_NODE_HAS_STATUS_OKAY(N) is a Zephyr 4.x
 * shorthand that doesn't exist here.
 */
#if !DT_NODE_HAS_STATUS(SHM_NODE, okay)
#error "apu_rpmsg_platform: DTS must enable apu_rpmsg_shmem (see reserved.dtsi)"
#endif
#if !DT_NODE_HAS_STATUS(RSC_NODE, okay)
#error "apu_rpmsg_platform: DTS must enable apu_rsctbl (see reserved.dtsi)"
#endif
#if !DT_NODE_HAS_STATUS(MBOX_NODE, okay)
#error "apu_rpmsg_platform: DTS must enable ipi_2_to_ipi_5 (link A mailbox -> APU IPI_5)"
#endif

#define SHM_START_ADDR  DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE        DT_REG_SIZE(SHM_NODE)

#define MBOX_TX_CH      0U
#define MBOX_RX_CH      1U

/* tessai-agent endpoint advertised name. Kernel driver matches
 * rpmsg_device_id "tessai-agent" - see rpu-fw tessai_agent_endpoint.c.
 */
#define TESSAI_AGENT_EPT_NAME  "tessai-agent"

/* ---------------------------------------------------------------------------
 * Static state
 * --------------------------------------------------------------------------- */

static const struct device *const mbox_dev = DEVICE_DT_GET(MBOX_NODE);

static metal_phys_addr_t shm_physmap = SHM_START_ADDR;
static metal_phys_addr_t rsc_physmap;

static struct metal_io_region shm_io_data;
static struct metal_io_region rsc_io_data;

static struct metal_io_region *shm_io = &shm_io_data;
static struct metal_io_region *rsc_io = &rsc_io_data;

static struct rpmsg_virtio_device rvdev;
static struct rpmsg_device       *rpdev_slave;

static K_SEM_DEFINE(virtio_kick_sem, 0, 1);

static bool platform_initialized;

/* RX/TX debug counters bumped from mbox_callback (ISR), apu_rpmsg_task,
 * and mailbox_notify. Kept post bring-up because (a) the cost is one
 * unsynchronised increment per IRQ and (b) JTAG / future post-mortems
 * scrape them directly out of .bss to triage IPI delivery, RX task
 * scheduling, and TX kick wiring without rebuilding. Marked volatile
 * so the debug overlay sees current values without a memory barrier
 * dance, and so the compiler doesn't fold the increments out under -O2.
 * The verbose per-event LOG_INFs that came with these counters during
 * link-A bring-up have been downgraded to TESSAI_DBG (no spam in release).
 */
static volatile uint32_t mbox_rx_irq_count;
static volatile uint32_t mbox_rx_wake_count;
static volatile uint32_t mbox_tx_count;
static volatile uint32_t mbox_tx_ebusy_count;

/* ---------------------------------------------------------------------------
 * MBOX + virtio notify glue
 * --------------------------------------------------------------------------- */

static void mbox_callback(const struct device *dev, mbox_channel_id_t channel_id,
			  void *user_data, struct mbox_msg *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);
	ARG_UNUSED(data);

	mbox_rx_irq_count++;
	TESSAI_DBG("[apu_rpmsg] RX IRQ #%u (link A)\n", mbox_rx_irq_count);
	k_sem_give(&virtio_kick_sem);
}

static int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);

	struct mbox_msg msg = { .data = &id, .size = sizeof(id) };
	int ret = mbox_send(mbox_dev, MBOX_TX_CH, &msg);

	if (ret == 0) {
		mbox_tx_count++;
		TESSAI_DBG("[apu_rpmsg] TX kick #%u id=%u (link A)\n",
			mbox_tx_count, id);
		return 0;
	}
	if (ret == -EBUSY) {
		mbox_tx_ebusy_count++;
		return 0;
	}
	TESSAI_ERR("[apu_rpmsg] mbox_send failed: %d\n", ret);
	return ret;
}

/* Name-service callback — APU userspace creates the tessai-agent endpoint
 * via the kernel driver; we just log it. The real endpoint is registered
 * locally by tessai_agent_endpoint.c (see phase1_tessai_endpoint_port).
 */
static void ns_announce_cb(struct rpmsg_device *rdev, const char *name,
			   uint32_t src)
{
	ARG_UNUSED(rdev);
	TESSAI_DBG("[apu_rpmsg] NS announce: %s (src=%u)\n", name, src);
}

/* ---------------------------------------------------------------------------
 * Receive task
 * --------------------------------------------------------------------------- */

#define APU_RPMSG_TASK_STACK  4096
#define APU_RPMSG_TASK_PRIO   K_PRIO_COOP(7)

K_THREAD_STACK_DEFINE(apu_rpmsg_task_stack, APU_RPMSG_TASK_STACK);
static struct k_thread apu_rpmsg_task_data;

static void apu_rpmsg_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	TESSAI_DBG("[apu_rpmsg] RX task started\n");

	while (1) {
		k_sem_take(&virtio_kick_sem, K_FOREVER);
		mbox_rx_wake_count++;
		TESSAI_DBG("[apu_rpmsg] RX wake #%u (irq=%u tx=%u busy=%u)\n",
			mbox_rx_wake_count, mbox_rx_irq_count,
			mbox_tx_count, mbox_tx_ebusy_count);
		/* Tell OpenAMP "the master kicked us, walk every vring".
		 *
		 * Why RSC_NOTIFY_ID_ANY and NOT VRING0_ID:
		 *
		 *   rproc_virtio_notified(vdev, X) matches vrings whose
		 *   vring_info[i].notifyid == X. The notifyid in our
		 *   vring_info is whatever the Linux remoteproc framework
		 *   wrote into vring_rsc->notifyid during resource-table
		 *   parsing (via idr_alloc, typically 0 for vring 0 and
		 *   1 for vring 1). The compile-time VRING0_ID/VRING1_ID
		 *   constants from open-amp's resource_table.h are NOT
		 *   guaranteed to match; they're only used as fallback
		 *   seed values when the slave creates its own resource
		 *   table (link B / master role).
		 *
		 *   On top of that, for a DEVICE-role vdev, OpenAMP's
		 *   rpmsg_init_vdev() assigns:
		 *     rdev->tx_vq = vrings_info[0]  (slave -> master)
		 *     rdev->rx_vq = vrings_info[1]  (master -> slave)
		 *   So even ignoring the notifyid mapping question, the
		 *   slave's RX queue is at index 1, not 0. Linux kicks
		 *   "vq index 1" (its TX = our RX) which arrives with
		 *   notifyid=1 - NOT VRING0_ID=0.
		 *
		 *   Earlier this file passed VRING0_ID, which made the
		 *   IPI fire, mbox_callback run, RX task wake -- but
		 *   then rproc_virtio_notified silently walked only
		 *   vring 0 (slave TX, empty) and dropped every Linux
		 *   cmd_submit on the floor (fw_echo timed out at
		 *   syncobj_wait with -ETIMEDOUT). Using
		 *   RSC_NOTIFY_ID_ANY makes the slave walk both queues
		 *   unconditionally, which is the correct behaviour for
		 *   a single-IPI dual-vring link where there's no
		 *   per-vring notify channel anyway.
		 *
		 *   rpu-fw's openamp_platform.c uses VRING1_ID (=1)
		 *   which happens to be the right notifyid in practice
		 *   for the Linux idr_alloc convention. RSC_NOTIFY_ID_ANY
		 *   is robust against any future Linux change in how
		 *   notifyids get allocated.
		 */
		rproc_virtio_notified(rvdev.vdev, RSC_NOTIFY_ID_ANY);
	}
}

/* ---------------------------------------------------------------------------
 * Platform initialisation
 * --------------------------------------------------------------------------- */

static int apu_rpmsg_platform_init(void)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device *vdev;
	/* rsc_table_get() prototype is
	 *   void rsc_table_get(struct fw_resource_table **table_ptr, int *length);
	 * (zephyr/lib/open-amp/resource_table.h). Typing this as
	 * `struct fw_resource_table *` keeps the &rsc_table call
	 * type-safe and lets rsc_table_to_vdev/get_vring0/1 (declared
	 * in the same header under the
	 * `#if CONFIG_OPENAMP_RSC_TABLE_NUM_RPMSG_BUFF > 0` guard, which
	 * we satisfy via the driver Kconfig default) consume it
	 * directly.
	 */
	struct fw_resource_table *rsc_table;
	int rsc_size;
	int ret;

	if (platform_initialized) {
		return 0;
	}

	TESSAI_DBG("[apu_rpmsg] platform init start (link A slave)\n");

	/* 1. Shared libmetal init (idempotent with link B master). */
	ret = tessai_metal_init_once();
	if (ret) {
		TESSAI_ERR("[apu_rpmsg] metal_init_once failed: %d\n", ret);
		return ret;
	}

	/* 2. Metal IO regions: SHM (vrings + buffers) and resource table. */
	TESSAI_DBG("[apu_rpmsg] shm  IO: addr=0x%08x size=0x%x\n",
		(uint32_t)SHM_START_ADDR, (uint32_t)SHM_SIZE);
	metal_io_init(shm_io, (void *)SHM_START_ADDR, &shm_physmap,
		      SHM_SIZE, -1, 0, NULL);

	rsc_table_get(&rsc_table, &rsc_size);
	rsc_physmap = (uintptr_t)rsc_table;
	TESSAI_DBG("[apu_rpmsg] rsc IO: addr=0x%08x size=%d\n",
		(uint32_t)rsc_physmap, rsc_size);
	metal_io_init(rsc_io, rsc_table, &rsc_physmap, rsc_size, -1, 0, NULL);

	/* 3. MBOX RX channel for link A IPI. */
	if (!device_is_ready(mbox_dev)) {
		TESSAI_ERR("[apu_rpmsg] MBOX device not ready\n");
		return -ENODEV;
	}
	ret = mbox_register_callback(mbox_dev, MBOX_RX_CH, mbox_callback, NULL);
	if (ret < 0) {
		TESSAI_ERR("[apu_rpmsg] mbox_register_callback failed: %d\n", ret);
		return ret;
	}
	ret = mbox_set_enabled(mbox_dev, MBOX_RX_CH, true);
	if (ret < 0) {
		TESSAI_ERR("[apu_rpmsg] mbox_set_enabled failed: %d\n", ret);
		return ret;
	}
	TESSAI_DBG("[apu_rpmsg] MBOX RX enabled (link A)\n");

	/* 4. Create DEVICE-side virtio. Resource table is what the standard
	 *    Zephyr CONFIG_OPENAMP_RSC_TABLE machinery placed at apu_rsctbl
	 *    via its resource_table.ld fragment; rsc_table_get() returned the
	 *    same pointer above.
	 */
	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE, VDEV_ID,
					rsc_table_to_vdev(rsc_table),
					rsc_io, NULL, mailbox_notify, NULL);
	if (!vdev) {
		TESSAI_ERR("[apu_rpmsg] rproc_virtio_create_vdev failed\n");
		return -ENOMEM;
	}

	/* 5. Block until the APU kernel-side remoteproc has parsed our
	 *    resource table and set vdev.status = VIRTIO_CONFIG_STATUS_DRIVER_OK.
	 *    Same pattern as rpu-fw openamp_platform.c.
	 */
	TESSAI_DBG("[apu_rpmsg] waiting for APU DRIVER_OK...\n");
	rproc_virtio_wait_remote_ready(vdev);
	TESSAI_DBG("[apu_rpmsg] APU DRIVER_OK observed\n");

	/* 6. Init vrings from the (now-populated) resource table. */
	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 0, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, shm_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		TESSAI_ERR("[apu_rpmsg] init_vring 0 failed: %d\n", ret);
		goto fail;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret = rproc_virtio_init_vring(vdev, 1, vring_rsc->notifyid,
				      (void *)(uintptr_t)vring_rsc->da, shm_io,
				      vring_rsc->num, vring_rsc->align);
	if (ret) {
		TESSAI_ERR("[apu_rpmsg] init_vring 1 failed: %d\n", ret);
		goto fail;
	}

	/* 7. rpmsg vdev init. shm_pool=NULL because the DRIVER side
	 *    (APU) owns the receive-buffer pool in a slave-mode link.
	 */
	ret = rpmsg_init_vdev(&rvdev, vdev, ns_announce_cb, shm_io, NULL);
	if (ret) {
		TESSAI_ERR("[apu_rpmsg] rpmsg_init_vdev failed: %d\n", ret);
		goto fail;
	}

	rpdev_slave = rpmsg_virtio_get_rpmsg_device(&rvdev);
	TESSAI_DBG("[apu_rpmsg] rpmsg slave ready (rdev=%p)\n", rpdev_slave);

	/* 8. RX task. */
	k_thread_create(&apu_rpmsg_task_data, apu_rpmsg_task_stack,
			K_THREAD_STACK_SIZEOF(apu_rpmsg_task_stack),
			apu_rpmsg_task, NULL, NULL, NULL,
			APU_RPMSG_TASK_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&apu_rpmsg_task_data, "apu_rpmsg_rx");

	platform_initialized = true;
	TESSAI_DBG("[apu_rpmsg] link A initialised\n");
	return 0;

fail:
	rproc_virtio_remove_vdev(vdev);
	return ret;
}

/* ---------------------------------------------------------------------------
 * Public accessors
 * --------------------------------------------------------------------------- */

struct rpmsg_device *apu_openamp_get_rpmsg_device(void)
{
	return rpdev_slave;
}

bool apu_openamp_platform_is_initialized(void)
{
	return platform_initialized;
}

const char *apu_rpmsg_tessai_ept_name(void)
{
	return TESSAI_AGENT_EPT_NAME;
}

/* ---------------------------------------------------------------------------
 * Zephyr sys_init — APPLICATION priority, mirrors rpmsg_platform.c
 * --------------------------------------------------------------------------- */

static int apu_rpmsg_platform_sys_init(void)
{
	int ret = apu_rpmsg_platform_init();

	if (ret < 0) {
		LOG_ERR("[apu_rpmsg] platform init failed: %d - link A disabled\n",
			ret);
		/* Non-fatal: the rest of the driver keeps running so the user
		 * sees a useful error rather than a boot hang. */
		return 0;
	}
	return 0;
}

SYS_INIT(apu_rpmsg_platform_sys_init, APPLICATION,
	 CONFIG_APPLICATION_INIT_PRIORITY);
