/* SPDX-License-Identifier: MIT */
#ifndef RPMSG_LITE_H
#define RPMSG_LITE_H

#include "vring_lite.h"

#define RPMSG_ADDR_ANY UINT32_MAX
#define RPMSG_NS_ADDR 53u
#define RPMSG_NS_CREATE 0u
#define RPMSG_NS_DESTROY 1u
#define VIRTIO_RPMSG_F_NS 0u /* Feature bit number, not a feature mask. */
#define VIRTIO_CONFIG_STATUS_DRIVER_OK 0x04u
#define RPMSG_LITE_MAX_EPTS 16u
#define RPMSG_LITE_NAME_SIZE 32u

struct rpmsg_hdr {
	uint32_t src;
	uint32_t dst;
	uint32_t reserved;
	uint16_t len;
	uint16_t flags;
} __attribute__((packed));

struct rpmsg_ns_msg {
	char name[RPMSG_LITE_NAME_SIZE];
	uint32_t addr;
	uint32_t flags;
} __attribute__((packed));

struct rpmsg_lite_cfg {
	void *shm_base;
	uint64_t shm_phys;
	size_t status_offset;
	/* OpenAMP driver vq[1] (svq): Zephyr VRING_RX_ADDRESS. */
	size_t vring_tx_offset;
	/* OpenAMP driver vq[0] (rvq): Zephyr VRING_TX_ADDRESS. */
	size_t vring_rx_offset;
	uint32_t vring_num;
	uint32_t vring_align;
	size_t buf_offset;
	uint32_t buf_size; /* 0 selects 512 bytes, including the 16-byte header. */
	uint32_t buf_count; /* Even, nonzero; half RX, half TX, each <= vring_num. */
	void (*notify)(void *priv, int vq_id); /* RX=0, TX=1; NULL for polling. */
	void *priv;
	/* Optional mapping size for bounds checking; 0 means caller guarantees
	 * that every configured region is mapped. Offsets must not overlap.
	 */
	size_t shm_size;
};

struct rpmsg_lite;
struct rpmsg_lite_ept;
/* Payload is borrowed until callback returns; copy it to retain it.
 * Calls to send/sendto are allowed here, but poll/init/deinit and endpoint
 * table modifications are not reentrant. All API calls must be serialized.
 */
typedef void (*rpmsg_lite_rx_cb)(struct rpmsg_lite_ept *ept,
				const void *data, size_t len, void *priv);

struct rpmsg_lite_ept {
	char name[RPMSG_LITE_NAME_SIZE];
	uint32_t local_addr;
	uint32_t dest_addr; /* RPMSG_ADDR_ANY until a matching NS CREATE arrives. */
	rpmsg_lite_rx_cb cb;
	void *priv;
	struct rpmsg_lite *ctx;
};

struct rpmsg_lite {
	struct rpmsg_lite_cfg cfg;
	struct vring_lite tx;
	struct vring_lite rx;
	struct rpmsg_lite_ept *epts[RPMSG_LITE_MAX_EPTS];
	uint16_t tx_free[VRING_LITE_MAX_NUM];
	uint16_t tx_free_count;
	uint16_t half;
	uint8_t initialized;
};

/* No allocation, interrupts, cache maintenance, or feature negotiation.
 * Caller provides little-endian, cache-coherent (or uncached) shared memory,
 * matching remote ring sizes/alignment and fixed buffer size. NS must be
 * enabled on the remote. Stop/reset the remote and clear its virtio status
 * before init; stop it again before deinit or reinitializing the rings.
 * Keep ctx, cfg's mapping and registered endpoint objects alive until deinit.
 * First half of pool is RX; second half is TX. Descriptor addresses are
 * shm_phys + pool offset, never host virtual pointers.
 */
int rpmsg_lite_init(struct rpmsg_lite *ctx, const struct rpmsg_lite_cfg *cfg);
int rpmsg_lite_create_ept(struct rpmsg_lite *ctx, struct rpmsg_lite_ept *ept,
			  const char *name, uint32_t local_addr,
			  rpmsg_lite_rx_cb cb, void *priv);
int rpmsg_lite_destroy_ept(struct rpmsg_lite *ctx, struct rpmsg_lite_ept *ept);
int rpmsg_lite_ns_announce(struct rpmsg_lite *ctx,
			   struct rpmsg_lite_ept *ept);
int rpmsg_lite_send(struct rpmsg_lite *ctx, struct rpmsg_lite_ept *ept,
		    const void *data, size_t len);
int rpmsg_lite_sendto(struct rpmsg_lite *ctx, struct rpmsg_lite_ept *ept,
		      const void *data, size_t len, uint32_t dst);
/* Poll consumes all RX completions (including NS) and reclaims TX.
 * Returns messages processed, or negative errno on malformed ring/message.
 * Rejected RX messages are recycled; callbacks already invoked stay invoked.
 */
int rpmsg_lite_poll(struct rpmsg_lite *ctx);
void rpmsg_lite_deinit(struct rpmsg_lite *ctx);

#endif
