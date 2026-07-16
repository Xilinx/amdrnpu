# amdrnpu

A small Linux **DRM render-node driver** + **userspace runtime library** that
brokers buffer allocation and command submission between application code on
the APU and firmware running on a remote Cortex-R52 (RPU) over RPMsg. The
driver exposes the standard DRM ioctl surface (GEM, dumb buffers, syncobj,
PRIME) plus two custom ioctls under the `'T'` magic; the library is a thin
wrapper around them.

Designed to be built either standalone (out-of-tree against any kernel
checkout) or via Yocto using the included `meta-amdrnpu` layer.

## Repository layout

```
tessai-agent/
├── drivers/accel/tessai-agent/      # out-of-tree kernel module (DRM accel subsystem)
│   ├── amdrnpu_drv.c           # platform driver, probe / DT bind, drm_dev_alloc
│   ├── amdrnpu_drm.c           # DRM driver ops, ioctl table, GEM/dumb hooks
│   ├── amdrnpu_bank.c          # per-bank carveout (reserved-memory) device
│   ├── amdrnpu_cmd.{c,h}       # CMD_SUBMIT lifecycle, syncobj/fence wiring
│   ├── amdrnpu_rpmsg.c         # RPMsg endpoint + wire packing/unpacking
│   ├── amdrnpu_internal.h
│   ├── Kbuild
│   └── Makefile
├── include/uapi/amdrnpu/
│   └── amdrnpu.h               # SoT for the kernel/user ABI (magic 'T')
├── lib/                              # user-space runtime library (CMake)
│   ├── CMakeLists.txt
│   ├── include/amdrnpu/amdrnpu_lib.h
│   ├── src/amdrnpu_lib.c
│   ├── examples/amdrnpu_test.c # CLI exercising every UAPI op
│   └── cmake/                        # pkg-config + CMake config templates
├── docs/                             # design documentation (Mermaid diagrams)
│   ├── architecture.md
│   ├── devicetree-binding.md
│   ├── buffer-management.md
│   ├── commands.md
│   ├── lib-userspace-flow.md
│   └── README.md
├── Documentation/devicetree/bindings/accel/
│   └── amd,amdrnpu.yaml         # machine-checkable DT schema
├── build/                            # build orchestrator (see "Building" below)
│   ├── build.sh                      # native / -cross-vitis / -cross-yocto
│   └── strip-dirty-vermagic.py       # post-process helper invoked by build.sh
└── meta-amdrnpu/                # Yocto/OE layer
    ├── conf/layer.conf
    ├── recipes-kernel/amdrnpu-mod/
    └── recipes-libs/amdrnpu-lib/
```

The single source of truth for the user/kernel ABI is
[`include/uapi/amdrnpu/amdrnpu.h`](include/uapi/amdrnpu/amdrnpu.h)
(magic `'T'`, ABI version `1` at time of writing); both the kernel module
and the user-space library include it directly.

## UAPI at a glance

Two custom ioctls under magic `'T'`:

| Ioctl | Purpose |
|---|---|
| `AMDRNPU_BUF_ALLOC`  | Allocate a GEM-backed BO from one of the device's reserved-memory **banks** (`AMDRNPU_MEMORY_RPU` for the RPU-reachable 512 MiB `linux,cma-default` pool, `AMDRNPU_MEMORY_DEVICE` for the 4 GiB high-DDR pool — see [`docs/devicetree-binding.md` §4](docs/devicetree-binding.md#4-reserved-memory-companion-nodes) for the official Versal-net layout and sharing semantics). Returns a DRM handle that can be `MAP_DUMB`-ed and `mmap`-ed. |
| `AMDRNPU_CMD_SUBMIT` | Submit a command to the RPU side. Args are an array of `struct amdrnpu_arg` entries; each is either a scalar QWORD or a GEM-slice descriptor `(drm_buffer_handle, offset)` that the driver resolves to an RPU-side physical address. Returns a `drm_syncobj` handle the caller waits on for completion. |

Standard DRM ioctls (GEM, dumb buffer, syncobj, PRIME) are reachable on the
same render-node file via the normal DRM core path.

## Building

For local development use the top-level orchestrator at
[`build/build.sh`](build/build.sh). One invocation builds the kernel
module, the userspace library, and the `amdrnpu_test` CLI into a
clean out-of-source tree, leaving `drivers/accel/tessai-agent/` and
`lib/` pristine.

```sh
# Native (default): build all three against the running kernel.
# Requires linux-headers-$(uname -r) and libdrm-dev on the host.
build/build.sh

# aarch64 cross-build using a full Yocto build tree at PATH.
# Toolchain, kernel, and target sysroot are auto-discovered under
# PATH/tmp/work-shared/ and PATH/tmp/work/.
build/build.sh -cross-yocto /path/to/yocto/build

# aarch64 cross-build using the Vitis-shipped toolchain and a manually
# prepared Xilinx kernel tree (no Yocto). PATH is an aarch64 sysroot
# that has usr/lib/pkgconfig/libdrm.pc.
build/build.sh -cross-vitis -sysroot /path/to/aarch64/sysroot

# Wipe build/Release/ and build/Release-aarch64/.
build/build.sh -clean
```

Outputs (per invocation):

| Path | Artifact |
|---|---|
| `build/<TYPE>/driver/amdrnpu.ko` | kernel module |
| `build/<TYPE>/lib/libamdrnpu.so*` | shared lib (+ `.a` static) |
| `build/<TYPE>/bin/amdrnpu_test` | CLI |

where `<TYPE>` is `Release` for native and `Release-aarch64` for either
cross mode. Loading the `.ko` on a target with a matching
`compatible = "amd,amdrnpu"` device-tree node creates a DRM render
node (typically `/dev/dri/renderD128` if no other DRM driver is
present, or whichever node `amdrnpu_test device` reports).

CMake options forwarded to the lib build:

| Option | Default | Meaning |
|---|---|---|
| `AMDRNPU_LIB_BUILD_SHARED`   | `ON` | build `libamdrnpu.so` |
| `AMDRNPU_LIB_BUILD_STATIC`   | `ON` | build `libamdrnpu.a` |
| `AMDRNPU_LIB_BUILD_EXAMPLES` | `ON` | build the `amdrnpu_test` CLI |
| `AMDRNPU_UAPI_DIR`           | `<repo>/include/uapi` | path to the shared UAPI headers |

The library depends on `libdrm` for syncobj / sync_file ioctls and
`struct drm_gem_close`. Downstream projects can consume it via either
CMake (`find_package(amdrnpu)`) or pkg-config
(`pkg-config --cflags --libs amdrnpu`).

Run `build/build.sh -help` for the full flag surface (toolchain path,
KDIR override, Yocto machine override, `-localversion` injection,
`-no-strip-dirty`, etc.).

### Fallback: raw kbuild / Yocto-internal

The kernel-module Makefile in `drivers/accel/tessai-agent/` is still
usable directly. `meta-amdrnpu/recipes-kernel/amdrnpu-mod_0.1.0.bb`
relies on it via Yocto's `module.bbclass`; the same invocation also works
as a manual fallback:

```sh
make -C drivers/accel/tessai-agent KDIR=/lib/modules/$(uname -r)/build
make -C drivers/accel/tessai-agent KDIR=... ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu-
make -C drivers/accel/tessai-agent KDIR=... INSTALL_MOD_PATH=/tmp/rootfs \
     modules_install
make -C drivers/accel/tessai-agent KDIR=... clean
```

`KERNEL_SRC` is accepted as a synonym for `KDIR` so the same Makefile
works under Yocto's `module.bbclass` without modification. Be aware that
this in-source flow writes `.o` / `.ko` / `.mod*` files into the source
directory; `build/build.sh` avoids that.

## Building under Yocto

The `meta-amdrnpu/` directory is a self-contained OE layer.

```sh
bitbake-layers add-layer /path/to/tessai-agent/meta-amdrnpu
bitbake amdrnpu-mod          # kernel module
bitbake amdrnpu-lib          # runtime library
```

To install both into an image, add to `local.conf` (or your image recipe):

```
IMAGE_INSTALL:append = " amdrnpu-mod amdrnpu-lib"
```

The layer uses `externalsrc` so it builds directly from the in-tree sources.
If your repository checkout lives elsewhere than the layer's parent
directory, override `AMDRNPU_SRC` in `local.conf`:

```
AMDRNPU_SRC = "/abs/path/to/tessai-agent"
```

See [`meta-amdrnpu/README.md`](meta-amdrnpu/README.md) for more.

## Runtime usage

The lib mirrors the UAPI 1:1. A minimal end-to-end (alloc → submit → wait →
close) looks like:

```c
#include <amdrnpu/amdrnpu_lib.h>

#define OP_CODE_CMD0  0x00000001u
#define INPUT_BO      1ull
#define OUTPUT_BO     2ull
#define SZ_16M        (16u * 1024u * 1024u)
#define SZ_8M         (8u * 1024u * 1024u)

int fd = amdrnpu_open_render_node();
uint32_t bo0_hd, bo1_hd;
struct amdrnpu_arg args[6];
struct amdrnpu_cmd_submit hdr;
int32_t status = 0;

amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, SZ_16M, &bo0_hd);
amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, SZ_8M, &bo1_hd);

args[0] = (struct amdrnpu_arg){ .size = 0, .value = INPUT_BO };
args[1] = (struct amdrnpu_arg){ .size = 0, .value = SZ_16M };
args[2] = (struct amdrnpu_arg){
	.size = SZ_16M,
	.bo = { .drm_buffer_handle = (__s32)bo0_hd, .offset = 0 },
};
args[3] = (struct amdrnpu_arg){ .size = 0, .value = OUTPUT_BO };
args[4] = (struct amdrnpu_arg){ .size = 0, .value = SZ_8M };
args[5] = (struct amdrnpu_arg){
	.size = SZ_8M,
	.bo = { .drm_buffer_handle = (__s32)bo1_hd, .offset = 0 },
};

amdrnpu_cmd_submit(fd, OP_CODE_CMD0, args, 6, &hdr, sizeof(hdr));
amdrnpu_syncobj_wait_complete(fd, hdr.drm_sync_obj_handle,
				   2000000000LL, &status);

amdrnpu_buf_close(fd, bo0_hd);
amdrnpu_buf_close(fd, bo1_hd);
close(fd);
```

A raw-`ioctl` variant (no library) is documented in
[`include/uapi/amdrnpu/amdrnpu.h`](include/uapi/amdrnpu/amdrnpu.h).

The `amdrnpu_test` CLI (built by default with the lib) exercises every
UAPI op individually plus a `lifecycle` shortcut and a fault-class
fence-error suite. Run with no args for usage:

```sh
amdrnpu_test                     # help
amdrnpu_test device              # report which /dev/dri/* node matches
amdrnpu_test buf_alloc rpu 4096
amdrnpu_test lifecycle           # alloc + no-op submit + wait + close
amdrnpu_test fw_echo 0xCAFE 0xBEEF   # round-trip text writeback via RPU
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
  reference for the `amd,amdrnpu` binding (parent/child schema,
  reserved-memory companions, single- and multi-RPU examples).
- [`docs/buffer-management.md`](docs/buffer-management.md) — buffer
  lifecycle, in-flight command pinning, `GEM_CLOSE` semantics.
- [`docs/commands.md`](docs/commands.md) — `AMDRNPU_CMD_SUBMIT` wire
  format, drm_syncobj / dma_fence interaction, fence error propagation.
- [`docs/lib-userspace-flow.md`](docs/lib-userspace-flow.md) — full
  library API walkthrough.

## License

- Kernel module: GPL-2.0-only (see SPDX headers in
  `drivers/accel/tessai-agent/`).
- User-space library and meta-layer: MIT (see [`LICENSE`](LICENSE)).
