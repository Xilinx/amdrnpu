# tessai_agent_inout_example

End-to-end Linux userspace test for **in buffer → RPU copy → out buffer** using
`tessai-agent` and matching RPU firmware.

## Device tree (`system-top-rpu-enlarged.dtb`)

OpenAMP reserved memory (must match `rpu_fw/boards/versal2_rpu.overlay`):

| Region | Physical start | Size |
|--------|----------------|------|
| Resource table | `0x09800000` | 256 B |
| VirtIO vring 0 | `0x09860000` | 16 KiB |
| VirtIO vring 1 | `0x09864000` | 16 KiB |
| VirtIO buffer pool | `0x09868000` | 256 KiB |
| RPU GEM carveout (`tess_rpu_gem_carveout`) | `0x20000000` | 512 MiB (reusable) |

IPI for RPMsg doorbell: RPU `ipi_nobuf2` @ `0xeb3b1000` notifies Linux `ipi_nobuf1`
@ `0xeb3b0000` (`xlnx,ipi-id = <10>` in the Zephyr overlay).

## RPU firmware

Build `rpu_fw` with `versal2_rpu` board; firmware must include:

- `tessai_agent_ept.c` — RPMsg channel **`tessai-agent`**
- `TESSAI_OP_COPY_IN_OUT` (`0x1`) — copies input GEM phys → output GEM phys

Rebuild and load via remoteproc after updating the ELF on the target.

## Linux

- `tessai_agent.ko` loaded
- DT node `amd,tessai-agent` with `bank@0` `memory-region` → `tess_rpu_gem_carveout`
- Demo allocates **both buffers as RPU BOs** (`TESSAI_MEMORY_RPU`) so firmware can
  access the resolved physical addresses in `tess_rpu_gem_carveout`.
- Example opens **`/dev/dri/cardN`** (not `renderD*`) for mmap

## Run

```bash
tessai_agent_inout_example
# or
tessai_agent_inout_example --device /dev/dri/card1
```

Success prints `verify OK: output matches input`.
