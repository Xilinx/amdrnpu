# amdrnpu

A small Linux **rpmsg char-device driver** + **userspace runtime library** that
brokers buffer sharing and command submission between application code on the
APU and firmware running on a remote Cortex-R52 (RPU) over RPMsg. The driver is
a `miscdevice` that exposes a char node (`/dev/amdrnpu.N`) with a tiny custom
ioctl surface under the `'R'` magic (buffer alloc/import/destroy + command submit);
completion is reported as a poll(2)-able `sync_file` fd. The library is a thin
wrapper around that surface.

> **Not a DRM driver.** Earlier revisions were a DRM render-node driver (GEM /
> dumb buffers / syncobj / PRIME on `/dev/dri/*`). That model has been replaced
> by the rpmsg char device described here: buffers are plain dma-buf objects,
> allocated by the driver from a reserved-memory region (or imported from
> elsewhere), and completion uses `sync_file` instead of `drm_syncobj`.

Designed to be built either standalone (out-of-tree against any kernel
checkout) or via Yocto using the included `meta-amdrnpu` layer.

## Repository layout

```
amdrnpu/
├── drivers/rpmsg/amdrnpu/           # out-of-tree kernel module (rpmsg char device)
│   ├── amdrnpu_drv.c           # miscdevice (/dev/amdrnpu.N), file ops, module init
│   ├── amdrnpu_rpmsg.c         # rpmsg_driver: probe/remove/callback + wire send
│   ├── amdrnpu_buf.c           # BUF_ALLOC/IMPORT/DESTROY/QUERY: dma-buf export/attach + arg address resolution
│   ├── amdrnpu_heap.c          # reserved-memory region per memory type, for BUF_ALLOC
│   ├── amdrnpu_configfs.c      # /sys/kernel/config/amdrnpu: region names
│   ├── amdrnpu_cmd.{c,h}       # CMD_SUBMIT lifecycle, dma_fence + sync_file completion
│   ├── amdrnpu_internal.h
│   ├── Kbuild
│   └── Makefile
├── include/uapi/amdrnpu/
│   └── amdrnpu.h               # SoT for the kernel/user ABI (magic 'R')
├── lib/                              # user-space runtime library (CMake)
│   ├── CMakeLists.txt
│   ├── include/amdrnpu/amdrnpu_lib.h
│   ├── src/amdrnpu_lib.c
│   ├── examples/                     # example programs (amdrnpu_test CLI, etc.)
│   └── cmake/                        # pkg-config + CMake config templates
├── docs/                             # design documentation (Mermaid diagrams)
│   ├── architecture.md
│   ├── devicetree-binding.md
│   ├── buffer-management.md
│   ├── commands.md
│   ├── lib-userspace-flow.md
│   └── README.md
├── build/                            # build orchestrator (see "Building" below)
│   └── build.sh                      # native / -sdk (Yocto SDK cross-build)
└── meta-amdrnpu/                # Yocto/OE layer
    ├── conf/layer.conf
    ├── recipes-kernel/amdrnpu-mod/
    └── recipes-libs/amdrnpu-lib/
```

The single source of truth for the user/kernel ABI is
[`include/uapi/amdrnpu/amdrnpu.h`](include/uapi/amdrnpu/amdrnpu.h)
(magic `'R'`, ABI version `1` at time of writing); both the kernel module
and the user-space library include it directly.

## UAPI at a glance

Five custom ioctls under magic `'R'`:

| Ioctl | Purpose |
|---|---|
| `AMDRNPU_BUF_ALLOC`   | Allocate a physically contiguous buffer for a `memory_type` — `AMDRNPU_MEMORY_RPU` (RPU-reachable, in the firmware DMA domain) or `AMDRNPU_MEMORY_DEVICE` (the accelerator). The driver allocates from the reserved-memory node configured for that type (`/sys/kernel/config/amdrnpu/rpu_region` / `device_region`, default `rpu_gem_cma` / `buffer`) and exports it as a dma-buf. Returns an opaque `handle` and a `dmabuf_fd`. |
| `AMDRNPU_BUF_IMPORT`  | Import an external **dma-buf** (from a dma-buf heap or another driver) for use as a command-buffer argument. `flags` selects the DMA domain — `AMDRNPU_BUF_IMPORT_DEVICE` (the accelerator) or `AMDRNPU_BUF_IMPORT_FW` (the RPU/firmware remoteproc device, so the emitted address is the one the RPU dereferences). Returns an opaque `handle`. |
| `AMDRNPU_BUF_DESTROY` | Release a buffer `handle`. If an in-flight command still references it, the buffer lives until that command completes. |
| `AMDRNPU_BUF_QUERY`   | Return a buffer `handle`'s DMA address and size. |
| `AMDRNPU_CMD_SUBMIT`  | Submit a command to the RPU. Args are an array of `struct amdrnpu_arg` entries; each is either a scalar QWORD (`size == 0`) or a buffer slice `(buffer_handle, offset)` (`size != 0`) that the driver resolves to an RPU-side DMA address. On success `sync_handle` is a **`sync_file` fd** the caller `poll()`s (`POLLIN`) or waits on for completion. |

There is no DRM surface. CPU mapping (`mmap(2)`) and cache maintenance
(`DMA_BUF_IOCTL_SYNC`) are performed on the buffer's dma-buf fd directly; the
library wraps allocation, mmap, and sync so callers don't have to.

## Building

For local development use the top-level orchestrator at
[`build/build.sh`](build/build.sh). One invocation builds the kernel
module, the userspace library, and the example programs into a
clean out-of-source tree, leaving `drivers/rpmsg/amdrnpu/` and
`lib/` pristine.

```sh
# Native (default): build all three against the running kernel.
# Host-arch artifacts for quick compile checks and the gtest suite.
# Requires linux-headers-$(uname -r) on the host.
build/build.sh

# aarch64 cross-build for the board, using an extracted Yocto SDK.
# PATH is the SDK install directory (the one containing
# environment-setup-*). This is the build that targets APU Linux.
build/build.sh -sdk /path/to/sdk

# Wipe build/Release/ and build/Release-aarch64/.
build/build.sh -clean
```

The `-sdk` mode is the one that produces board-ready aarch64 artifacts.
Generate the SDK from your Yocto build and extract it:

```sh
bitbake <your-image> -c populate_sdk      # produces a self-extracting *.sh
./tmp/deploy/sdk/<...>-toolchain-*.sh      # extract to e.g. /opt/<sdk> or ~/sdk
build/build.sh -sdk /opt/<sdk>
```

The SDK supplies everything the cross-build needs: the aarch64 toolchain,
the target sysroot, and — because it is built with `kernel-devsrc` — the
kernel dev sources used to build the `.ko`. `build.sh` sources the SDK's
`environment-setup-*` to discover the toolchain and sysroot, points `KDIR`
at `<target-sysroot>/usr/src/kernel`, runs a one-time `make
modules_prepare` to build the kernel's host scripts, then builds the
module, library, and examples. The example CLIs are statically linked
against `libamdrnpu.a`, so a staged binary runs on the board without
`libamdrnpu.so` needing to be on the loader path.

Outputs (per invocation):

| Path | Artifact |
|---|---|
| `build/<TYPE>/driver/amdrnpu.ko` | kernel module |
| `build/<TYPE>/lib/libamdrnpu.so*` | shared lib (+ `.a` static) |
| `build/<TYPE>/bin/amdrnpu_test` | example CLI (+ the other examples) |

where `<TYPE>` is `Release` for native and `Release-aarch64` for the
`-sdk` cross build. The module has no device-tree binding of its own:
loading the `.ko` registers an rpmsg driver, and a `miscdevice` char node
at `/dev/amdrnpu.N` appears once RPU firmware advertises the `amdrnpu`
RPMsg channel and the driver binds to it.

CMake options forwarded to the lib build:

| Option | Default | Meaning |
|---|---|---|
| `AMDRNPU_LIB_BUILD_SHARED`   | `ON` | build `libamdrnpu.so` |
| `AMDRNPU_LIB_BUILD_STATIC`   | `ON` | build `libamdrnpu.a` |
| `AMDRNPU_LIB_BUILD_EXAMPLES` | `ON` | build the example programs |
| `AMDRNPU_UAPI_DIR`           | `<repo>/include/uapi` | path to the shared UAPI headers |

The runtime library uses **no libdrm** — buffers are dma-buf objects and
completion is a `sync_file` fd, so it talks to the char device and the
dma-buf / sync_file interfaces directly. There is no external
library dependency for the build.

Downstream projects can consume the library via either CMake
(`find_package(amdrnpu)`) or pkg-config
(`pkg-config --cflags --libs amdrnpu`).

Run `build/build.sh -help` for the full flag surface (`-sdk`, `-kdir`
override, `-j`, `-no-tests`, `-unit_tests`).

### Fallback: raw kbuild / Yocto-internal

The kernel-module Makefile in `drivers/rpmsg/amdrnpu/` is still
usable directly. `meta-amdrnpu/recipes-kernel/amdrnpu-mod_0.1.0.bb`
relies on it via Yocto's `module.bbclass`; the same invocation also works
as a manual fallback:

```sh
# Native, against the running kernel's headers.
make -C drivers/rpmsg/amdrnpu KDIR=/lib/modules/$(uname -r)/build

# aarch64, against a Yocto SDK's kernel dev sources. Source the SDK
# environment first (it sets ARCH, CROSS_COMPILE, and SDKTARGETSYSROOT),
# then point KDIR at the SDK's kernel tree. A freshly extracted SDK needs
# a one-time `make -C "$KDIR" modules_prepare` before the first build.
. /path/to/sdk/environment-setup-*
make -C drivers/rpmsg/amdrnpu \
     KDIR="$SDKTARGETSYSROOT/usr/src/kernel"

make -C drivers/rpmsg/amdrnpu KDIR=... INSTALL_MOD_PATH=/tmp/rootfs \
     modules_install
make -C drivers/rpmsg/amdrnpu KDIR=... clean
```

`KERNEL_SRC` is accepted as a synonym for `KDIR` so the same Makefile
works under Yocto's `module.bbclass` without modification. Be aware that
this in-source flow writes `.o` / `.ko` / `.mod*` files into the source
directory; `build/build.sh` avoids that.

## Building under Yocto

The `meta-amdrnpu/` directory is a self-contained OE layer.

```sh
bitbake-layers add-layer /path/to/amdrnpu/meta-amdrnpu
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
AMDRNPU_SRC = "/abs/path/to/amdrnpu"
```

See [`meta-amdrnpu/README.md`](meta-amdrnpu/README.md) for more.

## Runtime usage

The lib mirrors the UAPI. A minimal end-to-end (open → alloc → submit → wait →
close) looks like:

```c
#include <amdrnpu/amdrnpu_lib.h>
#include <fcntl.h>
#include <unistd.h>

#define OP_CODE_CMD0  0x00000001u
#define SZ_16M        (16u * 1024u * 1024u)
#define SZ_8M         (8u * 1024u * 1024u)

int fd = open("/dev/amdrnpu.0", O_RDWR | O_CLOEXEC);

uint32_t in_bo, out_bo;
struct amdrnpu_cmd_submit hdr;
int32_t status = 0;

/* buf_alloc has the driver allocate from the memory type's reserved-memory
 * region (AMDRNPU_BUF_ALLOC), returning a handle for use in amdrnpu_arg.bo. */
amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, SZ_16M, &in_bo);
amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, SZ_8M, &out_bo);

struct amdrnpu_arg args[] = {
	{ .size = 0,     .value = SZ_16M },
	{ .size = SZ_16M, .bo = { .buffer_handle = (int32_t)in_bo,  .offset = 0 } },
	{ .size = 0,     .value = SZ_8M  },
	{ .size = SZ_8M,  .bo = { .buffer_handle = (int32_t)out_bo, .offset = 0 } },
};

amdrnpu_cmd_submit(fd, OP_CODE_CMD0, args, 4, &hdr, sizeof(hdr));
amdrnpu_syncobj_wait_complete(fd, hdr.sync_handle, 2000000000LL, &status);
close(hdr.sync_handle);   /* the completion handle is a sync_file fd */

amdrnpu_buf_close(fd, in_bo);
amdrnpu_buf_close(fd, out_bo);
close(fd);
```

For the raw-`ioctl` path (no library): `AMDRNPU_BUF_ALLOC` a buffer (or
`AMDRNPU_BUF_IMPORT` a dma-buf from elsewhere), build a
`struct amdrnpu_cmd_submit`, `ioctl(fd, AMDRNPU_CMD_SUBMIT, ...)`, then
`poll()` the returned `sync_handle`. The full ABI is documented in
[`include/uapi/amdrnpu/amdrnpu.h`](include/uapi/amdrnpu/amdrnpu.h).

The `lib/examples/` programs (the `amdrnpu_test` CLI and friends) exercise the
UAPI end-to-end and open `/dev/amdrnpu.N` (scanning `/dev/amdrnpu.*` when no
`--device` is given), as do the library API and the gtest unit-test suite.

See [`docs/lib-userspace-flow.md`](docs/lib-userspace-flow.md) for the
full library walkthrough and [`docs/commands.md`](docs/commands.md) for
the wire-format details.

## Design documentation

Start at [`docs/README.md`](docs/README.md) for the documentation map.
Highlights:

- [`docs/architecture.md`](docs/architecture.md) — system view, in-kernel
  block diagram, file-by-file responsibilities, rpmsg binding, the two
  DMA domains.
- [`docs/devicetree-binding.md`](docs/devicetree-binding.md) — platform
  integration: no DT binding of its own; how the rpmsg channel,
  remoteproc node, and reserved-memory regions are wired in.
- [`docs/buffer-management.md`](docs/buffer-management.md) — buffer
  lifecycle, in-flight command pinning, handle release semantics.
- [`docs/commands.md`](docs/commands.md) — `AMDRNPU_CMD_SUBMIT` wire
  format, `sync_file` / dma_fence interaction, fence error propagation.
- [`docs/lib-userspace-flow.md`](docs/lib-userspace-flow.md) — full
  library API walkthrough.

## License

This repository ships components under more than one license. Every source
file carries an SPDX `License-Identifier` header; the full license texts live
in [`LICENSE`](LICENSE) (MIT) and
[`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt) (GPL-2.0). The mapping:

| Part | License | Text |
|---|---|---|
| Kernel module `drivers/rpmsg/amdrnpu/` (incl. its `Kbuild`/`Makefile`) | GPL-2.0 | [`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt) |
| User-space library `lib/`, examples, and the CMake files | MIT | [`LICENSE`](LICENSE) |
| Build scripts — `build/build.sh`, `boards/vek385/dtbo/*.sh`, and the top-level `Makefile` | MIT | [`LICENSE`](LICENSE) |
| Yocto layer `meta-amdrnpu/` | MIT | [`meta-amdrnpu/COPYING.MIT`](meta-amdrnpu/COPYING.MIT) |
| UAPI header `include/uapi/amdrnpu/amdrnpu.h` | GPL-2.0 WITH Linux-syscall-note **OR** MIT (dual; recipient's choice) | [`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt) / [`LICENSE`](LICENSE) |

The UAPI header is deliberately dual-licensed so the MIT user-space library can
include the kernel/user ABI without GPL reach-through.
