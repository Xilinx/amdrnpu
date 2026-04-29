# meta-rpurt-rpc

Yocto/OpenEmbedded layer that builds the kernel module and user-space
runtime library shipped in the [rpu-rt-rpc](..) repository.

## Layout

```
meta-rpurt-rpc/
├── conf/layer.conf
├── recipes-kernel/
│   └── rpurt-rpc-mod/         # out-of-tree kernel module
└── recipes-libs/
    └── rpurt-rpc-lib/         # user-space runtime library
```

Both recipes default to `inherit externalsrc` and build directly from
the source tree that the layer ships inside (the parent directory of
`meta-rpurt-rpc/`). No tarballs, no separate fetch step.

## Adding the layer to a Yocto build

From your build directory (e.g.
`/scratch/wendy/t20/Vitis-AI-Telluride/examples/reference_design/vek385/vitis_platforms/vaiml_platform/sw/yocto/build`):

```sh
bitbake-layers add-layer /path/to/rpu-rt-rpc/meta-rpurt-rpc
```

Or edit `conf/bblayers.conf` directly:

```
BBLAYERS += " /path/to/rpu-rt-rpc/meta-rpurt-rpc "
```

If your repository checkout lives elsewhere than the layer's parent
directory, set `RPURT_RPC_SRC` in `conf/local.conf`:

```
RPURT_RPC_SRC = "/abs/path/to/rpu-rt-rpc"
```

## Building

```sh
bitbake rpurt-rpc-mod    # produces /lib/modules/.../extra/rpurt_rpc.ko
bitbake rpurt-rpc-lib    # produces librpurt_rpc.so + headers
```

To pull both into your image, add to `local.conf` (or your image
recipe):

```
IMAGE_INSTALL:append = " rpurt-rpc-mod rpurt-rpc-lib"
# Optional:
# IMAGE_INSTALL:append = " rpurt-rpc-lib-dev rpurt-rpc-lib-examples"
```

## Layer compatibility

`LAYERSERIES_COMPAT` covers `kirkstone` through `walnascar`. Add newer
release codenames as they are validated.
