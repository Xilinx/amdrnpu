# amdrnpu - design documentation

This folder describes the design of the `amdrnpu` driver. The
documents are written to be read in order, but each one stands on its
own.

`amdrnpu` is a lightweight rpmsg char-device driver (`/dev/amdrnpu.N`),
not a DRM driver. Buffers are userspace-allocated dma-bufs imported into
the driver; command completion is a `sync_file` fd.

| Document | What it covers |
|----------|----------------|
| [lib-userspace-flow.md](lib-userspace-flow.md) | **`libamdrnpu`** API (`amdrnpu_*`): buffer alloc/import/close, command submit, `sync_file` wait helpers, diagrams. |
| [architecture.md](architecture.md) | System view, in-kernel block diagram, file-by-file responsibilities, rpmsg binding, and the two DMA domains. |
| [devicetree-binding.md](devicetree-binding.md) | Platform integration: the driver has no DT binding of its own; how the rpmsg channel, remoteproc node, and dma-buf heaps are wired in. |
| [buffer-management.md](buffer-management.md) | Buffer import (`AMDRNPU_BUF_IMPORT`), dma-buf mmap/sync, handle lifetime, in-flight command pins, and release via `amdrnpu_buf_close` / `AMDRNPU_BUF_DESTROY`. |
| [commands.md](commands.md) | Command submission (`AMDRNPU_CMD_SUBMIT`), wire format, and `sync_file` / dma_fence interaction. |

## Quick links

- Userspace library: [`lib/`](../lib/) (`libamdrnpu` + `amdrnpu_test` CLI)
- UAPI header: [`include/uapi/amdrnpu/amdrnpu.h`](../include/uapi/amdrnpu/amdrnpu.h)
- Driver source: [`drivers/rpmsg/amdrnpu/`](../drivers/rpmsg/amdrnpu/)
- Build orchestrator: [`build/build.sh`](../build/build.sh) (native / `-cross-vitis` / `-cross-yocto`)
- Top-level overview: [`../README.md`](../README.md)

## Diagram conventions

All diagrams in these docs are [Mermaid](https://mermaid.js.org/)
fenced code blocks. They render natively on GitHub, GitLab, Gitiles,
VS Code (with the Markdown Preview Mermaid Support extension), and any
other Markdown viewer that supports Mermaid.

Common shapes used:

- **`flowchart` / `graph`** for static block diagrams (architecture,
  data layout).
- **`sequenceDiagram`** for time-ordered interactions across actors
  (ioctl + rpmsg + RPU).
- **`stateDiagram-v2`** for explicit state machines (buffer and
  command lifecycles).

If you need to render to PNG/SVG offline, the
[`mmdc`](https://github.com/mermaid-js/mermaid-cli) tool can convert
each fenced block.
