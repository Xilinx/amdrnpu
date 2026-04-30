# rpu-rt-rpc

Skeleton for the **RPU runtime RPC** stack: a small Linux kernel driver
that exposes an ioctl-based channel to user space, plus a thin C library
that wraps the ioctls for application use. Designed to be built either
standalone (out-of-tree against any kernel checkout) or via Yocto using
the included meta-layer.

## Repository layout

```
rpu-rt-rpc/
├── drivers/misc/rpurt-rpc/    # kernel module (out-of-tree, rpmsg-bound)
│   ├── rpurt_rpc.c            # transparent byte pipe over rpmsg
│   ├── Kbuild
│   └── Makefile
├── include/uapi/rpurt-rpc/    # shared kernel/user ABI header
│   └── rpurt_rpc.h
├── lib/                       # user-space runtime library (CMake)
│   ├── CMakeLists.txt
│   ├── include/rpurt_rpc/rpurt_rpc.h    # public lib API
│   ├── src/rpurt_rpc.c                  # lib implementation
│   ├── src/aie_msg.h          # vendored firmware mailbox wire format,
│   │                          # PRIVATE to lib (not installed); the
│   │                          # driver is currently a transparent byte
│   │                          # pipe so all framing happens here
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

The driver is a transparent byte pipe: `RPURT_RPC_IOC_SEND` calls
`rpmsg_send()` on the bound endpoint; `RPURT_RPC_IOC_RECV` returns the
next frame the rpmsg core delivered for that endpoint. Wire framing
(message headers, opcodes, etc.) is owned by user-space.

For the AIE management mailbox, the firmware wire format is vendored
privately inside the lib in
[`lib/src/aie_msg.h`](lib/src/aie_msg.h) (not installed, not part of
the lib's public ABI); clients only see the high-level helper:

```c
#include <rpurt_rpc/rpurt_rpc.h>

rpurt_rpc_handle *h = rpurt_rpc_open_path("/dev/rpurt_rpc.0");
if (!h) { /* errno set */ }

if (rpurt_rpc_aie_echo(h, 0x12345678u, 0xDEADBEEFu) == 0)
    /* firmware echoed both values back unchanged */;

rpurt_rpc_close(h);
```

Lower-level callers can still construct their own framing and use the
generic `rpurt_rpc_send()` / `rpurt_rpc_recv()` envelope - the kernel
forwards `msg.data[0..len]` verbatim to `rpmsg_send()` on the bound
endpoint.

### Verifying against the firmware ECHO

The `rpurt_rpc_example` binary (built by default with the lib) issues a
single AIE ECHO round-trip and reports pass/fail:

```sh
# Default values 0x12345678 / 0xDEADBEEF
rpurt_rpc_example /dev/rpurt_rpc.0

# Custom values
rpurt_rpc_example /dev/rpurt_rpc.0 0xCAFEBABE 0x0BADC0DE
```

The TX bytes are byte-identical to those produced by the firmware
team's `apu_aie_test echo` tool, so the same firmware that responds to
`apu_aie_test` will respond to this binary unchanged.

## License

- Kernel module: GPL-2.0-only (see SPDX headers in
  `drivers/misc/rpurt-rpc/`).
- User-space library and meta-layer: MIT (see [`LICENSE`](LICENSE)).
