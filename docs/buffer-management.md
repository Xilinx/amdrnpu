# Buffer Management

This document describes how userspace allocates, maps, and frees buffers
through the `amdrnpu` driver.

The driver uses **DRM GEM** as its buffer abstraction so it can use the
standard handle and mmap path. Each native buffer is backed by a contiguous
**cacheable** allocation (`dma_alloc_noncoherent()` on the per-bank struct
device) from a specific *memory bank* (RPU 32-bit or dev 64-bit). Userspace
must call **`AMDRNPU_BUF_SYNC`** (or **`amdrnpu_buf_sync()`**) to flush or
invalidate CPU cache lines around device DMA — see §3.

**Userspace helpers:** [`lib-userspace-flow.md`](lib-userspace-flow.md) covers
**`amdrnpu_buf_alloc()`**, **`amdrnpu_buf_close()`**, sync, mmap, and
how they line up with the ioctl flow below.

The buffer is **reference-counted**, the standard way GEM objects are managed.
The backing memory is freed only when the last reference is dropped — **not**
when any individual handle or command goes away. References are acquired and
released from several independent sources (handles, in-flight commands), and a
**GEM close** only decrements the count; whether memory is freed depends on
whether any *other* reference is still held.

---

## 1. Lifecycle overview

The buffer’s high-level state is **`Live`** vs **`Freed`**, with `mmap()` /
`munmap()` toggling mapping state inside **`Live`**. Leaving **`Live`** happens
when the GEM object’s refcount reaches zero.

```mermaid
stateDiagram-v2
    direction LR

    [*] --> Live : AMDRNPU_BUF_ALLOC, refcount = 1

    state Live {
        direction LR
        [*] --> Unmapped
        Unmapped --> Mapped : mmap on gem offset
        Mapped --> Unmapped : munmap
    }

    Live --> Freed : last reference released (refcount 1 to 0)
    Freed --> [*]
```

While **`Live`**, refcount can rise and fall as holders acquire and release.
**No single close frees the buffer on its own** — only the holder that drops
the *last* reference triggers **`amdrnpu_gem_free()`**.

| Holder | Acquired by | Released by |
|---|---|---|
| DRM handle | `AMDRNPU_BUF_ALLOC` (one ref per handle) | **`amdrnpu_buf_close()`** / `DRM_IOCTL_GEM_CLOSE`, or implicit teardown on last `close()` of the DRM fd |
| In-flight command pin | `AMDRNPU_CMD_SUBMIT` with a buffer arg (`size != 0`) | Terminal `AMDRNPU_RSP_*`, submit-failure cleanup, driver drain / `amdrnpu_cmd_release`; see §5 |

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant CORE as DRM core
    participant CMD as in-flight amdrnpu_cmd
    Note over CORE: refcount = 0

    U->>CORE: AMDRNPU_BUF_ALLOC
    Note over CORE: refcount = 1 (handle)

    U->>CMD: AMDRNPU_CMD_SUBMIT(BO=h)
    Note over CORE: refcount = 2 (handle + cmd pin)

    U->>CORE: GEM_CLOSE / amdrnpu_buf_close(handle)
    Note over CORE: refcount = 1 (cmd pin only)

    CMD-->>CORE: command terminates → drm_gem_object_put()
    Note over CORE: refcount = 0 → amdrnpu_gem_free()
```

---

## 2. Allocation: `AMDRNPU_BUF_ALLOC`

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant K as amdrnpu_drm.c
    participant CORE as DRM core
    participant DMA as dma_alloc_noncoherent
    participant BANK as bank->dev (per-bank struct device)

    U->>K: ioctl(AMDRNPU_BUF_ALLOC, {memory_type, size})
    K->>K: amdrnpu_bank_memory_type()
    K->>CORE: drm_gem_object_init(size)
    K->>DMA: dma_alloc_noncoherent(bank->dev, size, &dma_addr, DMA_BIDIRECTIONAL)
    DMA->>BANK: honors dma-mask + reserved-mem (kvaddr + dma_addr)
    DMA-->>K: kvaddr, dma_addr
    K->>CORE: drm_gem_create_mmap_offset(obj)
    K->>CORE: drm_gem_handle_create(file, obj, &handle)
    CORE->>CORE: handle = idr_alloc()
    K-->>U: drm_handle
```

**Inputs (`struct amdrnpu_buf_alloc`):**

| Field | Meaning |
|-------|---------|
| `flags` | must be 0 |
| `memory_type` | `AMDRNPU_MEMORY_RPU` or `AMDRNPU_MEMORY_DEVICE` (default via `AMDRNPU_MEMORY_DEFAULT`) |
| `reserved0` | must be 0 |
| `size` | bytes; rounded up internally |
| `drm_handle` | **output**: GEM handle for this `drm_file` |

**Failure modes:** `-ENODEV`, `-EINVAL`, `-ENOMEM`.

---

## 3. Cache sync: `AMDRNPU_BUF_SYNC`

Native driver-allocated buffers are **cacheable**. Before the device reads CPU
writes, flush with **`AMDRNPU_BUF_SYNC_TO_DEVICE`**. After the device writes data
the CPU will read, invalidate with **`AMDRNPU_BUF_SYNC_FROM_DEVICE`**.

```c
struct amdrnpu_buf_sync {
    __u32 drm_handle;
    __u32 direction;   /* AMDRNPU_BUF_SYNC_TO_DEVICE or AMDRNPU_BUF_SYNC_FROM_DEVICE */
    __u32 flags;       /* must be 0 */
    __u64 offset;      /* byte offset into the buffer */
    __u64 length;      /* bytes; 0 = from offset through end-of-buffer */
};
```

Library wrappers: **`amdrnpu_buf_sync()`**, **`amdrnpu_buf_sync_to_device()`**,
**`amdrnpu_buf_sync_from_device()`**. PRIME-imported handles return
**`-EOPNOTSUPP`** — use the exporter’s sync rules.

Typical flow around a command that DMAs a userspace-filled input buffer:

1. CPU writes via `mmap` or kernel mapping.
2. **`AMDRNPU_BUF_SYNC_TO_DEVICE`** on the written range (often the whole buffer).
3. **`AMDRNPU_CMD_SUBMIT`** referencing the handle.
4. After completion, **`AMDRNPU_BUF_SYNC_FROM_DEVICE`** on output ranges before CPU read.

---

## 4. mmap

`mmap()` on the DRM fd uses the standard GEM mmap offset path. **`amdrnpu_buf_mmap()`**
wraps **`DRM_IOCTL_MODE_MAP_DUMB`** + **`mmap()`** (requires a **card** fd, not
renderD*). **Partial mmap** is supported: pass a page-aligned **`offset`** and
**`length`**; only that range is mapped (the driver maps `page + offset/PAGE_SIZE`).
**`offset`** and **`length`** must be page-aligned; **`length == 0`** maps from
**`offset`** through end-of-buffer and requires **`buf_size`** from allocation.
Unmap with **`amdrnpu_buf_munmap(addr, length)`**.

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant DRM as DRM core
    participant K as amdrnpu_gem_obj_mmap
    participant DMA as dma_mmap_pages

    U->>DRM: GEM mmap path (offset from DRM_IOCTL_MODE_MAP_DUMB or equivalent)
    DRM->>K: amdrnpu_gem_obj_mmap(obj, vma)
    K->>DMA: dma_mmap_pages(bank->dev, vma, size, page)
    K-->>U: pages mapped
```

---

## 5. In-flight command pins

A buffer referenced by a command (any `amdrnpu_arg` with `size != 0`) is held
alive by the driver while that command runs.

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant SUB as AMDRNPU_CMD_SUBMIT
    participant CMD as amdrnpu_cmd
    participant CORE as DRM GEM core
    participant RPU as RPU firmware

    U->>SUB: ioctl AMDRNPU_CMD_SUBMIT with buffer arg
    SUB->>CORE: drm_gem_object_lookup on handle h
    CORE-->>SUB: obj refcount incremented
    SUB->>CMD: store obj in bo_refs array
    SUB-->>U: drm_sync_obj_handle

    Note over U,CORE: GEM close may run after submit returns, pin keeps object alive
    U->>CORE: amdrnpu_buf_close or GEM_CLOSE on h
    CORE->>CORE: drop handle ref object may remain pinned by cmd

    RPU-->>CMD: terminal mgmt response on RPMsg
    CMD->>CMD: amdrnpu_cmd_set_terminated
    CMD->>CORE: drm_gem_object_put for each bo_ref
```

**At termination** the driver puts each `bo_refs[]` entry once. That covers
normal completions, error-class responses, file teardown, and module unload
paths.

**Userspace:** after a successful submit you may call **`amdrnpu_buf_close`**
as soon as you no longer need the handle for your own lookups; the object stays
allocated until the command finishes and drops its pin.

---

## 6. Free / cleanup

A buffer is destroyed when refcount goes **1 → 0**. Typical paths:

- **`amdrnpu_buf_close()`** / **`DRM_IOCTL_GEM_CLOSE`** on the last open handle.
- **`close(drm_fd)`** walks the per-file handle table and closes GEMs.
- Command completion **`drm_gem_object_put`** when the last in-flight use ends.

```mermaid
sequenceDiagram
    autonumber
    participant U as Userspace
    participant CORE as DRM core
    participant K as amdrnpu_gem_free
    participant DMA as dma_free_noncoherent

    rect rgba(180,220,255,0.25)
    Note over U,CORE: Explicit GEM close
    U->>CORE: amdrnpu_buf_close / DRM_IOCTL_GEM_CLOSE
    CORE->>CORE: idr_remove + drm_gem_object_put
    CORE->>K: gem_obj->funcs->free(obj)
    end

    rect rgba(255,220,180,0.25)
    Note over U,CORE: Implicit on drm fd close
    U->>CORE: close(drm_fd)
    CORE->>CORE: drm_release walks handle IDR
    CORE->>K: ... free each object
    end

    K->>DMA: dma_free_noncoherent(bank->dev, size, kvaddr, dma_addr, dir)
```

While a command still pins a BO, **`GEM_CLOSE` only drops the userspace
handle** — memory stays until the submit completes and releases **`bo_refs`**.

---

## 7. Userspace sketch

```c
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <amdrnpu/amdrnpu_lib.h>

int main(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	uint32_t handle;
	int err;

	err = amdrnpu_buf_alloc(fd, AMDRNPU_MEMORY_DEVICE, 1u << 20, &handle);
	if (err)
		return 1;

	void *p;
	uint64_t sz = 1u << 20;

	amdrnpu_buf_mmap(fd, handle, sz, 0, 0, &p);
	memset(p, 0xa5, sz);
	amdrnpu_buf_sync_to_device(fd, handle, 0, 0);
	amdrnpu_buf_munmap(p, sz);
	amdrnpu_buf_close(fd, handle);
	close(fd);
	return 0;
}
```

---

## 8. See also

- [`architecture.md`](architecture.md) — per-bank platform devices and DMA windows.
- [`commands.md`](commands.md) — `amdrnpu_arg` buffer references and wire DMA addresses.
- [`lib-userspace-flow.md`](lib-userspace-flow.md) — library API for alloc, close, submit, wait.
