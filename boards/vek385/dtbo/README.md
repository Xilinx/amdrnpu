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

The overlay resolves these labels from the **booted** FDT `/__symbols__`:

| Label | Use |
|-------|-----|
| `r5f_1` | `amd,remoteproc` on `tessai-agent` (RPU running RPURuntime link-A endpoint) |
| `tess_rpu_gem_carveout` | `bank@0` `memory-region` (512 MiB reusable @ `0x20000000`) |
| `app_bank0` | `bank@1` `memory-region` (high DDR CMA pool) |

On VEK385, produce a symbol-exporting base DTB with
`/proj/xsjhdstaff5/wendlian/vek385/scripts/build_amdxdna_dt.sh` (outputs
`*-amdxdna-base.dtb` plus labels above). Boot that DTB from U-Boot, then apply
this overlay on Linux.

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
(binds `r5f_0` and `tess_rpu_gem_carveout`), **tessai-agent** adds `/tessai-agent`
(binds `r5f_1` and `tess_rpu_gem_carveout`). They share the GEM carveout but
target different remoteprocs and bind different kernel drivers / RPMsg channel
names, so both can coexist on the same boot.
