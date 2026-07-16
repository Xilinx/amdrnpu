# Command Submission and Termination

This document describes how userspace drives the RPU through the
`amdrnpu` driver: `AMDRNPU_CMD_SUBMIT`, the returned `drm_syncobj`
with a single `dma_fence`, and termination. There is **no** dedicated `AMDRNPU_CMD_CANCEL`
ioctl; commands end on a terminal RPU response, submit failure cleanup, or
`drm_file` close.

The model is minimal:

- **Two states only.** `SUBMITTED` (after `AMDRNPU_CMD_SUBMIT` returns 0) and
  `TERMINATED` (after `AMDRNPU_RSP_COMPLETE` / `AMDRNPU_RSP_*` failure from the RPU,
  failed submit cleanup, or process `close()` on the DRM fd). There is
  no separate `INIT` / `PENDING` exposed to userspace.
- **Identity.** Userspace identifies in-flight work only through the
  **`drm_sync_obj_handle`** returned in `struct amdrnpu_cmd_submit`. **Command ids
  are allocated and managed solely by the `amdrnpu` driver** via
  `xa_alloc_cyclic()` on `cmd_xa` (ids **0..`UINT_MAX`** via `xa_limit_32b`, reused when the slot is
  free). Userspace never chooses or stores the id. On RPMsg the id is sent as
  `amdrnpu_wire_cmd.seq` and echoed in `amdrnpu_wire_rsp.seq` on completion.
  Responses may arrive out of submit order; matching is always by that id, not by
  submit order.
- **No driver-specific wait ioctl.** Use `DRM_IOCTL_SYNCOBJ_WAIT`,
  `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` + `poll()`, and
  **`DRM_IOCTL_SYNCOBJ_QUERY`** to read signaled state and fence errno
  after a wakeup (or call the wrappers in **`amdrnpu_lib`** — see [lib-userspace-flow.md](lib-userspace-flow.md)).

---

## 1. State machine

```mermaid
stateDiagram-v2
    [*] --> SUBMITTED : AMDRNPU_CMD_SUBMIT returns 0
    SUBMITTED --> TERMINATED : terminal RPU RPMsg rsp
    SUBMITTED --> TERMINATED : submit teardown or fd close, -ECONNRESET
    TERMINATED --> [*] : cmd detached, syncobj destroyed at close
```

In `TERMINATED`, the syncobj’s last fence stays signalled;
`dma_fence_set_error()` carries `-EREMOTEIO` / `-ECONNRESET` (and other
driver errors) through `DRM_IOCTL_SYNCOBJ_WAIT` as appropriate.

---

## 2. End-to-end flow

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant K as amdrnpu
    participant SO as drm_syncobj + dma_fence
    participant RPMSG as rpmsg
    participant RPU as RPU firmware

    U->>K: ioctl(AMDRNPU_CMD_SUBMIT, hdr + args[])
    Note over K: hdr.drm_sync_obj_handle = 0 on entry
    K->>K: amdrnpu_cmd_alloc, xa_alloc_cyclic id (xa_limit_32b)
    K->>SO: drm_syncobj_create(unsignalled fence)
    K->>K: amdrnpu_resolve_args, rpmsg send amdrnpu_wire_cmd
    K->>SO: drm_syncobj_get_handle() -> drm_sync_obj_handle
    K-->>U: hdr updated (drm_sync_obj_handle set)

    U->>SO: DRM_IOCTL_SYNCOBJ_WAIT or HANDLE_TO_FD + poll

    RPU-->>RPMSG: amdrnpu_wire_rsp (terminal COMPLETE / error class)
    RPMSG->>K: amdrnpu_cmd_dispatch_response()
    K->>SO: dma_fence_signal (optional dma_fence_set_error)

    U->>SO: DRM_IOCTL_SYNCOBJ_QUERY (optional, fence status / errno)
```

---

## 3. Wire format

Wire layouts are defined in
[`amdrnpu_cmd.h`](../drivers/accel/tessai-agent/amdrnpu_cmd.h)
(driver-internal, not UAPI).

### Host → RPU (`amdrnpu_wire_cmd`)

| Field | Type | Meaning |
|-------|------|---------|
| `seq` | `__le32` | Monotonic driver command id (0..UINT_MAX, wraps); firmware echoes on response |
| `command_op` | `__le32` | Firmware opcode (same as ioctl `command_op`) |
| `flags` | `__le32` | Reserved (submit uses `0`) |
| `num_args` | `__le32` | Count of trailing `args[]` QWORDs |
| `args[]` | `__le64[]` | Resolved scalar/buffer DMA addresses |

### RPU → host (`amdrnpu_wire_rsp`)

| Field | Type | Meaning |
|-------|------|---------|
| `seq` | `__le32` | Echo of the driver-assigned command id from submit |
| `command_rsp_type` | `__le32` | Terminal status (`enum amdrnpu_rsp_type`) |

### `amdrnpu_arg` → wire word

| Argument | Encoding in `args[i]` (little-endian QWORD) |
|----------|-----------------------------------------------|
| Scalar   | `size == 0`: wire word = `value` |
| Buffer   | `size > 0`: `lookup(bo.drm_buffer_handle)`, validate `[offset, offset+size)`, wire word = `dma_addr + offset` |

### `command_rsp_type` (matches `enum amdrnpu_rsp_type` in `amdrnpu_cmd.h`)

| Value | Name | Fence / command |
|------:|------|-----------------|
| 0 | `AMDRNPU_RSP_COMPLETE` | Success; fence signalled |
| 1 | `AMDRNPU_RSP_REMOTEIO` | Terminate; fence error `-EREMOTEIO` |
| 2 | `AMDRNPU_RSP_NOTSUPPORTED` | Terminate; fence error `-EOPNOTSUPP` |
| 3 | `AMDRNPU_RSP_CANCELLED` | Terminate; fence error `-ECANCELED` |
| *other* | — | **Invalid:** driver logs and completes the slot with fence error `-EREMOTEIO` (same effect as an explicit remote fault, not ignored). |

There is **no** in-driver progress /
multi-stage fence for a single submission; firmware should use DMA or a
buffer-based side protocol if it needs to stream milestones while the command
slot stays occupied.

`AMDRNPU_RSP_ERROR` in legacy code is **`#define`d** to **`AMDRNPU_RSP_REMOTEIO`**
in [`amdrnpu_cmd.h`](../drivers/accel/tessai-agent/amdrnpu_cmd.h).

---

## 4. `AMDRNPU_CMD_SUBMIT`

Variable-length ioctl payload: the **UAPI** type is `struct amdrnpu_cmd_submit`
— fixed header through `offsetof(struct amdrnpu_cmd_submit, args)` plus
`num_args × sizeof(struct amdrnpu_arg)`.

| Field | Direction | Meaning |
|-------|-----------|---------|
| `drm_sync_obj_handle` | in/out | **In:** `0` for a new command. **Out:** DRM syncobj handle (binary) for waits. |
| `command_op` | in | Firmware opcode selector |
| `num_args` | in | Number of trailing `amdrnpu_arg`; ≤ `AMDRNPU_MAX_ARGS` |
| `args[]` | in | Scalar and buffer GEM slices |

### Example: `OP_CODE_CMD0` with input/output BOs

Firmware opcodes and scalar tags are defined by the RPU image; the pattern
below matches a common six-argument layout (tag + size + buffer slice per
direction).  `bo0_hd` / `bo1_hd` are DRM handles from `AMDRNPU_BUF_ALLOC` on
the same `drm_fd`.

```c
#define OP_CODE_CMD0  0x00000001u
#define INPUT_BO      1ull
#define OUTPUT_BO     2ull
#define SZ_16M        (16u * 1024u * 1024u)
#define SZ_8M         (8u * 1024u * 1024u)

struct amdrnpu_arg args[6];

args[0] = (struct amdrnpu_arg){ .size = 0, .value = INPUT_BO };
args[1] = (struct amdrnpu_arg){ .size = 0, .value = SZ_16M };
args[2] = (struct amdrnpu_arg){
	.size = SZ_16M,
	.bo = { .drm_buffer_handle = (__s32)bo0_hd, .offset = 0 },
};
args[3] = (struct amdrnpu_arg){ .size = 0, .value = OUTPUT_BO };
args[4] = (struct amdrnpu_arg){ .size = 0, .value = SZ_8M };
args[5] = (struct amdrnpu_arg){
	.size = SZ_8M,
	.bo = { .drm_buffer_handle = (__s32)bo1_hd, .offset = 0 },
};

struct amdrnpu_cmd_submit hdr = {
	.drm_sync_obj_handle = 0,
	.command_op = OP_CODE_CMD0,
	.num_args = 6,
};
/* ioctl: pass a buffer of offsetof(..., args) + 6*sizeof(struct amdrnpu_arg);
 * or amdrnpu_cmd_submit(fd, OP_CODE_CMD0, args, 6, &hdr, sizeof(hdr)). */
```

| Index | `size` | Payload | Role |
|------:|--------|---------|------|
| 0 | 0 | `INPUT_BO` | Firmware input-buffer tag (scalar) |
| 1 | 0 | `SZ_16M` | Input size (scalar) |
| 2 | `SZ_16M` | `bo0_hd` @ offset 0 | Input GEM slice → wire DMA address |
| 3 | 0 | `OUTPUT_BO` | Firmware output-buffer tag (scalar) |
| 4 | 0 | `SZ_8M` | Output size (scalar) |
| 5 | `SZ_8M` | `bo1_hd` @ offset 0 | Output GEM slice → wire DMA address |

The canonical copy of this example lives in
[`include/uapi/amdrnpu/amdrnpu.h`](../include/uapi/amdrnpu/amdrnpu.h).

Failure modes include `-EFAULT`, `-EINVAL`, `-E2BIG`, `-ENOMEM`,
`-ENOENT` (bad BO handle), `-ENOTCONN` (no rpmsg channel).

If submission fails **after** the RPU has been sent a `amdrnpu_wire_cmd` but
**before** userspace receives the syncobj handle (or if copying the
result header fails), the driver erases the command from `cmd_xa`, signals
the syncobj fence with the ioctl errno, and drops the command object.

---

## 5. Waiting and querying (DRM only)

Equivalent ways to block or poll:

- **`DRM_IOCTL_SYNCOBJ_WAIT`** — waits on handles using an absolute
  **`CLOCK_MONOTONIC`** deadline (`timeout_nsec` / `deadline_nsec` per your
  kernel UAPI); **`amdrnpu_syncobj_wait_complete()`** uses it internally
  when given a positive relative **`timeout_ns`** (lib converts to an absolute
  deadline).
- **`DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD`** with **`DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE`**
  + `poll()` / `epoll` on the **sync_file** (see **`amdrnpu_syncobj_to_fence_fd()`**).
- **`DRM_IOCTL_SYNCOBJ_QUERY`** — read **signaled** state and the backing
  **`dma_fence`** status (`errno`/completion reason) when available on your DRM.
- **`amdrnpu_syncobj_fence_status()`** — portable probe via **`SYNC_IOC_FILE_INFO`**
  on an exported sync_file (implemented in `libamdrnpu`); **non-blocking**.
- **`amdrnpu_syncobj_wait_busy_poll()`** — application-level polling loop:
  repeats `fence_status` with **`nanosleep(interval_ns)`** between probes (**sleeping poll**),
  or **`sched_yield()`** only when `interval_ns == 0` (**tight cooperative poll** — not hardware busy-spin).

There is **no** vendor `AMDRNPU_GET_LAST_EVENT` ioctl; introspection goes
through the syncobj/fence path above. End-to-end C flows are spelled out in
[`lib-userspace-flow.md`](lib-userspace-flow.md).

---

## 6. Single dma_fence per submission

Each command allocates **one** unsignaled `dma_fence` at submit time,
wraps it in the returned syncobj, and keeps that pairing until **one**
terminal RPMsg response (or IOCTL/teardown failures that complete the slot
without a response). Nothing calls `drm_syncobj_replace_fence()`: there is no
fence “rotation”. Reference-count semantics follow the usual DRM syncobj
helpers in `drm_syncobj.c`.

---

## 7. Userspace sketch

Prefer **`amdrnpu_cmd_submit()`** + **`amdrnpu_syncobj_wait_complete()`**
(or **`amdrnpu_syncobj_wait_busy_poll()`** for a polling loop) from
[`lib/include/amdrnpu/amdrnpu_lib.h`](../lib/include/amdrnpu/amdrnpu_lib.h).
Release GEMs with **`amdrnpu_buf_close()`** when appropriate (see
[`buffer-management.md`](buffer-management.md)).

Raw ioctl callers must allocate a contiguous blob:
`offsetof(struct amdrnpu_cmd_submit, args) + num_args*sizeof(struct amdrnpu_arg)`,
clear **`drm_sync_obj_handle`** on entry, and pass **`AMDRNPU_CMD_SUBMIT`**. For SYNCOBJ
timeouts, populate absolute monotonic **`timeout_nsec`** (see kernel
`drm_syncobj_wait`/your header); **`-1` is not a portable “infinity” sentinel**
on newer DRM — use **`INT64_MAX`** as the relative nanoseconds argument to
**`amdrnpu_syncobj_wait_complete()`**, use an unbounded
**`amdrnpu_syncobj_wait_busy_poll()`** (`max_wall_ns < 0`), or pass **`NULL`**
only when driving **`DRM_IOCTL_SYNCOBJ_WAIT`** yourself.

See **`docs/lib-userspace-flow.md`** for sequence diagrams tying buffers and
commands together.

Use `libdrm` helpers (`drmSyncobjWait`, `drmSyncobjQuery`, …) instead of
raw ioctls if convenient.

---

## 8. See also

- [`architecture.md`](architecture.md) — driver blocks and rpmsg bind.
- [`buffer-management.md`](buffer-management.md) — GEM allocation,
  buffer-style `amdrnpu_arg` pinning, and buffer lifetime.
