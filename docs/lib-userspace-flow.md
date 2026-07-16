# Userspace library API flow (`libamdrnpu`)

This page matches the **installed** helpers in
[`lib/include/amdrnpu/amdrnpu_lib.h`](../lib/include/amdrnpu/amdrnpu_lib.h)
and
[`lib/src/amdrnpu_lib.c`](../lib/src/amdrnpu_lib.c).
The **ABI** (structs, ioctl numbers) lives in
[`include/uapi/amdrnpu/amdrnpu.h`](../include/uapi/amdrnpu/amdrnpu.h).

Applications may call **raw ioctls** instead; the library only reduces
boilerplate (buffer packing, timed **`DRM_SYNCOBJ_WAIT`**, **`SYNC_IOC_FILE_INFO`**
probes, automatic **`DRM_IOCTL_SYNCOBJ_DESTROY`** on terminal fence from
**`…_wait_complete`** / **`…_wait_busy_poll`**).

**Prerequisite:** open the correct DRM render node (`open("/dev/dri/renderDn", …)`)
whose driver name matches `AMDRNPU_DRIVER_NAME` (`"amdrnpu"`), usually
via `drmGetVersion()` or `drmGetDevices2()`.

See also [`buffer-management.md`](buffer-management.md) and
[`commands.md`](commands.md) for kernel-side behavior.

---

## 1. Buffer flow (allocation / mmap / release)

Allocate with **`amdrnpu_buf_alloc()`** (`AMDRNPU_BUF_ALLOC`). Map with
**`amdrnpu_buf_mmap()`** (page-aligned offset/length; partial ranges OK) or
the raw **`DRM_IOCTL_MODE_MAP_DUMB`** + **`mmap`** path on a **card** fd.
Before device DMA reads CPU-written data (and after device DMA writes data the
CPU will read), call **`amdrnpu_buf_sync()`** or the
**`amdrnpu_buf_sync_to_device()`** / **`…_from_device()`** helpers
(**`AMDRNPU_BUF_SYNC`**; see [buffer-management §3](buffer-management.md#3-cache-sync-amdrnpu_buf_sync)).

When the CPU no longer needs the handle, call **`amdrnpu_buf_close()`** —
a thin wrapper around **`DRM_IOCTL_GEM_CLOSE`**. If a submitted command still
pins the GEM, the object stays alive until that command completes (see
[buffer-management §5](buffer-management.md#5-in-flight-command-pins)).

```mermaid
sequenceDiagram
    autonumber
    participant App
    participant Lib as amdrnpu_lib
    participant DRM as DRM core / tess GEM
    participant K as amdrnpu.ko

    App->>Lib: amdrnpu_buf_alloc(drm_fd, memory_type, size, &handle)
    Lib->>K: ioctl(AMDRNPU_BUF_ALLOC, amdrnpu_buf_alloc)
    K-->>Lib: drm_handle filled
    Lib-->>App: 0

    App->>Lib: amdrnpu_buf_mmap(drm_fd, handle, buf_size, off, len, &map)
    Lib->>DRM: MODE_MAP_DUMB + mmap(fd, off + map_dumb_offset)
    App->>Lib: amdrnpu_buf_sync_to_device(...) before submit (when needed)
    App->>Lib: amdrnpu_buf_close(drm_fd, handle) when handle no longer needed
```

| `memory_type` | Bank | Typical use |
|----------------|------|----------------|
| `AMDRNPU_MEMORY_RPU` (`0`) | `bank@0` (`AMDRNPU_BANK_RPU`) | R5F-accessible DDR carveout for firmware |
| `AMDRNPU_MEMORY_DEVICE` (`1`, `AMDRNPU_MEMORY_DEFAULT`) | `bank@1` (`AMDRNPU_BANK_DEV`) | Default 64-bit device pool. On VEK385 this is `buffer@50000000000` (4 GiB high-DDR reusable CMA pool at PA `0x500_0000_0000`), bound by numeric phandle `<0x01>` because the official Versal-net base DT leaves the pool unlabeled. Does **not** carry `linux,cma-default` — exclusive to consumers that bind it explicitly. |

---

## 2. Command flow (submit → wait → status)

Submit with **`amdrnpu_cmd_submit()`**, which builds a **`struct amdrnpu_cmd_submit`**
(header + `args[]`), zeroes **`drm_sync_obj_handle`** on entry, and issues
**`AMDRNPU_CMD_SUBMIT`**. On success **`hdr_out->drm_sync_obj_handle`** is the binary
syncobj handle for that submission.

**Waiting and completion** — public surface (blocking **`DRM_IOCTL_SYNCOBJ_WAIT`**
is **not** exported as its own symbol; only used inside **`wait_complete`**):

| API | Role |
|-----|------|
| **`amdrnpu_syncobj_wait_complete()`** | Relative timeout: probe (`timeout_ns == 0`), bounded wait (`timeout_ns > 0`), or sleep until deadline then **fence status**. When the fence reports a **terminal** state (`*out_wait_status != 0`), the library issues **`DRM_IOCTL_SYNCOBJ_DESTROY`** on that syncobj so the per-submit object does not linger. |
| **`amdrnpu_syncobj_wait_busy_poll()`** | Polling loop using **`fence_status`** until signaled, wall cap, or error. On first **non-pending** status (`*out_fence_status != 0`), invokes the same **`SYNCOBJ_DESTROY`** path as **`wait_complete`**. |
| **`amdrnpu_syncobj_fence_status()`** | **Non-blocking** probe only — **does not** destroy the syncobj (for custom polling). |
| **`amdrnpu_syncobj_to_fence_fd()`** | One-shot **sync_file** for **`poll(2)` / `epoll`**. Does **not** destroy the submit syncobj; complete the wait path with **`wait_complete`** / **`wait_busy_poll`**, or manage the handle yourself. |

While the reported fence status stays **0** (pending), **`wait_complete`** and
**`wait_busy_poll`** leave the syncobj handle valid. After the driver signals
completion (success or error class), the library tears down the syncobj handle
for both helpers.

### 2.1 Typical patterns

```text
Bounded wait + status     amdrnpu_syncobj_wait_complete(drm_fd, h, rel_ns, &st)
Probe once                amdrnpu_syncobj_wait_complete(drm_fd, h, 0, &st)
Sleeping poll loop        amdrnpu_syncobj_wait_busy_poll(..., interval_ns > 0)
Cooperative tight poll    amdrnpu_syncobj_wait_busy_poll(..., interval_ns == 0)
Block on fd               amdrnpu_syncobj_to_fence_fd + poll() then wait_complete
```

For an **unbounded** wait without spinning, use **`wait_busy_poll`** with
**`max_wall_ns < 0`** and a non-zero **`interval_ns`** (or pass a very large
**`timeout_ns`** to **`wait_complete`**).

```mermaid
sequenceDiagram
    autonumber
    participant App
    participant Lib as amdrnpu_lib
    participant K as amdrnpu.ko
    participant SO as drm_syncobj + dma_fence
    participant RPU as RPU firmware

    App->>Lib: amdrnpu_cmd_submit(drm_fd, op, args, n, &hdr, sizeof(hdr))
    Lib->>K: ioctl(AMDRNPU_CMD_SUBMIT, flex amdrnpu_cmd_submit)
    K->>RPU: rpmsg amdrnpu_wire_cmd (seq = driver command id from cmd_xa)
    K-->>Lib: hdr.drm_sync_obj_handle set
    Lib-->>App: 0

    App->>Lib: amdrnpu_syncobj_wait_complete(drm_fd, syncobj, timeout, out st)
    Lib->>K: optional SYNCOBJ_WAIT
    Lib->>Lib: HANDLE_TO_FD sync_file plus SYNC_IOC_FILE_INFO
    Lib-->>App: st equals 0 pending, 1 ok, or negative fence errno

    RPU-->>K: terminal amdrnpu_wire_rsp
    K->>SO: dma_fence_signal (optional dma_fence_set_error)
    Lib->>K: DRM_IOCTL_SYNCOBJ_DESTROY when st is non-zero (terminal)
    Note over App,SO: next query would need a new syncobj handle from a new submit
```

---

## 3. Putting buffers and commands together

Typical **`amdrnpu_arg`** usage:

- Scalar parameter: **`size == 0`**, set **`value`**.
- GEM slice passed to firmware: **`size > 0`**, **`bo.drm_buffer_handle`** +
  **`bo.offset`**; kernel replaces with **`dma_addr + offset`** on the wire.
  **`size`** must cover the bytes the firmware will touch (validated against the
  GEM object size).

**Six-argument command** (input tag + size + BO, then output tag + size + BO) —
see **`OP_CODE_CMD0`** in
[`include/uapi/amdrnpu/amdrnpu.h`](../include/uapi/amdrnpu/amdrnpu.h)
and [commands.md §4](commands.md#example-op_code_cmd0-with-inputoutput-bos):

```c
struct amdrnpu_arg args[6] = { /* INPUT_BO, SZ_16M, bo0 slice, OUTPUT_BO, SZ_8M, bo1 */ };
struct amdrnpu_cmd_submit hdr;

memset(&hdr, 0, sizeof(hdr));
if (amdrnpu_cmd_submit(drm_fd, OP_CODE_CMD0, args, 6, &hdr, sizeof(hdr)))
	...;
amdrnpu_syncobj_wait_complete(drm_fd, hdr.drm_sync_obj_handle, timeout_ns, &st);
```

```mermaid
flowchart LR
    A[AMDRNPU_BUF_ALLOC] --> B[GEM handle + optional mmap]
    B --> C[AMDRNPU_CMD_SUBMIT with amdrnpu_arg refs]
    C --> D[wait_complete or wait_busy_poll]
    D --> E[amdrnpu_buf_close / munmap after cmd terminates]
```

For runnable examples covering every UAPI op (buf_alloc, cmd_submit,
lifecycle, fw_echo writeback, and the full fault-class fence-error
suite) see
[`lib/examples/amdrnpu_test.c`](../lib/examples/amdrnpu_test.c).
