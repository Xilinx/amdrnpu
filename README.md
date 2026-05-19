# tessai-agent

A small Linux **DRM render-node driver** + **userspace runtime library** that
brokers buffer allocation and command submission between application code on
the APU and firmware running on a remote Cortex-R52 (RPU) over RPMsg. The
driver exposes the standard DRM ioctl surface (GEM, dumb buffers, syncobj,
PRIME) plus two custom ioctls under the `'T'` magic; the library is a thin
wrapper around them.

Designed to be built either standalone (out-of-tree against any kernel
checkout) or via Yocto using the included `meta-tessai-agent` layer.

## Repository layout

```
tessai-agent/
├── drivers/accel/tessai-agent/      # out-of-tree kernel module (DRM accel subsystem)
│   ├── tessai_agent_drv.c           # platform driver, probe / DT bind, drm_dev_alloc
│   ├── tessai_agent_drm.c           # DRM driver ops, ioctl table, GEM/dumb hooks
│   ├── tessai_agent_bank.c          # per-bank carveout (reserved-memory) device
│   ├── tessai_agent_cmd.{c,h}       # CMD_SUBMIT lifecycle, syncobj/fence wiring
│   ├── tessai_agent_rpmsg.c         # RPMsg endpoint + wire packing/unpacking
│   ├── tessai_agent_internal.h
│   ├── Kbuild
│   └── Makefile
├── include/uapi/tessai-agent/
│   └── tessai_agent.h               # SoT for the kernel/user ABI (magic 'T')
├── lib/                              # user-space runtime library (CMake)
│   ├── CMakeLists.txt
│   ├── include/tessai_agent/tessai_agent_lib.h
│   ├── src/tessai_agent_lib.c
│   ├── examples/tessai_agent_test.c # CLI exercising every UAPI op
│   └── cmake/                        # pkg-config + CMake config templates
├── docs/                             # design documentation (Mermaid diagrams)
│   ├── architecture.md
│   ├── devicetree-binding.md
│   ├── buffer-management.md
│   ├── commands.md
│   ├── lib-userspace-flow.md
│   └── README.md
├── Documentation/devicetree/bindings/accel/
│   └── amd,tessai-agent.yaml         # machine-checkable DT schema
├── scripts/                          # cross-build + deploy helpers
│   ├── build-vitis.sh
│   ├── build-oot-modules.sh
│   ├── yocto-cross-build.sh
│   └── strip-dirty-vermagic.py
└── meta-tessai-agent/                # Yocto/OE layer
    ├── conf/layer.conf
    ├── recipes-kernel/tessai-agent-mod/
    └── recipes-libs/tessai-agent-lib/
```

The single source of truth for the user/kernel ABI is
[`include/uapi/tessai-agent/tessai_agent.h`](include/uapi/tessai-agent/tessai_agent.h)
(magic `'T'`, ABI version `13` at time of writing); both the kernel module
and the user-space library include it directly.

## UAPI at a glance

Two custom ioctls under magic `'T'`:

| Ioctl | Purpose |
|---|---|
| `TESSAI_BUF_ALLOC`  | Allocate a GEM-backed BO from one of the device's reserved-memory **banks** (`TESSAI_MEMORY_RPU` for RPU-reachable carveouts, `TESSAI_MEMORY_DEVICE` for the larger device-memory pool). Returns a DRM handle that can be `MAP_DUMB`-ed and `mmap`-ed. |
| `TESSAI_CMD_SUBMIT` | Submit a command to the RPU side. Args are an array of `struct tessai_arg` entries; each is either a scalar QWORD or a GEM-slice descriptor `(drm_buffer_handle, offset)` that the driver resolves to an RPU-side physical address. Returns a `drm_syncobj` handle the caller waits on for completion. |

Standard DRM ioctls (GEM, dumb buffer, syncobj, PRIME) are reachable on the
same render-node file via the normal DRM core path.

## Building the kernel module (standalone)

The module builds out-of-tree against any prepared kernel source/build tree.
Point `KDIR` at it.

```sh
# Native build against the running kernel
make -C drivers/accel/tessai-agent KDIR=/lib/modules/$(uname -r)/build

# Cross-build for arm64
make -C drivers/accel/tessai-agent \
    KDIR=/path/to/linux \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu-

# Install to a staging root
make -C drivers/accel/tessai-agent KDIR=... INSTALL_MOD_PATH=/tmp/rootfs modules_install

# Clean
make -C drivers/accel/tessai-agent KDIR=... clean
```

Produces `drivers/accel/tessai-agent/tessai_agent.ko`. Loading it on a
target that has a matching `compatible = "amd,tessai-agent"` device-tree
node creates a DRM render node (typically `/dev/dri/renderD128` if no
other DRM driver is present, or whichever node `tessai_agent_test device`
reports).

`KERNEL_SRC` is accepted as a synonym for `KDIR` so the same Makefile works
under Yocto's `module.bbclass` without modification.

## Building the user-space library (standalone)

```sh
cmake -S lib -B build/lib -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/lib -j
sudo cmake --install build/lib
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `TESSAI_AGENT_LIB_BUILD_SHARED`   | `ON` | build `libtessai_agent.so` |
| `TESSAI_AGENT_LIB_BUILD_STATIC`   | `ON` | build `libtessai_agent.a` |
| `TESSAI_AGENT_LIB_BUILD_EXAMPLES` | `ON` | build the `tessai_agent_test` CLI |
| `TESSAI_AGENT_UAPI_DIR`           | `<repo>/include/uapi` | path to the shared UAPI headers |

Depends on `libdrm` for syncobj / sync_file ioctls and `struct drm_gem_close`.

Downstream projects can consume the library via either CMake
(`find_package(tessai_agent)`) or pkg-config
(`pkg-config --cflags --libs tessai_agent`).

## Building under Yocto

The `meta-tessai-agent/` directory is a self-contained OE layer.

```sh
bitbake-layers add-layer /path/to/tessai-agent/meta-tessai-agent
bitbake tessai-agent-mod          # kernel module
bitbake tessai-agent-lib          # runtime library
```

To install both into an image, add to `local.conf` (or your image recipe):

```
IMAGE_INSTALL:append = " tessai-agent-mod tessai-agent-lib"
```

The layer uses `externalsrc` so it builds directly from the in-tree sources.
If your repository checkout lives elsewhere than the layer's parent
directory, override `TESSAI_AGENT_SRC` in `local.conf`:

```
TESSAI_AGENT_SRC = "/abs/path/to/tessai-agent"
```

See [`meta-tessai-agent/README.md`](meta-tessai-agent/README.md) for more.

## Runtime usage

The lib mirrors the UAPI 1:1. A minimal end-to-end (alloc → submit → wait →
close) looks like:

```c
#include <tessai_agent/tessai_agent_lib.h>

int fd = tessai_agent_open_render_node();          /* picks the tessai_agent DRM node */

uint32_t bo;
tessai_agent_buf_alloc(fd, TESSAI_MEMORY_DEVICE, 4096, &bo);

uint64_t map_offset;
tessai_agent_map_dumb(fd, bo, &map_offset);
void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_offset);

struct tessai_cmd_submit_hdr hdr = {
    .command_op = 0xCAFEBABE,
    .num_args   = 0,
};
tessai_agent_cmd_submit(fd, &hdr, /* args */ NULL);

tessai_agent_syncobj_wait(fd, hdr.drm_sync_obj_handle, /*timeout_ns=*/2000000000);

tessai_agent_buf_close(fd, bo);
close(fd);
```

The `tessai_agent_test` CLI (built by default with the lib) exercises every
UAPI op individually plus a `lifecycle` shortcut and a fault-class
fence-error suite. Run with no args for usage:

```sh
tessai_agent_test                     # help
tessai_agent_test device              # report which /dev/dri/* node matches
tessai_agent_test buf_alloc rpu 4096
tessai_agent_test lifecycle           # alloc + no-op submit + wait + close
tessai_agent_test fw_echo 0xCAFE 0xBEEF   # round-trip text writeback via RPU
```

See [`docs/lib-userspace-flow.md`](docs/lib-userspace-flow.md) for the
full library walkthrough and [`docs/commands.md`](docs/commands.md) for
the wire-format details.

## Design documentation

Start at [`docs/README.md`](docs/README.md) for the documentation map.
Highlights:

- [`docs/architecture.md`](docs/architecture.md) — system view, in-kernel
  block diagram, file-by-file responsibilities, dual platform/rpmsg
  driver registration, per-bank memory layout.
- [`docs/devicetree-binding.md`](docs/devicetree-binding.md) — prose
  reference for the `amd,tessai-agent` binding (parent/child schema,
  reserved-memory companions, single- and multi-RPU examples).
- [`docs/buffer-management.md`](docs/buffer-management.md) — buffer
  lifecycle, in-flight command pinning, `GEM_CLOSE` semantics.
- [`docs/commands.md`](docs/commands.md) — `TESSAI_CMD_SUBMIT` wire
  format, drm_syncobj / dma_fence interaction, fence error propagation.
- [`docs/lib-userspace-flow.md`](docs/lib-userspace-flow.md) — full
  library API walkthrough.

## License

- Kernel module: GPL-2.0-only (see SPDX headers in
  `drivers/accel/tessai-agent/`).
- User-space library and meta-layer: MIT (see [`LICENSE`](LICENSE)).
