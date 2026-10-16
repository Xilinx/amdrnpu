# rpmsg-lite DMA echo test (x86 ⇄ A78 / T20 over OCM)

A minimal **echo** across the `rpmsg_lite_dma` stack:

- **x86 host** (rave2) runs the rpmsg **driver**. The vrings + buffer pool live in the
  **T20 endpoint's OCM**, reached by DMA through the **MDB5 client-driver**
  (`/dev/mdb5_write00` / `/dev/mdb5_read00`): each ring-field access is a positioned
  `pwrite`/`pread` = **one DMA**.
- **A78** (Linux on T20) runs the virtio **device** peer, reaching the *same* OCM directly via
  a `/dev/mem` wrapper. It consumes `"ping"` and produces `"pong"`.
- **Poll-only**: no IPI/doorbell. Both sides poll their rings.

```
  x86 host (driver)                         A78 (device peer)
  ┌────────────────────┐                    ┌────────────────────┐
  │ echo_host          │                    │ echo_device        │
  │  rpmsg_lite_dma    │   OCM (T20)        │  device_echo_run   │
  │  rpmsg_mem_mdb5 ───┼── pwrite/pread ──► ┌──────────────┐ ◄───┼─ /dev/mem mmap
  │  (+DMA counter)    │   (MDB5 DMA)       │ status       │     │  mem_devmem
  └────────────────────┘                    │ TX vring     │     └────────────────────┘
                                            │ RX vring     │
                                            │ buffer pool  │
                                            └──────────────┘
```

## Layout
| File | Side | Role |
|------|------|------|
| `echo_config.ini` / `.c` / `.h` | shared | memory-map config parsed by BOTH sides (layout can't drift) |
| `host/echo_host.c`, `host/Makefile` | x86 | driver app + per-message DMA counter; native x86_64 build |
| `a78/mem_devmem.{c,h}` | A78 | `rpmsg_mem` backend over `/dev/mem` (O_SYNC, uncached) |
| `a78/echo_device.{c,h}` | A78 | device-side echo loop (ports the peer in `rpmsg_lite_dma_test.c`) |
| `a78/main.c`, `a78/build.sh` | A78 | entry + aarch64 build (Yocto EDF SDK) |
| `cosim_test.c` | host-mock | no-hardware co-sim: driver ⇄ device in one process |

The driver/transport is the vetted library in `../../lib/src/rpmsg-lite/`
(`rpmsg_lite_dma`, `vring_lite_dma`, `rpmsg_mem_mdb5`) — not reimplemented here.

## DMA count per message (x86 side)
Each `rpmsg_mem_wr*/rd*` is one `pwrite`/`pread` = one MDB5 DMA; there is **no batching**.

**Send one message** (`rpmsg_lite_dma_sendto` → `send_raw` + `vring_lite_dma_add_buffer`):
| # | DMA | field |
|---|-----|-------|
| 1 | READ  | TX `used.idx` (reclaim check) |
| 2 | WRITE | payload (16B hdr + data, one contiguous DMA) |
| 3 | WRITE | `desc.addr` (u64) |
| 4 | WRITE | `desc.len`  (u32) |
| 5 | WRITE | `desc.flags`(u16) |
| 6 | WRITE | `desc.next` (u16) |
| 7 | WRITE | `avail.ring[slot]` (u16) |
| 8 | WRITE | `avail.idx` (u16) |

→ **8 DMAs** (1 read + 7 writes); `notify` is none (poll-only). When a prior TX buffer has
completed (steady-state ping-pong) the reclaim loop adds **+3 reads** (used.idx, ring.id, ring.len).

**Receive one message** (`rpmsg_lite_dma_poll` → `get_used` + `dispatch` + repost):
reads `rx used.idx`, `ring.id`, `ring.len`, `hdr`(16B), `payload`(len) = **5 reads**
(+1 empty-idx recheck, +1 TX-reclaim idx) and **6 writes** to repost the consumed RX buffer
(same desc+avail pattern) → **~13-14 DMAs**.

The app prints the *measured* counts per message so you can confirm these numbers live.

> Why so many: a single rpmsg op is ~8-14 DMAs because the descriptor (4 fields) and the
> avail/used rings are each a separate tiny DMA. The 4 contiguous descriptor fields could be
> coalesced into one 16-byte DMA — an optimization, out of scope for this echo.

## Build

### x86 host — built ON the rave2 x86 target (native)
The host app only needs `gcc` and the MDB5 nodes, so build it on the x86 box itself:
```
cd host && make          # -> host/echo_host (x86_64)
```
The Makefile auto-locates the rpmsg-lite sources and `echo_config.c`, so it works whether you
copy the whole tree or just a subset:
- **Whole tree** (or `host/` + `echo_config.*` + `../../lib/src/rpmsg-lite/` keeping the relative
  layout): `make` finds everything.
- **Flat copy**: drop `echo_host.c`, the `Makefile`, `echo_config.{c,h}`, and the rpmsg-lite
  `*.c/*.h` (`rpmsg_lite_dma`, `vring_lite_dma`, `rpmsg_mem`, `rpmsg_mem_mdb5`) into one dir on the
  target and run `make`.
- **Explicit paths**: `make RPMSG=/path/to/lib/src/rpmsg-lite CFGDIR=/path/to/echo_config_dir`.

The Makefile pins `gcc`/`CFLAGS` with `:=`, so even if an aarch64 SDK env is sourced it won't
hijack the native build (`make CC=...` still overrides). Append flags with `EXTRA_CFLAGS=...`.

### A78 device — built with the Yocto EDF SDK (cross, on the dev host)
Uses the SDK produced by the vek385 Yocto build
(`.../vek385/rev-b/sw/yocto/build/tmp/deploy/sdk/amd-edf-glibc-*-toolchain-*.sh`). If that SDK
isn't installed yet, install it once (default dir shown; pick your own with `-d`):
```
/scratch/wendy/t20/Vitis-AI-Telluride/versal_2ve/reference_design/vek385/rev-b/sw/yocto/build/\
tmp/deploy/sdk/amd-edf-glibc-x86_64-meta-edf-app-sdk-cortexa72-cortexa53-amd-cortexa78-mali-\
common-toolchain-26.06+release-bcee4c1cbf3dece311d193c4aa6b0f497477eb55.sh -y -d ~/t20-edf-sdk
```
Then build (the script sources the SDK env and cross-compiles):
```
cd a78 && ./build.sh     # -> a78/echo_device (dynamic aarch64; glibc on the A78 rootfs)
```
`build.sh` defaults to the already-installed SDK at
`/scratch/wendy/t20/sdk/environment-setup-cortexa72-cortexa53-amd-linux`; override with
`EDF_SDK_ENV=<env-setup> ./build.sh`. This SDK has no static `libc.a`, so the binary is dynamic
(fine — the A78 Yocto rootfs provides glibc). For a fully static ELF instead, use a toolchain
that ships `libc.a`: `STATIC=1 STATIC_CC=aarch64-linux-gnu-gcc ./build.sh`.

### Co-sim (no hardware) — recommended after any edit
```
R=../../lib/src/rpmsg-lite
gcc -O2 -std=c11 -Wall -I. -Ia78 -I$R cosim_test.c a78/echo_device.c echo_config.c \
    $R/rpmsg_lite_dma.c $R/vring_lite_dma.c $R/rpmsg_mem_host.c -lpthread -o cosim_test
./cosim_test        # expect: COSIM PASS
```

## Configure
Edit `echo_config.ini` (shared by both sides — keep the values identical). Key fields:
`dev_base` (OCM base as seen over MDB5 = rpmsg `shm_phys`), `a78_phys_base` (same OCM as the A78
sees via /dev/mem), `shm_size`, the vring/buffer offsets, `peer_dst`, `host_ept_addr`, and the
MDB5 node paths.

## Run (on hardware)
**Step 0 — address-equality bring-up (do this ONCE).** `rpmsg_lite_dma` uses one `dev_base`
both as the MDB5 DMA offset *and* as the buffer address the A78 dereferences, so the MDB5
endpoint address of an OCM location must equal the A78 physical address. Verify:
```
# x86: write a marker to dev_base via MDB5
printf '\xDE\xAD\xBE\xEF' | dd bs=4 count=1 of=/dev/mdb5_write00 seek=$((0xBBF00000)) oflag=seek_bytes
# A78: read the same physical address
devmem2 0xBBF00000 w        # expect 0xEFBEADDE (LE)
```
If they match, leave `dev_base == a78_phys_base`. If there's a fixed BAR offset, set `dev_base`
(x86) and `a78_phys_base` (A78) accordingly; the A78 wrapper translates.

**Step 1 — start the A78 device first** (it resets nothing; it waits for the driver's
`DRIVER_OK`), as root (for `/dev/mem`):
```
./echo_device echo_config.ini        # runs forever; or: ./echo_device echo_config.ini 1000
```
**Step 2 — start the x86 host** (resets the rings, posts buffers, sends pings):
```
./echo_host ../echo_config.ini
```
Expect the host to print each round-trip's `"pong"`, the measured DMA counts, and
`N/N round-trips validated "pong"`.

## Notes / assumptions
- The OCM region should be **reserved no-map** so `/dev/mem` (O_SYNC) and the PCIe DMA see the
  same uncached bytes. The default bank (`0xBBF00000`, 512 KiB) is a candidate — confirm it's
  free and reserved on your build.
- Start order matters only for a clean first handshake; the host clears status before init so a
  freshly started device won't act on a stale `DRIVER_OK`.
- Out of scope: IPI/doorbell notify, descriptor-write coalescing, NS handshake (echo uses a
  fixed `peer_dst`), and any throughput/latency harness beyond the round-trip timer.
