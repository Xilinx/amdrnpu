# Overall Architecture

This document describes the end-to-end architecture of the
`amdrnpu` driver: how the kernel module is structured, how it sits
between userspace and the RPU, and how data and control flow through
the major subsystems.

Everything below describes the driver under
[`drivers/accel/tessai-agent/`](../drivers/accel/tessai-agent/).

---

## 1. System view

The driver lives on the Application Processor Unit (APU) and brokers
communication between userspace processes on the APU and a Real-time
Processor Unit (RPU, e.g. an R5F) running an embedded runtime that
handles the actual hardware (AIE/NPU, sensors, etc.).

```mermaid
flowchart LR
    subgraph User["User space (APU)"]
        APP["Application<br/>(libamdrnpu + custom code)"]
    end

    subgraph Kernel["Kernel space (APU)"]
        DRM["DRM render node<br/>/dev/dri/renderD128"]
        TESS["amdrnpu driver"]
        RPMSG["rpmsg core"]
        REMOTEPROC["remoteproc<br/>(loads / boots R5F)"]
        DMA["non-cacheable<br/>reserved memory"]
    end

    subgraph RPU["RPU (R5F)"]
        FW["RPU firmware<br/>(OpenAMP / RPMsg)"]
        AIE["AIE / NPU access"]
    end

    APP -- "ioctl, mmap" --> DRM
    DRM --> TESS
    TESS -- "rpmsg_send /<br/>callback" --> RPMSG
    RPMSG <-- "vring" --> FW
    REMOTEPROC -- "boots,<br/>provides vring" --> RPU
    TESS -- "dma_alloc_noncoherent /<br/>AMDRNPU_BUF_SYNC /<br/>dma_buf" --> DMA
    FW <-- "shared buffers<br/>(physical addresses)" --> DMA
    FW --> AIE
```

**Key idea.** Buffer memory is shared between the APU and the RPU through
reserved-memory regions described in device tree. Native GEM buffers use
**cacheable** `dma_alloc_noncoherent()` mappings; userspace calls
**`AMDRNPU_BUF_SYNC`** to flush or invalidate ranges before/after device DMA.
The driver translates GEM handles into physical addresses and hands them to the
RPU as part of submission arguments. Control messages (wire commands and
responses/events) flow over rpmsg.

---

## 2. Driver-internal block diagram

```mermaid
flowchart TB
    subgraph User["User space"]
        FOPS["ioctl / mmap on<br/>/dev/dri/renderD*"]
    end

    subgraph TESS["amdrnpu kernel module"]
        DRV["amdrnpu_drv.c<br/>platform_driver +<br/>module init/exit"]
        DRM["amdrnpu_drm.c<br/>DRM device,<br/>fops, ioctl router,<br/>GEM ops"]
        BANK["amdrnpu_bank.c<br/>per-bank platform_devices,<br/>dma-mask, reserved-mem"]
        CMD["amdrnpu_cmd.c<br/>command IDR, dma_fence +<br/>drm_syncobj rotation"]
        RPMSG["amdrnpu_rpmsg.c<br/>rpmsg_driver,<br/>send / dispatch"]
    end

    subgraph KAPI["Kernel APIs"]
        DRMCORE["DRM core<br/>(GEM, syncobj, prime)"]
        DMABUF["dma-buf"]
        DMA["dma-mapping<br/>(dma_alloc_noncoherent,<br/>dma_sync_single_range)"]
        RPMSGC["rpmsg core"]
        OF["of_*, of_reserved_mem"]
    end

    FOPS --> DRM
    DRV --> BANK
    DRV --> CMD
    DRV --> DRM
    DRV --> RPMSG
    DRM --> CMD
    DRM --> BANK
    CMD --> RPMSG
    DRM --- DMABUF
    BANK --- DMA
    BANK --- OF
    RPMSG --- RPMSGC
```

| File | Responsibility |
|------|----------------|
| `amdrnpu_drv.c` | Module init/exit, registration of three handlers: parent `platform_driver` (matches `amd,amdrnpu`), bank `platform_driver` (matches the single shared `amd,amdrnpu-bank`), and `rpmsg_driver` (matches channel `amdrnpu`). The parent probe publishes drvdata then calls `devm_of_platform_populate()`. |
| `amdrnpu_bank.c` | Implements the bank `platform_driver`. Each probe reads `reg` (the logical bank index — `0`=RPU, `1`=device), owns one bank's reserved-memory + dma-mask (32 for rpu, 64 for dev with 32-bit fallback) and registers itself into `tdev->banks[reg]`. |
| `amdrnpu_drm.c` | Owns the DRM render node. Custom `unlocked_ioctl` routes magic-`'M'` ioctls to our handlers and falls through to `drm_ioctl()` for syncobj/prime/GEM ioctls. Implements GEM `alloc/import/export/mmap` against per-bank devices. |
| `amdrnpu_cmd.c` | Command lifecycle; **driver-owned command ids** (`cmd_xa` / `xa_alloc_cyclic`, wire `seq`); single `dma_fence` per submission on `drm_syncobj`; `amdrnpu_cmd_dispatch_response()` from rpmsg. |
| `amdrnpu_rpmsg.c` | `rpmsg_driver` registration, `rpmsg_send()` for outgoing `amdrnpu_wire_cmd`, callback that decodes `amdrnpu_wire_rsp` and feeds it to `amdrnpu_cmd_dispatch_response()`. |

---

## 3. Multiple instances, two driver halves each

A single APU may host several RPUs - for example one R5F per cluster -
and each gets its own `amd,amdrnpu` device-tree node pointing at
its own remoteproc via `amd,remoteproc`. The driver fully supports
this: every instance gets its own DRM render node, its own pair of
banks, and its own rpmsg channel.

```mermaid
flowchart LR
    subgraph DT["Device tree"]
        DT_R0["r5f_0"]
        DT_R1["r5f_1"]
        DT_T0["amdrnpu@0<br/>amd,remoteproc = r5f_0"]
        DT_T1["amdrnpu@1<br/>amd,remoteproc = r5f_1"]
        DT_T0 -. phandle .-> DT_R0
        DT_T1 -. phandle .-> DT_R1
    end

    subgraph KERN["amdrnpu.ko"]
        T0["amdrnpu_dev #0<br/>rpdev = ch0"]
        T1["amdrnpu_dev #1<br/>rpdev = ch1"]
    end

    DT_T0 -- platform_probe --> T0
    DT_T1 -- platform_probe --> T1

    subgraph RPMSG["rpmsg core"]
        CH0["channel amdrnpu<br/>parent: r5f_0"]
        CH1["channel amdrnpu<br/>parent: r5f_1"]
    end

    CH0 -- rpmsg_probe<br/>DT walk -> r5f_0<br/>match phandle --> T0
    CH1 -- rpmsg_probe<br/>DT walk -> r5f_1<br/>match phandle --> T1
```

The kernel module registers **three** handlers, regardless of how many
instances are present:

| Handler | Multiplicity | Match by |
|---------|-------------|----------|
| parent `platform_driver` | one probe per `amdrnpu` DT node | `compatible = "amd,amdrnpu"` |
| bank `platform_driver`   | two probes per parent (one per bank child; role from `reg`) | `compatible = "amd,amdrnpu-bank"` |
| `rpmsg_driver`           | one probe per RPU advertising the channel | `id_table.name = "amdrnpu"` |

The two halves find each other entirely through the device tree - no
global registry, no pending list, no driver-internal book-keeping.
When an rpmsg channel comes up:

1. Walk the rpmsg device's parent chain to find the first ancestor
   with an `of_node`. That node is the underlying **remoteproc** (the
   target of the `amd,remoteproc` phandle).
2. `for_each_compatible_node(np, NULL, "amd,amdrnpu")` - for each
   match, dereference its `amd,remoteproc` phandle and compare to the
   node from step 1.
3. Once the matching node is found, `of_find_device_by_node()` returns
   its `platform_device`. The `amdrnpu_dev` pointer is at
   `platform_get_drvdata(pdev)`.
4. If the platform device is not yet bound (`drvdata == NULL`),
   the rpmsg probe returns **`-EPROBE_DEFER`** and the driver core
   retries later.
5. Otherwise we install a managed `device_link` from the rpmsg device
   (consumer) to the platform device (supplier). This guarantees that
   on platform unbind, our `rpmsg_remove()` runs **before**
   `amdrnpu_platform_remove()`, so the platform side never sees a live
   `tdev->rpdev` pointer.

```mermaid
sequenceDiagram
    autonumber
    participant DTS as Device tree
    participant PR as platform_driver
    participant TDEV as "amdrnpu_dev instance N"
    participant RDR as rpmsg_driver
    participant DC as Driver core

    Note over DTS,TDEV: For each amd,amdrnpu node N
    DTS->>PR: bind amdrnpu slot N
    PR->>TDEV: alloc, parse banks,<br/>register DRM
    PR->>PR: platform_set_drvdata(pdev, tdev)

    Note over RDR: Independently...
    RDR->>RDR: rpmsg channel "amdrnpu"<br/>advertised by some RPU
    RDR->>RDR: walk rpdev->parent chain<br/>-> rproc DT node
    RDR->>RDR: for_each_compatible_node<br/>match amd,remoteproc phandle
    RDR->>RDR: of_find_device_by_node<br/>-> platform_device
    alt drvdata == NULL
        RDR-->>DC: return -EPROBE_DEFER
        DC-->>RDR: retry later
    else drvdata == tdev
        RDR->>DC: device_link_add(rpdev, tdev->dev,<br/>AUTOREMOVE_CONSUMER)
        RDR->>TDEV: tdev->rpdev = rpdev<br/>dev_set_drvdata(rpdev, tdev)
    end

    Note over TDEV: Buffers usable once platform probe completes
    Note over TDEV: Command submission returns -ENOTCONN until rpmsg binds

    Note over DTS,DC: ---------- Unbind path ----------
    DC->>RDR: rpmsg_remove(rpdev)<br/>(forced first by device_link)
    RDR->>TDEV: clear tdev->rpdev,<br/>clear rpdev drvdata
    DC->>PR: platform_remove(pdev)<br/>(safe: rpdev already gone)
```

In the fast path, `amdrnpu_rpmsg_cb()` recovers the owning `amdrnpu_dev`
with one `dev_get_drvdata(&rpdev->dev)` call - no lock, no list walk
per message.

Either half can come up first per-instance:

- **Platform first** (typical): buffers usable immediately. The rpmsg
  channel attaches whenever the RPU firmware boots and announces it.
- **Rpmsg first**: rpmsg probe returns `-EPROBE_DEFER`; the driver
  core re-runs it once the matching platform device finishes probing.

This decoupling matters because remoteproc may be loaded long after
the kernel boots and userspace has already opened the device.

---

## 4. Memory bank layout

The driver requires exactly two memory banks, modelled as DT child
nodes of the parent:

```mermaid
flowchart LR
    subgraph DT["Device tree"]
        PARENT["amdrnpu<br/>compatible = amd,amdrnpu<br/>#address-cells=1, #size-cells=0"]
        BANK0["bank@0<br/>compatible = amd,amdrnpu-bank<br/>reg = 0 (AMDRNPU_BANK_RPU / AMDRNPU_MEMORY_RPU)<br/>memory-region = shared_rpu_pool"]
        BANK1["bank@1<br/>compatible = amd,amdrnpu-bank<br/>reg = 1 (AMDRNPU_BANK_DEV / AMDRNPU_MEMORY_DEVICE)<br/>memory-region = shared_dev_pool"]
        PARENT --> BANK0
        PARENT --> BANK1
    end

    subgraph KERN["Kernel"]
        TDEV["amdrnpu_dev"]
        BD0["banks AMDRNPU_BANK_RPU<br/>struct device, 32-bit dma_mask,<br/>reserved-mem = shared_rpu_pool"]
        BD1["banks AMDRNPU_BANK_DEV<br/>struct device, 64-bit dma_mask,<br/>reserved-mem = shared_dev_pool"]
        TDEV --> BD0
        TDEV --> BD1
    end

    BANK0 -. devm_of_platform_populate<br/>+ bank platform_driver .-> BD0
    BANK1 -. devm_of_platform_populate<br/>+ bank platform_driver .-> BD1
```

Why per-bank platform devices? Native GEM allocations call
`dma_alloc_noncoherent(bank->dev, ...)` and **`AMDRNPU_BUF_SYNC`** for cache
maintenance (see `drivers/accel/tessai-agent/amdrnpu_drm.c`).
Each bank has its own `struct device` so:

1. The dma-mask is honoured per allocation. The RPU bank refuses
   addresses above 4 GiB; the dev bank goes up to its full 64-bit
   window.
2. The reserved-memory carve-out attached via
   `of_reserved_mem_device_init_by_idx()` is local to that bank, so the
   two pools never bleed into each other.
3. dma-buf imports re-attach the imported `dma_buf_attachment` to the
   bank's device, so the kernel re-maps the SG list within the bank's
   addressable window when needed.

`enum amdrnpu_memory_type` selects the bank for `AMDRNPU_BUF_ALLOC`:

| Value | Name | Bank |
|-------|------|------|
| `0` | `AMDRNPU_MEMORY_RPU` | RPU (`bank@0`, `AMDRNPU_BANK_RPU`) 32-bit pool |
| `1` | `AMDRNPU_MEMORY_DEVICE` (`AMDRNPU_MEMORY_DEFAULT`) | Dev (`bank@1`, `AMDRNPU_BANK_DEV`) 64-bit pool (default) |

PRIME imports attach to the dev bank device in `amdrnpu_gem_prime_import_sg_table()`.

`AMDRNPU_MEMORY_DEFAULT` expands to `AMDRNPU_MEMORY_DEVICE`, so callers using the macro get the dev bank without naming the constant explicitly.

If userspace passes `memory_type = AMDRNPU_MEMORY_DEVICE` (including via `AMDRNPU_MEMORY_DEFAULT`), the driver uses the dev bank.

---

## 5. Userspace surface

The driver presents itself as a DRM render node
(`/dev/dri/renderD<N>`). Userspace gets:

- **Per-process handle space.** GEM handles (`drm_handle`) from
  `AMDRNPU_BUF_ALLOC` and PRIME import are scoped to the open `drm_file`.
- **mmap.** `mmap(drm_fd, size, ..., offset)` with the offset returned
  by `DRM_IOCTL_GEM_MMAP` (the standard DRM mmap dance). The driver's
  GEM `mmap` op forwards to `dma_mmap_pages()` (native) or the exporter
  (imports). Native buffers require explicit **`AMDRNPU_BUF_SYNC`** around
  device DMA.
- **dma-buf interop.** Standard `DRM_IOCTL_PRIME_HANDLE_TO_FD` and
  `DRM_IOCTL_PRIME_FD_TO_HANDLE` on the render node; imports attach to
  the dev bank.
- **Sync waits.** `AMDRNPU_CMD_SUBMIT` returns a `sync_handle` in
  `struct amdrnpu_cmd_submit`; userspace waits with
  `DRM_IOCTL_SYNCOBJ_WAIT` (or `HANDLE_TO_FD` + `poll()`/`epoll()`) and
  uses `DRM_IOCTL_SYNCOBJ_QUERY` / sync_file probes for fence status.
  Convenience wrappers live in **`libamdrnpu`** (see [`lib-userspace-flow.md`](lib-userspace-flow.md)).
- **DRM syncobj (binary only).** `DRIVER_SYNCOBJ` is advertised; the
  driver does not enable `DRIVER_SYNCOBJ_TIMELINE`. Returned completion
  handles are single-fence (binary) syncobjs; userspace can still combine
  waits with other DRM devices in multi-handle waits where the API allows.
- **Unknown response types.** RPMsg payloads whose `command_rsp_type` is not
  one of the terminal `AMDRNPU_RSP_*` enums handled in
  `amdrnpu_cmd_dispatch_response()` are ignored at log level.

---

## 6. Code reference

The most relevant entry points:

```c
struct amdrnpu_dev {
	struct drm_device	drm;
	struct device		*dev;

	struct amdrnpu_bank	banks[AMDRNPU_NUM_BANKS];

	struct rpmsg_device	*rpdev;
	struct mutex		rpmsg_lock;

	struct mutex		cmd_lock;
	struct xarray		cmd_xa;
	u32			cmd_xa_next;
	atomic_t		fence_seqno;
	u64			fence_context;
};
```

The platform probe assembles all of the above:

See **`amdrnpu_platform_probe()`** in
[`drivers/accel/tessai-agent/amdrnpu_drv.c`](../drivers/accel/tessai-agent/amdrnpu_drv.c)
for the actual sequence:

1. Parse `amd,remoteproc`.
2. Allocate `amdrnpu_dev` (devm DRM).
3. Init command lifecycle.
4. **`platform_set_drvdata(pdev, tdev)`** — bank probes need this.
5. **`devm_of_platform_populate(dev)`** — triggers synchronous bank probes
   for both `bank@0` and `bank@1` (single shared compatible
   `amd,amdrnpu-bank`; role taken from `reg`).
6. **`amdrnpu_bank_check_all_present(tdev)`** verifies both slots are populated.
7. **`amdrnpu_drm_register(tdev)`** publishes the render node.

---

## 7. See also

- [`commands.md`](commands.md) — command submission, the two-state
  lifecycle, and the rotating fence behind every command's syncobj.
- [`buffer-management.md`](buffer-management.md) — buffer allocation,
  import, export, mmap and lifetime details.
