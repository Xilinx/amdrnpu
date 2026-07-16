# amdrnpu_xrt_bist_example

APU-side end-to-end test for **`tests/xrt_apu_rpu_test`** on RPU_APP.

Submits **`APU_XRT_OP_BIST_NPU_LATENCY`** (`'XRTN'`, `0x5852544e`) with
an ELF physical address/size and iteration count, waits for RPU_APP to run
`bist::npu_latency()` over XRT Lite (Link B → RPU_FW), then reports completion
status and optional latency writeback.

## ELF delivery modes

| Mode | How | args[1] on wire |
|------|-----|-----------------|
| `--elf-file PATH` | Tool reads ELF file, allocates RPU BO, copies data in, flushes cache. Driver resolves BO → DMA PA sent to RPU. | DMA PA of the BO |
| `--elf-pa PA --elf-size SZ` | Pre-loaded ELF; pass PA/size as scalars. | Scalar PA |
| *(neither)* | RPU_APP uses its built-in defaults (`nop.elf @ 0x20100000`). | `0` (RPU default) |

## Opcode / wire args

| Arg | Meaning |
|-----|---------|
| `args[0]` | Iteration count (scalar, default 100) |
| `args[1]` | ELF physical address — BO (from `--elf-file`) or scalar PA (from `--elf-pa`) or 0 (RPU default) |
| `args[2]` | ELF size in bytes (scalar; 0 → RPU default 15376) |
| `args[3]` | Optional RPU BO for `apu_xrt_bist_result` writeback |

Response: `amdrnpu_wire_rsp.command_rsp_type` → DRM syncobj fence status
(`1` = COMPLETE, negative = REMOTEIO / etc.).

## Target firmware

Build and load RPU_APP from RPU2:

```bash
cd RPU2 && ./build.sh tests/xrt_apu_rpu_test
```

Boot **RPU_FW**.  No pre-loading needed when using `--elf-file`; the APU
copies the ELF into a carveout BO and the driver passes its DMA PA to RPU.

If using `--elf-pa`, pre-load the ELF in the carveout via XSDB:

```tcl
dow -data nop.elf 0x20100000
```

## Linux

- `amdrnpu.ko` loaded
- Open **`/dev/dri/cardN`** (primary node for result BO mmap)

## Build

Built with the amdrnpu lib when examples are enabled:

```bash
cd tessai-agent/lib && cmake -B build && cmake --build build
```

Binary: `build/amdrnpu_xrt_bist_example`

## Run

```bash
# Load ELF from file (recommended — no pre-loading step)
amdrnpu_xrt_bist_example --elf-file /path/to/nop.elf

# Pass pre-loaded ELF physical address directly
amdrnpu_xrt_bist_example --elf-pa 0x20100000 --elf-size 15376

# Use RPU_APP built-in defaults (nop.elf must already be at 0x20100000)
amdrnpu_xrt_bist_example

# Full options
amdrnpu_xrt_bist_example --device /dev/dri/card1 --elf-file /path/to/nop.elf --iterations 50
```

Success prints `completion: fence_st=1 (COMPLETE)` and `PASS`.
