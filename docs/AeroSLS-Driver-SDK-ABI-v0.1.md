# AeroSLS Driver SDK — Capability Extensions ABI (v0.1)

*Design spec for the driver SDK milestone (M2 in docs/AeroSLS-Phase6-Milestones-Design-v0.1.md): device-MMIO, I/O-port, and IRQ-to-channel capabilities. Ground truth is the current ABI (kernel/cap.h, user/proto/src/kabi.rs, kernel/syscall_dispatch.c); everything marked **NEW** is proposed and not yet implemented.*

---

## 1. Status and scope

This spec defines the three capability extensions a hardware sidecar driver needs:

1. **`CAP_TYPE_DEV`** — device memory (MMIO BARs): a physical address range mapped into the driver's address space.
2. **`CAP_TYPE_IO`** — I/O port ranges (in/out).
3. **`CAP_TYPE_IRQ`** — device interrupt delivery, expressed as a *notification channel* so drivers reuse the existing park/wake machinery.

It also defines the new syscalls, the capability minting path (manifest extension), and the kernel touchpoints. The POSIX/VFS/NetClient layers are deliberately unchanged: drivers replace backends behind the same channel protocols.

---

## 2. Ground truth: the current ABI

### 2.1 Capability word (kernel/cap.h)

A capability is one 64-bit word, validated on every use:

| Field | Bits | Mask | Meaning |
|---|---|---|---|
| TYPE | 63:61 | `CAP_TYPE_MASK 0x7` | object kind |
| STATE | 60:57 | `CAP_STATE_MASK 0xF` | 0=VALID, 1=IN_TRANSIT, 2=REVOKED |
| OBJ | 56:40 | `CAP_OBJ_MASK 0x1FFFF` | object id (17 bits) |
| PERM | 39:32 | `CAP_PERM_MASK 0xFF` | rights (R=0x1, W=0x2, …) |
| OFF | 31:16 | `CAP_OFF_MASK 0xFFFF` | offset within the object |
| LEN | 15:4 | `CAP_LEN_MASK 0xFFF` | length (units vary by type) |
| RSVD | 3:0 | — | must be 0 |

Used types: `NONE=0, MEM=1, CHAN_R=2, CHAN_W=3, TRAMP=4`. **Types 5, 6, 7 are free.**

### 2.2 Existing syscalls (user/proto/src/kabi.rs, kernel/syscall_dispatch.c)

| Number | Syscall |
|---|---|
| 289–297 | SYS_SLS_CAP_* seed-kernel block |
| 300 | SYS_SLS_YIELD |
| 310 | SYS_SLS_CREATE_SIDECAR |
| 311 | SYS_SLS_CHAN_WAIT |
| 312 | SYS_SLS_CHAN_RECV |
| 313 | SYS_SLS_CHAN_SEND |
| 314 | SYS_SLS_CHAN_CLOSE |
| 315 | SYS_SLS_CAP_INFO |

**Free ranges: 307–309, 316+ (301–306 are already taken by PROGRAM_SPAWN_NB_HELD, CAP_SEND_MSG, CAP_RECV_MSG, ARENA_FREE, TRAMPOLINE_CREATE/CALL). This spec takes 307–309 (device block) and 316–318 (IRQ block).** Syscall ABI: number in `rax`, args `rdi, rsi, rdx, r10, r8, r9` (x86-64 syscall convention, already handled in arch/x86/syscall.asm).

### 2.3 Channel/wake machinery (relevant to IRQ delivery)

- `CHAN_WAIT_MAX_CHANS = 8` endpoints per `k_chan_wait` park (kernel/cap.h).
- The park/recv argument (Phase-1.5): a process registers `waiting_chans[]` *before* checking queues, so a wake can never be missed; a wake after enqueue is safe because the enqueued message is the receipt.
- `cap_wake_chan(chan_id)` (kernel/cap.h:622, chan.c:417) wakes every process parked on a channel — the ISR path reuses this verbatim.

---

## 3. NEW: capability types

### 3.1 `CAP_TYPE_DEV = 5` — device memory (MMIO)

Word layout (same shape as `CAP_TYPE_MEM` so the mapping machinery is shared):

| Field | Value |
|---|---|
| TYPE | 5 |
| STATE | VALID |
| OBJ | physical region id (17 bits), allocated by the kernel at mint time |
| PERM | R / W bits |
| OFF | byte offset into the region (16 bits) |
| LEN | length in 4 KiB pages (12 bits → up to 16 MiB per cap; chained caps cover larger BARs) |

Semantics: grants the holder access to physical `[region_base + OFF, region_base + OFF + LEN*4096)`. The region is not mapped until the holder calls `SYS_DEV_MMAP` (§4.2). Only one live mapping per cap at a time; `REVOKED` unmaps.

### 3.2 `CAP_TYPE_IO = 6` — I/O ports

| Field | Value |
|---|---|
| TYPE | 6 |
| STATE | VALID |
| OBJ | port base (16-bit port space fits the 17-bit OBJ field) |
| PERM | R / W bits |
| OFF | 0 (reserved; ports are addressed by base + index) |
| LEN | port count (1–4096) |

Semantics: grants `in`/`out` access to ports `[base, base + LEN)`. All port access is mediated by the kernel syscall path (`SYS_IO_IN`/`SYS_IO_OUT`); there is no direct user-space port I/O.

### 3.3 `CAP_TYPE_IRQ = 7` — device interrupt

| Field | Value |
|---|---|
| TYPE | 7 |
| STATE | VALID |
| OBJ | IRQ vector number (17 bits — covers PIC 0–15 and APIC vectors) |
| PERM | 1 (bind) |
| OFF | 0 |
| LEN | 0 |

Semantics: the right to bind that vector to a notification channel. The binding itself is `SYS_IRQ_BIND` (§4.4), which mints the channel pair; the IRQ cap is single-use (STATE → REVOKED after bind).

---

## 4. NEW: syscalls

Error returns use the existing errno space (kabi.rs): `ERR_RIGHTS=3, ERR_RANGE=4, ERR_STATE=8, ERR_TYPE=9, ERR_BUSY→ERR_STATE, ERR_NOMEM=11, ERR_TIMEOUT=13`.

### 4.1 `SYS_DEV_MMAP = 309` — map a DEV cap

```
rax=309, rdi=dev_cap_slot:u16, rsi=vaddr_hint:u64, rdx=flags:u32 → rax=user_vaddr (0xFFFF… = CAP_NONE on error)
```
Flags: bit0 `DEV_MMAP_WC` (write-combining, for framebuffers), bit1 `DEV_MMAP_UNCACHED`. The kernel validates the cap, allocates a window in the caller's address space (hint honored if free), maps the physical range, and marks the cap mapped (a second call returns the same vaddr). Unmapping happens on cap revoke or process exit.

> **Implemented (2026-09-01):** the request is a packed struct
> `SLSDevMmapRequest` (slot u16, flags u32, vaddr_hint u64, out_vaddr u64)
> passed through the repo's single-opaque-arg syscall convention; the
> syscall returns the positive CAP_ERR_* code and fills `out_vaddr`.
> Window allocation scans upward from 1 MiB when the hint is zero/busy.
> The DEV cap is object-backed (`CAP_OBJ_KIND_DEV`): the OBJ field names a
> kernel-allocated region object (phys base + npages), so revoke/teardown
> reuse the MEM unmap path verbatim. Cache hints travel as `CAP_PERM_DEV_WC`
> / `CAP_PERM_DEV_UC` bits in the map-perms word and the x86 arch hook
> turns them into PTE PWT/PCD. `k_cap_info` reports DEV regions like MEM.
> Kernel side: kernel/cap.h + kernel/chan.c + kernel/syscall_dispatch.c;
> shims in kabi.rs `k_dev_mmap`; host test `tests/dev_mmap_host_test.c`.

### 4.2 `SYS_IO_IN = 307` / `SYS_IO_OUT = 308` — port I/O

```
SYS_IO_IN:  rax=307, rdi=io_cap_slot:u16, rsi=index:u16, rdx=size:u8 (1|2|4) → rax=value:u32
SYS_IO_OUT: rax=308, rdi=io_cap_slot:u16, rsi=index:u16, rdx=size:u8, r10=value:u32 → rax=0/errno
```

> **Errata (2026-09-01):** the v0.1 draft proposed 301–303 for this block, but
> 301–306 were already assigned (PROGRAM_SPAWN_NB_HELD, CAP_SEND_MSG,
> CAP_RECV_MSG, ARENA_FREE, TRAMPOLINE_CREATE/CALL) before the driver SDK
> landed. The shipped numbers are 307 (IO_IN), 308 (IO_OUT), 309 (DEV_MMAP),
> implemented in kernel/cap.h + kernel/chan.c + kernel/syscall_dispatch.c
> (kabi.rs `k_io_in`/`k_io_out`, host test `tests/io_cap_host_test.c`).
`index` is relative to the cap's port base; `index + size ≤ LEN` or `ERR_RANGE`. Kernel executes the `in`/`out` with the size requested (R/W rights checked against the cap's PERM).

### 4.3 `SYS_IRQ_BIND = 316` — bind an IRQ to a notification channel

```
rax=316, rdi=irq_cap_slot:u16, rsi=budget:u32 → rax=chan_r_slot:u16 (CAP_NONE=0xFFFF on error)
```
The kernel:
1. Validates the `CAP_TYPE_IRQ` cap and PERM;
2. Creates a channel pair — the kernel holds the `CHAN_W` end, the caller receives the `CHAN_R` end in the next free slot (returned);
3. Installs the ISR stub: on fire, the ISR enqueues a single-byte notification message on the kernel-held `CHAN_W` (payload = vector number), then `cap_wake_chan` on that channel, then EOI — **reusing the existing enqueue-then-wake path verbatim**, so the Phase-1.5 no-missed-wake argument holds;
4. Marks the IRQ cap `REVOKED`.

The driver then simply parks with `k_chan_wait(&[chan_r], timeout)` and drains with `k_chan_recv` — identical to any other channel consumer. No new wake semantics, no new park state.

### 4.4 `SYS_IRQ_UNBIND = 317` — release a vector

```
rax=317, rdi=chan_r_slot:u16 → rax=0/errno
```
Disarms the ISR stub, closes the channel pair (blocked receivers get the close event per the existing `CH_KIND_CLOSE` semantics), and frees the vector for rebind.

### 4.5 `SYS_IRQ_MASK = 318` — mask/unmask (optional, later)

```
rax=318, rdi=chan_r_slot:u16, rsi=mask:u8 (0=unmask,1=mask) → rax=0/errno
```
Reserved for level-triggered/shared IRQ handling where the driver must mask in the handler. The v0.1 contract is edge-triggered, auto-EOI (like the LAPIC timer path); this syscall is the escape hatch.

---

## 5. Capability minting: manifest extension

Device caps are minted at `cap_create_sidecar` time from a new optional manifest section, so drivers are declarative — no runtime claim syscall:

```text
sidecar manifest, driver section (draft):
  devices:
    - name: "nic0.bar0"   type: dev   base: <phys>   len: <bytes>  perms: rw
    - name: "nic0.io"     type: io    base: <port>   len: <count>  perms: rw
    - name: "nic0.irq"    type: irq   vector: <n>
```

The kernel maps each entry to a cap word in the driver's table (slots are deterministic, so the driver's `find_cap(CAP_TYPE_DEV, "nic0.bar0")` works like the existing `find_cap(CAP_CHAN_W, "console")`). The Device Manager already produces the PCI inventory (vendor/device/BARs/irq_line — kernel/boot_image.c) — a future extension has the DM generate this section from PCI config instead of a hand-authored manifest.

---

## 6. Kernel touchpoints

| File | Change |
|---|---|
| kernel/cap.h | add `CAP_TYPE_DEV/IO/IRQ` (5/6/7), word-doc comments |
| kernel/cap.c | minting in the manifest path; DEV mapping; IO port checks; IRQ table (vector → channel) |
| kernel/syscall_dispatch.c | case handlers for 307–309, 316–318 |
| arch/x86/syscall.asm | add new numbers to the dispatch range checks |
| arch/x86/interrupt.asm + kernel | device-IRQ stub path (timer path is the template: housekeeping + EOI) — currently only the timer vector is wired |
| user/proto/src/kabi.rs | syscall constants, wrappers (`dev_mmap`, `io_in/out`, `irq_bind/unbind/mask`) |
| user/ramdisk, user/network | drivers consume the SDK; channel protocols unchanged |
| user/bootimage/src | manifest `devices` section authoring |

---

## 7. Example: e1000 NIC driver sketch (sidecar)

1. Manifest mints `nic0.bar0` (MMIO BAR), `nic0.io` (port BAR), `nic0.irq`.
2. `dev_mmap(nic0.bar0)` → RX/TX ring window; `io_out` configures the port BAR.
3. `irq_bind(nic0.irq)` → `chan_r`; driver arms the RX ring and parks `k_chan_wait(&[chan_r], …)`.
4. ISR fires → kernel enqueues the vector byte + wakes the driver → driver recvs, drains the ring, forwards frames over the existing NET_* protocol to the POSIX sidecar.
5. `nettest` runs unchanged — the NetClient never learns the backend changed.

---

## 8. Open questions

1. **EOI ownership** — auto-EOI in the stub (chosen) vs driver-controlled ack; matters for shared/level IRQs (escape hatch: SYS_IRQ_MASK).
2. **DEV mapping window** — fixed region per cap vs dynamic; chained caps for BARs > 16 MiB.
3. **IRQ cap minting authority** — DM-generated manifest (preferred) vs hand-authored; PCI MSI-X is out of scope for v0.1.
4. **Portability** — all three caps are x86-shaped (port I/O, PIC/APIC vectors); the spec should gain a device-tree-like description if aarch64/riscv targets appear.

---

## 9. Acceptance criteria (inherited from M2)

- [ ] A PIO-IDE disk sidecar behind the existing storage channel protocol — VFS unchanged, `ls`/reads pass against a real disk.
- [ ] An e1000 driver sidecar — `nettest ALL DONE` with no MockNetwork.
- [ ] Driver crash containment: respawn restores service within a bounded stall, no kernel panic.
- [ ] Host tests for the new syscalls: rights/range/state errno paths, map/bind lifecycle, unbind wake-close.
