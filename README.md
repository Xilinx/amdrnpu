# rpu-rt-rpc

Skeleton for the **RPU runtime RPC** stack: a small Linux kernel driver
that exposes an ioctl-based channel to user space, plus a thin C library
that wraps the ioctls for application use. Designed to be built either
standalone (out-of-tree against any kernel checkout) or via Yocto using
the included meta-layer.

## Repository layout

```
rpu-rt-rpc/
├── drivers/misc/rpurt-rpc/    # kernel module (out-of-tree)
│   ├── rpurt_rpc.c
│   ├── Kbuild
│   └── Makefile
├── include/uapi/rpurt-rpc/    # shared kernel/user ABI header
│   └── rpurt_rpc.h
├── lib/                       # user-space runtime library (CMake)
│   ├── CMakeLists.txt
│   ├── include/rpurt_rpc/rpurt_rpc.h
│   ├── src/rpurt_rpc.c
│   ├── examples/rpurt_rpc_example.c
│   └── cmake/
└── meta-rpurt-rpc/            # Yocto/OE layer
    ├── conf/layer.conf
    ├── recipes-kernel/rpurt-rpc-mod/
    └── recipes-libs/rpurt-rpc-lib/
```

The single source of truth for the user/kernel ABI is
[`include/uapi/rpurt-rpc/rpurt_rpc.h`](include/uapi/rpurt-rpc/rpurt_rpc.h);
both the kernel module and the user-space library include it.

## Building the kernel module (standalone)

The module builds out-of-tree against any prepared kernel source/build
tree. Point `KDIR` at it.

```sh
# Native build against the running kernel
make -C drivers/misc/rpurt-rpc KDIR=/lib/modules/$(uname -r)/build

# Cross-build for arm64
make -C drivers/misc/rpurt-rpc \
    KDIR=/path/to/linux \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu-

# Install to a staging root
make -C drivers/misc/rpurt-rpc KDIR=... INSTALL_MOD_PATH=/tmp/rootfs modules_install

# Clean
make -C drivers/misc/rpurt-rpc KDIR=... clean
```

Produces `drivers/misc/rpurt-rpc/rpurt_rpc.ko`. Loading it creates
`/dev/rpurt_rpc`.

`KERNEL_SRC` is accepted as a synonym for `KDIR` so the same Makefile
works under Yocto's `module.bbclass` without modification.

## Building the user-space library (standalone)

```sh
cmake -S lib -B build/lib -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build/lib -j
sudo cmake --install build/lib
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `RPURT_RPC_BUILD_SHARED` | `ON` | build `librpurt_rpc.so` |
| `RPURT_RPC_BUILD_STATIC` | `ON` | build `librpurt_rpc.a` |
| `RPURT_RPC_BUILD_EXAMPLES` | `ON` | build `rpurt_rpc_example` |
| `RPURT_RPC_UAPI_DIR` | `<repo>/include/uapi` | path to the shared UAPI headers |

Downstream projects can consume the library via either CMake
(`find_package(rpurt_rpc)`) or pkg-config (`pkg-config --cflags --libs
rpurt_rpc`).

## Building under Yocto

The `meta-rpurt-rpc/` directory is a self-contained OE layer. From your
build directory (for example
`/scratch/wendy/t20/Vitis-AI-Telluride/examples/reference_design/vek385/vitis_platforms/vaiml_platform/sw/yocto/build`):

```sh
bitbake-layers add-layer /path/to/rpu-rt-rpc/meta-rpurt-rpc
bitbake rpurt-rpc-mod          # kernel module
bitbake rpurt-rpc-lib          # runtime library
```

To install both into an image, add to `local.conf` (or your image
recipe):

```
IMAGE_INSTALL:append = " rpurt-rpc-mod rpurt-rpc-lib"
```

The layer uses `externalsrc` so it builds directly from the in-tree
sources. If you keep the repository in a non-default location, override
the source path in `local.conf`:

```
RPURT_RPC_SRC = "/abs/path/to/rpu-rt-rpc"
```

See [`meta-rpurt-rpc/README.md`](meta-rpurt-rpc/README.md) for more.

## Runtime usage

```c
#include <rpurt_rpc/rpurt_rpc.h>

rpurt_rpc_handle *h = rpurt_rpc_open();             /* /dev/rpurt_rpc */
struct rpurt_rpc_info info;
rpurt_rpc_get_info(h, &info);

rpurt_rpc_send(h, /*opcode=*/0x1, "ping", 5);

struct rpurt_rpc_msg msg;
rpurt_rpc_recv(h, &msg);

rpurt_rpc_close(h);
```

The kernel driver in this skeleton implements a loopback transport (any
message sent with `RPURT_RPC_IOC_SEND` is queued back for
`RPURT_RPC_IOC_RECV`) so the library can be exercised end-to-end before
a real RPU mailbox/OpenAMP transport is wired in. Replace the
placeholder transport in `drivers/misc/rpurt-rpc/rpurt_rpc.c` when ready.

## License

- Kernel module: GPL-2.0-only (see SPDX headers in
  `drivers/misc/rpurt-rpc/`).
- User-space library and meta-layer: MIT (see [`LICENSE`](LICENSE)).
