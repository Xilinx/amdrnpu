# tessai-agent - design documentation

This folder describes the design of the `tessai-agent` driver. The
documents are written to be read in order, but each one stands on its
own.

| Document | What it covers |
|----------|----------------|
| [lib-userspace-flow.md](lib-userspace-flow.md) | **`libtessai_agent`** API (`tessai_agent_*`): buffer alloc/close, command submit, syncobj wait helpers, diagrams. |
| [architecture.md](architecture.md) | System view, in-kernel block diagram, file-by-file responsibilities, the dual platform/rpmsg driver registration, and the per-bank memory layout. |
| [devicetree-binding.md](devicetree-binding.md) | Prose reference for the `amd,tessai-agent` DT binding: parent/child schema, reserved-memory companion nodes, single- and multi-RPU examples, validation, and common pitfalls. |
| [buffer-management.md](buffer-management.md) | `TESSAI_BUF_ALLOC`, GEM mmap, handle lifetime, in-flight command pins, and release via `tessai_agent_buf_close` / `DRM_IOCTL_GEM_CLOSE`. |
| [commands.md](commands.md) | Command submission (`TESSAI_CMD_SUBMIT`), wire format, and drm_syncobj / dma_fence interaction. |

The machine-checkable YAML schema for the binding lives at
[`Documentation/devicetree/bindings/accel/amd,tessai-agent.yaml`](../Documentation/devicetree/bindings/accel/amd,tessai-agent.yaml).

## Quick links

- Userspace library: [`lib/`](../lib/) (`libtessai_agent` + `tessai_agent_test` CLI)
- UAPI header: [`include/uapi/tessai-agent/tessai_agent.h`](../include/uapi/tessai-agent/tessai_agent.h)
- Driver source: [`drivers/accel/tessai-agent/`](../drivers/accel/tessai-agent/)
- Cross-build helper: [`scripts/yocto-cross-build.sh`](../scripts/yocto-cross-build.sh)
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
