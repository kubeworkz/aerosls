# AeroSLS Phase 5 — Self-Hosted: A Capability-Secure OS That Doesn't Need Containers or Hypervisors

*A from-scratch microkernel with a working POSIX userland, in-guest networking, and drivers that live outside the kernel — all isolated by capability channels, not by a nested runtime.*

---

## What Phase 5 is

Phase 5 is the self-hosted milestone: the AeroSLS microkernel now boots a complete, cooperating system from a single initrd — no monolithic kernel, no Linux underneath, no virtualization layer required to isolate workloads.

At boot, the kernel hands control to **five isolated sidecar processes**, each with only the capabilities it was granted:

| Sidecar | Role |
|---|---|
| `init` | Boot orchestration: spawns the system, runs `/etc/init.rc` |
| Device Manager | PCI discovery and device registry |
| Ramdisk driver | Storage service behind the VFS |
| POSIX sidecar | Full userland: VFS, fork/exec, applets, interactive shell |
| Network sidecar | Socket service over capability-wired channels |

Every one of them communicates over **kernel capability channels** — a message carries data and rights, and the kernel enforces both. Drivers are not kernel code; they are ordinary isolated processes with a capability contract.

## What actually works today (verified in QEMU)

- **A real POSIX environment** — aerofs VFS on the storage sidecar, `fork`/`exec`, a boot script (`init.rc`) with quotes, escapes, pipes, and `<`/`>` redirects, and a userland of applets (`echo`, `ls`, `cat`, …) ending in an **interactive `sh` shell** on the serial console.
- **Networking end-to-end** — a POSIX→network RPC path wired through capability channels: `NET_INFO` handshake, sockets, bind/listen/accept/connect. `nettest` passes the full round-trip on the real target.
- **Security by construction** — every syscall is mediated by per-process capability tables; a process can only touch what it was granted. The boot process is fail-fast: a failed network bring-up aborts the boot script at an explicit gate instead of limping on.
- **Engineering you can verify** — 100+ host tests, kernel-linked tests, an integration suite that boots the whole system under QEMU, and CI that runs source-only guard smokes on every push.

## The claim we can defend

> **AeroSLS replaces the container-and-hypervisor isolation stack with capability-based microkernel isolation.**

Containers and VMs achieve isolation by *nesting* — a container runtime inside a Linux kernel, a guest OS inside a hypervisor. AeroSLS achieves isolation with *the OS itself*: capability tables, channel IPC, and fault isolation in the kernel. There is no Linux, no Docker, and no VM layer in the trust chain.

**What we are not claiming (yet):** AeroSLS does not run Docker images, does not yet boot bare-metal hardware (development is on QEMU), and its network/storage drivers are not yet production device drivers. The claim is architectural: the isolation substrate exists and is demonstrated; the drivers and bare-metal port are the next milestones.

## Try it

```bash
make selfhost-bootimage   # build the five sidecars + initrd
make x86-iso              # build the bootable ISO
qemu-system-x86_64 -cdrom sls_operating_system.iso -m 4G -nographic \
    -serial file:/tmp/boot.log
```

Watch `Root filesystem mounted` → `nettest ALL DONE` → `System ready` → a live `$` shell.

## Roadmap to the full claim

1. **Bare-metal x86_64 boot** — remove QEMU from the story.
2. **Real drivers in sidecars** — NIC and disk drivers as capability-isolated services (replacing the mock TCP server and ramdisk).
3. **Bring your own workload** — port a real program onto the POSIX sidecar and demo it surviving a neighbor's fault, isolated by capabilities alone.

*Phase 5 status: self-hosted system booting in QEMU with POSIX userland and in-guest networking — verified end-to-end.*
