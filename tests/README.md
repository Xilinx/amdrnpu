# amdrnpu Link A test suite

This directory ships a single gtest binary, `amdrnpu_unittest`,
that exercises every documented Link A wire behavior of the
`amdrnpu` driver and its slave endpoint (RPURuntime on R5_1,
talking to APU userspace over rpmsg / OpenAMP). Layout and naming
mirror `testcases-v2/unittests/{common,hip}` so contributors used to
that tree don't have to relearn anything.

> Historical note: a libdrm-free C smoke binary (`src/amdrnpu_lifecycle.c`)
> covered the same wire surface in a single bottom-line PASS/FAIL
> format and lived alongside this gtest tree for a while. It was
> retired once the gtest suite reached parity (every one of its 14
> subtests has a direct `TEST_F` equivalent under `unittests/` — see
> the [coverage map](#what-each-subtest-validates) below). If you
> need it back for board bring-up on a stripped target rootfs that
> can't run a gtest binary, resurrect it from git history.

Running it end-to-end on a VEK385 confirms that:

1. The kernel `amdrnpu.ko` module is loaded.
2. The DT overlay binds the `amdrnpu` rpmsg endpoint to
   `remoteproc1` (RPURuntime on R5_1; the overlay source is the
   inline heredoc in `boards/vek385/dtbo/build_amdrnpu_dtbo.sh`).
3. RPURuntime is up on `remoteproc1` with a working Link A endpoint.
4. The full Link A wire surface works: opcode dispatch, fence-errno
   mapping, async/poll completion, in-flight pin lifetime, RPU-bank
   writeback (whole-BO and partial-window round trip via
   `fw_echo_region`), and burst submits.

No Link B traffic is exercised by this suite. The COPY_IN_OUT smoke
test (`bin/amdrnpu_inout_example`) is also a pure Link A test
in this build, because RPURuntime's `amdrnpu_copy_in_out` handler
does the memcpy locally on R5_1 (the Link B forwarder is preserved
under `#if 0` in `RPURuntime/xrt/src/driver/src/amdrnpu_endpoint.c`;
see branch `wip/link-b-phase2b-copy-in-out-forwarder` for the Link B
end-to-end path snapshot).

## Layout

```
tests/
├── CMakeLists.txt        # parent CMake: gate flag + adds unittests/ subtree
├── README.md             # this file
└── unittests/            # gtest-based unit suite
    ├── CMakeLists.txt
    ├── common/           # utest_common: KB/MB macros, buffer helpers
    ├── src/              # amdrnpu-specific helpers + test-*.cpp
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

`build/build.sh` keeps `amdrnpu_unittest` **opt-in** via
`-unit_tests` because it needs a C++17 cross toolchain plus a
libstdc++-complete sysroot (some board sysroots ship only the 64-bit
half of `bits/timesize*.h`, which would trip libstdc++'s `<chrono>`
include path on cross compilers that don't honour `CMAKE_SYSROOT`
correctly for the C++ frontend). Without `-unit_tests`, the `tests/`
phase is a no-op configure pass; pass `-no-tests` to skip even that.

```bash
# Native, gtest binary:
./build/build.sh -unit_tests

# Cross (Yocto SDK) -- build.sh uses the SDK's cross g++ and target
# sysroot automatically when -unit_tests is combined with -sdk:
./build/build.sh -sdk /path/to/sdk -unit_tests
```

Direct (CMake) callers default `AMDRNPU_BUILD_UNITTESTS=ON` in
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

### The RPU-side runtime library

The host-side gtest binary above only covers the APU half of Link A.
The matching RPU slave — the Zephyr image that registers the
`"amdrnpu"` rpmsg endpoint, replies to `fw_echo` and
`fw_echo_region`, and services the `REMI`/`NOTS`/`CANC`/`BADC`
fault-injection opcodes — is maintained as a separate component and
is **not** bundled in this repository. Build it from your RPU
firmware tree and flash the resulting `zephyr.elf` as described under
[Running on the board](#running-on-the-board).

The suite degrades gracefully against the handlers a given slave
build implements: any opcode the running endpoint doesn't recognize
is reported as a `GTEST_SKIP` rather than a failure (see
[Troubleshooting](#troubleshooting)).

## Running on the board

Prerequisites on the VEK385 target:

1. Boot a kernel built with the patched VEK385 DTB so that
   `/proc/device-tree/amdrnpu/` exists (the standalone Versal-net
   base DTB doesn't ship the amdrnpu node; `apply_amdrnpu_patch.py`
   injects it with `bank@0`/`bank@1` bound to the official
   `rpu_gem_cma@20000000` (phandle `0x500`) and `buffer@50000000000`
   (phandle `0x01`) pools by numeric phandle).
2. Push the RPU slave endpoint binary and the gtest binary. The RPU
   slave is built separately (see
   [The RPU-side runtime library](#the-rpu-side-runtime-library));
   copy its `zephyr.elf` into `/lib/firmware/` under the name you
   reference from remoteproc (here `amdrnpu_rpu.elf`).
   ```bash
   scp amdrnpu_rpu.elf root@vek385:/lib/firmware/
   scp build/Release-aarch64/bin/amdrnpu_unittest root@vek385:/usr/bin/
   ssh root@vek385 modprobe amdrnpu     # or insmod amdrnpu.ko
   ```
3. Apply the DT overlay and start `remoteproc1`:
   ```bash
   # boards/vek385/dtbo/load_amdrnpu_dtbo.sh apply  (if not auto-applied)
   echo amdrnpu_rpu.elf > /sys/class/remoteproc/remoteproc1/firmware
   echo start              > /sys/class/remoteproc/remoteproc1/state
   ```
4. Run the suite:
   ```bash
   amdrnpu_unittest
   amdrnpu_unittest --gtest_filter='AmdrnpuFence.*'
   amdrnpu_unittest --gtest_filter='-AmdrnpuFault.*'  # skip Tier B
   amdrnpu_unittest --gtest_output=xml:/tmp/results.xml
   ```

## What each subtest validates

The "C smoke (historical)" column records where each subtest came
from in the retired `amdrnpu_lifecycle.c`; the gtest column is what
`--gtest_filter` matches on today. Anything in the gtest column with
"(no equivalent)" in the C smoke column is new coverage gained by
porting to gtest.

| Area                         | C smoke (historical, `amdrnpu_lifecycle.c`) | Gtest (`amdrnpu_unittest`) | Validates |
|------------------------------|--------------------------------------------|----------------------------------|-----------|
| device discovery             | `open_amdrnpu_card` + `drm_get_name`       | `AmdrnpuBasic.DeviceNodeIsAmdrnpu` | `/dev/amdrnpu.N` opens (miscdevice; no name/version ioctl -- identity is the node path). |
| channel up                   | `test_noop_submit`                        | `AmdrnpuBasic.NoopSubmit` | Submit (num_args=0) -> syncobj SIGNALED. Channel-up, COMPLETE shape. |
| scalar args                  | `test_scalar_args_submit`                 | `AmdrnpuBasic.ScalarArgsSubmit` | Scalar `__u64` args propagate verbatim across the wire. |
| opcode dispatch              | `test_fw_echo_op_submit`                  | `AmdrnpuBasic.FwEchoOpSubmitScalarOnly` | `AMDRNPU_FW_ECHO_OP` (0x42424242) dispatch, scalar args. |
| writeback                    | `test_fw_echo_writeback`                  | `AmdrnpuCmd.FwEchoWriteback` | `fw_echo` with an RPU-bank BO at args[0] -> endpoint writes the canonical `rsp: command id: %u, num_args: %u\n` banner; sscanf verifies bytes. |
| burst submits                | `test_burst_sequential`                   | `AmdrnpuCmd.BurstSequentialSubmit` | 16 sequential submits all signal within timeout (no queue stalls / lost seqs). |
| Tier A (async/poll)          | `test_completion_via_poll_on_sync_file`   | `AmdrnpuFence.PollOnSyncFile` | Export the completion sync_file fd (`amdrnpu_syncobj_to_fence_fd`), `poll(POLLIN, 2s)` wakeup. |
| Tier A (fence query)         | `test_non_blocking_fence_status_before_after` | `AmdrnpuFence.FenceStatusBeforeAndAfter` | `SYNC_IOC_FILE_INFO` probe before+after blocking wait. |
| Tier A (busy poll)           | `test_wait_busy_poll_sleep_interval`      | `AmdrnpuFence.BusyPollSleepInterval` | 10ms-interval poll loop on `fence_status`. |
| Tier A (in-flight pin)       | `test_inflight_pin_survives_gem_close`    | `AmdrnpuFence.InflightPinSurvivesGemClose` | Buffer close (`amdrnpu_buf_close`) immediately after submit; verify writeback observable via the surviving mmap. |
| Tier A (RAII FD teardown)    | `test_unique_fd_closes_at_inner_scope_exit` | `AmdrnpuFence.UniqueFdClosesAtInnerScopeExit` | `int captured = -1; { unique_fd local{open_amdrnpu_device()}; captured = local.get(); }` then probe `captured` (`probe_fd`) and verify it returns `-EBADF` -- pins down the contract that RAII wrappers close the held fd at scope exit. |
| Tier B (fault opcodes)       | `test_fence_error_remoteio`               | `AmdrnpuFault.RemoteIo` | Op `'REMI'` -> `fence_status == -EREMOTEIO`. |
| Tier B (fault opcodes)       | `test_fence_error_notsupp`                | `AmdrnpuFault.NotSupported` | Op `'NOTS'` -> `fence_status == -EOPNOTSUPP`. |
| Tier B (fault opcodes)       | `test_fence_error_cancelled`              | `AmdrnpuFault.Cancelled` | Op `'CANC'` -> `fence_status == -ECANCELED`. |
| Tier B (fault opcodes)       | `test_invalid_rsp_type_to_remoteio`       | `AmdrnpuFault.InvalidRspTypeFallback` | Op `'BADC'` -> driver-side fallback `-EREMOTEIO` (endpoint emits out-of-range rsp_type=99). |
| partial BUF_SYNC round trip  | (no equivalent)                           | `AmdrnpuMemory.BufSyncPartialRoundTrip` | `fw_echo_region` (`'RGNE'`) round trip: partial `BUF_SYNC TO_DEVICE` window flushed -> RPU echoes 8-byte input back in the banner (proves TO_DEVICE reached memory) -> partial `BUF_SYNC FROM_DEVICE` invalidate -> banner visible (proves FROM_DEVICE reached cache); sentinels outside the window prove the partial sync didn't widen. |
| DEVICE-bank PA round trip    | (no equivalent)                           | `AmdrnpuCmd.DeviceShmemAddrRoundTrip` | `AMDRNPU_FW_OP_DEVICE_ADDR_PING` (`'DAPP'`): BO allocated in `AMDRNPU_MEMORY_DEVICE` (= `buffer@50000000000`, 4 GiB high-DDR reusable CMA pool at PA `0x500_0000_0000`, bound by numeric phandle `<0x01>` -- the official Versal-net layout leaves this pool unlabeled) at the largest size the host's CMA pool can satisfy — tries 1 GiB first, then halves down to 1 MiB on `-EINVAL`/`-ENOMEM`. The halving fallback distinguishes "CMA just fragmented" (succeeds at a smaller size) from "DEVICE bank wasn't registered at all" (every size fails -> SKIP with a dts-pointer diagnostic). Kernel patches the PA into `args[1]` and a separate small RPU-bank carveout BO into `args[0]`. RPU NEVER touches device shmem (it has no MPU window covering it) — it just `snprintf`s `"dev-ping pa=0x%016llx size=%llu seq=%u"` into the carveout. Userspace `sscanf`s the banner and asserts (a) PA inside `[0x50000000000, 0x50100000000)`, (b) `pa+size` doesn't overflow the bank, (c) echoed size == requested (whatever size won the fallback), (d) bytes past `kFwDappBannerMax` survived the pre-fill (cap-honoring sentinel). Skips cleanly when the bank looks broken or endpoint pre-dates the DAPP opcode. |
| UAPI version                 | (no equivalent)                           | `AmdrnpuDevice.AbiVersionMatchesUapiHeader` | UAPI ABI version surface (`AMDRNPU_ABI_VERSION`). |
| per-fd thread-safety         | (no equivalent)                           | `AmdrnpuDevice.ConcurrentSubmitSingleFd` | N threads × M submits on the same fd; per-file idr / xa thread-safety. |
| multi-fd                     | (no equivalent)                           | `AmdrnpuDeviceStandalone.MultipleOpenClose` | Multiple simultaneous opens of the same device, independent per-fd state. |
| multi-client open/close      | (no equivalent)                           | `AmdrnpuBasicStandalone.NoopSubmitMultiThreaded` | N threads each open their own fd, submit, close; multi-client open/close stress. |
| BO lifecycle (per-step)      | (no equivalent)                           | `AmdrnpuMemory.{BufAlloc*,BufMmap*,BufSyncBothDirectionsWholeBuffer,BufSyncPartialRange}` | Per-step BO lifecycle split out of the writeback round-trip. |
| negative API                 | (no equivalent)                           | `AmdrnpuError.*` | Negative API: invalid memory_type, zero-size alloc, bad direction, invalid handles, double-destroy, null-args-with-count, nonexistent /dev/amdrnpu.N path. |
| alloc/close stress           | (no equivalent)                           | `AmdrnpuError.BufAllocCloseStressMultiThreaded` | Tight alloc/close loop across threads; per-file xa allocation races. |

## Output

Standard gtest output: `[ RUN ]` / `[  OK  ]` / `[ FAILED ]` /
`[ SKIPPED ]` per-test, summary at the end. Exit code 0 iff every
non-skipped test passed. Use `--gtest_output=xml:results.xml` for
JUnit-style CI integration. When the driver isn't loaded the entire
suite skips cleanly (single `GTEST_SKIP` per test from the
`AmdrnpuTest` fixture) and exit code is still 0 — so the same
binary works on dev hosts without the hardware.

## Troubleshooting

- `no /dev/amdrnpu.N found` SKIP messages —
  `amdrnpu.ko` is not loaded, or the DT overlay was not applied,
  or the rpmsg channel did not bind. Check
  `dmesg | grep -E 'amdrnpu|rpmsg.*amdrnpu'`.
- Basic channel-up + scalar-args subtests PASS but `fw_echo` writeback
  or later fail — endpoint is alive but the running RPU slave build
  pre-dates the writeback / Tier-A / Tier-B handlers. Re-flash with an
  RPU slave that includes these handlers.
- `AmdrnpuMemory.BufSyncPartialRoundTrip` SKIPs with "RPU
  endpoint does not recognize 'RGNE'" — the running slave pre-dates
  the `fw_echo_region` opcode. Update the RPU slave endpoint (see
  above) and re-flash.
- Tier B fault tests (`AmdrnpuFault.*`) FAIL/`-ETIMEDOUT` while Tier
  A passes — the endpoint on the board does not recognize the
  fault-injection opcodes (older RPU slave build). Re-flash with an
  RPU slave that has all four fault handlers compiled in.
- Gtest binary fails to build with `thirdparty/googletest is missing` —
  run `git submodule update --init --recursive` once.
