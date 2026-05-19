# meta-tessai-agent

Yocto/OpenEmbedded layer that builds the kernel module and user-space
runtime library shipped in the [tessai-agent](..) repository.

## Layout

```
meta-tessai-agent/
├── conf/layer.conf
├── recipes-kernel/
│   └── tessai-agent-mod/        # out-of-tree DRM/RPMsg kernel module
└── recipes-libs/
    └── tessai-agent-lib/        # user-space runtime library + tessai_agent_test CLI
```

Both recipes default to `inherit externalsrc` and build directly from
the source tree that the layer ships inside (the parent directory of
`meta-tessai-agent/`). No tarballs, no separate fetch step.

## Adding the layer to a Yocto build

From your build directory (e.g.
`/scratch/wendy/t20/Vitis-AI-Telluride/examples/reference_design/vek385/vitis_platforms/vaiml_platform/sw/yocto/build`):

```sh
bitbake-layers add-layer /path/to/tessai-agent/meta-tessai-agent
```

Or edit `conf/bblayers.conf` directly:

```
BBLAYERS += " /path/to/tessai-agent/meta-tessai-agent "
```

If your repository checkout lives elsewhere than the layer's parent
directory, set `TESSAI_AGENT_SRC` in `conf/local.conf`:

```
TESSAI_AGENT_SRC = "/abs/path/to/tessai-agent"
```

## Building

```sh
bitbake tessai-agent-mod    # produces /lib/modules/.../extra/tessai_agent.ko
bitbake tessai-agent-lib    # produces libtessai_agent.so + headers + tessai_agent_test
```

To pull both into your image, add to `local.conf` (or your image
recipe):

```
IMAGE_INSTALL:append = " tessai-agent-mod tessai-agent-lib"
# Optional dev headers and example CLI:
# IMAGE_INSTALL:append = " tessai-agent-lib-dev tessai-agent-lib-examples"
```

The library's `RRECOMMENDS` pulls in `kernel-module-tessai-agent` by
default, so installing just `tessai-agent-lib` will normally drag the
module in too.

## Layer compatibility

`LAYERSERIES_COMPAT` covers `kirkstone` through `walnascar`. Add newer
release codenames as they are validated.
