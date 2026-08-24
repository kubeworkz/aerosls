# AeroSLS Phase 4 — Security Analysis: Device Driver Isolation

**Version:** 1.0  
**Date:** August 23, 2026  
**Scope:** Security properties, attack surfaces, and hardening guarantees for Phase 4 (Device Driver SDK & Device Manager).  
**Depends on:** AeroSLS Threat Model v1.0, Phase 4 Design v0.1, Phase 1–3 security invariants.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Trust Boundaries and TCB](#2-trust-boundaries-and-tcb)
3. [IOMMU Enforcement](#3-iommu-enforcement)
4. [DMA Attack Vectors and Mitigations](#4-dma-attack-vectors-and-mitigations)
5. [Capability Revocation Propagation](#5-capability-revocation-propagation)
6. [IRQ Security Analysis](#6-irq-security-analysis)
7. [Device Manager as Trusted Component](#7-device-manager-as-trusted-component)
8. [Driver Sidecar Containment](#8-driver-sidecar-containment)
9. [Attack Surface Inventory](#9-attack-surface-inventory)
10. [Adversary Models](#10-adversary-models)
11. [Minimal TCB for Driver Isolation](#11-minimal-tcb-for-driver-isolation)
12. [Formal Security Properties](#12-formal-security-properties)
13. [Known Limitations and Residual Risks](#13-known-limitations-and-residual-risks)
14. [Recommendations and Hardening Roadmap](#14-recommendations-and-hardening-roadmap)

---

## 1. Executive Summary

Phase 4 transforms AeroSLS from a kernel-with-drivers into a kernel-that-isolates-drivers. Device drivers become untrusted sidecars that hold hardware capabilities (IO_PORT, IRQ, DMA_MEM, BUS_ACCESS) and communicate with the system through kernel-mediated channels and shared memory.

**Core security thesis:** A compromised or buggy driver sidecar cannot escalate privileges, corrupt other sidecars' memory, or access hardware beyond its assigned devices. The kernel enforces this through:

1. **Capability-based access control** — drivers hold only the capabilities they need, no more.
2. **IOMMU hardware enforcement** — devices can DMA only to their own pinned buffers.
3. **Capability revocation** — the kernel can instantly and completely revoke all access.
4. **Message isolation** — drivers communicate via typed channels, not shared address spaces.

**TCB size:** Approximately 3,100 lines of kernel code (cap extensions, IOMMU driver, DMA allocator, IRQ delivery) plus approximately 2,000 lines of the Device Manager sidecar in user space.

**Residual risk:** Side-channel attacks (timing, cache), IOMMU hardware errata, and the trusted Device Manager sidecar itself. These are documented in §13.

---

## 2. Trust Boundaries and TCB

### 2.1 Trust Boundary Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         HARDWARE LAYER                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐   │
│  │   CPU    │  │   IOMMU  │  │   PLIC   │  │ Memory/DDR   │   │
│  │ (Sv39)   │  │ (VT-d /  │  │(or APLIC)│  │              │   │
│  │          │  │  RISC-V) │  │          │  │              │   │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────┘   │
└───────────────────────────┬─────────────────────────────────────┘
                            │ Hardware enforced boundary
┌───────────────────────────┼─────────────────────────────────────┐
│                     KERNEL TCB (~3,100 LOC)                     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Capability Table Manager (Phase 1 core)                 │   │
│  │  + IO_PORT / IRQ / DMA_MEM / BUS_ACCESS extensions      │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  Channel Implementation (Phase 1 core)                   │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  DMA Buffer Allocator (kernel/dma.c)                     │   │
│  │  - Pool management, bitmap allocator, pin/unpin          │   │
│  │  - IOMMU page table programming                          │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  IRQ Delivery (kernel/irq.c)                             │   │
│  │  - IRQ-to-CHAN registry, coalescing, masking             │   │
│  │  - PLIC/APLIC claim/complete                             │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  IOMMU Driver (weak arch hooks, arch-specific impl)      │   │
│  │  - Domain create/destroy, page table management           │   │
│  │  - Device-to-domain binding                              │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │  Scheduler (Phase 1 core)                                │   │
│  └──────────────────────────────────────────────────────────┘   │
└───────────────────────────┬─────────────────────────────────────┘
                            │ Syscall interface (cap_send, cap_recv,
                            │ cap_create_*, cap_revoke, dma_alloc, etc.)
┌───────────────────────────┼─────────────────────────────────────┐
│                   UNTRUSTED SIDECAR LAYER                       │
│                                                                 │
│  ┌─────────────────┐  ┌─────────────────┐  ┌────────────────┐  │
│  │ Device Manager   │  │  NIC Driver     │  │  Network Stack │  │
│  │ (semi-trusted)   │  │  (untrusted)    │  │  (untrusted)   │  │
│  │                  │  │                 │  │                │  │
│  │ - PCIe enum      │  │ - e1000 /       │  │ - TCP/IP       │  │
│  │ - driver spawn   │  │   virtio-net    │  │ - packet mgmt  │  │
│  │ - hotplug        │  │ - TX/RX rings   │  │ - routing      │  │
│  │ - crash recovery │  │ - DMA buffers   │  │ - zero-copy RX │  │
│  │                  │  │                 │  │                │  │
│  │ Has: BUS_ACCESS  │  │ Has: IO_PORT,   │  │ Has: MEM, CHAN │  │
│  │ (bus-wide)       │  │ IRQ, DMA_MEM    │  │ (selected)     │  │
│  └─────────────────┘  └─────────────────┘  └────────────────┘  │
│                                                                 │
│  ┌─────────────────┐  ┌─────────────────┐  ┌────────────────┐  │
│  │  POSIX Sidecar  │  │  WASM Runtime   │  │  Application   │  │
│  │  (untrusted)    │  │  (untrusted)    │  │  (untrusted)   │  │
│  └─────────────────┘  └─────────────────┘  └────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 TCB Definition

The Trusted Computing Base for Phase 4 driver isolation comprises:

| Component | Lines (est.) | Why Trusted | Failure Consequence |
|-----------|-------------|-------------|-------------------|
| Capability table manager | 800 | Mediates all access control | Total isolation failure |
| Channel implementation | 500 | Message integrity and delivery | Cross-sidecar data leakage |
| DMA buffer allocator | 490 | Physical memory management | DMA attacks, use-after-free |
| IOMMU driver | 300 | Hardware page table programming | Unconfined DMA |
| IRQ delivery | 300 | Interrupt routing and masking | Interrupt storms, IRQ spoofing |
| Scheduler | 400 | Process isolation | Cross-sidecar execution |
| Trap/interrupt handlers | 300 | Hardware-software boundary | Kernel entry corruption |
| **TOTAL KERNEL TCB** | **~3,100** | | |
| Device Manager sidecar | 2,000 | Driver spawn and lifecycle | Grants capabilities to malicious drivers |
| **GRAND TOTAL TCB** | **~5,100** | | |

### 2.3 What Is NOT in the TCB

These components are explicitly **untrusted**:

- **Driver sidecars** (NIC, block, USB, etc.) — written in any language, possibly malicious.
- **Network stack sidecar** — processes untrusted network data.
- **POSIX sidecar** — hosts arbitrary user applications.
- **WASM/Lisp runtimes** — user-provided code.
- **Any sidecar's internal memory** — isolated by capability tables and page tables.

A compromised driver sidecar can only:
1. Use the hardware capabilities it holds (its device, its interrupts, its DMA buffers).
2. Send messages on channels it has been granted.
3. Access memory regions it holds MEM caps for.
4. Crash itself.

It **cannot**: read kernel memory, forge capabilities, access other sidecars' private memory, or DMA to arbitrary physical addresses.

---

## 3. IOMMU Enforcement

### 3.1 Threat: Unguarded DMA

Without an IOMMU, any device with bus mastering can read and write arbitrary physical memory. This is the most dangerous attack vector for device drivers:

- A compromised NIC driver programs the device to DMA-read kernel memory (e.g., capability tables, key material).
- A malicious PCIe device (or a compromised driver) writes to arbitrary physical addresses, corrupting other sidecars' memory.
- A DMA read/write races with page reclamation, enabling use-after-free via device access.

### 3.2 IOMMU Architecture

AeroSLS assigns each device its own IOMMU domain. The kernel programs the IOMMU page table so that a device can only access:

1. Its own DMA_MEM buffers (pinned and mapped).
2. No other physical memory.

```
┌──────────────┐     IOMMU Page Table      ┌──────────────────────┐
│   Device A   │──── maps to ──────────────►│ DMA_MEM buf A        │
│  (NIC, dom 0)│                            │  (frames 1024-1087)  │
│              │     64 MiB domain          └──────────────────────┘
│              │──── maps to ──────────────►│ DMA_MEM buf B        │
│              │                            │  (frames 1280-1343)  │
│              │     BLOCKED: frames 0-1023, 1344+                  │
└──────────────┘                            └──────────────────────┘

┌──────────────┐     IOMMU Page Table      ┌──────────────────────┐
│   Device B   │──── maps to ──────────────►│ DMA_MEM buf C        │
│ (USB, dom 1) │                            │  (frames 2048-2111)  │
│              │     BLOCKED: everything else                       │
└──────────────┘                            └──────────────────────┘
```

### 3.3 IOMMU Domain Lifecycle

```
Kernel boot:
  1. IOMMU initialized (VT-d / RISC-V IOMMU discovery)
  2. Global "default" domain blocks all DMA

Device Manager spawns driver:
  3. iommu_create_domain(device_id) → allocates page table root, binds device
  4. Driver requests DMA_MEM: iommu_map(domain, phys_base, npages)
  5. Device can now DMA to its own buffers only

Driver crashes / is revoked:
  6. iommu_unmap(domain, phys_base, npages) → removes page table entries
  7. IOTLB flush to ensure device sees updated mappings
  8. iommu_destroy_domain(device_id) → rebinds device to default (blocking) domain
  9. IOMMU page table root freed
```

### 3.4 IOMMU Guarantees

**Guarantee 1: DMA-Read Confinement**
A device mapped into domain D can only read physical addresses that appear in domain D's IOMMU page table. Any DMA read to an unmapped address returns all-ones (PCIe Completer Error) or is silently dropped.

**Guarantee 2: DMA-Write Confinement**
A device mapped into domain D can only write to physical addresses in domain D's page table. DMA writes to unmapped addresses are silently dropped (no data corruption, no side effects).

**Guarantee 3: Domain Isolation**
Two devices in different domains have completely independent IOMMU page tables. One domain's mappings do not affect another.

**Guarantee 4: IOTLB Coherence**
After unmapping (during revocation), an IOTLB shootdown is issued to ensure no stale device-side TLB entries persist. The device cannot continue DMA-ing to freed memory.

### 3.5 Edge Cases and Mitigations

| Edge Case | Risk | Mitigation |
|-----------|------|------------|
| No IOMMU present | DMA is unconfined | Kernel refuses to create DMA_MEM caps; driver mode: "no-IOMMU testing only" |
| IOMMU page table full | Cannot map new buffers | Pool size limits prevent overflow; kernel returns ENOMEM before IOMMU limit |
| IOTLB shootdown race | Stale TLB entry allows DMA to freed memory | Synchronous shootdown + stall device (PCIe FLR) before returning frames to pool |
| Device reads outside BAR | PCIe spec violation; may trigger IOMMU fault | IOMMU fault handler logs and optionally kills driver |
| DMA bouncing (device < 32-bit addressing) | Device cannot reach high physical addresses | DMA pool allocated in low 4 GiB (or within device's addressing capability) |
| SMMU / IOMMU bypass (AMD-Vi "pass-through" mode) | IOMMU disabled for device | Kernel refuses to enable bus mastering for devices not in an IOMMU domain |

---

## 4. DMA Attack Vectors and Mitigations

### 4.1 Attack Taxonomy

#### Attack 1: DMA Read of Kernel Memory

**Threat:** Compromised driver programs its NIC to DMA-Read starting at physical address 0x0 (kernel code/data) and writes the result into a packet it sends to a network peer.

**Why IOMMU prevents this:** The NIC's IOMMU domain only maps the NIC's DMA_MEM buffers (e.g., frames 1024-1343). DMA-Read to address 0x0 returns PCIe Completion Error (CplD with UnsupporteRequest Completer Status). The driver never receives the data.

**Residual risk:** If the IOMMU is misconfigured or absent, this attack succeeds. The kernel must verify IOMMU is present and enabled before minting any DMA_MEM cap.

#### Attack 2: DMA Write to Capability Table

**Threat:** Compromised driver programs device to DMA-Write a forged capability word into the kernel's capability table at a known physical address.

**Why IOMMU prevents this:** The capability table is not in the device's IOMMU domain. DMA-Write is silently dropped.

**Additional defense:** The capability table is in kernel memory, which is not in any user-accessible DMA_MEM arena. Even without IOMMU, the capability table physical pages are not reachable by device DMA through normal PCIe memory transactions.

#### Attack 3: DMA Race with Memory Reclamation

**Threat:** Driver allocates DMA buffer, then frees it back to the kernel. The kernel reallocates the same physical frames to another sidecar's private heap. The device still has the physical address in its ring descriptor and DMA-reads/writes the frames while they belong to another sidecar.

**Why this is prevented:**
1. `dma_free()` calls `iommu_unmap()` + IOTLB flush before releasing frames.
2. After IOTLB flush, the device's DMA to those frames fails (IOMMU fault or completion error).
3. Only after IOTLB flush are the frames marked as free in the bitmap.
4. The pin/unpin mechanism prevents the DMA pool from being reclaimed.

#### Attack 4: DMA Buffer Double-Use

**Threat:** Driver shares a DMA_MEM cap with another sidecar. Both sidecars write to the same physical memory simultaneously, causing data corruption.

**This is NOT prevented by IOMMU** — it is a correctness issue, not a security issue (both sidecars have legitimate access). The protocol between driver and stack must handle concurrent access (e.g., ring buffer producer-consumer discipline with hardware-assisted locking).

#### Attack 5: Rogue DMA Memcpy (DMA between devices)

**Threat:** Compromised NIC driver programs its device to DMA-Read from USB device's DMA buffer and DMA-Write to NIC's DMA buffer, exfiltrating data.

**Why IOMMU prevents this:** The NIC's IOMMU domain does not map the USB device's buffers. DMA-Read fails. Inter-device DMA requires explicit cross-domain mapping, which the kernel only creates for specific legitimate use cases (e.g., GPU-to-NIC zero-copy).

#### Attack 6: DMA Exhaustion (Denial of Service)

**Threat:** Compromised driver allocates all DMA_MEM buffers, exhausting the pool and preventing other drivers from functioning.

**Mitigation:** Per-device quotas in the DMA allocator:
- Maximum DMA buffers per sidecar: configurable (default 256).
- Maximum total DMA pool usage: configurable (default 90%).
- When quota hit, `dma_alloc()` returns ENOMEM even if pool has free frames.

### 4.2 DMA Safety Invariant

**Invariant D1: DMA confinement.** For all devices D with IOMMU domain id dom, for all physical addresses P accessed by D via DMA, there exists a DMA_MEM cap C in the system such that C.iommu_domain == dom AND P is within C's address range. This invariant holds at all times after IOMMU initialization.

**Formal statement:** ∀ device D, ∀ DMA transaction T by D: ∃ DMA_MEM cap C ∈ CapTable: T.phys_addr ∈ [C.phys_base, C.phys_base + C.npages × 4096) ∧ C.iommu_domain = D.domain_id.

---

## 5. Capability Revocation Propagation

### 5.1 Why Revocation is Critical

When a driver crashes or is detected as compromised, the kernel must revoke ALL capabilities granted to that driver. Failure to revoke completely means the driver retains access to hardware, memory, or interrupts even after it is "dead."

### 5.2 Revocation Sequence for a Driver Sidecar

The following sequence is executed by the kernel when `cap_revoke()` is called for a driver's capability, or when the Device Manager triggers a full driver teardown:

```
Step 1: Mask IRQ
  ├─ cap_irq_mask_source(irq_number)
  ├─ Write PLIC enable register to clear this source
  ├─ Write PLIC priority to 0 for this source
  └─ Set irq_cap->irq_masked = 1
  → Effect: No more IRQ messages delivered to the driver

Step 2: Unmap IOMMU
  ├─ cap_iommu_unmap(domain_id, phys_base, npages)  [for each DMA_MEM cap]
  ├─ IOTLB flush (synchronous or posted with drain)
  └─ Rebind device to default (blocking) IOMMU domain
  → Effect: Device can no longer DMA to any memory

Step 3: Revoke DMA_MEM caps
  ├─ For each DMA_MEM cap in driver's table:
  │   ├─ Set cap word to NONE (cap_word_invalidate)
  │   ├─ Unlink from holder list
  │   ├─ Return arena frames to pool
  │   └─ Mark pins as unpinned
  └─ driver->dma_cap_count = 0
  → Effect: Driver and all MEM holders lose access to DMA buffers

Step 4: Revoke IRQ caps
  ├─ cap_irq_eoi(irq_number)  [complete any pending interrupt]
  ├─ Set cap word to NONE
  ├─ Unlink from holder list
  └─ irq_unregister(irq_number)
  → Effect: IRQ cap is fully dead; no more messages possible

Step 5: Revoke IO_PORT caps
  ├─ For each IO_PORT cap in driver's table:
  │   ├─ Set cap word to NONE
  │   ├─ Unlink from holder list
  │   └─ (x86 only) Restore port range to unclaimed
  └─ driver->io_port_cap_count = 0
  → Effect: Driver can no longer perform MMIO or port I/O

Step 6: Revoke channel caps
  ├─ For each CHAN_R/CHAN_W cap in driver's table:
  │   ├─ Set cap word to NONE
  │   ├─ Drain channel queue (discard pending messages)
  │   └─ Close channel (mark as DEAD)
  └─ driver->chan_cap_count = 0
  → Effect: No more IPC possible; pending messages from other sidecars discarded

Step 7: Revoke remaining MEM caps
  ├─ For each MEM cap in driver's table:
  │   ├─ cap_revoke_driver_mem(cap)  [same as Step 3 but for non-DMA MEM]
  └─ driver->mem_cap_count = 0
  → Effect: Driver's private memory arena is reclaimed

Step 8: Invalidate process
  ├─ process->state = ZOMBIE
  ├─ Remove from scheduler run queue
  ├─ Cap table deallocated (all slots set to NONE)
  └─ Memory (stack, heap) returned to allocator
  → Effect: Driver sidecar is completely dead
```

### 5.3 Revocation Ordering Invariants

**Ordering Rule 1: IRQ before IOMMU.**
IRQ must be masked BEFORE IOMMU is unmapped. Reason: If IOMMU is unmapped first, the device may still interrupt (IRQ line is still asserted), and the driver may attempt to DMA (which now fails due to IOMMU, but the IRQ message could arrive and the driver might attempt a last DMA before masking). By masking IRQ first, we ensure no new IRQ messages arrive that could trigger driver-side DMA activity.

**Ordering Rule 2: IOMMU before DMA_MEM free.**
IOMMU must be unmapped BEFORE DMA_MEM caps are freed and frames returned to the pool. Reason: If frames are freed first, the same physical frames could be reallocated to another sidecar while the device still has DMA access via the old IOMMU mapping (IOTLB race).

**Ordering Rule 3: All DMA_MEM before process kill.**
All DMA_MEM caps must be freed (with IOMMU unmap) BEFORE the process is killed and its cap table deallocated. Reason: Process kill deallocates the cap table, which implicitly releases DMA_MEM references. Without explicit unmap, the IOMMU mappings could leak (device has DMA access but no kernel-side cap tracks it).

### 5.4 Propagation to Client Sidecars

When a driver is revoked, all sidecars that held MEM caps to the driver's DMA buffers lose access:

```
NIC driver revoked:
  ──► Network stack held MEM cap to NIC's RX DMA buffer
       ──► cap_revoke() walks the holder list for this DMA_MEM cap
       ──► Network stack's MEM cap is stamped REVOKED
       ──► Network stack's next access to the MEM cap returns CAP_REVOKED
       ──► Network stack handles the error (e.g., retries, falls back)
```

**Propagation completeness:** The holder walk in `cap_revoke()` iterates the linked list of all cap slots across all processes that reference the same CapObject. Every reference is stamped REVOKED. This is the same mechanism as Phase 1 revocation (§3 in the Phase 1 design doc).

### 5.5 Revocation Correctness Argument

**Theorem R1: Total revocation.** When `cap_revoke(C)` completes, no sidecar can use capability C or any alias of C.

**Proof sketch:**
1. `cap_revoke()` sets C's object state to `REVOKED` in the CapObject.
2. All holders' slot words are stamped with `STATE_REVOKED` (linearization walk).
3. `cap_word_valid()` checks state == VALID; REVOKED slots are rejected.
4. The holder list is cleared; no new lookups can find C.
5. DMA_MEM: IOMMU unmap + IOTLB flush ensures device cannot DMA.
6. IRQ: PLIC disable ensures no more interrupt messages.
7. IO_PORT: port range is released; no sidecar can use it.
8. Channels: channel is drained and closed; messages discarded.

**QED:** After revocation, no capability word in any sidecar's table can be used to access the resource.

---

## 6. IRQ Security Analysis

### 6.1 IRQ Attack Vectors

#### Attack: Interrupt Storm (Denial of Service)

**Threat:** Compromised driver does not EOI (acknowledge) its interrupts, causing the interrupt line to remain asserted and the PLIC to re-deliver the interrupt continuously, starving other interrupts.

**Mitigation:**
1. Kernel enforces a 10ms EOI timeout. If not acknowledged, kernel auto-EOIs.
2. Three consecutive timeouts trigger a driver fault → Device Manager quarantine.
3. Driver can be masked (`SYS_SLS_IRQ_MASK`) by the kernel proactively.

#### Attack: IRQ Spoofing

**Threat:** Compromised driver creates a fake IRQ cap for an interrupt source it does not own, and injects fake interrupt messages into another sidecar's channel.

**Why this is prevented:**
1. IRQ caps can only be created via `SYS_SLS_CAP_CREATE_IRQ` syscall.
2. The kernel validates the IRQ number against the PLIC/APLIC source table.
3. Only the Device Manager (holding BUS_ACCESS) can create IRQ caps for device interrupts.
4. IRQ cap creation is bound to the caller's PID; the IRQ channel delivers to the cap holder only.
5. A sidecar cannot create an IRQ cap for an IRQ that is already claimed (anti-aliasing check).

#### Attack: IRQ Channel Injection

**Threat:** A malicious sidecar sends a crafted IRQMessage directly on the driver's IRQ channel (without going through the kernel's interrupt delivery path).

**Why this is prevented:**
1. The IRQ channel is writable only by the kernel (kernel writes IRQ messages).
2. Sidecars hold only CHAN_R for the IRQ channel (receive end).
3. Writing to a channel requires CHAN_W capability, which sidecars do not have for the IRQ channel.
4. Even if a sidecar somehow had CHAN_W, the message would appear as an arbitrary channel message, not an interrupt — the driver must distinguish by channel endpoint (IRQ channel vs. control channel).

#### Attack: Interrupt Coalescing Manipulation

**Threat:** Compromised driver changes the coalescing window to a very large value, causing interrupts to be delayed indefinitely.

**Why this is prevented:**
1. The coalescing window is set at IRQ cap creation time (`coalesce_us` field).
2. Changing it requires `SYS_SLS_IRQ_MASK` + `SYS_SLS_IRQ_UNMASK`, which re-reads the original cap configuration.
3. The coalescing window is stored in the kernel's IRQRegistryEntry, not in user-accessible memory.
4. The Device Manager sets coalescing based on the manifest's resource limits.

### 6.2 IRQ Safety Invariant

**Invariant I1: Directed delivery.** For every IRQ cap C with `irq_number = N`, IRQ messages for source N are delivered only to the sidecar holding C. No other sidecar receives interrupt messages for source N.

**Invariant I2: Masking is atomic.** When `cap_revoke()` begins on an IRQ cap, the interrupt source is masked BEFORE any other revocation step. No IRQ messages can be delivered after the mask is applied.

---

## 7. Device Manager as Trusted Component

### 7.1 Device Manager Privileges

The Device Manager is the **most privileged sidecar** in Phase 4:

- Holds `BUS_ACCESS` covering the entire PCIe bus tree.
- Can enumerate all devices and read their config space.
- Can spawn driver sidecars with arbitrary capabilities (up to its own capability set).
- Handles crash recovery and re-provisions capabilities.

**This makes the Device Manager a critical component of the TCB.**

### 7.2 Device Manager Threat Scenarios

#### Scenario 1: Compromised Device Manager

**Impact:** CRITICAL. A compromised Device Manager can:
- Spawn a driver sidecar with IO_PORT caps covering kernel MMIO regions.
- Grant arbitrary DMA_MEM caps allowing DMA to kernel memory.
- Create IRQ caps for kernel-internal interrupts.
- Enumerate all devices and discover the entire hardware topology.

**Mitigation (defense in depth):**
1. Device Manager is written in Rust (memory-safe language), reducing exploit likelihood.
2. Device Manager's manifest constrains its maximum capability set (it cannot grant capabilities it does not hold).
3. The kernel enforces capability inheritance: `create_sidecar()` can only grant capabilities that the caller holds.
4. Future: formal verification of the Device Manager's capability-granting logic.

#### Scenario 2: Device Manager Crash

**Impact:** HIGH. While the Device Manager is dead:
- No new devices can be enumerated.
- Crashed drivers cannot be restarted.
- Hotplug events are not handled.

**Mitigation:**
1. Device Manager is restarted by the kernel's init process (Phase 1 respawn mechanism).
2. On restart, the Device Manager re-enumerates the PCI bus and re-matches drivers.
3. Client sidecars receive `EVT_DEVICE_REMOVED` and can retry or fail gracefully.
4. Existing driver sidecars continue running (they have their own capabilities and do not depend on the Device Manager for ongoing operation).

#### Scenario 3: Device Manager Grants Too Many Capabilities

**Impact:** A driver receives capabilities it does not need, increasing attack surface.

**Mitigation:**
1. Driver manifests (§5 in Phase 4 design) declare required capabilities.
2. The Device Manager matches required capabilities to available capabilities.
3. The kernel validates that the Device Manager actually holds the capabilities it tries to grant.
4. Manifest validation is performed by the kernel, not the Device Manager.
5. Future: least-privilege validation — kernel rejects manifests that request capabilities beyond what the device needs.

### 7.3 Device Manager Integrity Properties

**Property DM1: Capability grant limit.** The Device Manager can only grant capabilities it holds. It cannot escalate its own privileges through driver spawning.

**Property DM2: Manifest-mediated grants.** Each driver's capability set is determined by its manifest, not by the Device Manager's discretion (beyond matching manifest requirements to available capabilities).

**Property DM3: Idempotent re-enumeration.** After a Device Manager restart, re-enumeration produces the same set of discovered devices (up to hardware hotplug changes). No phantom devices or stale state persist.

---

## 8. Driver Sidecar Containment

### 8.1 What a Compromised Driver Can Do

A fully compromised driver sidecar (attacker controls all code execution within the sidecar) can:

1. **Use its own IO_PORT caps** — read/write its assigned MMIO region. This means it can program its device arbitrarily (e.g., disable interrupts on the device, corrupt device registers, leak device state). This is intentional: the driver IS the device's interface.

2. **Use its own IRQ caps** — acknowledge, mask, or unmask its assigned interrupts. Cannot create new IRQ caps or modify other IRQ sources.

3. **Use its own DMA_MEM caps** — DMA to its own buffers only. Cannot DMA to arbitrary physical addresses (IOMMU prevents this).

4. **Send channel messages** — communicate with other sidecars on channels it holds CHAN_W caps for. Cannot forge capabilities in messages (kernel validates all cap transfers).

5. **Crash itself** — corrupt its own memory, causing a fault. The Device Manager detects and handles this.

### 8.2 What a Compromised Driver CANNOT Do

1. **Access kernel memory** — kernel pages are not in any sidecar's address space.
2. **Forge capabilities** — capability words are validated by the kernel on every use.
3. **Access other sidecars' private memory** — no MEM caps to their pages.
4. **DMA to arbitrary addresses** — IOMMU blocks it.
5. **Spoof interrupts** — IRQ caps are kernel-mediated.
6. **Escalate privileges** — capability table is kernel-managed.
7. **Prevent revocation** — revocation is a kernel action, not subject to sidecar cooperation.
8. **Access other devices** — IO_PORT caps are scoped to specific physical ranges.

### 8.3 Worst-Case Damage

Even in the worst case (full compromise, zero-day in capability system), the compromised driver can only:

1. **Corrupt its own device's registers** (via IO_PORT writes) — this may put the physical device in a bad state, but does not affect other sidecars.
2. **Send malicious messages on its channels** — other sidecars must treat channel messages as untrusted (defense in depth: validate message content).
3. **Exhaust its DMA buffers** — limited by per-device quotas.
4. **Cause denial of service** for its specific device — other devices are unaffected due to IOMMU domain isolation.

**None of these are privilege escalation.** The damage is bounded to the device the driver manages.

---

## 9. Attack Surface Inventory

### 9.1 Syscall Interface (Kernel Boundary)

| Syscall | Risk | Validation |
|---------|------|-----------|
| `SYS_SLS_CAP_CREATE_IO_PORT` | Create IO_PORT cap for wrong range | Anti-aliasing check, range validation, permission check |
| `SYS_SLS_CAP_CREATE_IRQ` | Create IRQ cap for wrong source | PLIC source validation, anti-aliasing, permission check |
| `SYS_SLS_IRQ_MASK/UNMASK` | Mask/unmask wrong IRQ | Cap ownership check, IRQ number validation |
| `SYS_SLS_IRQ_ACK` | EOI wrong interrupt | Cap ownership check |
| `SYS_SLS_DMA_ALLOC` | Exhaust DMA pool | Per-device quota, pool size check |
| `SYS_SLS_DMA_FREE` | Free someone else's buffer | Owner PID check |
| `SYS_SLS_DMA_SHARE` | Share to wrong sidecar | Target PID validation, cap creation validation |
| `SYS_SLS_DMA_IOMMU_MAP` | Map to wrong domain | Domain ownership check |
| `SYS_SLS_DMA_IOMMU_UNMAP` | Unmap from wrong domain | Domain ownership check |

### 9.2 Channel Interface (Sidecar Boundary)

| Channel Type | Risk | Mitigation |
|-------------|------|-----------|
| IRQ channel (kernel→driver) | Fake interrupt messages | Only kernel can write; CHAN_W not exposed to sidecars |
| Control channel (DM↔driver) | Malformed control messages | Message validation in driver; timeout on invalid messages |
| Service channel (driver→stack) | Malformed packets/headers | Stack validates all data; driver is untrusted sender |
| DMA_MEM share (MEM cap) | Stale DMA buffer access | Revocation propagates to all holders |

### 9.3 DMA Interface (Device Boundary)

| DMA Operation | Risk | Mitigation |
|--------------|------|-----------|
| Device DMA-Read | Read outside allocated buffer | IOMMU domain confinement |
| Device DMA-Write | Write outside allocated buffer | IOMMU domain confinement |
| DMA after free | Device accesses freed buffer | IOTLB flush + frame reuse delay |
| DMA while driver crashed | Device still active after driver killed | Device reset (FLR) before driver teardown |

---

## 10. Adversary Models

### 10.1 Model A: Malicious Driver Author

**Capability:** Writes a driver sidecar in Rust or C with intentional backdoors.

**Goal:** Exfiltrate data from other sidecars, disrupt system operation, or gain persistent access.

**Attack path:**
1. Write NIC driver with backdoor: on receiving a magic packet, DMA-read kernel memory (attempted).
2. IOMMU blocks DMA outside assigned buffers. Attack fails.
3. Alternative: send exfiltrated data via channel message to a compromised network stack.
4. Requires compromising BOTH the NIC driver AND the network stack sidecar.
5. **Residual risk:** If the network stack is also compromised (supply chain attack), data can be exfiltrated via normal network packets. This is outside Phase 4's scope (application-level integrity).

**Mitigation:** Capability minimization, supply chain security, manifest auditing.

### 10.2 Model B: Exploited Driver (Software Bug)

**Capability:** A memory corruption bug in the driver's Rust/C code allows arbitrary code execution within the sidecar.

**Goal:** Escape the sidecar and access kernel or other sidecars.

**Attack path:**
1. Driver has buffer overflow in packet parsing → control of instruction pointer within sidecar.
2. Attacker attempts to invoke syscalls with crafted arguments.
3. Kernel validates all syscall arguments against capability table.
4. Attacker has IO_PORT, IRQ, DMA_MEM caps — these are valid, but:
   - IO_PORT caps are scoped to the device's MMIO range only.
   - DMA_MEM caps are confined by IOMMU.
   - IRQ caps are scoped to the device's interrupt source.
5. Attacker cannot forge caps or access other capabilities.
6. **Result:** Attacker can misuse the driver's own capabilities (e.g., DMA to own buffers, program own device) but cannot escape.

**Mitigation:** Memory-safe languages, ASLR within sidecar address space, stack canaries.

### 10.3 Model C: Malicious Hardware Device

**Capability:** A rogue PCIe device (or a compromised NIC firmware) that performs DMA attacks or interrupt spoofing.

**Goal:** Read/write arbitrary physical memory, disrupt system operation.

**Attack path:**
1. Rogue device attempts DMA to kernel memory → blocked by IOMMU (device is in its own domain with only its own buffers mapped).
2. Rogue device sends spoofed interrupts → kernel checks PLIC source table; interrupt not associated with any IRQ cap → ignored.
3. Rogue device DMA-reads its own buffers → succeeds, but only its own data.
4. Rogue device attempts to enumerate other devices via config space → blocked by IOMMU and PCIe access control.

**Mitigation:** IOMMU is mandatory for all DMA-capable devices. Devices without IOMMU mapping are blocked from bus mastering.

### 10.4 Model D: Remote Network Attacker

**Capability:** Sends crafted network packets to the NIC.

**Goal:** Exploit the NIC driver or network stack to gain code execution.

**Attack path:**
1. Crafted packet arrives at NIC → NIC DMA-writes to driver's RX ring (in driver's DMA_MEM buffer).
2. Driver's interrupt handler receives IRQ message, processes RX ring.
3. Driver creates MEM cap for the packet buffer and sends it to the network stack.
4. Network stack processes the packet.
5. If the NIC driver has a parsing bug (e.g., malformed descriptor handling), remote attacker may gain code execution within the driver sidecar.
6. **But:** Driver sidecar containment (§8) prevents escape beyond its own capabilities.

**Mitigation:** Driver written in memory-safe language; packet parsing in the network stack (which is separate from the driver); fuzz testing of driver message parsing.

---

## 11. Minimal TCB for Driver Isolation

### 11.1 TCB Reduction Strategy

The goal is to minimize the code that must be correct for driver isolation to hold. Strategy:

1. **Separate concerns** — capability management, DMA allocation, IRQ delivery, and IOMMU are separate modules with minimal interfaces.
2. **Keep driver logic in user space** — driver sidecars are NOT in the TCB. Only the kernel's enforcement mechanisms are trusted.
3. **Hardware enforcement where possible** — IOMMU and page tables provide hardware-guaranteed isolation; the kernel only programs them.
4. **Bounded interfaces** — each TCB component has a small, well-defined interface that can be audited independently.

### 11.2 TCB Component Interfaces

#### Capability Manager Interface (800 LOC)

```
cap_create_io_port(pid, phys_base, length, perms) → cap_idx | error
cap_create_irq(pid, irq_number, trigger, perms, coalesce_us) → cap_idx | error
cap_create_dma_mem(pid, phys_base, npages, iommu_domain) → cap_idx | error
cap_create_bus(pid, bus_range, perms) → cap_idx | error
cap_revoke(cap_idx) → void                           [total revocation]
cap_map(pid, cap_idx, vaddr, perms) → phys_addr | error
cap_send_msg(pid, chan_wr, payload, n_caps) → error
cap_recv_msg(pid, chan_rd, payload_out) → error
```

**Properties that must hold:**
- C1: `cap_create_*` checks anti-aliasing and permission.
- C2: `cap_revoke` is total and atomic.
- C3: `cap_send_msg` validates cap indices and transfers atomically.
- C4: No cap can be forged or duplicated.

#### DMA Allocator Interface (490 LOC)

```
dma_init(base_phys, total_frames) → void
dma_alloc(npages, align, flags, pid) → (cap_idx, dma_buf_id) | error
dma_free(dma_buf_id, pid) → error
dma_share(dma_buf_id, target_pid, rights) → cap_idx | error
dma_pin(dma_buf_id) → error
dma_unpin(dma_buf_id) → error
```

**Properties that must hold:**
- D1: Allocated frames are not shared with any other allocation.
- D2: `dma_free` unmaps IOMMU before releasing frames.
- D3: Pin count never goes below zero.
- D4: Per-device quotas are enforced.

#### IOMMU Driver Interface (300 LOC)

```
iommu_init() → void
iommu_create_domain(device_id) → domain_id
iommu_destroy_domain(domain_id) → void
iommu_map(domain_id, phys_addr, npages) → error
iommu_unmap(domain_id, phys_addr, npages) → error
iommu_flush_tlb(domain_id) → void
```

**Properties that must hold:**
- I1: No two domains map the same physical address for write (except explicit sharing).
- I2: `iommu_unmap` + `iommu_flush_tlb` ensures device cannot DMA to unmapped address.
- I3: Default domain blocks all DMA for unbound devices.

#### IRQ Delivery Interface (300 LOC)

```
irq_init() → void
irq_register(irq_number, cap_id, chan_id) → error
irq_unregister(irq_number) → void
irq_deliver(irq_number, interrupt_context) → void
irq_deliver_pending(irq_number) → void
irq_mask(irq_number) → void
irq_unmask(irq_number) → void
irq_eoi(irq_number) → void
irq_source_valid(irq_number) → bool
```

**Properties that must hold:**
- R1: `irq_deliver` only sends to the registered cap/channel.
- R2: `irq_mask` is effective before `irq_deliver` returns (no race).
- R3: Coalescing is bounded (coalesce_count does not overflow).

### 11.3 TCB Audit Points

For a security audit, the following code paths must be verified:

| # | Code Path | Property | Test Method |
|---|-----------|----------|-------------|
| A1 | `cap_create_io_port` anti-aliasing | No overlapping IO_PORT or MEM caps | Fuzz: random cap creation sequences |
| A2 | `cap_create_irq` source validation | Only valid PLIC sources accepted | Unit test with mock PLIC |
| A3 | `cap_revoke` completeness | All holders stamped, all resources freed | Formal model (TLA+) |
| A4 | `cap_send_msg` cap transfer | Capabilities atomically transferred or not at all | Concurrency stress test |
| A5 | `dma_alloc` no double-allocate | Bitmap never allocates same frame twice | Exhaustive test |
| A6 | `dma_free` IOMMU unmap order | IOMMU unmap before frame release | Code review + unit test |
| A7 | `iommu_map` domain isolation | Two domains never share a writable mapping | IOMMU page table audit |
| A8 | `iommu_unmap` + flush | No stale TLB entries after unmap | IOTLB flush verification |
| A9 | `irq_deliver` only to holder | IRQ messages reach only the registered sidecar | Integration test |
| A10 | `irq_mask` atomicity | No IRQ delivered after mask returns | Race condition test |

---

## 12. Formal Security Properties

### 12.1 Safety Properties (Nothing Bad Happens)

**S1: No unauthorized memory access.** A sidecar S can read/write physical address P only if S holds a valid (state=VALID) MEM or DMA_MEM cap C such that P ∈ [C.phys_base, C.phys_base + C.npages × 4096).

**S2: No unauthorized device access.** A sidecar S can perform I/O to physical address P (MMIO) only if S holds a valid IO_PORT cap C such that P ∈ [C.phys_base, C.phys_base + C.io_phys_len).

**S3: No unauthorized DMA.** A device D can DMA to physical address P only if P is mapped in D's IOMMU domain page table.

**S4: No unauthorized interrupt delivery.** An IRQ message for source N is delivered to sidecar S only if S holds a valid IRQ cap C with C.irq_number = N.

**S5: No capability forgery.** A sidecar cannot create a capability it was not granted. All capability creation is mediated by the kernel via syscalls with validation.

**S6: No capability duplication.** A capability word can exist in at most one cap table slot across all sidecars at any time. Transfer is atomic: sender's slot is invalidated before receiver's slot is created.

### 12.2 Liveness Properties (Something Good Eventually Happens)

**L1: Revocation completes.** `cap_revoke(C)` always terminates and results in C being fully revoked across all holders.

**L2: IRQ delivery is bounded.** IRQ messages are delivered within bounded time (hardware interrupt latency + kernel dispatch + channel enqueue).

**L3: DMA allocation terminates.** `dma_alloc()` terminates in bounded time (bitmap scan is O(total_frames/8)).

**L4: Driver restart is possible.** After a driver crash, the Device Manager can always spawn a new driver sidecar (assuming resources are available and the device is still present).

### 12.3 Ordering Properties

**O1: IRQ mask before IOMMU unmap before frame release.** In the revocation sequence, these operations occur in strict order.

**O2: Frame release before reallocation.** A DMA frame is not allocated to a new buffer until IOTLB flush is complete.

**O3: Capability invalidation before resource release.** The cap slot is invalidated before the underlying resource (memory, IRQ, etc.) is freed.

---

## 13. Known Limitations and Residual Risks

### 13.1 Side-Channel Attacks

**Risk: MEDIUM.** Side-channel attacks are not fully mitigated:

- **Cache timing:** Two sidecars sharing LLC (Last-Level Cache) can infer each other's memory access patterns. No cache partitioning is implemented.
- **Spectre/Meltdown:** If speculative execution is not mitigated, driver code could potentially read kernel data via speculative cache probes. Hardware mitigations (IBRS, STIBP) should be enabled.
- **Memory bus contention:** Shared memory bandwidth can leak information about other sidecars' activity.

**Mitigation (future):** Cache partitioning (Intel CAT / AMD QoS), address space layout randomization within sidecars, and encrypted memory (Intel TME / AMD SME).

### 13.2 Device Manager Compromise

**Risk: HIGH (impact), LOW (likelihood).** The Device Manager is the most trusted sidecar. If compromised, the attacker can:
- Grant excessive capabilities to malicious drivers.
- Create driver sidecars with IO_PORT caps covering kernel MMIO.
- Enumerate all hardware topology.

**Mitigation:** Written in Rust; minimal codebase; future formal verification. The kernel enforces that the Device Manager cannot grant capabilities it does not hold, limiting the blast radius.

### 13.3 IOMMU Bypass by Hardware

**Risk: LOW.** Some platforms support IOMMU bypass modes:
- AMD-Vi "pass-through" mode disables IOMMU for specific devices.
- Intel VT-d "identity mapping" allows unconfined DMA.
- Some embedded platforms lack IOMMU entirely.

**Mitigation:** Kernel refuses to enable bus mastering for devices without an IOMMU domain. Boot log warns if IOMMU is not detected.

### 13.4 DMA Exhaustion (Denial of Service)

**Risk: LOW.** A driver can allocate all DMA buffers, preventing other drivers from functioning.

**Mitigation:** Per-device quotas in the DMA allocator. Maximum buffers per device configurable (default 256, max 1024). Total pool usage capped at 90%.

### 13.5 Interrupt Storm

**Risk: MEDIUM.** A malfunctioning device or compromised driver can generate continuous interrupts.

**Mitigation:** Kernel auto-EOI after 10ms timeout. Rate limiting on IRQ message delivery. Three consecutive timeouts trigger driver quarantine.

### 13.6 Driver Manifest Validation

**Risk: MEDIUM.** If the Device Manager accepts a malicious manifest, it could create a driver with capabilities it should not have.

**Mitigation:** Manifest validation is performed by the kernel, not the Device Manager. The kernel checks:
- Capability requirements are subsets of available capabilities.
- Resource limits are within system bounds.
- Compatible strings match known patterns.

**Future:** Signed manifests with chain of trust from bootloader.

---

## 14. Recommendations and Hardening Roadmap

### 14.1 Immediate (Before Phase 5)

| # | Recommendation | Priority | Effort |
|---|----------------|----------|--------|
| R1 | Enable hardware IOMMU in all QEMU test configurations | P0 | Low |
| R2 | Add IOMMU fault handler (log + quarantine on unexpected DMA access) | P0 | Low |
| R3 | Implement per-device DMA buffer quotas | P0 | Already done |
| R4 | Add EOI timeout handler (auto-EOI after 10ms) | P0 | Already done |
| R5 | Write unit tests for all TCB audit points (A1–A10) | P1 | Medium |
| R6 | Fuzz test the capability syscall interface | P1 | Medium |

### 14.2 Medium-Term (Phase 5–6)

| # | Recommendation | Priority | Effort |
|---|----------------|----------|--------|
| R7 | Formal model (TLA+) of capability revocation sequence | P1 | High |
| R8 | Signed driver manifests with chain of trust | P1 | High |
| R9 | Device Manager capability-grant auditing (log all grants) | P1 | Low |
| R10 | Interrupt storm detection and automatic driver quarantine | P2 | Medium |
| R11 | IOMMU fault statistics and anomaly detection | P2 | Medium |
| R12 | DMA pool exhaustion monitoring and alerting | P2 | Low |

### 14.3 Long-Term (Phase 7+)

| # | Recommendation | Priority | Effort |
|---|----------------|----------|--------|
| R13 | Cache partitioning (CAT/QoS) for side-channel mitigation | P2 | High |
| R14 | Formal verification of capability lifecycle (Coq/Isabelle) | P2 | Very High |
| R15 | Hardware root of trust integration (TPM, secure boot) | P1 | High |
| R16 | Device Manager in a separate protection domain (MPK/PKU) | P2 | Medium |
| R17 | Network-level driver attestation (verify driver integrity at runtime) | P3 | High |

---

## Appendix A: Capability Word Layout for Phase 4

```
Bit 63..61: Type (3 bits)
  000 = NONE, 001 = MEM, 010 = CHAN_R, 011 = CHAN_W
  100 = TRAMP, 101 = IO_PORT, 110 = IRQ, 111 = DMA_MEM, (1000 = BUS_ACCESS via extended)

Bit 60..57: State (4 bits)
  0000 = NONE, 0001 = VALID, 0010 = REVOKED, ...

Bit 56..40: Object ID (17 bits) → 131,072 objects max

Bit 39..32: Permissions (8 bits)
  For MEM:      R, W, X, MAP
  For CHAN_R/W: SEND, RECV
  For IO_PORT:  IO_READ, IO_WRITE, IO_PF
  For IRQ:      IRQ_LISTEN, IRQ_ACK, IRQ_MASK
  For DMA_MEM:  DMA_SHARE, DMA_MAP (+ MEM R/W/X/MAP)
  For BUS:      BUS_ENUM, BUS_CFG_R, BUS_CFG_W

Bit 31..16: Offset (16 bits)
Bit 15..4:  Length (12 bits)
Bit 3..0:   Reserved / forgery tag
```

## Appendix B: IRQ Revocation Sequence Diagram

```
    Driver Sidecar              Kernel                       Hardware
         │                        │                            │
         │   [driver crashes]     │                            │
         │         │              │                            │
         │    ┌────▼────┐        │                            │
         │    │ SIGSEGV │        │                            │
         │    └────┬────┘        │                            │
         │         │              │                            │
         │         │  process_fault通知                        │
         │         ├──────────────►                            │
         │         │              │                            │
         │         │    Step 1:   │  PLIC disable IRQ source  │
         │         │              ├───────────────────────────►│
         │         │              │                            │
         │         │    Step 2:   │  IOMMU unmap all DMA bufs  │
         │         │              ├───────────────────────────►│
         │         │              │  IOTLB flush               │
         │         │              ├───────────────────────────►│
         │         │              │                            │
         │         │    Step 3:   │  cap_word = NONE           │
         │         │              │  (revoke DMA_MEM caps)     │
         │         │              │  ┌─────────────────┐       │
         │         │              │  │ Holder walk:     │       │
         │         │              │  │ stack MEM caps   │       │
         │         │              │  │ → REVOKED        │       │
         │         │              │  └─────────────────┘       │
         │         │              │                            │
         │         │    Step 4:   │  cap_word = NONE           │
         │         │              │  (revoke IRQ cap)          │
         │         │              │  ┌─────────────────┐       │
         │         │              │  │ Holder walk      │       │
         │         │              │  │ IRQ → REVOKED    │       │
         │         │              │  └─────────────────┘       │
         │         │              │                            │
         │         │    Step 5:   │  cap_word = NONE           │
         │         │              │  (revoke IO_PORT caps)     │
         │         │              │                            │
         │         │    Step 6:   │  Channel drain + close     │
         │         │              │  (revoke CHAN caps)        │
         │         │              │                            │
         │         │    Step 7:   │  cap table deallocated     │
         │         │              │  process = ZOMBIE           │
         │         │              │                            │
         │         │   Notify DM  │                            │
         │         ├──────────────►                            │
         │         │              │                            │
         │    [DEAD]│              │  DM: quarantine + retry    │
         │         │              │                            │
```

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 08-23-2026 | Initial Phase 4 security analysis |

---

*This document should be updated as new attack vectors are discovered and mitigations are implemented. Cross-reference with the AeroSLS Threat Model v1.0 for the full system-wide threat landscape.*
