# `rpu_app/` — Link A reference RPURuntime firmware (sources + prebuilt)

The APU-side `tessai-agent` driver and its `tessai_agent_unittest`
gtest suite are only one half of the Link A contract; every test
that pokes the `tessai-agent` rpmsg endpoint on `/dev/dri/cardN`
needs a matching slave on R5_1 that registers the `"tessai-agent"`
endpoint, replies to `fw_echo`, services the fault-injection
opcodes (`REMI` / `NOTS` / `CANC` / `BADC`), and otherwise idles.

This subtree is the **minimal source snapshot** of the
[RPURuntime](https://github.com/Xilinx/RPURuntime) tree needed to
produce that slave — `rpu_app_link_a.elf` — plus a bundled prebuilt
copy so end users do not need to clone, fetch SDKs for, or build
RPURuntime to run the Link A gtest suite.

Nothing else from RPURuntime ships here: the other test apps
(`xrt_test`, `xrt_test_rpmsg`, `bo_test`, `memory_test`, `runlist`),
the `xrt/src/core` + `xrt/src/shim` libraries used only by those
tests, and the bootloader / platform artifacts unrelated to the
upstream R52 cortexr52_2 build are all dropped. The exact provenance
of what was copied is in [Source snapshot](#source-snapshot) below.

## Layout

```text
rpu_app/
├── build.sh                            # 13-step Zephyr SDK + west bootstrap + build
├── setup.sh                            # one-time apt install for the build host
├── LICENSE                             # RPURuntime upstream license
├── rpu_app_link_a.elf                  # prebuilt slave (see Prebuilt ELF below)
├── platform/
│   ├── dts/
│   │   └── reserved.dtsi               # rsync'd into the upstream R52 board DTS by build.sh
│   ├── linker/rpu_apu_rpmsg.ld         # custom SOC linker (RSC_TABLE → apu_rsctbl)
│   └── example_design_pfm_fixed.xsa    # XSA → lopper → SDT for cortexr52_2
├── xrt/
│   └── src/driver/                     # Zephyr module: ZEPHYR_EXTRA_MODULES target.
│                                       #   - apu_rpmsg_platform.c + tessai_agent_endpoint.c
│                                       #     (link A — gated on CONFIG_TESSAI_APU_RPMSG_SLAVE;
│                                       #     TESSAI_DBG / TESSAI_ERR logging macros live at
│                                       #     the top of apu_rpmsg_platform.c)
│                                       #   - metal_init_once.c (libmetal one-shot guard,
│                                       #     gated on CONFIG_OPENAMP)
│                                       #   - include/tessai_agent_msg_def.h (wire types:
│                                       #     enum tessai_rsp_type + packed request/
│                                       #     response structs, shared with userspace
│                                       #     mirror)
│                                       #   - src/tessai_agent_endpoint.h (endpoint
│                                       #     contract: opcodes, size caps, RPU-bank
│                                       #     carveout bounds, init API)
└── tests/
    ├── common/prj.conf                 # base Kconfig: logging, heap, stacks, CPP toggle
    └── tessai_agent_test/              # build target — produces zephyr.elf
        ├── CMakeLists.txt              # ZEPHYR_EXTRA_MODULES += xrt/src/driver,
        │                               #   DTC_OVERLAY_FILE = boards/rpu.overlay,
        │                               #   zephyr_linker_sources resource_table.ld
        ├── prj.conf                    # CONFIG_TESSAI_APU_RPMSG_SLAVE=y + TESSAI_VERBOSE=y
        ├── boards/rpu.overlay          # chosen.zephyr,sram → &apu_rpmsg_fw (= ddrboot@a000100)
        ├── linker/resource_table.ld    # .resource_table → RSC_TABLE (apu_rsctbl@0x0A000000)
        └── src/main.c                  # passive: SYS_INIT brings up endpoint, main idles
```

### Link-A-only cleanup history

The snapshot was previously a near-verbatim mirror of the full
RPURuntime driver tree (Link-B XRT C++ headers, Link-B device/hwctx
/cmd/mgmt sources, the Link-B rpmsg + shmem transports, the OCM
mailbox binding plus the DT node it fed, and the legacy Link-B
logging header). All of that is now removed.
Surviving Link-A logging macros live inlined and renamed
(`TESSAI_DBG`/`TESSAI_ERR`) at the top of `apu_rpmsg_platform.c`, and
the surviving wire pieces are split across `include/tessai_agent_msg_def.h`
(packed request/response structs + `enum tessai_rsp_type`) and
`src/tessai_agent_endpoint.h` (opcodes, size caps, carveout bounds,
init API). The `#if 0` Link-B COPY_IN_OUT
forwarder bookmark inside `tessai_agent_endpoint.c` is also gone;
branch `wip/link-b-phase2b-copy-in-out-forwarder` still holds the
full state for revival.

## Prebuilt ELF

`rpu_app_link_a.elf` is bundled so `boot_vek385.py --flow link-a-suite`
works out of the box. `boot_vek385.py:LINK_A_BUNDLED_ELF_DEFAULT`
points at this exact path.

| Property | Value |
|---|---|
| sha256 | `e4df045a2c6cd7b472ab7e189d77e5d32245626f315c112985ff01900b59247e` |
| Size | 2,583,436 bytes |
| Built from | RPURuntime branch `link-a-test-suite` @ `746ae9b12f04df2a4d760714abf42e5c285d5f3c` |
| Toolchain | `zephyr-sdk-0.16.8` (`arm-zephyr-eabi`) |
| Zephyr | `xlnx_rel_v2025.2` (`zephyr-amd`, Zephyr 3.7.0 base) |
| Built | 2026-05-29 |

The bundled ELF is functionally equivalent to what
`./build.sh tests/tessai_agent_test` will produce from the sources
in this subtree, but the byte contents will differ (build timestamps,
and the in-tree sources track a slightly later RPURuntime snapshot —
see [Source snapshot](#source-snapshot)). Use the bundled ELF for
fast bring-up; rebuild only when you've made local source changes.

### Verifying the bundled ELF on the board

```bash
$ readelf -l rpu_app_link_a.elf | grep -m1 LOAD
  LOAD           0x000170 0x0a000100 0x0a000100 ...   ← must start at 0x0a000100
```

`0x0a000100` is the `apu_rpmsg_fw` carveout in the live VEK385 DTB
(injected by `rpu-fw/.../apply_tessai_patch.py`). A LOAD segment at
`0x00100000` is a JTAG-only `xrt_test`/`xrt_test_rpmsg` build and
Linux `remoteproc1` will reject it with `bad phdr da 0x100000`. See
`tests/tessai_agent_test/boards/rpu.overlay` for why.

### What's compiled in

- `"tessai-agent"` rpmsg endpoint creation (link A, OpenAMP slave).
- `Phase 1 RPURuntime up: link A tessai-agent endpoint ready` banner
  on the RPU console.
- `fw_echo` writeback into `tess_rpu_gem_carveout` with the canonical
  `rsp: command id: %u, num_args: %u\n` banner —
  `TessaiAgentCmd.FwEchoWriteback` gtest reads this back.
- All four Phase 2d fault-injection opcodes (`'REMI'`, `'NOTS'`,
  `'CANC'`, `'BADC'`) recognized in `tessai_agent_ept_cb` —
  `TessaiAgentFault.*` gtests exercise these.
- Link B (RPURuntime → rpu-fw) is **not compiled**. The Link-B rpmsg
  / shmem transports, the dispatch table they registered into, and
  the AIE hwctx/cmd `__syscall` surface were dropped in the
  Link-A-only cleanup. The `COPY_IN_OUT` opcode now memcpys locally
  on R5_1 inside `tess_rpu_gem_carveout` (`tessai_agent_endpoint.c
  tessai_copy_in_out()`); the Link-B forwarder lives on branch
  `wip/link-b-phase2b-copy-in-out-forwarder` for revival.

### Key memory regions

```text
RAM                    : 672 KiB / 1023 KiB used   (apu_rpmsg_fw carveout)
RSC_TABLE              :  88 B   /  256 B used     (apu_rsctbl, loader handshake)
APU_RPMSG_SHM          :   0     /  288 KiB         (sized by remoteproc DT)
TESS_RPU_GEM_CARVEOUT  :   0     /  512 MiB         (sized by remoteproc DT)
```

## Rebuilding from source

`./build.sh tests/tessai_agent_test` from this directory drives all
13 steps in one shot. First run is 15-25 minutes (Zephyr SDK
download, Xilinx Zephyr clone, west update, SDT generation from
the XSA via lopper). Subsequent runs are 30 s - 2 min (the script
detects `.venv` / `.west` / `zephyr` / `output_dir` and jumps
straight to `west build -p always`).

```bash
# One-time host setup (apt install — needs sudo):
./setup.sh

# Build:
./build.sh tests/tessai_agent_test

# The freshly-built ELF is at:
ls -l zephyr/build/zephyr/zephyr.elf

# To replace the bundled prebuilt copy that boot_vek385.py picks up
# under --flow link-a-suite, overwrite rpu_app_link_a.elf in-place
# and refresh the sha256 in this README:
cp zephyr/build/zephyr/zephyr.elf rpu_app_link_a.elf
sha256sum rpu_app_link_a.elf
```

Every directory listed in `.gitignore` (`.venv/`, `.west/`,
`zephyr/`, `zephyr-sdk-*/`, `lopper/`, `output_dir/`, `build/`, …)
is created by `./build.sh` and is safe to delete; `./build.sh clean`
removes them all in one go.

## Source snapshot

Snapshot pulled from RPURuntime branch `link-a-pr` at commit
`1a0a00e0fdbd9ddf829f914aa17d25fc71c83343` (HEAD at copy time),
plus the staged-but-uncommitted Link A additions on that branch.
The branch is rebased on top of the upstream `link-a-test-suite`
work that produced the bundled ELF.

The copy is a strict subset of the RPURuntime tree: only files
that participate in
`./build.sh tests/tessai_agent_test → zephyr/build/zephyr/zephyr.elf`
landed here. Bring a stricter check or pulled in additional
RPURuntime subdirectories (xrt/src/core, xrt/src/shim, alternative
test apps, bootloader artifacts) by re-copying from the source
tree if a future Link A target needs them; today they would only
be dead weight.

To refresh this snapshot from a newer RPURuntime checkout, mirror
the same minimal copy plus a fresh `rpu_app_link_a.elf` build, and
update the sha256 + commit fields above. The exact `cp` set is:

```bash
SRC=/path/to/RPURuntime
DST=/path/to/tessai-agent/tests/rpu_app

cp     "$SRC/build.sh" "$SRC/setup.sh" "$SRC/LICENSE" "$DST/"
cp     "$SRC/platform/dts/reserved.dtsi"                  "$DST/platform/dts/"
cp     "$SRC/platform/linker/rpu_apu_rpmsg.ld"            "$DST/platform/linker/"
cp     "$SRC/platform/example_design_pfm_fixed.xsa"       "$DST/platform/"
cp -a  "$SRC/xrt/include/."     "$DST/xrt/include/"
cp -a  "$SRC/xrt/src/driver/."  "$DST/xrt/src/driver/"
cp     "$SRC/tests/common/prj.conf"      "$DST/tests/common/"
cp -a  "$SRC/tests/tessai_agent_test/."  "$DST/tests/tessai_agent_test/"
```
