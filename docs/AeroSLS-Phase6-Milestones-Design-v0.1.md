# AeroSLS Phase 6 — Milestones Design: Bare-Metal Boot, Real Sidecar Drivers, and "Run Your App" (v0.1)

*Planning doc for the three milestones that make the Phase 5 announcement's full claim true. Each milestone states the current state (grounded in the codebase), the gap, the proposed approach, and acceptance criteria.*

---

## 0. Purpose

Phase 5 proved the architecture: a capability-secure microkernel boots five cooperating sidecars (init, Device Manager, ramdisk driver, POSIX, network) with a working POSIX userland and in-guest networking — all verified in QEMU. The announcement (docs/AeroSLS-Phase5-Announcement.md) makes a defensible claim today and an explicit "not claiming yet" list. These three milestones close that list:

1. **M1 — Boot on bare metal (x86_64)** — removes the "it only runs in a VM" objection.
2. **M2 — Real NIC/disk drivers as sidecars** — replaces the mock TCP server and kernel-backed ramdisk with capability-isolated hardware drivers.
3. **M3 — "Run your existing app"** — a real program loaded from storage, surviving a driver crash, isolated from a neighbor's fault.

---

## 1. Current state (what Phase 5 gives us)

| Area | Today |
|---|---|
| Boot path | GRUB → Multiboot v1/v2 (arch/x86/boot.asm), MB1 memory info + module parsing; kernel boots an initrd (sidecars.cpio) |
| Console | Serial (COM1/16550) both directions; console service drains sidecar output and forwards typed input — portable to real hardware |
| Timer | Local APIC + IRQ0 timer (kernel.c); tick-driven scheduler and park/wake deadlines |
| PCI | Kernel-side config-space probe at boot (pci_read_config, kernel/boot_image.c) → device registry handed to the Device Manager |
| Capabilities | CAP_TYPE_MEM / CHAN_R / CHAN_W (+ TRAMP). No I/O-port, device-MMIO, or IRQ capability types yet |
| Storage | Ramdisk driver sidecar backed by kernel memory; exposes a channel protocol that the VFS (aerofs) consumes |
| Network | Network sidecar with a mock TCP server (MockNetwork); POSIX↔network RPC verified end-to-end (nettest ALL DONE) |
| Exec | BusyBox-style applet dispatch: `exec(path)` reads the file, then looks it up in the applet registry — no ELF loading |
| Fault handling | Fail-fast boot script, sidecar crash/respawn machinery, service-registry health states (DOWN/DEGRADED) |

---

## 2. M1 — Bare-metal x86_64 boot

**Goal:** boot the same Phase 5 ISO to the interactive shell on a physical x86_64 PC, with the full sidecar set, no QEMU.

**What transfers:** Multiboot v1 handoff and memory map (BIOS provides the same mmap the kernel already parses), the 16550 serial console, the LAPIC timer, the ISO's GRUB boot path.

**Gaps and work items:**
1. **Memory-map validation** — the boot-image layout assumes specific physical regions (base_phys and spans from user/bootimage/src/layout.rs). Add a boot-time check that the multiboot mmap actually covers the reserved span, with a clear error instead of an obscure fault on machines with less RAM.
2. **Timer verification** — confirm LAPIC/IRQ0 behavior on real silicon (APIC base, frequency assumptions); fall back to PIT calibration if needed.
3. **Real-hardware smoke checklist** — define a serial-capture procedure (null-modem or IPMI/console redirection) and a verification matrix: boot → init → DM (real PCI inventory, not QEMU's i440fx) → ramdisk → POSIX → nettest → shell.
4. **Firmware variance** — BIOS vs UEFI (UEFI may need a different boot path or CSM); document which firmware targets are in scope for the first pass (BIOS/CSM recommended).

**Acceptance criteria:**
- [ ] Phase 5 ISO boots to `$` on ≥1 physical machine, serial-log captured end to end
- [ ] `nettest ALL DONE` passes on the real machine (against a real NIC once M2 lands; until then, the mock network still exercises the stack)
- [ ] Boot fails with a *named* error when the machine has insufficient/odd memory, not a page fault
- [ ] CI keeps QEMU as the default; bare-metal runs are scripted smoke runs, not part of the default suite

**Dependencies:** none (independent of M2/M3). Recommended first.

---

## 3. M2 — Real NIC/disk drivers as sidecars

**Goal:** hardware storage and networking behind the same channel protocols the VFS and NetClient already consume — with the drivers as capability-isolated sidecars.

**Gaps vs today:**
- No I/O-port or device-MMIO capability types (sidecars can't touch hardware directly)
- No device-IRQ delivery to user space (only the LAPIC timer IRQ reaches the kernel)
- Storage is kernel memory, not a disk; network is a mock

**Proposed capability extensions (the driver SDK):**
1. **Device-MMIO capability** — grant a physical address range with RW bits, mapped into the driver sidecar's address space (reuses the MEM-cap mapping machinery; add a `CAP_TYPE_DEV` or a MEM subtype flagged device).
2. **I/O-port capability** — grant a port range (in/out) with R/W rights; kernel mediates port access in the syscall path.
3. **IRQ→channel notification** — a sidecar registers an IRQ number and gets a wake on a notification channel when it fires (reuses the existing park/wake/deadline machinery; kernel ISR does EOI + wake instead of dropping the IRQ).

**Driver targets (in order):**
1. **Disk: PIO-IDE → AHCI.** Start with the simplest real path (PIO IDE on the legacy controller) as a sidecar behind the *same* storage channel protocol the ramdisk driver uses today — the VFS should not notice the swap. Then AHCI for SATA.
2. **NIC: e1000 (first, QEMU + common real NIC) then rtl8139/others.** Replace MockNetwork with a ring-buffer driver behind the same NetClient-facing protocol; `nettest` must still pass unchanged.

**Acceptance criteria:**
- [ ] VFS reads/writes survive a storage-backend swap from ramdisk to a real disk sidecar (same channel protocol, no VFS change)
- [ ] `nettest ALL DONE` with a real e1000 driver (no MockNetwork)
- [ ] A driver sidecar crash is contained: the kernel or service watchdog respawns it, and clients observe a bounded outage, not a hang
- [ ] IRQ notification latency is bounded and measured (no lost-wake races; reuses the Phase-1.5 park/recv argument against missed-wake)

**Dependencies:** M1 (bare metal for real drivers), IRQ delivery design (blocks NIC; disk can start with PIO polling). Enables M3's "boot from storage".

---

## 4. M3 — "Run your existing app"

**Goal:** a real, non-applet program (a C binary) that:
(a) boots from storage, (b) survives a driver crash, (c) runs isolated from a neighbor's fault.

**Gaps vs today:** exec is applet dispatch, not ELF loading; no libc/CRT for user programs; storage is the ramdisk.

**Proposed approach:**
1. **ELF loader in the POSIX sidecar** — extend `exec` so that when the target file is an ELF for the sidecar ABI (x86_64, freestanding), the task maps it instead of doing an applet lookup. The loader reuses the existing fork/exec task-replacement plumbing.
2. **Minimal freestanding C runtime** — a small crt0 + syscall wrappers against the existing kabi (open/read/write/exec/socket…), compiled with an x86_64-elf toolchain (the repo already vendors x86_64-elf-gcc). No libc: a self-contained `main()` with a handful of syscalls is the first target.
3. **Ship it on aerofs** — the demo program is a *file* in the rootfs (like /etc/init.rc today), loaded through the storage channel — the full "app data is storage, code is storage" story.
4. **Crash-survival demo** — kill the storage (or network) driver sidecar while the workload runs; the existing respawn machinery brings it back; the workload either keeps running or recovers cleanly. This is the public proof that drivers are replaceable services, not kernel code.
5. **Neighbor-isolation demo** — run two workloads with disjoint capabilities; fault one (exhaust its quota / crash it) and show the other unaffected. This is the capability-model demo that replaces the "container wall" narrative.

**Acceptance criteria:**
- [ ] A C program (e.g., a small TCP echo server or compute loop) compiled with the freestanding toolchain runs on the POSIX sidecar via `exec`, loaded from aerofs
- [ ] Kill the storage driver mid-run: workload survives/reconnects with a bounded stall (no kernel panic, no hang)
- [ ] Two workloads with disjoint caps: faulting one leaves the other's file descriptors, memory, and network channel untouched
- [ ] `nettest` and the boot script still pass (no regression to the applet path)

**Dependencies:** ELF loader is independent; the full "boot from storage + crash survival" story wants M2's real storage and M1's bare metal for credibility.

---

## 5. Sequencing

```
M1 (bare metal) ──────────► M2 (drivers: disk → NIC) ──► M3 (app story)
      │                            │                            │
      │  (validates memory,        │  (IRQ delivery + device   │  (ELF loader can start
      │   timer, firmware)         │   caps = driver SDK)      │   in parallel with M2)
```

- **M1 first**: cheapest, removes the biggest objection, de-risks everything else.
- **M2 disk before NIC**: storage unlocks M3's "boot from storage"; PIO-IDE avoids the IRQ dependency initially (NIC needs IRQ delivery).
- **M3's ELF loader can proceed in parallel** with M2; the crash-survival and isolation demos depend on respawn machinery that already exists.

## 6. Risks & open questions

1. **Real-hardware memory maps** — machines with < the reserved boot-image span, or exotic layouts; mitigation is the M1 named-error check.
2. **APIC vs PIC on real silicon** — LAPIC assumptions verified on QEMU may need calibration; PIT fallback.
3. **IRQ→channel latency and races** — the park/wake argument must be re-verified for device IRQs (interrupt-asm currently handles only the timer path).
4. **How much libc?** — a freestanding crt0 + syscall shim is in scope; full libc (malloc/stdio threads) is a later decision.
5. **NIC choice** — e1000 first (QEMU default + widely cloned); rtl8139 as fallback; real gigabit NICs later.
6. **UEFI** — first pass targets BIOS/CSM; UEFI native boot is a separate work item.

## 7. Definition of done for Phase 6

- The ISO boots on real hardware (M1) with real storage and networking drivers (M2) and runs a user C program loaded from storage (M3).
- The demo script captures one continuous transcript: `boot → mount from real disk → nettest over real NIC → run app → kill driver → app survives → neighbor unaffected`.
- The announcement's "not claiming yet" section is retired item by item as each milestone lands.
