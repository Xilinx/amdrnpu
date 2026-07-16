# meta-amdrnpu

Yocto/OpenEmbedded layer that builds the kernel module and user-space
runtime library shipped in the [amdrnpu](..) repository.

## Layout

```
meta-amdrnpu/
├── conf/layer.conf
├── recipes-kernel/
│   └── amdrnpu-mod/        # out-of-tree DRM/RPMsg kernel module
└── recipes-libs/
    └── amdrnpu-lib/        # user-space runtime library + amdrnpu_test CLI
```

Both recipes default to `inherit externalsrc` and build directly from
the source tree that the layer ships inside (the parent directory of
`meta-amdrnpu/`). No tarballs, no separate fetch step.

## Adding the layer to a Yocto build

From your build directory (e.g.
`/scratch/wendy/t20/Vitis-AI-Telluride/examples/reference_design/vek385/vitis_platforms/vaiml_platform/sw/yocto/build`):

```sh
bitbake-layers add-layer /path/to/tessai-agent/meta-amdrnpu
```

Or edit `conf/bblayers.conf` directly:

```
BBLAYERS += " /path/to/tessai-agent/meta-amdrnpu "
```

If your repository checkout lives elsewhere than the layer's parent
directory, set `AMDRNPU_SRC` in `conf/local.conf`:

```
AMDRNPU_SRC = "/abs/path/to/tessai-agent"
```

## Building

```sh
bitbake amdrnpu-mod    # produces /lib/modules/.../extra/amdrnpu.ko
bitbake amdrnpu-lib    # produces libamdrnpu.so + headers + amdrnpu_test
```

To pull both into your image, add to `local.conf` (or your image
recipe):

```
IMAGE_INSTALL:append = " amdrnpu-mod amdrnpu-lib"
# Optional dev headers and example CLI:
# IMAGE_INSTALL:append = " amdrnpu-lib-dev amdrnpu-lib-examples"
```

The library's `RRECOMMENDS` pulls in `kernel-module-amdrnpu` by
default, so installing just `amdrnpu-lib` will normally drag the
module in too.

## Layer compatibility

`LAYERSERIES_COMPAT` covers `kirkstone` through `walnascar`. Add newer
release codenames as they are validated.
