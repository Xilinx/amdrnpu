# VEK385 runtime DT overlay for `tessai_agent.ko`

Adds a live **`/tessai-agent`** device-tree node after boot (configfs overlay), using the
same symbol names as the amdxdna VEK385 flow.

## Files

| File | Role |
|------|------|
| `build_tessai_agent_dtbo.sh` | Generate `tessai-agent-add.dtbo` (the overlay dts is a heredoc inside this script — single source of truth, no checked-in `.dts` to drift) |
| `load_tessai_agent_dtbo.sh` | `apply` / `remove` / `status` on the board |
| `tessai-agent-add.dtbo` | Build artifact (regenerate by re-running the script above) |

## Base DTB prerequisites

The overlay resolves one **label** from the booted FDT `/__symbols__`
and two **numeric phandles** baked into the official Versal-net base DT:

| Reference | Source | Use |
|-----------|--------|-----|
| `&r5f_1` (label) | `/__symbols__` | `amd,remoteproc` on `tessai-agent` (RPU running RPURuntime link-A endpoint) |
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
[`apply_tessai_patch.py`](../../../../rpu-fw/rpu-fw-platforms/platforms/siva-sentry-mode/dts/apply_tessai_patch.py)
adds the second R52 core with that label. Until the upstream kernel DT
carries `r5f_1` natively this overlay path is primarily a reference
artifact; production boot on VEK385 uses the patcher, which injects an
equivalent `tessai-agent` node (with the same `<0x500>` / `<0x01>`
bindings) straight into the kernel DTB.

On VEK385, produce a symbol-exporting base DTB with
`/proj/xsjhdstaff5/wendlian/vek385/scripts/build_amdxdna_dt.sh` (outputs
`*-amdxdna-base.dtb` plus the `r5f_*` labels). Boot that DTB from U-Boot,
then apply this overlay on Linux.

RPU firmware must advertise the **`tessai-agent`** RPMsg channel (`tessai_agent_ept.c`).

## Build (workstation)

```bash
cd boards/vek385/dtbo
./build_tessai_agent_dtbo.sh
```

## Deploy and load (target)

```bash
cp tessai-agent-add.dtbo load_tessai_agent_dtbo.sh /root/
chmod +x /root/load_tessai_agent_dtbo.sh
/root/load_tessai_agent_dtbo.sh apply
# optional:
LOAD_MODULE=1 /root/load_tessai_agent_dtbo.sh apply
```

Check:

```bash
/root/load_tessai_agent_dtbo.sh status
ls /proc/device-tree/tessai-agent/
```

## Relation to `amdxdna-add.dtbo`

Both overlays can be applied on the same base DTB: **amdxdna** adds `/amdxdna`
(binds `r5f_0` and the RPU GEM carveout via its own references), **tessai-agent**
adds `/tessai-agent` (binds `r5f_1` plus `rpu_gem_cma@20000000` /
`buffer@50000000000` by numeric phandle). They share the RPU GEM CMA pool
(`rpu_gem_cma@20000000`, phandle `0x500`, carrying `linux,cma-default`) but
target different remoteprocs and bind different kernel drivers / RPMsg channel
names, so both can coexist on the same boot.
