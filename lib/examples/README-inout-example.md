# amdrnpu_inout_example

End-to-end Linux userspace test for **in buffer → RPU copy → out buffer** using
`amdrnpu` and matching RPU firmware.

## Device tree (`system-top-rpu-enlarged.dtb`)

OpenAMP reserved memory (must match `rpu_fw/boards/versal2_rpu.overlay`):

| Region | Physical start | Size |
|--------|----------------|------|
| Resource table | `0x09800000` | 256 B |
| VirtIO vring 0 | `0x09860000` | 16 KiB |
| VirtIO vring 1 | `0x09864000` | 16 KiB |
| VirtIO buffer pool | `0x09868000` | 256 KiB |
| RPU carveout (`rpu_gem_cma@20000000`, phandle `<0x500>`) | `0x20000000` | 512 MiB (reusable + `linux,cma-default`) |

IPI for RPMsg doorbell: RPU `ipi_nobuf2` @ `0xeb3b1000` notifies Linux `ipi_nobuf1`
@ `0xeb3b0000` (`xlnx,ipi-id = <10>` in the Zephyr overlay).

## RPU firmware

Build `rpu_fw` with `versal2_rpu` board; firmware must include:

- `amdrnpu_ept.c` — RPMsg channel **`amdrnpu`**
- `AMDRNPU_OP_COPY_IN_OUT` (`0x1`) — copies input buffer phys → output buffer phys

Rebuild and load via remoteproc after updating the ELF on the target.

## Linux

- `amdrnpu.ko` loaded and bound to the `amdrnpu` RPMsg channel
- An RPU-reachable `shared-dma-pool` reserved-memory region for the RPU
  buffers. The driver uses `rpu_gem_cma` by default (`rpu_gem_cma@20000000`
  at `0x20000000`, 512 MiB, in the official Versal-net layout); choose a
  different node with `/sys/kernel/config/amdrnpu/rpu_region`.
- Demo allocates **both buffers as RPU BOs** (`AMDRNPU_MEMORY_RPU`) so firmware can
  access the resolved physical addresses.
- Example opens **`/dev/amdrnpu.N`** (the driver is a miscdevice); CPU mapping is
  done on each buffer's dma-buf via `amdrnpu_buf_mmap()`.

## Run

```bash
amdrnpu_inout_example
# or
amdrnpu_inout_example --device /dev/amdrnpu.0
```

Success prints `verify OK: output matches input`.
