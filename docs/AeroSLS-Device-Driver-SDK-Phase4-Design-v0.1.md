# AeroSLS Device Driver SDK & Device Manager — Phase 4 Design v0.1

**Status:** Draft for review.  
**Date:** August 23, 2026.  
**Scope:** Turns device drivers into first-class sidecars, replacing in-kernel driver modules with user-space (or same-ring, memory-isolated) sidecars that hold hardware capabilities and communicate via channels and shared memory.

**Depends on:**
- Phase 1: Capability lifecycle (`docs/AeroSLS-Capability-SDK-Phase1-Seed-Kernel-Design-v0.1.md`)
- Phase 2: POSIX sidecar (`docs/AeroSLS-POSIX-Sidecar-Phase2-Design-v0.1.md`)
- Phase 3: Polyglot Nexus (`docs/AeroSLS-Polyglot-Nexus-Phase3-Design-v0.1.md`)
- Kernel capability layer (`docs/AeroSLS-Kernel-Capability-Layer-Spec-v0.1.md`)
- Driver respawn spec (`docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`)
- Threat model (`docs/AeroSLS-Threat-Model.md`)

**Hardware target:** RISC-V 64-bit with Sv39 virtual memory, PCIe, PLIC/APLIC/IMSIC interrupts, and IOMMU (or PMP/ePMP for simple cases). x86-64 with PCIe, MSI/MSI-X, and VT-d/IOMMU as secondary target. Parameterized where needed.

---

## Table of Contents

1. [New Capability Types](#1-new-capability-types)
2. [Device Manager Sidecar Architecture](#2-device-manager-sidecar-architecture)
3. [IRQ Delivery via Channels](#3-irq-delivery-via-channels)
4. [DMA Buffer Allocator API](#4-dma-buffer-allocator-api)
5. [Driver Manifest Format](#5-driver-manifest-format)
6. [Full NIC Driver Sidecar Lifecycle](#6-full-nic-driver-sidecar-lifecycle)
7. [Crash Recovery](#7-crash-recovery)
8. [Security Analysis](#8-security-analysis)
9. [Performance Estimates](#9-performance-estimates)

---

## 1. New Capability Types

Phase 1 introduced `CAP_TYPE_MEM` (1), `CAP_TYPE_CHAN_R` (2), `CAP_TYPE_CHAN_W` (3), and Phase 3 added `CAP_TYPE_TRAMP` (4). Phase 4 adds four new types for hardware access. All fit into the existing 64-bit capability word format: bits [63:61] = type, [60:57] = state, [56:40] = object id, [39:32] = permissions, [31:16] = offset, [15:4] = length, [3:0] = reserved (forgery tag).

### 1.1 Type Assignments

```c
#define CAP_TYPE_IO_PORT    5   /* Port I/O (x86 in/out; RISC-V: MMIO via PMP-mapped pages) */
#define CAP_TYPE_IRQ        6   /* Hardware interrupt delivery */
#define CAP_TYPE_DMA_MEM    7   /* Physically pinned memory for DMA */
#define CAP_TYPE_BUS_ACCESS 8   /* PCIe config space / bus topology access */
```

### 1.2 CAP_TYPE_IO_PORT — Port/PIO Capability

Grants the holder the right to perform I/O on a specific physical address range.

**RISC-V variant:** RISC-V has no port I/O instruction. On RISC-V, IO_PORT caps are backed by MMIO pages mapped via the IOMMU or PMP into the sidecar's address space. The cap's physical range is an MMIO BAR, not a port number. The same cap type serves both architectures; the kernel maps it appropriately.

**x86 variant:** The cap names a range of x86 port numbers (e.g., `0xCF8-0xCFF` for PCI config, or an NVMe BAR's I/O port range).

**Kernel representation (CapObject extensions):**

```c
struct CapObject {
    /* ... existing fields ... */
    /* Phase 4 extensions: */
    uint64_t io_phys_base;      /* IO_PORT: physical base (MMIO on RV, port base on x86) */
    uint32_t io_phys_len;       /* IO_PORT: length in bytes (MMIO) or port count (x86) */
    uint8_t  io_width;          /* IO_PORT: 1/2/4 bytes per access */
    uint8_t  io_flags;          /* bit0=UNCACHEABLE, bit1=READ_ONLY, bit2=BE_MEM Bar */
    uint16_t _io_pad;
};
```

**Permission bits (bits [39:32] in the cap word):**

```c
#define CAP_PERM_IO_READ    0x01   /* allow reads from this range */
#define CAP_PERM_IO_WRITE   0x02   /* allow writes to this range */
#define CAP_PERM_IO_PF      0x04   /* allow port fallback / BAR sizing */
```

**Syscall to create:**

```c
#define SYS_SLS_CAP_CREATE_IO_PORT  307  /* next free after Phase 3's 306 */

struct SLSCapCreateIOPortRequest {
    uint64_t phys_base;       /* physical address of MMIO range (or port number on x86) */
    uint32_t length;          /* length in bytes (or port count) */
    uint8_t  width;           /* 1, 2, or 4 bytes per access */
    uint8_t  flags;           /* IO_FLAGS_* */
    uint8_t  perm;            /* CAP_PERM_IO_READ | CAP_PERM_IO_WRITE */
    uint8_t  _pad;
};
```

**Anti-aliasing:** `cap_create_mem()` already checks that no two MEM objects overlap the same physical range. IO_PORT caps perform the identical check against existing IO_PORT objects and also against MEM objects (a physical range cannot be both DMA-accessible memory and an MMIO device register). The check is:

```c
/* Inside cap_create_io_port(): */
for (uint32_t i = 0; i < CAP_OBJECT_MAX; i++) {
    const struct CapObject* o = &cap_objects[i];
    if (!o->active) continue;
    if (o->kind == CAP_OBJ_KIND_IO_PORT) {
        /* Must not overlap existing IO_PORT */
        if (phys_base < o->io_phys_base + o->io_phys_len &&
            phys_base + length > o->io_phys_base)
            return CAP_ECONFLICT;
    }
    if (o->kind == CAP_OBJ_KIND_MEM) {
        /* Must not overlap a MEM region */
        uint64_t mend = o->phys_base + (uint64_t)o->npages * 4096u;
        if (phys_base < mend && phys_base + length > o->phys_base)
            return CAP_ECONFLICT;
    }
}
```

**Revocation:** `cap_revoke()` on an IO_PORT cap stamps all holders as REVOKED, unlinks them from the holder list, and (on x86) restores the port range to "unclaimed" in the I/O bitmap. The IOMMU domain is torn down if this was the last IO_PORT cap for that device. Revocation is total: no sidecar can use the port/MMIO range after `cap_revoke()` returns.

### 1.3 CAP_TYPE_IRQ — Interrupt Capability

Grants the holder the right to receive hardware interrupts as CHAN messages (see §3). One IRQ cap per interrupt line.

**Kernel representation:**

```c
struct CapObject {
    /* ... existing + IO_PORT fields ... */
    /* Phase 4 IRQ extensions: */
    uint32_t irq_number;       /* IRQ: interrupt number (PLIC source ID or MSI vector) */
    uint8_t  irq_trigger;      /* IRQ: 0=level-low, 1=edge-rising, 2=level-high, 3=edge-falling */
    uint8_t  irq_polarity;     /* IRQ: reserved, must be 0 */
    uint16_t irq_affinity_cpu; /* IRQ: target CPU (0xFFFF = any) */
    uint32_t irq_chan_id;      /* IRQ: channel object id for message delivery */
    uint32_t irq_masked;       /* IRQ: 1 = masked, 0 = enabled */
    uint64_t irq_coalesce_us;  /* IRQ: minimum interval between messages (microseconds) */
};
```

**Permission bits:**

```c
#define CAP_PERM_IRQ_LISTEN  0x01   /* receive IRQ messages */
#define CAP_PERM_IRQ_ACK     0x02   /* acknowledge/EOI the interrupt */
#define CAP_PERM_IRQ_MASK    0x04   /* mask/unmask the interrupt */
```

**Syscall to create:**

```c
#define SYS_SLS_CAP_CREATE_IRQ  308

struct SLSCapCreateIRQRequest {
    uint32_t irq_number;       /* PLIC source or MSI vector */
    uint8_t  trigger;          /* edge/level, high/low */
    uint8_t  perm;             /* CAP_PERM_IRQ_LISTEN | CAP_PERM_IRQ_ACK | CAP_PERM_IRQ_MASK */
    uint16_t _pad;
    uint64_t coalesce_us;      /* 0 = no coalescing */
};
```

**Creation path:** Only the Device Manager (or a sidecar holding a `CAP_TYPE_BUS_ACCESS` capability with write rights) can create IRQ caps. The kernel validates that the IRQ number is a valid PLIC/APLIC source by checking the interrupt controller's `ID俭` register. For MSI/MSI-X, the IRQ cap is created as a side-effect of programming the device's MSI table entry (see §6). The IRQ cap is bound to exactly one sidecar at creation time and cannot be transferred — IRQ caps are **non-transferable** (a design choice; see §8.3 for rationale).

**Revocation:** `cap_revoke()` on an IRQ cap masks the interrupt at the PLIC/APLIC source, stamps all holders REVOKED, and disconnects the IRQ-to-CHAN binding. The interrupt controller's enable bit is cleared atomically. No further interrupt messages are delivered.

### 1.4 CAP_TYPE_DMA_MEM — DMA Buffer Capability

A DMA_MEM cap is a MEM cap with the additional property that the physical pages are **pinned** (not swappable or reclaimable) and mapped into an IOMMU domain. It is the only type of memory a device can DMA to/from.

**Kernel representation:**

```c
struct CapObject {
    /* ... existing MEM fields (phys_base, npages, max_perms) ... */
    /* Phase 4 DMA extensions: */
    uint32_t dma_iommu_domain; /* DMA_MEM: IOMMU domain id (0 = no IOMMU) */
    uint32_t dma_device_id;    /* DMA_MEM: owning device's object id */
    uint8_t  dma_flags;        /* bit0=COHERENT, bit1=READ_ONLY_DEVICE, bit2=WRITE_ONLY_DEVICE */
    uint8_t  _dma_pad[3];
    uint64_t dma_pinned_phys;  /* physical base (== phys_base; duplicated for fast IOMMU path) */
};
```

**Permission bits:** DMA_MEM inherits MEM permissions (R/W/X) plus:

```c
#define CAP_PERM_DMA_SHARE    0x10   /* allow sharing to other sidecars */
#define CAP_PERM_DMA_MAP      0x20   /* allow IOMMU mapping */
```

**Creation:** DMA_MEM caps are created by the kernel's DMA buffer allocator (§4), not by sidecars directly. A driver requests physically contiguous, pinned memory via `SYS_SLS_DMA_ALLOC`, and the kernel returns a DMA_MEM cap. The IOMMU mapping is performed automatically: the kernel programs the IOMMU page table for the device's domain to include the newly allocated pages.

**Revocation:** `cap_revoke()` on a DMA_MEM cap unmaps the IOMMU pages for all domains, unpinnes the physical pages (they become reclaimable again), and frees the arena frames. This is the mechanism by which a crashed driver's DMA buffers are reclaimed (§7).

### 1.5 CAP_TYPE_BUS_ACCESS — Bus Topology Capability

Grants the holder the right to enumerate the PCIe bus, read/write config space, and discover devices.

**Kernel representation:**

```c
struct CapObject {
    /* ... Phase 4 bus extensions: */
    uint8_t  bus_root;          /* BUS_ACCESS: starting bus number */
    uint8_t  bus_max;           /* BUS_ACCESS: ending bus number */
    uint8_t  bus_flags;         /* bit0=CONFIG_READ, bit1=CONFIG_WRITE, bit2=ENUMERATE */
    uint8_t  _bus_pad;
    uint32_t bus_domain;        /* BUS_ACCESS: IOMMU domain for bus-mastering devices */
};
```

**Permission bits:**

```c
#define CAP_PERM_BUS_ENUM    0x01   /* enumerate devices */
#define CAP_PERM_BUS_CONFIG_R 0x02  /* read config space */
#define CAP_PERM_BUS_CONFIG_W 0x04  /* write config space (e.g., enable bus mastering) */
```

**Creation:** Only the Device Manager sidecar receives a BUS_ACCESS cap at boot time. The kernel mints one BUS_ACCESS cap covering the entire PCI bus topology during `create_sidecar()` for the Device Manager manifest. No sidecar can create its own BUS_ACCESS cap — it is boot-granted only.

### 1.6 Capability Object Kind Table

```c
#define CAP_OBJ_KIND_MEM      1   /* existing */
#define CAP_OBJ_KIND_CHAN     2   /* existing */
#define CAP_OBJ_KIND_IO_PORT  5   /* new: port I/O / MMIO */
#define CAP_OBJ_KIND_IRQ      6   /* new: interrupt delivery */
#define CAP_OBJ_KIND_DMA_MEM  7   /* new: DMA-pinned memory */
#define CAP_OBJ_KIND_BUS      8   /* new: bus topology access */
```

Values 3 and 4 are reserved for future use (TRAMP uses type=4 in the cap word but shares CHAN's object kind internally since it binds to a channel pair).

### 1.7 Override Table: Existing cap.c Modifications

The existing `cap_create_mem()` and `cap_revoke()` functions need extension, not replacement. The approach is:

1. `cap_word_make()` is unchanged — the 3-bit type field accommodates types 5–8.
2. `cap_word_valid()` is unchanged — it only checks type != NONE and state == VALID.
3. A new `cap_object_expand()` function adds the Phase 4 fields to the existing `CapObject` struct. Because objects are monotonically allocated and never recycled (Phase 1 invariant §3), growing the struct does not break existing objects.
4. `cap_revoke()` gains a `CAP_OBJ_KIND_IRQ` case that disables the interrupt source before the standard holder walk.
5. `cap_revoke()` gains a `CAP_OBJ_KIND_DMA_MEM` case that unmaps IOMMU pages before freeing frames.

---

## 2. Device Manager Sidecar Architecture

### 2.1 Role and Responsibilities

The Device Manager is a privileged sidecar — not a kernel module — that:

1. **Enumerates** the PCIe bus tree using its BUS_ACCESS capability.
2. **Reads** PCI config space to identify vendor/device IDs, BARs, interrupt pins.
3. **Matches** discovered devices against driver manifests in the kernel's validated registry.
4. **Spawns** driver sidecars via `create_sidecar()` with appropriate IO_PORT, IRQ, and BUS_ACCESS caps.
5. **Handles hotplug/removal** by watching for device appearance/disappearance events (PCIe hotplug via AER or ACPI notification).
6. **Supervises** driver sidecar lifecycles: monitors health, restarts crashed drivers, propagates removal events.

### 2.2 Boot-time Initialization

At boot, the kernel creates the Device Manager sidecar with:

```
Initial capabilities:
  BUS_ACCESS (entire PCI bus, CONFIG_READ | CONFIG_WRITE | ENUMERATE)
  CHAN_R + CHAN_W to parent (POSIX core or kernel init)
  CHAN_R + CHAN_W to IRQ controller service (kernel-internal)
  MEM cap over a small config/config-state region (read/write)
  Console channel (for logging)
```

The Device Manager's manifest (§5) declares these requirements. The kernel validates the manifest, builds the initial capability table, and starts the Device Manager at its entry point.

### 2.3 PCIe Enumeration Algorithm

```
function enumerate_pci_bus():
    for bus in 0..BUS_MAX:
        for slot in 0..31:
            for func in 0..7:
                vendor_id = config_read16(bus, slot, func, PCI_VENDOR_ID)
                if vendor_id == 0xFFFF:
                    continue    /* no device */
                device_id = config_read16(bus, slot, func, PCI_DEVICE_ID)
                class_code = config_read32(bus, slot, func, PCI_CLASS_REVISION) >> 16
                header_type = config_read8(bus, slot, func, PCI_HEADER_TYPE)

                /* Read BARs */
                bars[6]
                for i in 0..5:
                    bars[i] = config_read32(bus, slot, func, PCI_BAR0 + i*4)

                /* Read interrupt pin/line */
                int_pin = config_read8(bus, slot, func, PCI_INTERRUPT_PIN)
                int_line = config_read8(bus, slot, func, PCI_INTERRUPT_LINE)

                /* MSI/MSI-X detection */
                msi_cap = find_pci_capability(bus, slot, func, PCI_CAP_MSI)
                msix_cap = find_pci_capability(bus, slot, func, PCI_CAP_MSIX)

                device = register_device(bus, slot, func,
                    vendor_id, device_id, class_code,
                    bars, int_pin, int_line, msi_cap, msix_cap)

                /* Bridge recursion */
                if class_code >> 8 == 0x06:  /* PCI bridge */
                    secondary_bus = config_read8(bus, slot, func, PCI_SECONDARY_BUS)
                    enumerate_bus_range(secondary_bus, max_bus)
```

### 2.4 Driver Matching

The Device Manager maintains a registry of driver manifests (loaded from the kernel's validated manifest store at boot, §5). Matching uses an ordered priority:

1. **Exact vendor/device ID match** — e.g., `{vendor: 0x8086, device: 0x100E}` → `drv.e1000.0`
2. **Class code fallback** — e.g., class `0x020000` (Ethernet controller) → generic NIC driver
3. **Compatible string match** — e.g., `"virtio,net"` → `drv.virtio-net.0`

```
function match_driver(device):
    for manifest in driver_registry:
        if manifest.vendor_id != 0 && manifest.vendor_id == device.vendor_id:
            if manifest.device_id == device.device_id:
                return manifest
        if manifest.compatible != NULL:
            if device_has_compatible(device, manifest.compatible):
                return manifest
        if manifest.class_code != 0 && manifest.class_code == device.class_code:
            return manifest
    return NULL   /* no driver found */
```

### 2.5 Device Lifecycle State Machine

```
                    ┌──────────────────────────────────────────┐
                    │              DISCOVERED                   │
                    │  (PCIe enumeration found device,          │
                    │   no driver matched or not yet matched)   │
                    └──────────────┬───────────────────────────┘
                                   │ match found
                                   ▼
                    ┌──────────────────────────────────────────┐
                    │              MATCHED                      │
                    │  (driver manifest selected,               │
                    │   capabilities being provisioned)         │
                    └──────────────┬───────────────────────────┘
                                   │ create_sidecar() succeeded
                                   ▼
                    ┌──────────────────────────────────────────┐
                    │         DRIVER_RUNNING                    │
                    │  (driver sidecar alive and serving,       │
                    │   channels established with clients)      │
                    └──────┬─────────────────┬────────────────┘
                           │                 │
                    driver crashed       device removed
                           │                 │
                           ▼                 ▼
                    ┌──────────────┐  ┌──────────────────────────┐
                    │    FAILED    │  │         REMOVED           │
                    │ (crash count │  │  (PCIe AER or hotplug     │
                    │  < budget)   │  │   notification received)   │
                    └──────┬───────┘  └──────────┬──────────────┘
                           │                     │
                      retry budget          revoke all caps
                      exhausted?            notify clients
                           │                     │
                     yes   │   no          ┌─────▼──────────────────┐
                           │   │           │        REMOVED_WAIT    │
                           │   └──────────►│  (clients notified,    │
                           │    backoff    │   awaiting re-detect   │
                           │    + respawn  │   or manual intervention│
                           │               └────────────────────────┘
                           │
                    ┌──────▼───────┐
                    │    DEAD       │
                    │ (no more      │
                    │  restarts)    │
                    └──────────────┘
```

**State transition table:**

| Current State | Event | Next State | Actions |
|---|---|---|---|
| DISCOVERED | driver match found | MATCHED | Load manifest, validate caps |
| MATCHED | create_sidecar() ok | DRIVER_RUNNING | Wire IRQ channels, notify clients |
| MATCHED | create_sidecar() fails | FAILED | Log, backoff, retry |
| DRIVER_RUNNING | driver exits (exit code 0) | REMOVED | Revoke caps, notify clients |
| DRIVER_RUNNING | driver crashes (signal/fault) | FAILED | Log, revoke caps, backoff+respawn |
| DRIVER_RUNNING | PCIe device removed | REMOVED_WAIT | Mask IRQ, revoke caps, notify clients |
| FAILED | retry budget < max | MATCHED | Backoff timer → respawn |
| FAILED | retry budget exhausted | DEAD | Log, wait for operator or `sidecar_spawn` |
| REMOVED_WAIT | device re-detected | MATCHED | Re-enumerate, rematch, respawn |
| DEAD | operator restart | MATCHED | Re-enumerate, rematch, respawn |

### 2.6 Where the Device Manager Gets Its Capabilities

The Device Manager is **created by the kernel at boot** — it is the first sidecar after the POSIX core. Its capabilities come from the kernel's boot manifest, not from any other sidecar:

```
Kernel boot sequence:
  1. cap_init() — arena, tables, holders, channels
  2. create_sidecar("mgr.device.0") — Device Manager
     → kernel mints: BUS_ACCESS, IRQ controller channel, control channel to parent,
       MEM cap for config store, console channel
  3. Device Manager enumerates PCI bus
  4. For each discovered device:
     → Device Manager calls create_sidecar("drv.e1000.0", device_context)
     → kernel mints: IO_PORT (BAR MMIO), IRQ (interrupt line),
       DMA_MEM (initial RX/TX ring buffers),
       control channel to Device Manager, console channel
```

The Device Manager's BUS_ACCESS capability grants it the right to enumerate the bus and read/write config space. It does **not** have the right to directly access device MMIO — it discovers devices and then hands their capabilities to the spawned driver sidecars. This is the key separation: the Device Manager knows topology; driver sidecars know hardware protocols.

### 2.7 Hotplug and Removal

PCIe hotplug is detected via:
1. **AER (Advanced Error Reporting):** the Device Manager subscribes to a kernel-internal AER notification channel. AER errors include device disappearance (CPL timeout, poison TLP).
2. **ACPI hotplug:** the firmware notifies the kernel of slot changes; the Device Manager polls via an IPC port.
3. **Polling fallback:** periodic re-enumeration of the PCI bus at a configurable interval (default: 5 seconds).

On removal detection:

```
function handle_device_removal(device):
    driver = device.active_driver
    if driver == NULL:
        return  /* no driver was running */

    /* 1. Mask interrupts */
    mask_irq(driver.irq_cap)

    /* 2. Notify clients via a REMOVED event on the driver's service channel */
    for client in driver.clients:
        send_event(client.channel, EVT_DEVICE_REMOVED, device.id)

    /* 3. Kill the driver sidecar */
    process_kill(driver.pid)

    /* 4. Revoke all capabilities */
    cap_revoke(driver.io_port_cap)
    cap_revoke(driver.irq_cap)
    for dma_cap in driver.dma_caps:
        cap_revoke(dma_cap)

    /* 5. Update state machine */
    device.state = REMOVED_WAIT
```

---

## 3. IRQ Delivery via Channels

### 3.1 Design Principle

Hardware interrupts are **not** delivered as signals or traps to sidecar code. Instead, the kernel translates each interrupt into a CHAN message delivered to a dedicated IRQ channel. This means:

- Drivers receive interrupts as ordinary channel messages — no special handler registration.
- IRQ delivery is subject to the same capability checks as any channel message.
- IRQ caps can be revoked to cleanly disable interrupt delivery.
- The driver's interrupt handler is just its message-processing loop — no distinction between "interrupt" and "RPC" messages.

### 3.2 IRQ Channel Binding

When an IRQ cap is created (or when a driver is spawned with an IRQ cap), the kernel binds the IRQ to a dedicated channel pair. The IRQ cap holds a reference to the channel's channel object:

```c
struct CapObject {
    /* ... */
    uint32_t irq_chan_id;     /* channel id for IRQ message delivery */
};
```

The channel is created internally by the kernel during IRQ cap creation. The IRQ channel is **unidirectional for interrupts**: the kernel writes IRQ messages into the channel's queue; the driver reads them via `cap_recv_msg()`. The driver can send ACK messages back through a separate control channel (see §3.4).

### 3.3 IRQ Message Format

Each interrupt is delivered as a channel message with a fixed-format payload:

```c
struct IRQMessage {
    uint32_t irq_number;        /* the interrupt source (PLIC source ID) */
    uint32_t timestamp_lo;      /* low 32 bits of hardware timestamp (cycle counter) */
    uint32_t timestamp_hi;      /* high 32 bits */
    uint16_t sequence;          /* monotonically increasing per-IRQ cap */
    uint8_t  priority;          /* 0=lowest..255=highest (maps from PLIC priority) */
    uint8_t  flags;             /* bit0=COALESCED (multiple IRQs collapsed), bit1=SHARED_IRQ */
    uint32_t coalesce_count;    /* if COALESCED: how many interrupts were merged */
    uint32_t device_status;     /* driver-specific status bits (optional, read from device) */
};
```

Total: 24 bytes, well within `CAP_MSG_MAX_PAYLOAD` (4096 bytes). Delivered via `cap_send_msg()` with `n_caps=0` (no capabilities transferred with interrupt messages — IRQ messages carry data only).

**Sequence numbers:** Each IRQ cap maintains a monotonic `sequence` counter. The driver uses gaps in sequence numbers to detect lost interrupts. If the channel queue overflows (all `CHAN_QUEUE_DEPTH` slots full), the kernel increments a `dropped_count` counter and delivers a single overflow notification message with `flags |= CAP_IRQ_OVERFLOW`.

### 3.4 IRQ Priority and Coalescing

**Priority mapping:**
- RISC-V PLIC: source priority (0–7) is mapped to IRQ message priority as `priority = source_priority * 36` (saturating at 255).
- x86 MSI/MSI-X: vector number is mapped as `priority = min(vector, 255)`.

**Coalescing:** The IRQ cap's `irq_coalesce_us` field (set at creation time) specifies a minimum interval between IRQ messages. If two interrupts from the same source arrive within the coalescing window, the kernel merges them:

```
function deliver_irq(irq_cap, interrupt):
    now = read_hardware_timer()
    if (now - irq_cap.last_deliver_us) < irq_cap.coalesce_us:
        /* Coalesce: increment count, don't send yet */
        irq_cap.coalesce_count++
        return

    /* Deliver */
    msg.irq_number = irq_cap.irq_number
    msg.sequence = irq_cap.next_sequence++
    msg.coalesce_count = irq_cap.coalesce_count + 1
    msg.flags = (irq_cap.coalesce_count > 0) ? CAP_IRQ_COALESCED : 0

    chan_send(irq_cap.irq_chan_id, &msg, sizeof(msg))
    irq_cap.last_deliver_us = now
    irq_cap.coalesce_count = 0
```

### 3.5 Masking and Unmasking

Drivers can mask/unmask interrupts via the IRQ cap's `CAP_PERM_IRQ_MASK` permission:

```c
#define SYS_SLS_IRQ_MASK    309
#define SYS_SLS_IRQ_UNMASK  310

struct SLSIRQMaskRequest {
    uint16_t irq_cap_idx;    /* IRQ capability slot */
    uint8_t  masked;          /* 1 = mask, 0 = unmask */
    uint8_t  _pad;
};
```

**Masking behavior:**
- Masked: the kernel disables the interrupt at the PLIC/APLIC source (clears the enable bit). No IRQ messages are delivered. Existing queued messages remain in the channel.
- Unmasked: the kernel re-enables the interrupt source. If the interrupt is pending (hardware line asserted), a new IRQ message is delivered immediately.

**Kernel-side masking on revocation:** When `cap_revoke()` is called on an IRQ cap, the kernel masks the interrupt source before the holder walk — this is the first action, ensuring no IRQ messages can arrive after revocation begins.

### 3.6 Full Sequence: Hardware Interrupt to Driver Handler

```
1. Device asserts interrupt line (e.g., NIC RX descriptor done)

2. RISC-V PLIC or x86 APIC/MSI delivers interrupt to the kernel
   → trap_riscv.c or isr_stub receives the interrupt
   → identifies the PLIC source ID (or MSI vector)

3. Kernel's interrupt dispatch:
   a. Look up the IRQ cap for this source
   b. If no cap exists: spurious interrupt, log and ignore
   c. If cap is masked: set pending bit, return
   d. Check coalescing window

4. Build IRQMessage:
   msg.irq_number = source_id
   msg.timestamp = read_cycle_counter()
   msg.sequence = cap->next_sequence++
   msg.priority = plic_priority_to_msg(source_id)

5. Enqueue into IRQ channel:
   cap_send_msg(driver_pid, irq_chan_wr, &msg, sizeof(msg), ...)
   If channel full: increment dropped_count, return

6. If driver is parked on cap_recv_msg() for this channel:
   → cap_wake_chan() wakes the driver sidecar
   → scheduler schedules it to run

7. Driver sidecar's message loop receives the IRQ message:
   recv_msg(irq_chan_rd, &msg)
   // msg.irq_number tells which device generated the interrupt
   // driver reads device status registers via IO_PORT/MMIO cap
   // driver processes the event (e.g., reads RX ring)

8. Driver acknowledges the interrupt:
   a. Write to device's interrupt acknowledge register (via IO_PORT cap)
   b. Send ACK message back to the Device Manager (or directly to
      the PLIC/APLIC via an ACK syscall):
      ack_irq(irq_cap_idx)

9. Kernel performs EOI (End of Interrupt):
   → PLIC: write source priority to the PLIC claim/complete register
   → x86 APIC: write to EOI register
```

### 3.7 Interrupt Acknowledgment Syscall

```c
#define SYS_SLS_IRQ_ACK  311

struct SLSIRQAckRequest {
    uint16_t irq_cap_idx;    /* IRQ capability slot */
    uint8_t  _pad[6];
};
```

The kernel's `sys_sls_irq_ack()` performs the hardware EOI:
- RISC-V PLIC: `PLIC_CLAIM = irq_number` (writes to the claim register to signal completion).
- x86 APIC: writes to the IOAPIC EOI register or the APIC EOI MSR.
- MSI/MSI-X: no explicit EOI needed (edge-triggered), but the kernel still performs the claim cycle for bookkeeping.

The driver must EOI within a configurable timeout (default: 10 ms). If the timeout expires without an EOI, the kernel logs a warning and re-EOIs automatically (the interrupt may be stuck or the driver may be too slow). Three consecutive timeout EOIs trigger a driver fault notification to the Device Manager.

---

## 4. DMA Buffer Allocator API

### 4.1 Design Goals

- Drivers request physically contiguous, pinned memory for DMA ring buffers and packet payloads.
- The kernel programs IOMMU mappings so a device can only DMA to its own buffers.
- DMA buffers can be shared with other sidecars via MEM caps for zero-copy paths.
- The allocator is bounded: fixed-size pool, no dynamic growth.

### 4.2 Data Structures

```c
/* DMA pool: a separate physically contiguous region carved from the frame pool at boot.
 * Distinct from the cap arena (which is for general shared memory).
 * Size: configurable via boot parameter, default 128 MiB (32768 frames). */
#define DMA_POOL_MAX_FRAMES   32768
#define DMA_POOL_FRAME_SIZE   4096

struct DMAPool {
    uint64_t base_phys;                          /* physical base address */
    uint32_t total_frames;                       /* total frames in pool */
    uint32_t free_frames;                        /* currently free */
    uint8_t  bitmap[(DMA_POOL_MAX_FRAMES + 7) / 8];  /* allocation bitmap */
    struct DMABuffer {
        uint32_t frame_index;                    /* base frame index */
        uint32_t npages;                         /* number of contiguous pages */
        uint32_t iommu_domain;                   /* owning IOMMU domain */
        uint16_t owner_pid;                      /* owning sidecar pid */
        uint8_t  flags;                          /* COHERENT, READ_ONLY_DEVICE, etc. */
        uint8_t  shared_count;                   /* number of MEM caps referencing this */
        uint32_t next_free;                      /* freelist next (only when free) */
    } buffers[DMA_BUFFER_MAX];                   /* 1024 buffer slots */
    uint32_t buffer_free_head;                   /* freelist head */
    uint32_t buffer_alloc_count;                 /* monotonic allocation counter */
};
```

```c
#define DMA_BUFFER_MAX     1024
#define DMA_BUF_ALIGN_4K   1
#define DMA_BUF_ALIGN_64K  16
#define DMA_BUF_ALIGN_2M   512

struct SLSDMAAllocRequest {
    uint32_t npages;          /* number of 4K pages, must be power of 2 for large alignments */
    uint32_t align_pages;     /* minimum alignment in pages (1 = 4K, 16 = 64K, 512 = 2M) */
    uint8_t  flags;           /* DMA_BUF_FLAG_* */
    uint8_t  _pad[3];
    uint32_t out_cap_idx;     /* [out] MEM cap index in caller's table */
    uint32_t out_dma_buf_id;  /* [out] DMA buffer id (for IOMMU operations) */
};

struct SLSDMAFreeRequest {
    uint32_t dma_buf_id;      /* DMA buffer id to free */
    uint8_t  _pad[4];
};

struct SLSDMAShareRequest {
    uint32_t dma_buf_id;      /* DMA buffer to share */
    uint32_t target_pid;      /* sidecar to share with */
    uint8_t  rights;          /* CAP_PERM_R | CAP_PERM_W */
    uint8_t  _pad[3];
    uint32_t out_cap_idx;     /* [out] MEM cap in target's table */
};
```

### 4.3 Syscalls

```c
#define SYS_SLS_DMA_ALLOC      312
#define SYS_SLS_DMA_FREE        313
#define SYS_SLS_DMA_SHARE       314
#define SYS_SLS_DMA_IOMMU_MAP   315
#define SYS_SLS_DMA_IOMMU_UNMAP 316
```

### 4.4 Pseudocode: DMA Buffer Allocation

```
function dma_alloc(npages, align_pages, flags, caller_pid):
    /* Validate */
    if npages == 0 or npages > DMA_POOL_MAX_FRAMES:
        return ERR(EINVAL)
    if not is_power_of_two(align_pages):
        return ERR(EINVAL)

    lock(dma_pool.lock)

    /* Find first-fit in bitmap with alignment */
    start = find_free_run(dma_pool.bitmap, npages, align_pages, dma_pool.total_frames)
    if start == NOT_FOUND:
        unlock(dma_pool.lock)
        return ERR(ENOMEM)

    /* Mark frames as allocated */
    for i in 0..npages-1:
        dma_pool.bitmap[start + i] = 1

    /* Allocate a buffer slot */
    buf_idx = buffer_freelist_pop(&dma_pool)
    if buf_idx == NOT_FOUND:
        /* Rollback: unmark frames */
        for i in 0..npages-1:
            dma_pool.bitmap[start + i] = 0
        unlock(dma_pool.lock)
        return ERR(ENOMEM)

    buf = &dma_pool.buffers[buf_idx]
    buf.frame_index = start
    buf.npages = npages
    buf.iommu_domain = get_device_domain(caller_pid)  /* which device this sidecar drives */
    buf.owner_pid = caller_pid
    buf.flags = flags
    buf.shared_count = 0

    phys_base = dma_pool.base_phys + start * DMA_POOL_FRAME_SIZE

    /* Pin pages: mark as non-reclaimable in frame_pool */
    pin_pages(phys_base, npages)

    /* Create MEM cap in caller's table */
    cap_idx = cap_create_mem(caller_pid, phys_base, npages,
                             CAP_PERM_R | CAP_PERM_W | CAP_PERM_MAP | CAP_PERM_DMA_SHARE)

    /* Program IOMMU mapping */
    iommu_map(buf.iommu_domain, phys_base, npages)

    unlock(dma_pool.lock)

    out.dma_buf_id = buf_idx
    out.cap_idx = cap_idx
    return OK
```

### 4.5 Pseudocode: DMA Buffer Free

```
function dma_free(dma_buf_id, caller_pid):
    lock(dma_pool.lock)

    buf = &dma_pool.buffers[dma_buf_id]
    if buf.owner_pid != caller_pid:
        unlock(dma_pool.lock)
        return ERR(EPERM)

    /* Unmap from IOMMU */
    phys_base = dma_pool.base_phys + buf.frame_index * DMA_POOL_FRAME_SIZE
    iommu_unmap(buf.iommu_domain, phys_base, buf.npages)

    /* Unpin pages */
    unpin_pages(phys_base, buf.npages)

    /* Free frames in bitmap */
    for i in 0..buf.npages-1:
        dma_pool.bitmap[buf.frame_index + i] = 0

    /* Free buffer slot */
    buffer_freelist_push(&dma_pool, dma_buf_id)
    dma_pool.free_frames += buf.npages

    /* Revoke any outstanding MEM caps referencing this buffer */
    revoke_dma_caps(dma_buf_id)

    unlock(dma_pool.lock)
    return OK
```

### 4.6 Pseudocode: DMA Buffer Share

```
function dma_share(dma_buf_id, target_pid, rights, caller_pid):
    lock(dma_pool.lock)

    buf = &dma_pool.buffers[dma_buf_id]
    if buf.owner_pid != caller_pid:
        unlock(dma_pool.lock)
        return ERR(EPERM)

    if buf.shared_count >= DMA_MAX_SHARES:
        unlock(dma_pool.lock)
        return ERR(ENOSPC)

    phys_base = dma_pool.base_phys + buf.frame_index * DMA_POOL_FRAME_SIZE

    /* Create a new MEM cap in the target's table */
    cap_idx = cap_create_mem(target_pid, phys_base, buf.npages, rights)
    if cap_idx < 0:
        unlock(dma_pool.lock)
        return cap_idx

    buf.shared_count++

    unlock(dma_pool.lock)
    return OK(out_cap_idx = cap_idx)
```

### 4.7 IOMMU Enforcement

The IOMMU is the hardware mechanism that prevents a device from DMA-ing to memory it should not access. Each driver sidecar's device gets its own IOMMU domain.

**IOMMU domain lifecycle:**

```
function iommu_create_domain(device_id):
    domain_id = next_domain_id++
    iommu_context->domains[domain_id] = {
        .device_id = device_id,
        .page_table_root = allocate_page_table(),
        .map_count = 0
    }
    /* Bind device to domain via IOMMU device table */
    iommu_set_device_domain(device_id, domain_id)
    return domain_id

function iommu_map(domain_id, phys_base, npages):
    domain = iommu_context->domains[domain_id]
    for page in 0..npages-1:
        iommu_page_table_map(domain.page_table_root,
            virtual_address = phys_base,  /* identity map: device sees phys addr */
            physical_address = phys_base + page * 4096,
            permissions = RW)             /* device always gets read/write */
    domain.map_count++

function iommu_unmap(domain_id, phys_base, npages):
    domain = iommu_context->domains[domain_id]
    for page in 0..npages-1:
        iommu_page_table_unmap(domain.page_table_root, phys_base + page * 4096)
    domain.map_count -= npages
```

**Key security property:** A driver can only DMA to physical pages that:
1. Were allocated by the DMA allocator for that driver's device.
2. Are mapped in that device's IOMMU domain.
3. Are backed by valid MEM caps in the driver's capability table.

A buggy or malicious driver cannot DMA to arbitrary physical memory because the IOMMU silently blocks unmapped addresses (device sees a bus error or reads zeros).

---

## 5. Driver Manifest Format

### 5.1 Binary Layout

Driver manifests are packed binary blobs, validated at boot by the kernel and stored in the kernel's manifest registry. The format extends the existing sidecar manifest format from Phase 2 (`user/proto/src/manifest.rs`).

```
Driver Manifest Binary Format (little-endian, byte-aligned):
┌─────────────────────────────────────────────────────────────┐
│ Offset  │ Size  │ Field                                     │
├─────────┼───────┼───────────────────────────────────────────┤
│ 0       │ 4     │ magic: 0x41455244 ("AERD")               │
│ 4       │ 2     │ version: manifest format version (1)      │
│ 6       │ 2     │ total_length: bytes of this manifest      │
│ 8       │ 4     │ crc32: checksum of bytes [8..total_length]│
│ 12      │ 16    │ name: null-terminated UTF-8 driver name   │
│         │       │   e.g. "drv.e1000.0"                      │
│ 28      │ 2     │ vendor_id: PCI vendor ID (0 = any)        │
│ 30      │ 2     │ device_id: PCI device ID (0 = any)        │
│ 32      │ 4     │ class_code: PCI class code (0 = any)      │
│ 36      │ 2     │ compatible_len: length of compatible str   │
│ 38      │ N     │ compatible: null-terminated compatible str │
│         │       │   e.g. "intel,e1000" or "virtio,net"      │
│ 38+N    │ 2     │ driver_image_len: bytes of driver binary   │
│ 40+N    │ M     │ driver_image: ELF or flat binary           │
│ 40+N+M  │ 4     │ entry_offset: entry point offset in image │
├─────────┼───────┼───────────────────────────────────────────┤
│ Capability Requirements (variable count):                    │
│         │ 1     │ n_requirements: number of cap entries      │
│         │ 1     │ cap_type: IO_PORT / IRQ / DMA_MEM / etc   │
│         │ 1     │ cap_perm: required permissions             │
│         │ 1     │ cap_count: number of this cap needed       │
│         │ 4     │ cap_detail: type-specific params           │
│         │  ...  │ (repeat for each requirement)              │
├─────────┼───────┼───────────────────────────────────────────┤
│ Resource Limits:                                             │
│         │ 4     │ max_memory_bytes: RAM budget               │
│         │ 4     │ max_dma_frames: DMA buffer budget          │
│         │ 2     │ max_channels: channel cap budget           │
│         │ 2     │ max_irqs: IRQ cap budget                   │
│         │ 4     │ max_cpu_us_per_sec: CPU time budget        │
│         │ 4     │ stack_pages: user stack size in pages      │
├─────────┼───────┼───────────────────────────────────────────┤
│ Exported Services (variable count):                          │
│         │ 1     │ n_exports: number of service entries       │
│         │ 16    │ service_name: null-terminated service name │
│         │ 2     │ service_port: IPC port for this service    │
│         │ 2     │ service_version: version number            │
│         │  ...  │ (repeat for each export)                   │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Cap Requirement Types

```c
#define MANIFEST_CAP_IO_PORT   0x01   /* detail: {base_phys, length, width} */
#define MANIFEST_CAP_IRQ       0x02   /* detail: {irq_number, trigger} */
#define MANIFEST_CAP_DMA_BUF   0x03   /* detail: {min_pages, align_pages} */
#define MANIFEST_CAP_MEM       0x04   /* detail: {npages, perm} */
#define MANIFEST_CAP_CHAN_R    0x05   /* detail: {service_name_hash} */
#define MANIFEST_CAP_CHAN_W    0x06   /* detail: {service_name_hash} */
#define MANIFEST_CAP_CONSOLE   0x07   /* detail: {0} (always one console) */
```

### 5.3 Example: e1000 NIC Driver Manifest

```json
{
  "magic": "0x41455244",
  "version": 1,
  "name": "drv.e1000.0",
  "description": "Intel e1000 Gigabit Ethernet driver sidecar",
  "vendor_id": "0x8086",
  "device_id": "0x100E",
  "class_code": "0x020000",
  "compatible": "intel,e1000",
  "driver_image": "e1000_driver.bin",
  "entry_offset": "0x1000",

  "capabilities": [
    {
      "type": "IO_PORT",
      "perm": "READ|WRITE",
      "count": 1,
      "detail": { "base_phys": "dynamic", "length": "128KiB", "width": 4,
                  "flags": "UNCACHEABLE" }
    },
    {
      "type": "IRQ",
      "perm": "LISTEN|ACK|MASK",
      "count": 1,
      "detail": { "irq_number": "dynamic", "trigger": "edge_rising",
                  "coalesce_us": 50 }
    },
    {
      "type": "DMA_BUF",
      "perm": "READ|WRITE",
      "count": 3,
      "detail": [
        { "purpose": "TX_RING", "min_pages": 1, "align_pages": 16,
          "note": "128 descriptors x 16B = 2 KiB, aligned to 64K for e1000" },
        { "purpose": "RX_RING", "min_pages": 1, "align_pages": 16,
          "note": "128 descriptors x 16B = 2 KiB" },
        { "purpose": "RX_BUFFERS", "min_pages": 128, "align_pages": 1,
          "note": "128 x 2048B = 256 KiB, one page per buffer" }
      ]
    },
    {
      "type": "CHAN_R",
      "perm": "RECV",
      "count": 1,
      "detail": { "service": "stack.ipv4" }
    },
    {
      "type": "CHAN_W",
      "perm": "SEND",
      "count": 1,
      "detail": { "service": "stack.ipv4" }
    },
    {
      "type": "CONSOLE",
      "perm": "READ|WRITE",
      "count": 1,
      "detail": {}
    }
  ],

  "resource_limits": {
    "max_memory_bytes": "4194304",
    "max_dma_frames": 160,
    "max_channels": 8,
    "max_irqs": 2,
    "max_cpu_us_per_sec": 100000,
    "stack_pages": 8
  },

  "exported_services": [
    {
      "name": "nic.transmit",
      "port": 0x3001,
      "version": 1
    },
    {
      "name": "nic.receive",
      "port": 0x3002,
      "version": 1
    },
    {
      "name": "nic.control",
      "port": 0x3003,
      "version": 1
    }
  ]
}
```

### 5.4 Example: virtio-net Driver Manifest

```json
{
  "magic": "0x41455244",
  "version": 1,
  "name": "drv.virtio-net.0",
  "description": "Virtio Network Device driver sidecar",
  "vendor_id": "0x1AF4",
  "device_id": "0x1000",
  "class_code": "0x020000",
  "compatible": "virtio,net",
  "driver_image": "virtio_net_driver.bin",
  "entry_offset": "0x1000",

  "capabilities": [
    {
      "type": "IO_PORT",
      "perm": "READ|WRITE",
      "count": 1,
      "detail": { "base_phys": "dynamic", "length": "4KiB", "width": 4,
                  "flags": "UNCACHEABLE|BE_MEM" }
    },
    {
      "type": "IRQ",
      "perm": "LISTEN|ACK|MASK",
      "count": 1,
      "detail": { "irq_number": "dynamic", "trigger": "edge_rising",
                  "coalesce_us": 20 }
    },
    {
      "type": "DMA_BUF",
      "perm": "READ|WRITE",
      "count": 4,
      "detail": [
        { "purpose": "CTRL_QUEUE", "min_pages": 1, "align_pages": 1 },
        { "purpose": "RX_QUEUE", "min_pages": 2, "align_pages": 16 },
        { "purpose": "TX_QUEUE", "min_pages": 2, "align_pages": 16 },
        { "purpose": "RX_BUFFERS", "min_pages": 64, "align_pages": 1 }
      ]
    },
    {
      "type": "CHAN_R",
      "perm": "RECV",
      "count": 1,
      "detail": { "service": "stack.ipv4" }
    },
    {
      "type": "CHAN_W",
      "perm": "SEND",
      "count": 1,
      "detail": { "service": "stack.ipv4" }
    },
    {
      "type": "CONSOLE",
      "perm": "READ|WRITE",
      "count": 1,
      "detail": {}
    }
  ],

  "resource_limits": {
    "max_memory_bytes": "2097152",
    "max_dma_frames": 70,
    "max_channels": 8,
    "max_irqs": 1,
    "max_cpu_us_per_sec": 50000,
    "stack_pages": 8
  },

  "exported_services": [
    {
      "name": "nic.transmit",
      "port": 0x3011,
      "version": 1
    },
    {
      "name": "nic.receive",
      "port": 0x3012,
      "version": 1
    }
  ]
}
```

---

## 6. Full NIC Driver Sidecar Lifecycle

### 6.1 Spawn and Initialization

```
/* Pseudocode for the e1000 NIC driver sidecar */

function _start(bib: BootInfoBlock):
    /* 1. Parse boot info block — kernel-provided capability table */
    irq_cap = bib.caps[0]          /* IRQ capability for this NIC */
    io_port_cap = bib.caps[1]      /* IO_PORT capability for BAR0 MMIO */
    dma_tx_ring = bib.caps[2]      /* DMA_MEM for TX descriptor ring */
    dma_rx_ring = bib.caps[3]      /* DMA_MEM for RX descriptor ring */
    dma_rx_bufs = bib.caps[4]      /* DMA_MEM for RX packet buffers */
    net_stack_chan_wr = bib.caps[5] /* CHAN_W to network stack */
    net_stack_chan_rd = bib.caps[6] /* CHAN_R from network stack */
    control_chan_rd = bib.caps[7]   /* CHAN_R from Device Manager (control) */

    /* 2. Map IO_PORT capability into address space */
    mmio_base = cap_map(io_port_cap, NULL, CAP_PERM_R | CAP_PERM_W)
    /* mmio_base is the virtual address where BAR0 is mapped */

    /* 3. Map DMA_MEM capabilities into address space */
    tx_ring = cap_map(dma_tx_ring, NULL, CAP_PERM_R | CAP_PERM_W)
    rx_ring = cap_map(dma_rx_ring, NULL, CAP_PERM_R | CAP_PERM_W)
    rx_bufs = cap_map(dma_rx_bufs, NULL, CAP_PERM_R | CAP_PERM_W)

    /* 4. Read MAC address from device */
    ral = mmio_read32(mmio_base + E1000_REG_RAL0)
    rah = mmio_read32(mmio_base + E1000_REG_RAH0)
    mac = extract_mac(ral, rah)

    /* 5. Initialize TX ring (128 descriptors, each 16 bytes) */
    for i in 0..E1000_RING_SIZE-1:
        tx_ring[i].buffer_addr = 0
        tx_ring[i].status = 0xFF  /* mark all as done */
    mmio_write32(mmio_base + E1000_REG_TDBAL, low32(dma_tx_ring_phys))
    mmio_write32(mmio_base + E1000_REG_TDBAH, high32(dma_tx_ring_phys))
    mmio_write32(mmio_base + E1000_REG_TDLEN, E1000_RING_SIZE * 16)
    mmio_write32(mmio_base + E1000_REG_TDH, 0)
    mmio_write32(mmio_base + E1000_REG_TDT, 0)
    mmio_write32(mmio_base + E1000_REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x040 << 12))

    /* 6. Initialize RX ring (128 descriptors, 128 x 2048B buffers) */
    for i in 0..E1000_RING_SIZE-1:
        rx_ring[i].buffer_addr = rx_bufs_phys + i * E1000_RX_BUF_SIZE
        rx_ring[i].status = 0
    mmio_write32(mmio_base + E1000_REG_RDBAL, low32(dma_rx_ring_phys))
    mmio_write32(mmio_base + E1000_REG_RDBAH, high32(dma_rx_ring_phys))
    mmio_write32(mmio_base + E1000_REG_RDLEN, E1000_RING_SIZE * 16)
    mmio_write32(mmio_base + E1000_REG_RDH, 0)
    mmio_write32(mmio_base + E1000_REG_RDT, E1000_RING_SIZE - 1)
    mmio_write32(mmio_base + E1000_REG_RCTL, RCTL_EN | RCTL_BAM | (1<<3) | (1<<4))

    /* 7. Send NIC_INFO to network stack: MAC address, capabilities */
    send_msg(net_stack_chan_wr, NIC_INFO, { mac, max_frame_size=1514 })

    /* 8. Enter main event loop */
    event_loop(irq_cap, net_stack_chan_rd, net_stack_chan_wr, control_chan_rd)
```

### 6.2 Event Loop

```
function event_loop(irq_cap, net_rd, net_wr, ctrl_rd):
    while true:
        /* Non-blocking poll on all channels */
        result = cap_recv_any(irq_cap, net_rd, ctrl_rd, timeout=10ms)

        switch result.source:
            case IRQ_CHANNEL:
                handle_interrupt(result.msg)

            case NET_STACK_CHANNEL:
                handle_stack_message(result.msg)

            case CONTROL_CHANNEL:
                handle_control_message(result.msg)

            case TIMEOUT:
                /* Periodic maintenance: reclaim completed TX descriptors */
                reclaim_tx_completions()
```

### 6.3 Interrupt Handler

```
function handle_interrupt(msg: IRQMessage):
    /* Read device status to determine cause */
    icr = mmio_read32(mmio_base + E1000_REG_ICR)  /* Interrupt Cause Read */

    if icr & E1000_ICR_RXT0:    /* RX timer interrupt */
        deliver_rx_packets()

    if icr & E1000_ICR_TXDW:    /* TX descriptor written back */
        reclaim_tx_completions()

    /* Acknowledge the IRQ */
    irq_ack(irq_cap)
```

### 6.4 RX Path (Hardware → Driver → Network Stack)

```
function deliver_rx_packets():
    while true:
        next = (rx_tail + 1) % E1000_RING_SIZE
        desc = &rx_ring[next]

        if !(desc.status & 0x01):  /* DD bit not set: no more packets */
            break

        packet_len = desc.length
        packet_phys = rx_bufs_phys + next * E1000_RX_BUF_SIZE

        /* Create a MEM cap for this packet buffer and send to stack */
        packet_cap = cap_create_mem(my_pid, packet_phys,
                                     pages_for(packet_len),
                                     CAP_PERM_R)
        send_msg(net_wr, RX_PACKET, {
            cap = packet_cap,
            length = packet_len,
            buf_id = next,          /* so stack can return the buffer */
            checksum_ok = !(desc.status & 0x02)  /* IPCSV bit */
        })

        /* Return descriptor to hardware */
        desc.status = 0
        mmio_write32(mmio_base + E1000_REG_RDT, next)
        rx_tail = next
```

**Zero-copy detail:** The network stack receives a MEM cap pointing at the DMA buffer's physical memory. The stack maps this into its own address space via `cap_map()` and reads the packet data directly — no copy from driver to stack. After processing, the stack sends the buffer back (or the driver reclaims it after a timeout).

### 6.5 TX Path (Network Stack → Driver → Hardware)

```
function handle_stack_message(msg):
    switch msg.opcode:
        case TX_PACKET:
            transmit_packet(msg.buf_phys, msg.length, msg.buf_cap)

function transmit_packet(phys_buf, length, buf_cap):
    desc = &tx_ring[tx_tail]
    desc.buffer_addr = phys_buf
    desc.length = length
    desc.cmd = EOP | IFCS | RS  /* end of packet, insert FCS, report status */
    desc.status = 0

    /* Advance tail and kick the hardware */
    tx_tail = (tx_tail + 1) % E1000_RING_SIZE
    mmio_write32(mmio_base + E1000_REG_TDT, tx_tail)

    /* Wait for descriptor completion (optional: async via IRQ) */
    timeout = 0
    while !(desc.status & 0x01):
        if ++timeout > 2000000:
            break   /* timeout: will be reclaimed later */
        pause()

    /* Notify the stack that the buffer can be reused */
    send_msg(net_wr, TX_DONE, { buf_cap = buf_cap, success = (desc.status & 0x01) })
```

### 6.6 Message Protocol Between NIC Driver and Network Stack

```
NIC Driver → Network Stack:
  RX_PACKET { buf_cap, length, buf_id, checksum_ok }
  TX_DONE   { buf_cap, success }
  NIC_INFO  { mac, max_frame_size, link_status }

Network Stack → NIC Driver:
  TX_PACKET { buf_phys, length, buf_cap }
  SET_MAC   { mac }
  GET_STATUS {}
  SET_LINK  { up/down }
```

All messages flow over the driver's `net_stack_chan_wr` / `net_stack_chan_rd` channels. MEM caps travel with `TX_PACKET` and `RX_PACKET` for zero-copy.

---

## 7. Crash Recovery

### 7.1 Detection

The Device Manager monitors driver sidecars via three mechanisms:

1. **Channel close event:** When a driver sidecar exits (voluntarily or due to a crash), the kernel tears down its capability table (Phase 2's `cap_table_teardown()`), which closes all channel endpoints. The Device Manager's `cap_recv_msg()` on the driver's control channel returns a `CLOSE_PEER_DEAD` event.

2. **Scheduler notification:** The kernel's `process_exit()` notifies the Device Manager via a dedicated IPC channel when a supervised process terminates (the PID is registered in the Device Manager's supervision table at spawn time).

3. **Health check poll:** The Device Manager periodically sends a `HEARTBEAT` message on each driver's control channel. If the driver does not respond within a timeout (default: 5 seconds), the driver is declared unhealthy and recovery begins.

### 7.2 Recovery Sequence

```
function handle_driver_crash(device):
    /* Phase 1: Quarantine (immediate) */

    /* 1.1 Mask all IRQs for this device */
    mask_irq(device.irq_cap)

    /* 1.2 Revoke all capabilities held by the dead driver */
    /*     This happens automatically via cap_table_teardown() in the kernel, */
    /*     but the Device Manager also explicitly revokes any caps it holds */
    /*     referencing the dead driver (e.g., control channel) */
    cap_revoke(device.control_chan_cap)

    /* 1.3 Notify all clients of the failure */
    for client in device.registered_clients:
        send_event(client.channel, EVT_DRIVER_FAILED,
            { device_id = device.id, reason = CRASH, exit_code = device.last_exit })

    /* 1.4 Update state machine */
    device.state = FAILED
    device.crash_count++
    device.last_crash_time = now()

    /* Phase 2: Recovery (delayed) */

    if device.crash_count > MAX_CRASH_RETRIES:
        device.state = DEAD
        log("[DM] driver %s crashed %d times, giving up", device.manifest_name, device.crash_count)
        return

    /* Exponential backoff with jitter */
    backoff_ms = min(100 * (1 << (device.crash_count - 1)), 5000)
    jitter_ms = random() % (backoff_ms / 4)
    total_backoff = backoff_ms + jitter_ms

    schedule_timer(total_backoff, lambda: attempt_respawn(device))

function attempt_respawn(device):
    /* Phase 2.1: Create new driver sidecar */
    device.state = MATCHED
    new_channel = create_sidecar(device.manifest_name, device.device_context)

    if new_channel == ERR:
        device.state = FAILED
        device.crash_count++
        schedule_timer(backoff(device), lambda: attempt_respawn(device))
        return

    /* Phase 2.2: Validate the new driver */
    device.state = ATTACHING
    send_msg(new_channel, DRIVER_INIT, { device_config = device.config })

    result = recv_msg_timeout(new_channel, 5000)  /* 5 second timeout */
    if result == TIMEOUT or result.msg.opcode != DRIVER_READY:
        cap_revoke(new_channel)
        device.state = FAILED
        device.crash_count++
        schedule_timer(backoff(device), lambda: attempt_respawn(device))
        return

    /* Phase 2.3: Re-establish client connections */
    for client in device.registered_clients:
        driver_chan = cap_chan_create(new_channel, client.pid)
        send_event(client.channel, EVT_DRIVER_RESTORED,
            { device_id = device.id, new_channel = driver_chan })

    /* Phase 2.4: Unmask IRQs */
    unmask_irq(device.irq_cap)

    /* Phase 2.5: Mark live */
    device.state = DRIVER_RUNNING
    device.crash_count = 0  /* reset on successful recovery */
    device.active_driver = new_pid
```

### 7.3 Partial Failure Scenarios

**Scenario: Driver crashes mid-packet-processing**

The RX path is: hardware writes packet to DMA buffer → driver sends MEM cap to stack → stack processes. If the driver crashes between step 1 and step 2, the DMA buffer still holds the packet. On recovery:

1. The new driver is spawned with fresh DMA buffers (same physical region from its manifest).
2. The old DMA buffers are reclaimed (revoked via `cap_revoke()`).
3. Any in-flight packets in the old buffers are lost. The network stack's TCP layer will retransmit.
4. Any in-flight packets in the stack's processing pipeline are unaffected — they hold MEM caps that point to DMA buffer physical addresses, but the IOMMU is reprogrammed for the new driver, so the old addresses are now unmapped. The stack must check `cap_valid()` before accessing DMA-sourced memory.

**Scenario: Driver crashes while a client is blocked on cap_recv_msg**

The client's blocking `cap_recv_msg()` is on the driver's service channel. When the driver crashes, `cap_table_teardown()` closes all channel endpoints, which delivers `CLOSE_PEER_DEAD` to the client's pending recv. The client receives the close event and can retry or connect to the new driver.

**Scenario: Driver crashes repeatedly (crash loop)**

The exponential backoff ensures the system is not overwhelmed. After `MAX_CRASH_RETRIES` (default: 8) consecutive crashes, the device is declared DEAD. The operator (or an automated system) can inspect the failure via `sidecar_status()` and restart manually. The Device Manager also logs the crash pattern (interval, exit codes) for diagnostics.

### 7.4 Client Notification Protocol

```
EVT_DRIVER_FAILED:
    { device_id, reason, exit_code, timestamp }
    → Client should stop using the device, retry later

EVT_DRIVER_RESTORED:
    { device_id, new_channel }
    → Client should reconnect to the new channel and re-establish state

EVT_DEVICE_REMOVED:
    { device_id }
    → Device physically removed; no recovery expected
```

Clients that need automatic reconnection can subscribe to the Device Manager's device event channel. The POSIX sidecar's VFS follows the same `STALE → SPAWNING → ATTACHING → LIVE` state machine described in the driver respawn spec (`docs/AeroSLS-Driver-Respawn-Spec-Decision-v0.1.md`).

---

## 8. Security Analysis

### 8.1 IOMMU Enforcement

**What it protects against:**
A DMA-capable device (or a malicious driver programming that device) can only access physical memory pages that are:
1. Allocated by the DMA buffer allocator for that specific device.
2. Mapped in the device's IOMMU domain.
3. Backed by valid MEM capabilities in the driver's capability table.

**What it does NOT protect against:**
- **IOMMU bypass attacks:** Some devices can bypass the IOMMU via quirks (e.g., legacy PCI devices without ACS). The Device Manager must refuse to grant DMA capabilities to devices without IOMMU support. The `BUS_ACCESS` capability includes a `flags` field indicating IOMMU availability.
- **Device-to-device DMA:** If two devices share an IOMMU domain (a misconfiguration), one can DMA into the other's buffers. The kernel ensures each device gets its own domain.
- **DMA during boot:** Before the IOMMU is programmed, devices can DMA freely. The kernel must program the IOMMU default domain (blocking all DMA) before enabling any device's bus mastering.

### 8.2 DMA Attacks

**Attack: Rogue DMA read**
A malicious driver programs the NIC to DMA-read from arbitrary physical addresses. Mitigation: the IOMMU restricts the device to its mapped pages only. The MMU page table entry for unmapped addresses returns a bus error to the device.

**Attack: Rogue DMA write**
A malicious driver programs the NIC to DMA-write to kernel memory. Mitigation: IOMMU restricts writes to mapped pages. The DMA allocator never maps kernel-owned pages into any device domain.

**Attack: DMA overwrite of another sidecar's data**
A malicious NIC driver programs the NIC to overwrite a network stack sidecar's packet buffer. Mitigation: the IOMMU domain for the NIC only contains the NIC's own DMA buffers. The network stack's private memory is in a different domain (or no device domain at all).

**Attack: DMA after driver crash**
A crashed driver's NIC might still be DMA-ing from its last programmed ring buffers. Mitigation: on driver crash, the kernel immediately:
1. Masks the device's interrupt at the PLIC.
2. Unmaps all IOMMU pages for the device's domain (device DMA now causes bus errors).
3. Disables bus mastering in the PCI config space (stops the device from issuing DMA).

### 8.3 Capability Revocation Propagation

When a driver sidecar crashes or is revoked:

1. **cap_table_teardown()** walks every slot in the dead driver's capability table:
   - MEM caps: refcount decremented, frames freed at refcount 0.
   - CHAN caps: channel endpoints closed, pending messages drained.
   - IO_PORT caps: port range released, MMIO mapping removed.
   - IRQ caps: interrupt source masked, IRQ-to-CHAN binding severed.
   - DMA_MEM caps: IOMMU pages unmapped, physical pages unpinned and freed.

2. **IOMMU teardown** is the critical step: after IOMMU unmap, the device can no longer DMA to any memory. This happens before the frame pool reclaims the physical pages, so there is no window where a device can DMA into a page that has been reallocated to another sidecar.

3. **Client-side propagation:** clients holding MEM caps to the dead driver's DMA buffers find their caps REVOKED (the `cap_word_valid()` check fails on the next use). Clients holding CHAN caps receive `CLOSE_PEER_DEAD`.

4. **Ordering guarantee:** revocation is total and atomic from each holder's perspective: the last `cap_revoke()` call returns only after all holders are unlinked. No new references can be created after the revocation begins (Phase 1 invariant §4).

### 8.4 Untrusted Driver Containment

A driver sidecar is **untrusted** in the same way any sidecar is: it can only use the capabilities it was granted. Specifically:

- It cannot access device MMIO beyond its IO_PORT cap range.
- It cannot DMA to memory outside its DMA_MEM caps.
- It cannot receive interrupts from sources other than its IRQ cap.
- It cannot create its own capabilities (no `CAP_PERM_CREATE` concept).
- It cannot access other sidecars' memory.
- It cannot forge capability words (the reserved-bit forgery tag from Phase 1).

**Worst case if a driver is fully compromised:** it can only:
1. Misuse its own hardware (e.g., put the NIC in promiscuous mode, send crafted packets).
2. Crash itself (which triggers recovery, §7).
3. Send malformed messages on its channels (which clients should validate).
4. Use its DMA buffers for computation (but this is its own memory, not others').

### 8.5 Minimal TCB for Driver Isolation

The Trusted Computing Base for driver isolation includes:

| Component | Lines of Code (est.) | Justification |
|---|---|---|
| Capability manager (cap.c extensions) | ~500 | Creates/validates/revokes IO_PORT, IRQ, DMA_MEM caps |
| IOMMU driver (iommu.c) | ~800 | Programs IOMMU page tables, domain management |
| DMA buffer allocator (dma.c) | ~600 | Physically contiguous allocation, pinning, sharing |
| IRQ delivery (irq_deliver.c) | ~400 | Translates hardware interrupts to CHAN messages |
| Device Manager sidecar | ~2000 | Enumerates, matches, spawns, supervises drivers |
| Kernel manifest parser | ~300 | Validates driver manifests at boot |

**Total estimated TCB: ~4,600 lines of kernel code + ~2,000 lines of Device Manager code.**

The Device Manager sidecar is the largest component but runs in user space (or same-ring with memory isolation). If compromised, it can:
- Spawn arbitrary drivers (but each driver's capabilities come from its manifest, not the Device Manager).
- Kill legitimate drivers (but the recovery mechanism restarts them).
- It **cannot**: create capabilities it does not hold, access other sidecars' memory, or modify the kernel.

The **kernel-side** TCB for driver isolation is ~3,100 lines (the cap extensions, IOMMU, DMA allocator, and IRQ delivery). This is the truly trusted code; everything above it is defense-in-depth.

---

## 9. Performance Estimates

### 9.1 Interrupt Latency

**Path:** hardware asserts interrupt → PLIC/APLIC → kernel trap → IRQ message enqueued → driver sidecar woken → driver handler runs.

| Component | Estimated Latency | Notes |
|---|---|---|
| Hardware → PLIC delivery | 1–5 µs | Device-specific; PCIe propagation adds ~1 µs |
| PLIC → kernel trap entry | 2–5 µs | RISC-V trap entry: save regs, read PLIC claim |
| Kernel: build IRQ message | 0.5–1 µs | Copy fixed-format struct |
| Kernel: enqueue to channel | 1–2 µs | Spinlock, queue write, capability validation |
| Kernel: wake driver sidecar | 5–15 µs | Context switch: save kernel regs, load driver regs, MMU switch (TLB flush avoided via PCID/ASID) |
| Driver handler starts | — | Total: **10–28 µs** |

**Comparison with Linux in-kernel driver:** Linux interrupt-to-handler latency is typically 3–10 µs for a well-tuned NIC driver. AeroSLS adds ~10–18 µs overhead from the channel message and context switch. This is comparable to Linux's NAPI polling model when interrupts are batched (NAPI itself adds similar latency from softirq scheduling).

**Mitigation for high-rate devices:** coalescing (§3.3) reduces the number of context switches. With 50 µs coalescing on a 10 Gb/s NIC generating ~14.8M packets/second, interrupts drop from ~14.8M/s to ~20K/s (each coalesced message represents ~740 packets), bringing per-packet interrupt cost below 1 µs amortized.

### 9.2 DMA Setup Cost

**Path:** driver requests DMA buffer → kernel allocates → pins → programs IOMMU → creates MEM cap.

| Component | Estimated Cost | Notes |
|---|---|---|
| DMA allocator (bitmap search) | 1–5 µs | First-fit on 128 MiB pool, 4K pages |
| Page pinning | 2–5 µs | Mark in frame_pool bitmap, set page table bits |
| IOMMU page table update | 5–20 µs | Walk IOMMU page table, create/update entries |
| MEM cap creation | 3–5 µs | Same as existing cap_create_mem() |
| **Total (first allocation)** | **11–35 µs** | Cold path |
| **Total (reuse of existing buffer)** | **0 µs** | Driver holds the cap; no re-allocation |

For the NIC driver, DMA buffers are allocated once at init (TX ring, RX ring, RX buffers). The hot path (TX/RX packet processing) uses pre-allocated DMA buffers and does not invoke the allocator.

### 9.3 Throughput Analysis: AeroSLS vs. Linux In-Kernel Driver

**1 Gb/s NIC (e1000), 1500-byte packets:**

| Metric | Linux In-Kernel | AeroSLS Sidecar | Overhead Factor |
|---|---|---|---|
| Max packet rate (RX) | ~800K pps | ~400K pps | 2.0x |
| Max packet rate (TX) | ~900K pps | ~450K pps | 2.0x |
| Throughput (RX) | ~960 Mb/s | ~480 Mb/s | 2.0x |
| Throughput (TX) | ~960 Mb/s | ~500 Mb/s | 1.9x |
| Latency (packet-in to stack-ready) | ~5 µs | ~15–25 µs | 3–5x |

**10 Gb/s NIC (virtio-net), 1500-byte packets:**

| Metric | Linux In-Kernel | AeroSLS Sidecar | Overhead Factor |
|---|---|---|---|
| Max packet rate (RX) | ~6M pps | ~2M pps | 3.0x |
| Max packet rate (TX) | ~7M pps | ~2.5M pps | 2.8x |
| Throughput (RX) | ~9.4 Gb/s | ~3 Gb/s | 3.1x |
| Throughpath (TX) | ~9.4 Gb/s | ~3.5 Gb/s | 2.7x |

**Dominant overhead sources:**

1. **Context switch (driver ↔ stack):** ~5–15 µs per packet on the zero-copy path. This is the single largest cost. For batched polling (NAPI-style), this is amortized over multiple packets.
2. **Capability validation:** ~1–2 µs per cap_send_msg / cap_recv_msg. The kernel walks the holder list and validates the cap word.
3. **Channel message copy:** the IDL payload is copied from sender to receiver via the kernel's staging pool. For small messages (RX_PACKET header is ~32 bytes), this is ~1 µs.
4. **IOMMU TLB miss:** ~5–10 µs on first DMA access after page table change. Subsequent accesses hit the IOMMU TLB.

### 9.4 Benchmark Plan

**Micro-benchmarks (host-side, using QEMU or native):**

1. **IRQ-to-handler latency:** Instrument the kernel's IRQ delivery path with cycle-counter timestamps. Measure from PLIC delivery to driver handler entry. Run 10,000 iterations and report p50/p99/p99.9.

2. **Channel message throughput:** Create a tight loop of cap_send_msg / cap_recv_msg between two sidecars. Measure messages/second for payloads of 32, 256, 1024, 4096 bytes.

3. **DMA allocation latency:** Time `dma_alloc()` for 1, 4, 16, 64, 128 pages. Report cold (first allocation) and warm (reuse) paths.

4. **Zero-copy throughput:** Create a DMA buffer, share it via MEM cap, and have the receiver map and read it. Measure bandwidth for sequential reads of 4K, 64K, 1M regions.

**Macro-benchmarks (end-to-end, using QEMU):**

5. **NIC packet rate:** Run iperf3 between the AeroSLS sidecar and a host. Measure TCP throughput with various buffer sizes.

6. **Interrupt coalescing effectiveness:** Measure packet rate and CPU utilization with coalescing disabled, 10 µs, 50 µs, 100 µs, and 500 µs windows.

7. **Driver crash recovery time:** Kill the NIC driver sidecar, measure time from crash detection to packet flow resumption. Report p50/p99 over 100 iterations.

8. **Comparison baseline:** Run the same benchmarks on Linux with the in-kernel e1000 driver (same QEMU configuration) to establish the overhead factor.

**Micro-architectural effects that dominate:**

- **TLB pressure:** each context switch between sidecars potentially flushes TLB entries (mitigated by ASID/PCID if available). On RISC-V with Sv39, the TLB has 44 bits of ASID, so driver ↔ stack switches with different ASIDs will miss in the TLB. The first memory access after a switch costs ~10–20 cycles for the TLB miss.
- **Cache pollution:** the driver's hot data (descriptor rings, packet buffers) may be evicted by the network stack's working set. A cache-partitioning mechanism (e.g., Intel CAT or software-assisted) would help but is out of scope for Phase 4.
- **Branch prediction:** the channel message dispatch path is less predictable than a function-pointer-based in-kernel driver dispatch. Indirect branch predictors may miss on the first few invocations after a context switch.
- **IOMMU TLB:** IOMMU TLB entries are limited (typically 32–256 entries). For a NIC with 128 RX buffers, the IOMMU TLB may thrash if all buffers are accessed within the same TLB window. IOMMU page superpage support (2 MiB pages) mitigates this.

---

## Appendix A: Syscall Number Allocation

Phase 4 allocates syscalls 307–320 (next free after Phase 3's 306):

| Syscall | Number | Description |
|---|---|---|
| SYS_SLS_CAP_CREATE_IO_PORT | 307 | Create IO_PORT capability |
| SYS_SLS_CAP_CREATE_IRQ | 308 | Create IRQ capability |
| SYS_SLS_IRQ_MASK | 309 | Mask an IRQ |
| SYS_SLS_IRQ_UNMASK | 310 | Unmask an IRQ |
| SYS_SLS_IRQ_ACK | 311 | Acknowledge (EOI) an IRQ |
| SYS_SLS_DMA_ALLOC | 312 | Allocate DMA buffer |
| SYS_SLS_DMA_FREE | 313 | Free DMA buffer |
| SYS_SLS_DMA_SHARE | 314 | Share DMA buffer with another sidecar |
| SYS_SLS_DMA_IOMMU_MAP | 315 | Explicit IOMMU mapping (device setup) |
| SYS_SLS_DMA_IOMMU_UNMAP | 316 | Explicit IOMMU unmap |
| SYS_SLS_DEVICE_STATUS | 317 | Query device state (for observability) |
| SYS_SLS_DEVICE_EVENT_SUBSCRIBE | 318 | Subscribe to device events (hotplug, crash) |

## Appendix B: Integration Points with Existing Code

1. **kernel/cap.h / cap.c:** Extend `CapObject` with Phase 4 fields. Add `CAP_OBJ_KIND_*` constants. Add `cap_create_io_port()`, `cap_create_irq()`. Modify `cap_revoke()` for IRQ and DMA_MEM kinds.

2. **kernel/frame_pool.h / frame_pool.c:** Add `pin_pages()` / `unpin_pages()` for DMA buffer pinning. These are thin wrappers around the existing bitmap, marking pages as non-reclaimable.

3. **kernel/syscall_dispatch.c:** Add dispatch cases for syscalls 307–320.

4. **kernel/process.h / process.c:** Add a `supervisor_pid` field to `ProcessDescriptor` so the kernel can notify the Device Manager on driver crash.

5. **arch/riscv/plic.c:** Add `plic_mask_source()` / `plic_unmask_source()` / `plic_complete()` for IRQ delivery integration.

6. **drivers/pci.c:** The existing `pci_read_config()` / `pci_write_config()` are the low-level primitives the Device Manager will call via its IO_PORT capability.

7. **net/e1000.c / e1000.h:** The existing in-kernel e1000 driver serves as the reference implementation for the sidecar version. The sidecar driver's logic is identical; the hardware access paths change from direct MMIO to cap-mapped MMIO.

## Appendix C: Open Questions

1. **IRQ cap transferability:** Phase 4 makes IRQ caps non-transferable (bound to one sidecar). Should there be a mechanism for the Device Manager to transfer an IRQ cap to a replacement driver without revoking and re-creating? The current design revokes the old cap and creates a new one, which is safe but requires IOMMU reprogramming.

2. **Multi-queue NICs:** Modern NICs (Mellanox, Intel ice) support multiple receive/transmit queues, each with its own interrupt. Should the manifest declare a variable number of IRQ and DMA_BUF capabilities based on the device's queue count? This requires the Device Manager to read the device's queue configuration before spawning the driver.

3. **SR-IOV:** Phase 4 does not address SR-IOV (Single Root I/O Virtualization), where a physical NIC exposes multiple virtual functions. Each VF could be a separate sidecar with its own capabilities. This is a natural extension but requires IOMMU support for VF isolation.

4. **NUMA awareness:** DMA buffer allocation should ideally be local to the NUMA node where the device is attached. The current design uses a single global DMA pool. A multi-node design would partition the pool per NUMA node.

5. **Capability versioning:** If a driver is upgraded (new manifest, new capabilities), should existing client connections be preserved? The current design kills the old driver and starts fresh. An in-place upgrade mechanism could be more efficient.

---

**End of Phase 4 Design Document.**
