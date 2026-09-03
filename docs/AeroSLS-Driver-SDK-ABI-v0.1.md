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

> **Manifest minting + on-target verification (2026-09-03):** DEV caps are
> now minted by `cap_create_sidecar` from a `TAG_CAP_DEV` (0x000D) manifest
> record — the same wire shape as `CAP_MEM` (`name, phys_base u64, size
> u64 bytes, rights u8`) — into an object-backed `CAP_OBJ_KIND_DEV` region
> with a holder inserted under the object lock (so `cap_table_teardown`
> resolves and frees it exactly like a MEM cap). The init sidecar mints the
> POSIX manifest's `nic0.bar0` DEV cap from the **device registry's PCI
> scan** (init reads the e1000's `bar0_phys` — no hardcoded address; the
> §5 "DM generates the manifest" direction, in miniature) and the devtest
> applet (user/sidecar/src/applets.rs, run by init.rc) proves the path on
> target: trial-mmap finds the cap, the window lands in the user half, a
> re-map returns the same address (§4.1 idempotency), and the e1000's MAC
> reads back from RAL0/RAH0 (0x5400/0x5404) through the mapping — unicast,
> nonzero, not broadcast, OUI 52:54:00 (the MAC every repo QEMU config
> uses). A boot without `-device e1000` has no DEV cap and devtest prints
> SKIP so the boot script still reaches the shell; `tests/phase5_boot_smoke.sh`
> boots WITH the e1000 and gates CI on `devtest] PASS` alongside irqtest.
> Golden wire test: `manifest.rs cap_dev_wire_bytes_golden`; host test for
> the mint path rides `tests/cap_create_sidecar_host_test.c`.

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

> **Implemented (2026-09-01):** the request is a packed struct
> `SLSIrqBindRequest` (slot u16, budget u32, out_chan_r u16). `k_irq_bind`
> creates the channel with end0 = the driver (its CHAN_R is minted into a
> fresh slot) and end1 = the kernel, installs vector → channel in a
> 256-entry kernel registry, and stamps the IRQ cap `STATE_REVOKED` in
> place (single-use — a second bind on the same slot fails, and the second
> cap on the same vector fails with `ERR_STATE`). The ISR body is
> `cap_irq_notify(vector)`: it resolves the registry, enqueues a one-byte
> notification (payload = tag = the vector) on the kernel-held end,
> `cap_wake_chan`, then the weak `cap_irq_eoi` hook (no-op by default;
> LAPIC/PIC layers override). Notifications are dropped rather than
> blocked when the driver's budget is exhausted. `cap_table_teardown`
> unbinds the dying driver's vectors so they are free for rebind.
> Kernel side: kernel/cap.h + kernel/cap.c + kernel/syscall_dispatch.c;
> shim in kabi.rs `k_irq_bind`; host test `tests/irq_bind_host_test.c`.
> The arch ISR-stub wiring that calls `cap_irq_notify` on a real device
> vector is live: arch/x86/ioapic.c programs IO-APIC redirection-table
> entries for legacy ISA pins (bind-time unmask), and the IDT stub path
> (arch/x86/interrupt.asm) reaches `cap_irq_notify` from a genuine
> non-timer edge — the serial port's IRQ4, remapped to vector 36 — as
> proven on target by irqtest (§4.5).

### 4.4 `SYS_IRQ_UNBIND = 317` — release a vector

```
rax=317, rdi=chan_r_slot:u16 → rax=0/errno
```
Disarms the ISR stub, closes the channel pair (blocked receivers get the close event per the existing `CH_KIND_CLOSE` semantics), and frees the vector for rebind.

> **Implemented (2026-09-01):** the request is a packed struct
> `SLSIrqUnbindRequest` (chan_r u16 — the CHAN_R slot from a prior bind).
> `k_irq_unbind` resolves the slot, verifies the channel is registered in
> the IRQ table, disarms the registry FIRST (racing ISR enqueues find
> CAP_NONE), closes the driver's endpoint via the existing close machinery
> (its recv fails `ERR_STATE`) and marks the kernel end closed, then
> frees the vector for rebind. A non-IRQ channel is `ERR_STATE`; a bad
> slot is `ERR_RANGE`. Kernel side: kernel/cap.h + kernel/cap.c +
> kernel/syscall_dispatch.c; shim in kabi.rs `k_irq_unbind`. Mask/unmask
> (SYS_IRQ_MASK, 318) is §4.5.

### 4.5 `SYS_IRQ_MASK = 318` — mask/unmask (implemented, verified on target)

```
rax=318, rdi=chan_r_slot:u16, rsi=mask:u8 (0=unmask,1=mask) → rax=0/errno
```
Driver-side arm/disarm of a bound vector. The ISR self-masks a device pin on delivery: an edge-triggered RTE whose device line stays asserted re-fires on every EOI (the emulated IO-APIC latches the level), wedging the kernel in an ISR storm that starves user code. The driver services the device, drops the line, then re-arms with `mask=0` to keep receiving edges. The LAPIC timer (vector 32) is unaffected — its edges come from the local LAPIC, not the IO-APIC pin.

> **Implemented and verified on target (2026-09-01):** the request is a
> packed struct `SLSIrqMaskRequest` (chan_r u16, mask u8). `k_irq_mask`
> resolves the CHAN_R slot (same validation path as `k_irq_unbind`),
> finds the vector the channel is registered under in the 256-entry IRQ
> registry, and flips the RTE's masked bit via `cap_irq_set_mask`
> (weak default in cap.c for host tests; strong override in
> arch/x86/ioapic.c programs the IO-APIC redirection table with MMIO
> writes for legacy ISA pins, vectors 0x20–0x2F). `cap_irq_notify`
> self-masks (mask=1) before enqueuing, so the driver must re-arm after
> draining its device. **On-target verification:** irqtest drives the
> 16550 in loopback (MCR bit 4) with received-data interrupts enabled
> (IER bit 0); each transmitted byte asserts IRQ4, which the IO-APIC
> routes as vector 36 to the bound channel — **10/10 edges delivered**,
> each acknowledged by a successful `SYS_IRQ_MASK` re-arm, plus 50/50
> LAPIC-timer edges. Error paths are covered by
> `tests/irq_bind_host_test.c` (bad slot ERR_RANGE, revoked ERR_REVOKED,
> non-IRQ channel ERR_STATE). Kernel side: kernel/cap.h + kernel/cap.c +
> kernel/syscall_dispatch.c + arch/x86/ioapic.c; shim in kabi.rs
> `k_irq_mask`.

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
| arch/x86/interrupt.asm + kernel + arch/x86/ioapic.c | device-IRQ stub path (timer path is the template: housekeeping + EOI); IO-APIC legacy pins wired — serial IRQ4 (vector 36) verified end-to-end by irqtest |
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

> **NIC ownership prerequisite (2026-09-03):** the kernel's own network
> stack (mgmt HTTP/ARP/IP + cluster DSPP) drives the first e1000 it finds
> and polls it on every tick, so a user driver cannot share that card's
> registers. Ownership is settled by ROLE, not handed out ad hoc: grub.cfg
> passes `nic0=both nic1=none` on the multiboot line, kernel.c assigns
> roles BEFORE bring-up, and a role-less NIC is never programmed or polled
> by the kernel — `e1000_driver_handoff()` (net/e1000.c) only enables PCI
> bus mastering + marks the BAR uncacheable, then the driver sidecar owns
> every register from reset on (this is also what lets `devtest` keep
> probing nic0 read-only). Single-NIC boots are byte-identical: nic0=both
> is their existing implicit role and `nic1=` is ignored when only one
> card exists. The sidecar driver that consumes the handed-off NIC
> (drv.e1000.0, spawned by the DM per §5) is the next composition step;
> `nettest`/MockNetwork are untouched until that driver serves frames.

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
