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

## Linux (userspace, no kernel module)

- `/etc/amdrnpu.ini` (or `$AMDRNPU_INI`) declares the RPU pool and the rpmsg
  shared-memory geometry. The pool regions are **`no-map`** reserved-memory in
  the device tree (kernel-unmanaged); the library allocates from them with a
  4 KiB bitmap and accesses them via **`/dev/mem`** (needs root / mmap
  permission).
- The library submits commands over the userspace **rpmsg_lite** leader on the
  `amdrnpu` channel — no `amdrnpu.ko`.
- Demo allocates **both buffers as RPU BOs** (`AMDRNPU_MEMORY_RPU`) so firmware
  can access the resolved physical addresses; CPU mapping is a `/dev/mem` window
  via `amdrnpu_buf_mmap()`.

## Run

```bash
# Uses /etc/amdrnpu.ini by default:
amdrnpu_inout_example
# or point at a specific config:
AMDRNPU_INI=/path/to/amdrnpu.ini amdrnpu_inout_example
amdrnpu_inout_example --device /path/to/amdrnpu.ini
```

Success prints `verify OK: output matches input`.
