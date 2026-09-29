# `rpu_app/` — RPURuntime library (sources)

The APU-side `amdrnpu` driver and its `amdrnpu_unittest`
gtest suite are only one half of the Link A contract; every test
that pokes the `amdrnpu` rpmsg endpoint on `/dev/amdrnpu.N`
needs a matching slave on R5_1 that registers the `"amdrnpu"`
endpoint, replies to `fw_echo`, services the fault-injection
opcodes (`REMI` / `NOTS` / `CANC` / `BADC`), and otherwise idles.

This subtree is the **minimal source snapshot** of the
[RPURuntime](https://github.com/Xilinx/RPURuntime) tree needed to
produce that slave runtime library. The subtree is fully self-contained —
no external RPURuntime clone required.

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
├── platform/
│   ├── dts/
│   │   └── reserved.dtsi               # rsync'd into the upstream R52 board DTS by build.sh
│   ├── linker/rpu_apu_rpmsg.ld         # custom SOC linker (RSC_TABLE → apu_rsctbl)
│   └── example_design_pfm_fixed.xsa    # XSA → lopper → SDT for cortexr52_2
├── src/
│   └── driver/                         # Zephyr module: ZEPHYR_EXTRA_MODULES target.
│                                       #   - apu_rpmsg_platform.c + amdrnpu_endpoint.c
│                                       #     (link A — gated on CONFIG_AMDRNPU_APU_RPMSG_SLAVE;
│                                       #     AMDRNPU_DBG / AMDRNPU_ERR logging macros live at
│                                       #     the top of apu_rpmsg_platform.c)
│                                       #   - metal_init_once.c (libmetal one-shot guard,
│                                       #     gated on CONFIG_OPENAMP)
│                                       #   - include/amdrnpu_msg_def.h (wire types:
│                                       #     enum amdrnpu_rsp_type + packed request/
│                                       #     response structs, shared with userspace
│                                       #     mirror)
│                                       #   - src/amdrnpu_endpoint.h (endpoint
│                                       #     contract: opcodes, size caps, RPU-bank
│                                       #     carveout bounds, init API)
└── tests/
    ├── common/prj.conf                 # base Kconfig: logging, heap, stacks, CPP toggle
    └── amdrnpu/              # build target — produces zephyr.elf
        ├── CMakeLists.txt              # ZEPHYR_EXTRA_MODULES += src/driver,
        │                               #   DTC_OVERLAY_FILE = boards/rpu.overlay,
        │                               #   zephyr_linker_sources resource_table.ld
        ├── prj.conf                    # CONFIG_AMDRNPU_APU_RPMSG_SLAVE=y + AMDRNPU_VERBOSE=y
        ├── boards/rpu.overlay          # chosen.zephyr,sram → &apu_rpmsg_fw (= ddrboot@a000000)
        ├── linker/resource_table.ld    # .resource_table → RSC_TABLE (apu_rsctbl, OCM Bank 1 @0xBBF80000)
        └── src/main.c                  # passive: SYS_INIT brings up endpoint, main idles
```

### Link-A-only cleanup history

The snapshot was previously a near-verbatim mirror of the full
RPURuntime driver tree (Link-B XRT C++ headers, Link-B device/hwctx
/cmd/mgmt sources, the Link-B rpmsg + shmem transports, the OCM
mailbox binding plus the DT node it fed, and the legacy Link-B
logging header). All of that is now removed.
Surviving Link-A logging macros live inlined and renamed
(`AMDRNPU_DBG`/`AMDRNPU_ERR`) at the top of `apu_rpmsg_platform.c`, and
the surviving wire pieces are split across `include/amdrnpu_msg_def.h`
(packed request/response structs + `enum amdrnpu_rsp_type`) and
`src/amdrnpu_endpoint.h` (opcodes, size caps, carveout bounds,
init API). The `#if 0` Link-B COPY_IN_OUT
forwarder bookmark inside `amdrnpu_endpoint.c` is also gone;

### What's compiled in

- `"amdrnpu"` rpmsg endpoint creation (link A, OpenAMP slave).
- `fw_echo` writeback into `tess_rpu_gem_carveout` with the canonical
  `rsp: command id: %u, num_args: %u\n` banner —
  `AmdrnpuCmd.FwEchoWriteback` gtest reads this back.
- All four fault-injection opcodes (`'REMI'`, `'NOTS'`,
  `'CANC'`, `'BADC'`) recognized in `amdrnpu_ept_cb` —
  `AmdrnpuFault.*` gtests exercise these.
- Link B (RPURuntime → rpu-fw) is **not compiled**. The Link-B rpmsg
  / shmem transports, the dispatch table they registered into, and
  the AIE hwctx/cmd `__syscall` surface were dropped in the
  Link-A-only cleanup. The `COPY_IN_OUT` opcode now memcpys locally
  on R5_1 inside `tess_rpu_gem_carveout` (`amdrnpu_endpoint.c
  amdrnpu_copy_in_out()`).

### Key memory regions

```text
RAM                    : 674 KiB / 1024 KiB used   (apu_rpmsg_fw carveout, DDR @0x0A000000)
RSC_TABLE              :  88 B   /    4 KiB         (apu_rsctbl, OCM Bank 1 @0xBBF80000)
APU_RPMSG_SHM          :   0     /  288 KiB         (vrings+buffer, OCM Bank 1 @0xBBF81000)
TESS_RPU_GEM_CARVEOUT  :   0     /  512 MiB         (sized by remoteproc DT)
```

## Rebuilding from source

`./build.sh tests/amdrnpu` from this directory drives all
13 steps in one shot. First run is 15-25 minutes (Zephyr SDK
download, Xilinx Zephyr clone, west update, SDT generation from
the XSA via lopper). Subsequent runs are 30 s - 2 min (the script
detects `.venv` / `.west` / `zephyr` / `output_dir` and jumps
straight to `west build -p always`).

```bash
# One-time host setup (apt install — needs sudo):
./setup.sh

# Build:
./build.sh tests/amdrnpu

# The freshly-built ELF is at:
ls -l zephyr/build/zephyr/zephyr.elf

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
`./build.sh tests/amdrnpu → zephyr/build/zephyr/zephyr.elf`
landed here. Bring a stricter check or pulled in additional
RPURuntime subdirectories (xrt/src/core, xrt/src/shim, alternative
test apps, bootloader artifacts) by re-copying from the source
tree if a future Link A target needs them; today they would only
be dead weight.

To refresh this snapshot from a newer RPURuntime checkout, mirror
the same minimal copy. The exact `cp` set is:

```bash
SRC=/path/to/RPURuntime
DST=/path/to/tessai-agent/tests/rpu_app

cp     "$SRC/build.sh" "$SRC/setup.sh" "$SRC/LICENSE" "$DST/"
cp     "$SRC/platform/dts/reserved.dtsi"                  "$DST/platform/dts/"
cp     "$SRC/platform/linker/rpu_apu_rpmsg.ld"            "$DST/platform/linker/"
cp     "$SRC/platform/example_design_pfm_fixed.xsa"       "$DST/platform/"
cp -a  "$SRC/xrt/src/driver/."  "$DST/src/driver/"
cp     "$SRC/tests/common/prj.conf"      "$DST/tests/common/"
cp -a  "$SRC/tests/amdrnpu/."  "$DST/tests/amdrnpu/"
```
