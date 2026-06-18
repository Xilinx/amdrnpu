# tessai-agent Link A test suite

This directory ships a single gtest binary, `tessai_agent_unittest`,
that exercises every documented Link A wire behavior of the
`tessai-agent` driver and its slave endpoint (RPURuntime on R5_1,
talking to APU userspace over rpmsg / OpenAMP). Layout and naming
mirror `testcases-v2/unittests/{common,hip}` so contributors used to
that tree don't have to relearn anything.

> Historical note: a libdrm-free C smoke binary (`src/tessai_lifecycle.c`)
> covered the same wire surface in a single bottom-line PASS/FAIL
> format and lived alongside this gtest tree for a while. It was
> retired once the gtest suite reached parity (every one of its 14
> subtests has a direct `TEST_F` equivalent under `unittests/` — see
> the [coverage map](#what-each-subtest-validates) below). If you
> need it back for board bring-up on a stripped target rootfs that
> can't run a gtest binary, resurrect it from git history.

Running it end-to-end on a VEK385 confirms that:

1. The kernel `tessai_agent.ko` module is loaded.
2. The DT overlay binds the `tessai-agent` rpmsg endpoint to
   `remoteproc1` (RPURuntime on R5_1; the overlay source is the
   inline heredoc in `boards/vek385/dtbo/build_tessai_agent_dtbo.sh`).
3. RPURuntime is up on `remoteproc1` with a working Link A endpoint.
4. The full Link A wire surface works: opcode dispatch, fence-errno
   mapping, async/poll completion, in-flight pin lifetime, RPU-bank
   writeback (whole-BO and partial-window round trip via
   `fw_echo_region`), and burst submits.

No Link B traffic is exercised by this suite. The COPY_IN_OUT smoke
test (`bin/tessai_agent_inout_example`) is also a pure Link A test
in this build, because RPURuntime's `tessai_copy_in_out` handler
does the memcpy locally on R5_1 (the Link B forwarder is preserved
under `#if 0` in `RPURuntime/xrt/src/driver/src/tessai_agent_endpoint.c`;
see branch `wip/link-b-phase2b-copy-in-out-forwarder` for the Link B
end-to-end path snapshot).

## Layout

```
tests/
├── CMakeLists.txt        # parent CMake: gate flag + adds unittests/ subtree
├── README.md             # this file
├── rpu_app/              # RPURuntime source snapshot (Link A slave endpoint)
│   ├── README.md         # provenance / rebuild instructions
│   ├── build.sh          # `./build.sh tests/tessai_agent_test` → zephyr.elf
│   ├── rpu_app.elf       # RPURuntime slave runtime library (see rpu_app/README.md)
│   ├── platform/         # XSA, reserved.dtsi, linker fragment, dt-bindings
│   ├── src/              # src/driver Zephyr module (link A transport + endpoint)
│   └── tests/            # tessai_agent_test target + tests/common/prj.conf
└── unittests/            # gtest-based unit suite
    ├── CMakeLists.txt
    ├── common/           # utest_common: KB/MB macros, buffer helpers
    ├── src/              # tessai-specific helpers + test-*.cpp
    │   ├── common.{h,cpp}
    │   ├── test-basic.cpp   # noop/scalar submit, channel-up
    │   ├── test-device.cpp  # version probe, multi-fd open/close
    │   ├── test-error.cpp   # negative API tests (new coverage)
    │   ├── test-memory.cpp  # BO alloc/mmap/buf_sync (incl. partial round trip)
    │   ├── test-cmd.cpp     # fw_echo writeback + burst sequential
    │   ├── test-fence.cpp   # sync_file poll, busy_poll, in-flight pin, nested-scope capture
    │   └── test-fault.cpp   # REMI/NOTS/CANC/BADC fence-errno mapping
    └── thirdparty/
        └── googletest/   # vendored gtest v1.17.0 (git submodule)
```

The gtest submodule must be initialised once:

```bash
git submodule update --init --recursive
```

## Building

`build/build.sh` keeps `tessai_agent_unittest` **opt-in** via
`-unit_tests` because it needs a C++17 cross toolchain plus a
libstdc++-complete sysroot (some board sysroots ship only the 64-bit
half of `bits/timesize*.h`, which would trip libstdc++'s `<chrono>`
include path on cross compilers that don't honour `CMAKE_SYSROOT`
correctly for the C++ frontend). Without `-unit_tests`, the `tests/`
phase is a no-op configure pass; pass `-no-tests` to skip even that.

```bash
# Native, gtest binary:
./build/build.sh -unit_tests

# Cross (Yocto):
./build/build.sh -cross-yocto /path/to/yocto/build -unit_tests

# Cross (Vitis) -- build.sh also sets CMAKE_CXX_COMPILER to the
# matching aarch64-linux-gnu-g++ automatically when -unit_tests is
# combined with -cross-vitis:
./build/build.sh -cross-vitis -sysroot /path/to/rootfs -unit_tests
```

Direct (CMake) callers default `TESSAI_AGENT_BUILD_UNITTESTS=ON` in
`tests/CMakeLists.txt` so a plain `cmake -S tests` developer flow
picks the gtest binary up automatically:

```bash
cmake -S tests -B build/tests-native
cmake --build build/tests-native -j
ctest --test-dir build/tests-native --output-on-failure

# Or pin the gtest subtree only:
cmake -S tests/unittests -B build/utest-native
cmake --build build/utest-native -j
ctest --test-dir build/utest-native --output-on-failure
```

### Building the RPU-side runtime library

The host-side gtest binary above only covers the APU half of Link A.
The matching RPU slave — the Zephyr image that registers the
`"tessai-agent"` rpmsg endpoint, replies to `fw_echo` and
`fw_echo_region`, and services the `REMI`/`NOTS`/`CANC`/`BADC`
fault-injection opcodes — ships as
`rpu_app/rpu_app.elf` so the manual recipe under
[Running on the board](#running-on-the-board) works out of the box
without an external RPURuntime clone.

When you need to rebuild it (driver source changes under
`rpu_app/src/driver/`, a new fault opcode in
`tessai_agent_endpoint.c`, sharper debug printk in
`apu_rpmsg_platform.c`, etc.) the `rpu_app/` subtree is fully
self-contained — no external RPURuntime clone required:

```bash
cd tests/rpu_app

# One-time host setup on a fresh build machine (apt install, needs sudo):
./setup.sh

# Build the slave ELF. First run is 15-25 min (Zephyr SDK download,
# xlnx_rel_v2025.2 clone, west update, lopper SDT gen from the XSA).
# Subsequent runs are 30 s - 2 min (script auto-detects .venv / .west
# / zephyr / output_dir and jumps straight to `west build -p always`).
./build.sh tests/tessai_agent_test

# Fresh ELF lands here:
ls -l zephyr/build/zephyr/zephyr.elf

```

Layout, what was/wasn't copied from RPURuntime, and the exact `cp`
set to refresh the snapshot from a newer upstream checkout are in
`rpu_app/README.md`. `rpu_app/.gitignore` keeps all the build
artifacts (`.venv/`, `.west/`, `zephyr/`, `zephyr-sdk-*/`, `lopper/`,
`output_dir/`, `build/`, …) out of the tessai-agent repo, so a stray
`./build.sh` won't accidentally commit hundreds of MB of SDK state.

## Running on the board

Prerequisites on the VEK385 target:

1. Boot a kernel built with the patched VEK385 DTB so that
   `/proc/device-tree/tessai-agent/` exists (the standalone Versal-net
   base DTB doesn't ship the tessai-agent node; `apply_tessai_patch.py`
   injects it with `bank@0`/`bank@1` bound to the official
   `rpu_gem_cma@20000000` (phandle `0x500`) and `buffer@50000000000`
   (phandle `0x01`) pools by numeric phandle).
2. Push the RPURuntime endpoint binary and the gtest binary.
   `rpu_app.elf` is the in-tree slave endpoint; rebuild via
   `cd tests/rpu_app && ./build.sh tests/tessai_agent_test` and
   substitute `tests/rpu_app/zephyr/build/zephyr/zephyr.elf` if you
   need local source changes picked up
   (see [Building the RPU-side runtime library](#building-the-rpu-side-runtime-library)).
   ```bash
   scp tests/rpu_app/rpu_app.elf root@vek385:/lib/firmware/
   scp build/Release-aarch64/bin/tessai_agent_unittest root@vek385:/usr/bin/
   ssh root@vek385 modprobe tessai_agent     # or insmod tessai_agent.ko
   ```
3. Apply the DT overlay and start `remoteproc1`:
   ```bash
   # boards/vek385/dtbo/load_tessai_agent_dtbo.sh apply  (if not auto-applied)
   echo rpu_app.elf > /sys/class/remoteproc/remoteproc1/firmware
   echo start              > /sys/class/remoteproc/remoteproc1/state
   ```
4. Run the suite:
   ```bash
   tessai_agent_unittest
   tessai_agent_unittest --gtest_filter='TessaiAgentFence.*'
   tessai_agent_unittest --gtest_filter='-TessaiAgentFault.*'  # skip Tier B
   tessai_agent_unittest --gtest_output=xml:/tmp/results.xml
   ```

## What each subtest validates

The "C smoke (historical)" column records where each subtest came
from in the retired `tessai_lifecycle.c`; the gtest column is what
`--gtest_filter` matches on today. Anything in the gtest column with
"(no equivalent)" in the C smoke column is new coverage gained by
porting to gtest.

| Area                         | C smoke (historical, `tessai_lifecycle.c`) | Gtest (`tessai_agent_unittest`) | Validates |
|------------------------------|--------------------------------------------|----------------------------------|-----------|
| device discovery             | `open_tessai_card` + `drm_get_name`       | `TessaiAgentBasic.DriverNameIsTessaiAgent` | `/dev/dri/cardN` opens; `DRM_IOCTL_VERSION.name == "tessai_agent"`. |
| channel up                   | `test_noop_submit`                        | `TessaiAgentBasic.NoopSubmit` | Submit (num_args=0) -> syncobj SIGNALED. Channel-up, COMPLETE shape. |
| scalar args                  | `test_scalar_args_submit`                 | `TessaiAgentBasic.ScalarArgsSubmit` | Scalar `__u64` args propagate verbatim across the wire. |
| opcode dispatch              | `test_fw_echo_op_submit`                  | `TessaiAgentBasic.FwEchoOpSubmitScalarOnly` | `TESSAI_FW_ECHO_OP` (0x42424242) dispatch, scalar args. |
| writeback                    | `test_fw_echo_writeback`                  | `TessaiAgentCmd.FwEchoWriteback` | `fw_echo` with an RPU-bank BO at args[0] -> endpoint writes the canonical `rsp: command id: %u, num_args: %u\n` banner; sscanf verifies bytes. |
| burst submits                | `test_burst_sequential`                   | `TessaiAgentCmd.BurstSequentialSubmit` | 16 sequential submits all signal within timeout (no queue stalls / lost seqs). |
| Tier A (async/poll)          | `test_completion_via_poll_on_sync_file`   | `TessaiAgentFence.PollOnSyncFile` | `DRM_SYNCOBJ_HANDLE_TO_FD` + `EXPORT_SYNC_FILE`, `poll(POLLIN, 2s)` wakeup. |
| Tier A (fence query)         | `test_non_blocking_fence_status_before_after` | `TessaiAgentFence.FenceStatusBeforeAndAfter` | `SYNC_IOC_FILE_INFO` probe before+after blocking wait. |
| Tier A (busy poll)           | `test_wait_busy_poll_sleep_interval`      | `TessaiAgentFence.BusyPollSleepInterval` | 10ms-interval poll loop on `fence_status`. |
| Tier A (in-flight pin)       | `test_inflight_pin_survives_gem_close`    | `TessaiAgentFence.InflightPinSurvivesGemClose` | `GEM_CLOSE` immediately after submit; verify writeback observable via the surviving mmap. |
| Tier A (RAII FD teardown)    | `test_unique_fd_closes_at_inner_scope_exit` | `TessaiAgentFence.UniqueFdClosesAtInnerScopeExit` | `int captured = -1; { unique_fd local{open_drm()}; captured = local.get(); }` then exercise `captured` with `DRM_IOCTL_VERSION` and verify it returns `-EBADF` -- pins down the contract that RAII wrappers close the held fd at scope exit. |
| Tier B (fault opcodes)       | `test_fence_error_remoteio`               | `TessaiAgentFault.RemoteIo` | Op `'REMI'` -> `fence_status == -EREMOTEIO`. |
| Tier B (fault opcodes)       | `test_fence_error_notsupp`                | `TessaiAgentFault.NotSupported` | Op `'NOTS'` -> `fence_status == -EOPNOTSUPP`. |
| Tier B (fault opcodes)       | `test_fence_error_cancelled`              | `TessaiAgentFault.Cancelled` | Op `'CANC'` -> `fence_status == -ECANCELED`. |
| Tier B (fault opcodes)       | `test_invalid_rsp_type_to_remoteio`       | `TessaiAgentFault.InvalidRspTypeFallback` | Op `'BADC'` -> driver-side fallback `-EREMOTEIO` (endpoint emits out-of-range rsp_type=99). |
| partial BUF_SYNC round trip  | (no equivalent)                           | `TessaiAgentMemory.BufSyncPartialRoundTrip` | `fw_echo_region` (`'RGNE'`) round trip: partial `BUF_SYNC TO_DEVICE` window flushed -> RPU echoes 8-byte input back in the banner (proves TO_DEVICE reached memory) -> partial `BUF_SYNC FROM_DEVICE` invalidate -> banner visible (proves FROM_DEVICE reached cache); sentinels outside the window prove the partial sync didn't widen. |
| DEVICE-bank PA round trip    | (no equivalent)                           | `TessaiAgentCmd.DeviceShmemAddrRoundTrip` | `TESSAI_FW_OP_DEVICE_ADDR_PING` (`'DAPP'`): BO allocated in `TESSAI_MEMORY_DEVICE` (= `buffer@50000000000`, 4 GiB high-DDR reusable CMA pool at PA `0x500_0000_0000`, bound by numeric phandle `<0x01>` -- the official Versal-net layout leaves this pool unlabeled) at the largest size the host's CMA pool can satisfy — tries 1 GiB first, then halves down to 1 MiB on `-EINVAL`/`-ENOMEM`. The halving fallback distinguishes "CMA just fragmented" (succeeds at a smaller size) from "DEVICE bank wasn't registered at all" (every size fails -> SKIP with a dts-pointer diagnostic). Kernel patches the PA into `args[1]` and a separate small RPU-bank carveout BO into `args[0]`. RPU NEVER touches device shmem (it has no MPU window covering it) — it just `snprintf`s `"dev-ping pa=0x%016llx size=%llu seq=%u"` into the carveout. Userspace `sscanf`s the banner and asserts (a) PA inside `[0x50000000000, 0x50100000000)`, (b) `pa+size` doesn't overflow the bank, (c) echoed size == requested (whatever size won the fallback), (d) bytes past `kFwDappBannerMax` survived the pre-fill (cap-honoring sentinel). Skips cleanly when the bank looks broken or endpoint pre-dates the DAPP opcode. |
| UAPI version                 | (no equivalent)                           | `TessaiAgentDevice.{AbiVersionMatchesUapiHeader,DrmVersionPopulated}` | UAPI ABI version + `DRM_IOCTL_VERSION` triple. |
| per-fd thread-safety         | (no equivalent)                           | `TessaiAgentDevice.ConcurrentSubmitSingleFd` | N threads × M submits on the same fd; per-file idr / xa thread-safety. |
| multi-fd                     | (no equivalent)                           | `TessaiAgentDeviceStandalone.MultipleOpenClose` | Multiple simultaneous opens of the same card, independent per-fd state. |
| multi-client open/close      | (no equivalent)                           | `TessaiAgentBasicStandalone.NoopSubmitMultiThreaded` | N threads each open their own fd, submit, close; multi-client open/close stress. |
| BO lifecycle (per-step)      | (no equivalent)                           | `TessaiAgentMemory.{BufAlloc*,BufMmap*,BufSyncBothDirectionsWholeBuffer,BufSyncPartialRange}` | Per-step BO lifecycle split out of the writeback round-trip. |
| negative API                 | (no equivalent)                           | `TessaiAgentError.*` | Negative API: invalid memory_type, zero-size alloc, bad direction, bad offset, invalid handles, double-destroy, lying num_args, nonexistent /dev/dri path. |
| alloc/close stress           | (no equivalent)                           | `TessaiAgentError.BufAllocCloseStressMultiThreaded` | Tight alloc/close loop across threads; per-file xa allocation races. |

## Output

Standard gtest output: `[ RUN ]` / `[  OK  ]` / `[ FAILED ]` /
`[ SKIPPED ]` per-test, summary at the end. Exit code 0 iff every
non-skipped test passed. Use `--gtest_output=xml:results.xml` for
JUnit-style CI integration. When the driver isn't loaded the entire
suite skips cleanly (single `GTEST_SKIP` per test from the
`TessaiAgentTest` fixture) and exit code is still 0 — so the same
binary works on dev hosts without the hardware.

## Troubleshooting

- `tessai-agent DRM card not found` SKIP messages —
  `tessai_agent.ko` is not loaded, or the DT overlay was not applied,
  or the rpmsg channel did not bind. Check
  `dmesg | grep -E 'tessai-agent|rpmsg.*tessai'`.
- Basic channel-up + scalar-args subtests PASS but `fw_echo` writeback
  or later fail — endpoint is alive but the running RPURuntime build
  pre-dates the writeback / Tier-A / Tier-B handlers. Re-flash with
  the endpoint from `tests/rpu_app/`.
- `TessaiAgentMemory.BufSyncPartialRoundTrip` SKIPs with "RPU
  endpoint does not recognize 'RGNE'" — the running slave pre-dates
  the `fw_echo_region` opcode. Rebuild from `tests/rpu_app/` (see
  above) and re-flash.
- Tier B fault tests (`TessaiAgentFault.*`) FAIL/`-ETIMEDOUT` while Tier
  A passes — the endpoint on the board does not recognize the
  fault-injection opcodes (older RPURuntime build). Re-flash with the
  bundled ELF, which has all four fault handlers compiled in.
- Gtest binary fails to build with `thirdparty/googletest is missing` —
  run `git submodule update --init --recursive` once.
