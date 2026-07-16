# amdrnpu - design documentation

This folder describes the design of the `amdrnpu` driver. The
documents are written to be read in order, but each one stands on its
own.

| Document | What it covers |
|----------|----------------|
| [lib-userspace-flow.md](lib-userspace-flow.md) | **`libamdrnpu`** API (`amdrnpu_*`): buffer alloc/close, command submit, syncobj wait helpers, diagrams. |
| [architecture.md](architecture.md) | System view, in-kernel block diagram, file-by-file responsibilities, the dual platform/rpmsg driver registration, and the per-bank memory layout. |
| [devicetree-binding.md](devicetree-binding.md) | Prose reference for the `amd,amdrnpu` DT binding: parent/child schema, reserved-memory companion nodes, single- and multi-RPU examples, validation, and common pitfalls. |
| [buffer-management.md](buffer-management.md) | `AMDRNPU_BUF_ALLOC`, GEM mmap, handle lifetime, in-flight command pins, and release via `amdrnpu_buf_close` / `DRM_IOCTL_GEM_CLOSE`. |
| [commands.md](commands.md) | Command submission (`AMDRNPU_CMD_SUBMIT`), wire format, and drm_syncobj / dma_fence interaction. |

The machine-checkable YAML schema for the binding lives at
[`Documentation/devicetree/bindings/accel/amd,amdrnpu.yaml`](../Documentation/devicetree/bindings/accel/amd,amdrnpu.yaml).

## Quick links

- Userspace library: [`lib/`](../lib/) (`libamdrnpu` + `amdrnpu_test` CLI)
- UAPI header: [`include/uapi/amdrnpu/amdrnpu.h`](../include/uapi/amdrnpu/amdrnpu.h)
- Driver source: [`drivers/accel/tessai-agent/`](../drivers/accel/tessai-agent/)
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
