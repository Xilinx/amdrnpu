# VEK385 runtime DT overlay for `amdrnpu.ko`

Adds a live **`/amdrnpu`** device-tree node after boot (configfs overlay), using the
same symbol names as the amdxdna VEK385 flow.

## Files

| File | Role |
|------|------|
| `build_amdrnpu_dtbo.sh` | Generate `amdrnpu-add.dtbo` (the overlay dts is a heredoc inside this script — single source of truth, no checked-in `.dts` to drift) |
| `load_amdrnpu_dtbo.sh` | `apply` / `remove` / `status` on the board |
| `amdrnpu-add.dtbo` | Build artifact (regenerate by re-running the script above) |

## Base DTB prerequisites

The overlay resolves one **label** from the booted FDT `/__symbols__`
and two **numeric phandles** baked into the official Versal-net base DT:

| Reference | Source | Use |
|-----------|--------|-----|
| `&r5f_1` (label) | `/__symbols__` | `amd,remoteproc` on `amdrnpu` (RPU running RPURuntime link-A endpoint) |
| `<0x500>` (phandle) | `/reserved-memory/rpu_gem_cma@20000000` | `bank@0` `memory-region` (512 MiB reusable @ `0x20000000`, also carries `linux,cma-default`) |
| `<0x01>` (phandle) | `/reserved-memory/buffer@50000000000` | `bank@1` `memory-region` (4 GiB high-DDR reusable pool @ `0x500_0000_0000`) |

The pool nodes are intentionally **unlabeled** in the official Versal-net
base DT — only the explicit `phandle = <0x500>` / `phandle = <0x01>`
properties make them addressable. The overlay therefore binds the banks
by numeric phandle, not by `&label`. Phandle values `0x500` and `0x01`
are part of the Versal-net SoC-level DT contract (see
[`docs/devicetree-binding.md` §4](../../../docs/devicetree-binding.md#4-reserved-memory-companion-nodes)).

The `r5f_1` label is the only `/__symbols__` dependency. The standalone
Versal-net base DT does not export it today — only
[`apply_amdrnpu_patch.py`](../../../../rpu-fw/rpu-fw-platforms/platforms/siva-sentry-mode/dts/apply_amdrnpu_patch.py)
adds the second R52 core with that label. Until the upstream kernel DT
carries `r5f_1` natively this overlay path is primarily a reference
artifact; production boot on VEK385 uses the patcher, which injects an
equivalent `amdrnpu` node (with the same `<0x500>` / `<0x01>`
bindings) straight into the kernel DTB.

On VEK385, produce a symbol-exporting base DTB using the `build_amdxdna_dt.sh`
script from the amdxdna board support package (outputs `*-amdxdna-base.dtb`
plus the `r5f_*` labels). Boot that DTB from U-Boot, then apply this overlay
on Linux.

RPU firmware must advertise the **`amdrnpu`** RPMsg channel (`amdrnpu_ept.c`).

## Build (workstation)

```bash
cd boards/vek385/dtbo
./build_amdrnpu_dtbo.sh
```

## Deploy and load (target)

```bash
cp amdrnpu-add.dtbo load_amdrnpu_dtbo.sh /root/
chmod +x /root/load_amdrnpu_dtbo.sh
/root/load_amdrnpu_dtbo.sh apply
# optional:
LOAD_MODULE=1 /root/load_amdrnpu_dtbo.sh apply
```

Check:

```bash
/root/load_amdrnpu_dtbo.sh status
ls /proc/device-tree/amdrnpu/
```

## Relation to `amdxdna-add.dtbo`

Both overlays can be applied on the same base DTB: **amdxdna** adds `/amdxdna`
(binds `r5f_0` and the RPU GEM carveout via its own references), **amdrnpu**
adds `/amdrnpu` (binds `r5f_1` plus `rpu_gem_cma@20000000` /
`buffer@50000000000` by numeric phandle). They share the RPU GEM CMA pool
(`rpu_gem_cma@20000000`, phandle `0x500`, carrying `linux,cma-default`) but
target different remoteprocs and bind different kernel drivers / RPMsg channel
names, so both can coexist on the same boot.
