# Polling userspace rpmsg leader

`vring_lite` implements single-buffer legacy split virtqueues;
`rpmsg_lite` is the virtio **driver/master**, without OpenAMP, libmetal,
kernel rpmsg, allocation, threads, or interrupt handling. Both are included
in the existing shared/static amdrnpu libraries, or can be compiled directly
as two C11 sources. Installed headers are `amdrnpu/rpmsg_lite.h` and
`amdrnpu/vring_lite.h`.

## Shared memory contract

Supply an already mapped, little-endian, **cache-coherent or uncached** region.
Memory fences order accesses but do not clean/invalidate caches. This library
does not map physical memory or negotiate features. The caller must arrange
matching buffer size, descriptor count, alignment, NS support, and remote
device role. Both sides must poll their queues if `notify` is NULL; host-side
polling does not make an interrupt-driven remote poll.
In particular, the stock [Zephyr receive path][zephyr-rx] runs from IPM
notification work, not a periodic poll. It needs a remote-side polling change
for mailbox-free operation, or a caller-supplied notify hook for its mailbox.

Stop/reset the peer and clear its status before initialization. Do not zero or
reinitialize live rings. Initialization clears both rings, posts the first half
of the buffer pool to RX, reserves the second half for TX, and publishes status
`ACKNOWLEDGE | DRIVER | DRIVER_OK` (`0x07`). Stop the peer before deinit; writing
status zero is not a synchronized shutdown handshake.

For the [specified Zephyr backend][zephyr] and its [pinned OpenAMP][openamp]:

| Leader queue | OpenAMP driver index | Zephyr device-side address |
| --- | --- | --- |
| RX (`vring_rx_offset`) | 0, `rvq` | `VRING_TX_ADDRESS` |
| TX (`vring_tx_offset`) | 1, `svq` | `VRING_RX_ADDRESS` |

Offsets are relative to `shm_base`, while descriptor addresses use
`shm_phys + buf_offset + buffer_index * buf_size`. Supply the actual shared
status-byte offset (resource-table status when applicable), not an assumed
fixed offset. Ring bases must be aligned to both 16 and `vring_align`; regions
must be disjoint. `shm_size`, when nonzero, enables mapping bounds checks.
With zero, the caller guarantees all configured regions are mapped.

The limits are 256 descriptors per ring and 16 caller-owned endpoints.
`vring_num` is a power of two; `buf_count` is even and at most
`2 * vring_num`. `buf_size=0` selects 512 bytes (496-byte payload). Both
directions publish full buffer capacity in descriptor lengths, as pinned
OpenAMP does; the 16-byte header carries the actual payload length.
The legacy trailing event words are reserved, but EVENT_IDX, indirect
descriptors, chaining and feature negotiation are not implemented.

## API sequence

1. Populate `struct rpmsg_lite_cfg` from the peer's memory layout and call
   `rpmsg_lite_init(&ctx, &cfg)`. Optional `notify(priv, vq_id)` receives IDs
   0 (RX buffers recycled) and 1 (TX available).
2. Create caller-owned endpoints before the first poll:
   `rpmsg_lite_create_ept(&ctx, &ept, "service-name", RPMSG_ADDR_ANY, cb, priv)`.
   Automatic local addresses start at 1024. Names must be unique, nonempty
   and at most 31 bytes.
3. Poll until `ept.dest_addr != RPMSG_ADDR_ANY`. Remote NS CREATE announcements
   bind by name; DESTROY unbinds the matching remote address. Announcements for
   unregistered names are not cached. `rpmsg_lite_ns_announce(&ctx, &ept)`
   explicitly sends a local CREATE; creation itself does not announce.
4. Send with `rpmsg_lite_send(&ctx, &ept, data, len)` or
   `rpmsg_lite_sendto(&ctx, &ept, data, len, dst)`. Sending reclaims TX first;
   `-EAGAIN` means all TX buffers are in flight, `-ENOTCONN` means unbound,
   and `-EMSGSIZE` means payload too large. Successful sends return zero.
5. Call `rpmsg_lite_poll(&ctx)` regularly. It returns the number of consumed
   RX messages, including NS and unknown destinations, or a negative errno.
   Malformed messages are rejected and their buffers recycled. Corrupt
   completion IDs/counters return `-EPROTO`; stop/reset and reinitialize the
   transport rather than trying to continue with corrupted ownership.
6. Stop the peer, then call `rpmsg_lite_deinit(&ctx)`.

Serialize all calls. Callbacks borrow the payload only until return and may
send replies; they must not recursively poll or alter endpoints/lifecycle.
Keep endpoint objects and the mapping alive until destroyed/deinitialized.
For timeout handling, loop over poll with the application's monotonic clock
and chosen sleep/yield policy; the transport itself never blocks.

## Hardware-free test

From the repository root:

```sh
cmake -S lib -B /tmp/amdrnpu-rpmsg-build \
  -DAMDRNPU_LIB_BUILD_RPMSG_TEST=ON
cmake --build /tmp/amdrnpu-rpmsg-build
ctest --test-dir /tmp/amdrnpu-rpmsg-build --output-on-failure
```

`rpmsg_lite_test.c` simulates the device side, including NS binding,
send/reclaim, callback replies, recycling, malformed inputs, physical-address
translation above 4 GiB, and more than 65536 exchanges to test index wraparound.
It requires no device access.

[zephyr]: https://github.com/zephyrproject-rtos/zephyr/blob/e989541ec34fc7fde81dc9bdf504cdcce93e1463/subsys/ipc/rpmsg_service/rpmsg_backend.c#L270-L326
[openamp]: https://github.com/zephyrproject-rtos/open-amp/blob/01032a8a7bdfdd541686f5dfa3671e602b9fcbff/open-amp/lib/rpmsg/rpmsg_virtio.c#L855-L951
[zephyr-rx]: https://github.com/zephyrproject-rtos/zephyr/blob/e989541ec34fc7fde81dc9bdf504cdcce93e1463/subsys/ipc/rpmsg_service/rpmsg_backend.c#L174-L196
