# AeroSLS Self-Hosted Phase 5 — Design v0.1

**Status:** Draft for review.
**Scope:** Design of the self-hosted AeroSLS: booting on real hardware with all system
services (storage, network, filesystem, console) implemented as sidecars, culminating
in a live demo where POSIX shell, WASM modules, and Lisp sidecars interact through
the capability-mediated channel infrastructure built in Phases 1–4.

**Depends on:**
- Phase 1: capability tables, MEM caps, channel endpoints, `create_sidecar()`
- Phase 2: POSIX sidecar, ramdisk driver sidecar, channel transport framing
- Phase 3: Polyglot Nexus (AeroIDL, shared arena, WASM↔Lisp cross-calls)
- Phase 4: Device Driver SDK, Device Manager sidecar, per-sidecar MMU isolation

**Relationship to existing code:** The kernel boot path (`kernel/kernel.c:kernel_main`)
already scans PCI for e1000 NICs and NVMe controllers, initializes the microkernel
supervision layer (`kernel/microkernel.c`), and runs either the HTTP server or the
shell. This design extends that path: the kernel becomes a capability-restricted
microkernel that boots an init sidecar, which then orchestrates all remaining system
composition as sidecars connected by channels.

---

## 0. Design summary

**The kernel shrinks; sidecars grow.** The current AeroSLS kernel initializes ~40
subsystems directly in `kernel_main()`. In Phase 5, the kernel retains only the
irreducible minimum: interrupt handling, physical memory management, the capability
layer, channel transport, sidecar creation, and PCI/MMIO discovery. Everything else
— NVMe driver, NIC driver, TCP/IP stack, filesystem, POSIX shell — becomes a sidecar
whose authority is bounded by capabilities and whose lifecycle is managed by the
init sidecar.

**What "self-hosted" means here:** the system boots to a POSIX shell prompt without
any external API server, without QEMU user-net, without the Navigator web frontend.
The shell can `ls`, `cat`, pipe between commands, and fetch data over the network.
A WASM module can call a Lisp function and use the network. All I/O flows through
sidecars connected by kernel-enforced channels.

**What "self-hosted" does NOT mean (yet):** the kernel itself is not compiled from
a sidecar — the build toolchain runs on the host. A sidecar cannot modify the kernel
image. The POSIX sidecar does not yet run arbitrary untrusted binaries (vfork-only
fork semantics from Phase 2 §4.4). True process isolation (Tier 2) is Phase 4's
extension path and is not required for the demo.

---

## 1. Boot sequence

### 1.1 From firmware to kernel entry

The boot chain depends on the target architecture:

**x86-64 (GRUB/Multiboot2 path — the primary demo target):**
```
UEFI BIOS → GRUB → load kernel ELF + initrd → Multiboot2 entry → kernel_main()
```
The existing `arch/x86/boot.asm` declares a Multiboot2 header. GRUB loads the
kernel ELF and an initrd containing all sidecar images, manifests, and the
root filesystem image. The Multiboot2 memory map, command line, and initrd
pointer are passed to `kernel_main()` as today.

**RISC-V (OpenSBI path — secondary target):**
```
OpenSBI (fw_dynamic) → load kernel ELF → S-mode entry → kernel_main()
```
The existing `arch/riscv/boot_riscv.S` handles OpenSBI handoff. The initrd
is passed as a second boot module via the device tree's `/chosen/module` nodes
or as a QEMU `-initrd` parameter.

### 1.2 The Phase 5 kernel boot — what changes

`kernel_main()` gains a new final phase after the existing PCI scan and before
the HTTP server / shell entry:

```c
// ── 9. Sidecar subsystem init ───────────────────────────────────────────
// Discover hardware and build the device registry that the init sidecar
// will consume.  This replaces the direct nvme_io_init() / e1000_init()
// calls that currently live in steps 7/7b — those drivers are now
// sidecars that the init sidecar spawns.
sidecar_subsystem_init();

// ── 10. Init sidecar launch ────────────────────────────────────────────
// The kernel loads the init sidecar from the initrd, builds its
// capability table (budget, device registry MEM cap, console channel,
// manifest registry CAP_SPAWN), and starts it.
launch_init_sidecar(initrd);
```

### 1.3 What the kernel discovers before init

The kernel performs the same PCI enumeration it does today, but instead of
initializing drivers directly, it populates a **device registry** — a
kernel-owned table describing every PCI device discovered:

```c
struct SidecarDeviceInfo {
    uint8_t  class_code;       // e.g. 0x01 (mass storage), 0x02 (network)
    uint8_t  subclass;         // e.g. 0x08 (NVMe), 0x00 (Ethernet)
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  pci_slot;
    uint8_t  pci_bus;
    uint64_t bar0_phys;        // MMIO base address
    uint8_t  irq_line;         // APIC IRQ for MSI-X
    uint8_t  is_64bit_bar;
    char     driver_manifest[64]; // e.g. "drv.nvme.0", "drv.e1000.0"
};

#define MAX_DEVICES 16
struct SidecarDeviceInfo device_registry[MAX_DEVICES];
uint32_t device_count;
```

The kernel also:
1. Stamps each device with a `driver_manifest` name from a compile-time table
   that maps `(vendor_id, device_id)` → manifest name.
2. Allocates the shared-memory arena for sidecar communication.
3. Creates the console channel endpoint (kernel ↔ init sidecar).

### 1.4 Initial capability grants to the init sidecar

The init sidecar receives the following capabilities at creation time:

```json
{
  "format": "aerosls/sidecar-manifest",
  "version": "1.0",
  "personality": "aerosls.init.v1",

  "image": { "kind": "elf", "offset": 0x4000, "size": 0x8000, "entry": "_start" },

  "budget": {
    "mem_bytes": 16777216,
    "stack_bytes": 131072,
    "heap_initial_bytes": 4194304
  },

  "cpu": { "share": 300, "preemptible": false },

  "limits": {
    "max_tasks": 1,
    "max_fds": 32,
    "max_channels": 64,
    "max_open_files": 32,
    "chan_queue_depth": 16
  },

  "caps": [
    { "name": "budget",     "type": "mem",  "size": 16777216, "rights": "rw" },
    { "name": "img.ro",     "type": "mem",  "size": 524288,   "rights": "rx" },
    { "name": "console",    "type": "chan", "peer": "kernel.debug.console", "rights": "rw" },
    { "name": "device_registry", "type": "mem", "size": 4096, "rights": "r" },
    { "name": "spawn.init", "type": "spawn", "manifest": "drv.device_manager.0", "rights": "spawn|one_at_a_time" }
  ],

  "bootstrap": {
    "console_channel": "console",
    "debug_channel": null,
    "log_level": "info"
  },

  "flags": { "isolated": false, "wx_policy": "w^x" }
}
```

Key design decisions:
- The init sidecar gets `device_registry` as a READ-ONLY MEM cap — it can
  inspect what the kernel found but cannot modify it.
- `spawn.init` is a `CAP_SPAWN` for the Device Manager manifest only — the
  init sidecar can create exactly one kind of sidecar.
- The init sidecar has no channels to any driver yet — the Device Manager
  will be wired to drivers via the respawn path (Phase 2 §5, Driver Respawn
  Spec §3.1).

### 1.5 Init sidecar bootstrap (Rust entry)

```rust
// user/init/src/entry.rs
#[no_mangle]
pub extern "C" fn rust_entry(bib: *const BootInfo) -> ! {
    let bib = unsafe { &*bib };
    assert_eq!(bib.magic, BOOT_INFO_MAGIC);

    let budget          = bib.find_cap(CAP_MEM,  "budget").expect("no budget");
    let console         = bib.find_cap(CAP_CHAN, "console").expect("no console");
    let devreg          = bib.find_cap(CAP_MEM,  "device_registry").expect("no devreg");
    let spawn_devmgr    = bib.find_cap(CAP_SPAWN, "spawn.init").expect("no spawn");

    // 1. Init heap from budget
    unsafe { HEAP.init(budget.base, budget.len) };

    // 2. Channel runtime
    chan_runtime::init();
    let console = chan_runtime::open(console, "console");

    // 3. Read device registry (read-only memory view)
    let devices = unsafe {
        core::slice::from_raw_parts(
            devreg.base as *const SidecarDeviceInfo,
            *(devreg.base as *const u32),  // first u32 is count
        )
    };

    log::info!(console, "[INIT] found {} PCI device(s)", devices.len());

    // 4. Spawn the Device Manager sidecar
    let dm_channel = create_sidecar(spawn_devmgr, "drv.device_manager.0");

    // 5. Send the device registry snapshot over the channel
    //    (the DM reads it once at boot, then manages devices itself)
    dm_channel.send(DeviceRegistryMsg {
        devices: devices.to_vec(),
    });

    // 6. The Device Manager now owns device lifecycle.
    //    Init waits for all-caps-ready signal, then spawns POSIX sidecar.
    loop {
        match dm_channel.recv() {
            DevicesReady { boot_order } => break,
            _ => {}
        }
    }

    // 7. Spawn POSIX sidecar with channels to filesystem + console
    //    (wired by the Device Manager's manifest declarations)
    create_sidecar_and_run("aerosls.posix.v1", initrd_path);

    // 8. Idle forever — the kernel scheduler runs the POSIX sidecar
    scheduler::run()
}
```

### 1.6 Initial manifest and capability table

The complete capability table for the init sidecar at boot:

| Slot | Name | Type | Rights | Source |
|------|------|------|--------|--------|
| 0 | `budget` | MEM | rw | Kernel (from manifest) |
| 1 | `img.ro` | MEM | rx | Kernel (from manifest) |
| 2 | `console` | CHAN | rw | Kernel ↔ init (kernel-bootstrap) |
| 3 | `device_registry` | MEM | r | Kernel (from manifest) |
| 4 | `spawn.init` | SPAWN | spawn | Kernel (from manifest) |
| 5–N | *(child channels)* | CHAN | rw | Returned by `create_sidecar` calls |

---

## 2. System composition

### 2.1 Required sidecars for the self-hosted demo

| Sidecar | Manifest name | Purpose | Dependencies |
|---------|---------------|---------|--------------|
| **Device Manager** | `drv.device_manager.0` | Discovers hardware, creates driver sidecars, manages hotplug | init (parent channel) |
| **NVMe Driver** | `drv.nvme.0` | Block I/O to NVMe device | Device Manager (control channel) |
| **NIC Driver (e1000)** | `drv.e1000.0` | Ethernet frame TX/RX | Device Manager (control channel) |
| **Block Service** | `svc.block.0` | Block device abstraction (sector read/write) | NVMe Driver (block channel) |
| **Filesystem** | `svc.filesystem.0` | ext2-lite / aerofs-lite mount, inode management | Block Service (block channel) |
| **Network Stack** | `svc.network.0` | TCP/IP, DNS, ARP, ICMP | NIC Driver (packet channel) |
| **POSIX** | `aerosls.posix.v1` | POSIX shell, process model, VFS | Filesystem (VFS channel), Console (console channel), Network Stack (socket channel) |
| **Display/Console** | `drv.console.0` | VGA text-mode / serial output | Device Manager (control channel) |
| **Watchdog** | `svc.watchdog.0` | Restart policies, heartbeat monitoring | All sidecars (monitoring channels) |
| **WASM Runtime** | `svc.wasm.0` | WebAssembly execution | POSIX (spawn channel), Network Stack (socket channel) |
| **Lisp Runtime** | `svc.lisp.0` | Common Lisp execution | POSIX (spawn channel), Network Stack (socket channel) |

### 2.2 Dependency graph

```
                           ┌──────────────────────────┐
                           │    kernel (microkernel)   │
                           │  capability layer, MMU,   │
                           │  scheduler, channels      │
                           └────────────┬─────────────┘
                                        │
                              ┌─────────▼─────────┐
                              │    init sidecar    │
                              │  orchestrator       │
                              └─────────┬─────────┘
                                        │
                              ┌─────────▼─────────┐
                              │   Device Manager   │
                              │  PCI enumeration,  │
                              │  hotplug, respawn  │
                              └──┬──────┬──────┬──┘
                                 │      │      │
                    ┌────────────┘      │      └────────────┐
                    ▼                   ▼                    ▼
            ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
            │ NVMe Driver  │  │ NIC Driver   │  │   Console    │
            │ (drv.nvme.0) │  │ (drv.e1000.0)│  │ (drv.console)│
            └──────┬───────┘  └──────┬───────┘  └──────────────┘
                   │                  │
            ┌──────▼───────┐  ┌──────▼───────┐
            │ Block Service│  │ Network Stack │
            │ (svc.block.0)│  │(svc.network.0)│
            └──────┬───────┘  └──────┬───────┘
                   │                  │
            ┌──────▼───────┐         │
            │  Filesystem  │         │
            │(svc.filesystem)│        │
            └──────┬───────┘         │
                   │                  │
         ┌─────────▼──────────────────▼─────────┐
         │          POSIX sidecar                │
         │  shell, VFS, process model, fds       │
         │  (aerosls.posix.v1)                   │
         └──┬──────────┬────────────────────────┘
            │          │
            ▼          ▼
     ┌──────────┐ ┌──────────┐
     │   WASM   │ │   Lisp   │
     │ Runtime  │ │ Runtime  │
     └──────────┘ └──────────┘
```

### 2.3 Service discovery via capability registry

Phase 5 introduces a **capability registry** — an extension of the existing
`kernel/service_registry.c` that is visible to sidecars through a dedicated
channel. The registry answers: "given a service name, which sidecar owns it,
and what channel do I use to reach it?"

The mechanism:
1. The Device Manager registers each service it creates in the kernel's
   `services_registry[]` (via the existing `SYS_SLS_SERVICE_REGISTER` syscall
   over its control channel to the kernel).
2. Any sidecar can resolve a service name to a `{sidecar_id, channel_slot}`
   tuple by sending a `SVC_RESOLVE` message to the registry channel.
3. The registry channel is a kernel-bootstrap channel (path 1 from the
   transport spec) available to every sidecar that declares it in its
   manifest.

**Why this differs from the cluster registry:** The existing `service_registry`
is for cross-node discovery (name → partition → node). The capability registry
is for in-kernel discovery (name → sidecar → channel). They share the same
kernel data structure but serve different scopes.

The registry protocol (over a kernel-bootstrap channel):

```aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.registry")

struct ServiceEndpoint {
    sidecar_id: u32,
    channel_slot: u32,
    rights: u8,         // what rights the client should hold
}

interface CapabilityRegistry {
    resolve(name: string) -> Result<ServiceEndpoint, RegistryError>;
    register(name: string, endpoint: ServiceEndpoint) -> Result<(), RegistryError>;
    list() -> ServiceEndpoint[];
}
```

### 2.4 Boot order and sidecar creation sequence

```
TIME    KERNEL                          INIT SIDECAR
─────   ─────────────────────────────   ──────────────────────────────
0       kernel_main() entry
1       PCI scan → device_registry[]
2       cap_init(), scheduler init
3       launch_init_sidecar(initrd) →
4                                          boot, read BIB
5                                          read device_registry
6                                          create_sidecar(Device Manager)
7                                          send device_registry → DM
8                                          ───────────────────────────
                                          DM: enumerate devices
9                                          DM: create NVMe driver sidecar
10                                         DM: create NIC driver sidecar
11                                         DM: create console driver sidecar
12                                         DM: create block service sidecar
13                                         DM: create filesystem sidecar
14                                         DM: create network stack sidecar
15                                         DM: create watchdog sidecar
16                                         DM: signal "devices ready"
17                                          ───────────────────────────
18                                         init: create POSIX sidecar
19                                         init: create WASM runtime
20                                         init: create Lisp runtime
21                                         POSIX: mount root filesystem
22                                         POSIX: spawn /bin/busybox init
23                                         POSIX: open shell → prompt
```

---

## 3. Storage stack

### 3.1 NVMe driver sidecar

The NVMe driver sidecar wraps the existing `drivers/nvme.c` and
`drivers/nvme_io.c` code into a sidecar with a channel-based block I/O
interface. The driver receives:
- A MEM cap covering the NVMe MMIO BAR (from the Device Manager)
- A control channel from the Device Manager
- A console channel for logging

**Channel protocol (block I/O):**

```aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.block")

enum BlockOp {
    INFO    = 1,
    READ    = 2,
    WRITE   = 3,
    FLUSH   = 4,
    MAP     = 5,
}

struct BlockInfo {
    sector_size: u32,
    total_sectors: u64,
    flags: u32,        // bit0 = read-only
}

struct BlockRequest {
    op:     BlockOp,
    lba:    u64,
    count:  u32,
    tag:    u32,       // client-chosen, echoed in reply
}

struct BlockReply {
    status: u16,       // 0 = OK, non-zero = error
    bytes:  u64,
    tag:    u32,
}

interface BlockDevice {
    info() -> BlockInfo;
    read(lba: u64, count: u32, buf: MemCap @arena) -> Result<u64, BlockError>;
    write(lba: u64, count: u32, buf: MemCap @arena) -> Result<u64, BlockError>;
    flush() -> Result<(), BlockError>;
}
```

### 3.2 Zero-copy block I/O path

The block I/O path achieves zero copies for large reads:

1. **Client allocates a buffer** from its budget MEM cap.
2. **Client sends `RD_READ`** with the buffer as a MEM cap argument
   (rights: `W` only — the driver writes into it).
3. **Kernel mints a derived W-only cap** in the driver's table.
4. **Driver writes directly into the client's buffer** via the mapped region.
   No kernel copy of payload data — only the cap descriptor crosses the
   channel message.
5. **Driver sends reply** — kernel auto-revokes the transient grant.

For small reads (< 4 KiB), the inline payload path is used instead:
the data is copied into the channel message payload directly. This avoids
the overhead of granting a MEM cap for a single sector read.

```
ZERO-COPY PATH (≥ 1 sector):
  Client → [RD_READ{lba,count}] → Kernel → [W-only cap grant] → Driver
  Driver → [memcpy: NVMe buffer → client buffer] → [RD_OK] → Client
  Grant auto-revoked on reply.

INLINE PATH (single sector, fast path):
  Client → [RD_READ{lba,1}] → Kernel → Driver
  Driver → [memcpy: NVMe → msg payload] → [RD_OK + 512B payload] → Client
  No cap grant needed.
```

### 3.3 Block service sidecar

The Block Service sits between the NVMe driver and the filesystem. Its
responsibilities:
- Block cache (LRU, 4 KiB pages, matching the existing `user/blockcache` crate)
- Read-ahead and write-back buffering
- Multi-device support (each NVMe namespace is a separate block device)

The Block Service connects to the NVMe driver via a block channel and to
the filesystem via a block channel. The filesystem never talks to the NVMe
driver directly — the Block Service is the intermediary.

### 3.4 Filesystem sidecar

The filesystem sidecar implements:
- **ext2-lite** for read/write support (sufficient for POSIX VFS)
- **aerofs-lite** (the existing Phase 2 read-only FS) for the root filesystem
  image

The filesystem sidecar connects to the Block Service via a block channel and
to the POSIX sidecar via a VFS channel.

**VFS channel protocol:**

```aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.vfs")

enum VfsOp {
    OPEN      = 1,
    CLOSE     = 2,
    READ      = 3,
    WRITE     = 4,
    LSEEK     = 5,
    STAT      = 6,
    FSTAT     = 7,
    READDIR   = 8,
    MKDIR     = 9,
    UNLINK    = 10,
}

struct VfsRequest {
    op:      VfsOp,
    fd:      u32,       // file descriptor (for fd-based ops)
    path:    string,    // for path-based ops (OPEN, STAT, MKDIR, UNLINK)
    offset:  u64,       // for LSEEK, READ, WRITE
    count:   u32,       // byte count for READ, WRITE
    flags:   u32,       // OPEN flags (O_RDONLY, etc.)
    tag:     u32,       // request ID for reply pairing
}

struct VfsReply {
    status:  i32,       // 0 = OK, negative = errno
    count:   u32,       // bytes read/written
    fd:      u32,       // new fd number (for OPEN)
    tag:     u32,
}

interface VfsService {
    open(path: string, flags: u32, mode: u32) -> Result<u32, VfsError>;
    read(fd: u32, buf: MemCap @arena, count: u32) -> Result<u32, VfsError>;
    write(fd: u32, buf: MemCap @arena, count: u32) -> Result<u32, VfsError>;
    close(fd: u32) -> Result<(), VfsError>;
    stat(path: string) -> Result<FileStat, VfsError>;
    readdir(path: string, buf: MemCap @arena) -> Result<u32, VfsError>;
}
```

### 3.5 How a file read reaches the NVMe driver

Complete trace for `cat /etc/passwd`:

```
 shell task          POSIX core           Filesystem         Block Service        NVMe Driver
    │                   │                    │                   │                   │
    │── read(5, 200)──▶│                    │                   │                   │
    │                   │── VFS_READ{fd:5,   │                   │                   │
    │                   │   count:200}──────▶│                   │                   │
    │                   │                    │── check cache     │                   │
    │                   │                    │   (miss)          │                   │
    │                   │                    │── BLK_READ{ino,   │                   │
    │                   │                    │   blk:12}────────▶│                   │
    │                   │                    │                   │── RD_READ{lba,    │
    │                   │                    │                   │   count:1}        │
    │                   │                    │                   │   caps:[W-cap]    │
    │                   │                    │                   │──────────────────▶│
    │                   │                    │                   │                   │
    │                   │                    │                   │   driver writes   │
    │                   │                    │                   │   directly into   │
    │                   │                    │                   │   block cache buf │
    │                   │                    │                   │◀── RD_OK ─────────│
    │                   │                    │◀── data ready ────│   (grant revoked) │
    │                   │                    │   copy to file    │                   │
    │                   │                    │   cache slot      │                   │
    │                   │◀── data ───────────│                   │                   │
    │◀── 200 bytes ─────│   (from cache)     │                   │                   │
```

With `RD_MAP` (the zero-copy fast path), after initial mount the filesystem
maps the entire ramdisk image read-only into its own address space. File
metadata reads are direct memory references — no channel round-trips for
inode/directory lookups. Only data block reads for files not in the root
image require channel calls to the block service.

---

## 4. Network stack

### 4.1 NIC driver sidecar (e1000)

The NIC driver sidecar wraps the existing `net/e1000.c` driver code. It
receives:
- MEM cap covering the e1000 MMIO BAR (from Device Manager)
- MEM cap for shared ring buffers (TX/RX descriptor rings)
- Interrupt assignment (MSI-X entry for the e1000 IRQ)

**Channel protocol (packet I/O):**

```aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.nic")

struct PacketHeader {
    len:      u16,
    flags:    u16,     // bit0=broadcast, bit1=multicast, bit2=promisc
    vlan:     u16,
    reserved: u16,
}

interface NicDriver {
    // Zero-copy packet transmit: buffer transferred via MEM cap
    transmit(buf: MemCap @arena, len: u32) -> Result<(), NicError>;
    // Zero-copy packet receive: driver grants a MEM cap per packet
    receive() -> Result<(MemCap @arena, PacketHeader), NicError>;
    // Return a consumed receive buffer
    release_buf(buf: MemCap) -> ();
    // Get hardware info (MAC address, link status)
    get_info() -> NicInfo;
    // Set MAC filter, promiscuous mode
    configure(config: NicConfig) -> Result<(), NicError>;
}
```

### 4.2 Zero-copy packet buffers

The NIC driver uses a **buffer pool** for packet I/O:

1. The NIC driver pre-allocates a pool of receive buffers from its budget
   (typically 256 × 2 KiB = 512 KiB).
2. On `receive()`, the driver returns a MEM cap granting R-only access to
   one buffer — the network stack reads the packet data directly from
   the driver's memory.
3. The network stack processes the packet, then calls `release_buf()` to
   return the buffer (which auto-revokes the grant).
4. On `transmit()`, the network stack grants W-only access to its own
   buffer — the driver DMA's from it directly.

This means packet data is never copied through a channel message payload:
the channel messages carry only cap descriptors (16 bytes each).

### 4.3 TCP/IP stack sidecar

The network stack sidecar implements:
- **ARP** (address resolution, 14-byte Ethernet + 28-byte ARP)
- **ICMP** (ping, echo request/reply)
- **IPv4** (fragmentation, header checksum)
- **TCP** (connection management, sliding window, congestion control, retransmission)
- **UDP** (datagram service)
- **DNS resolver** (UDP port 53)

The network stack connects to:
- NIC driver (packet channel)
- POSIX sidecar (socket channel)
- Kernel bootstrap (console channel for logging)

### 4.4 Socket channel protocol

```aeroidl
@version("aeroidl/1.0")
@namespace("aerosls.socket")

enum SocketOp {
    CREATE     = 1,   // socket(AF_INET, SOCK_STREAM, 0)
    CONNECT    = 2,   // connect(fd, addr, len)
    BIND       = 3,   // bind(fd, addr, len)
    LISTEN     = 4,   // listen(fd, backlog)
    ACCEPT     = 5,   // accept(fd) → new fd
    SEND       = 6,   // send(fd, buf, len)
    RECV       = 7,   // recv(fd, buf, len)
    CLOSE      = 8,   // close(fd)
    RESOLVE    = 9,   // getaddrinfo(host, service)
}

struct SocketAddr {
    family:   u16,    // AF_INET = 2
    port:     u16,    // network byte order
    addr:     u32,    // IPv4 address, network byte order
}

struct SocketRequest {
    op:     SocketOp,
    fd:     u32,
    addr:   SocketAddr,
    flags:  u32,
    tag:    u32,
}

struct SocketReply {
    status: i32,       // 0 = OK, negative = errno
    fd:     u32,       // new fd for ACCEPT
    count:  u32,       // bytes sent/received
    tag:    u32,
}

interface SocketService {
    create(family: u16, sock_type: u16, proto: u16) -> Result<u32, SockError>;
    connect(fd: u32, addr: SocketAddr) -> Result<(), SockError>;
    send(fd: u32, buf: MemCap @arena, count: u32) -> Result<u32, SockError>;
    recv(fd: u32, buf: MemCap @arena, count: u32) -> Result<u32, SockError>;
    close(fd: u32) -> Result<(), SockError>;
    resolve(host: string, service: string) -> Result<SocketAddr, SockError>;
}
```

### 4.5 Connections as capabilities

Each TCP connection is represented as a **socket fd** in the POSIX sidecar
that maps to a **CHAN cap** to the network stack sidecar. The mapping is:

| POSIX concept | AeroSLS implementation |
|---|---|
| `socket(AF_INET, SOCK_STREAM, 0)` | POSIX core creates an fd entry, sends `SOCK_CREATE` to network stack, receives back a CHAN cap to a connection-specific endpoint |
| `connect(fd, addr, len)` | POSIX core sends `SOCK_CONNECT` with addr; network stack performs TCP handshake and returns status |
| `send(fd, buf, len)` | POSIX core sends `SOCK_SEND` with a MEM cap to the buffer; network stack DMA's packet data through the NIC driver |
| `close(fd)` | POSIX core sends `SOCK_CLOSE`; network stack performs TCP FIN, revokes the CHAN cap |

The CHAN cap is the connection's identity. If the network stack sidecar dies,
every connection CHAN cap becomes stale (kernel delivers `CLOSE_PEER_DEAD`),
and the POSIX sidecar's socket fds return `ECONNRESET`. The POSIX core can
optionally re-establish connections via the respawn mechanism.

---

## 5. Inter-sidecar service contracts

### 5.1 IDL definitions (complete)

All service interfaces are defined in `idl/`:

| File | Interface | Purpose |
|------|-----------|---------|
| `idl/blockdevice.aeroidl` | `BlockDevice` | Sector-level block I/O |
| `idl/filesystem.aeroidl` | `VfsService` | File operations, directory traversal |
| `idl/network.aeroidl` | `SocketService` | TCP/UDP socket operations |
| `idl/nic.aeroidl` | `NicDriver` | Ethernet frame TX/RX |
| `idl/registry.aeroidl` | `CapabilityRegistry` | Service discovery |
| `idl/watchdog.aeroidl` | `WatchdogService` | Health monitoring, restart |

### 5.2 Capability advertising and revocation

**Advertising:**
When a sidecar starts, it registers its capabilities with the Device Manager
by sending a `ServiceRegistered` message over its parent channel. The Device
Manager then:
1. Registers the service in the kernel's `services_registry[]` (via
   `SYS_SLS_SERVICE_REGISTER`).
2. Stores the sidecar's channel slot in the capability registry.
3. Signals readiness to the init sidecar.

**Revocation:**
Capabilities are revoked through three mechanisms:

1. **Explicit revocation:** A sidecar calls `cap_revoke(handle)` to
   voluntarily release a capability. The kernel revokes the entry and all
   its descendants (lineage, capability-layer spec §3.4).

2. **Channel close:** When a sidecar dies, the kernel tears down all its
   channels, delivering `CLOSE_PEER_DEAD` to every peer. All cap grants
   are reaped (capability-layer spec §6.3).

3. **Timeout-based revocation:** The POSIX sidecar's VFS uses client-side
   deadlines (Phase 2 §5.4): if a block request times out, the VFS tears
   down the channel and re-opens via the respawn path. Stale caps are
   reclaimed by the kernel's close sweep.

### 5.3 Service discovery and binding

A client discovers and binds to a service through this sequence:

```
1. Client sends SVC_RESOLVE{name: "svc.filesystem.0"} to registry channel
2. Registry replies with {sidecar_id: 5, channel_slot: 3, rights: 0x03}
3. Client calls cap_info(slot 3) to verify the CHAN cap
4. Client sends requests on that channel; kernel validates rights per message
```

If the target sidecar dies and is respawned:
1. Registry entry is updated by the Device Manager (new sidecar_id).
2. Old CHAN caps are dead (kernel delivered CLOSE_PEER_DEAD).
3. Client detects the close event and re-resolves the service name.
4. Client receives the new CHAN cap to the respawned sidecar.

This is the same pattern as the Driver Respawn Spec's stale→backoff→spawn→attach
state machine, generalized to any service.

---

## 6. Self-hosting requirements

### 6.1 Minimal userland tools

To reach a POSIX shell and run the demo, the root filesystem image must
contain:

| Path | Size | Purpose |
|------|------|---------|
| `/bin/busybox` | ~300 KiB | Multi-call binary (init, sh, ls, cat, echo, grep, mkdir, mount, curl-like) |
| `/bin/sh` | symlink → busybox | POSIX shell |
| `/etc/passwd` | 128 B | User database |
| `/etc/group` | 64 B | Group database |
| `/etc/fstab` | 128 B | Mount table |
| `/tmp/` | empty dir | Writable tmpfs mount point |
| `/dev/console` | device node | Console device |
| `/dev/null` | device node | Null device |

Total: ~300 KiB of image data.

### 6.2 Cross-compilation toolchain

The sidecars are compiled as freestanding ELFs:

```
HOST: x86-64 Linux (or macOS with cross toolchain)
TARGET: x86_64-unknown-none (Rust) or x86-64-elf (C for kernel)

Toolchain components:
  - rustc (nightly, target: x86_64-unknown-none)
  - cargo (for Rust sidecars: init, POSIX, block service, filesystem,
    network stack, watchdog, WASM runtime, Lisp runtime)
  - gcc (for C kernel and drivers: nvme, e1000, console)
  - grub-mkrescue (for x86-64 bootable ISO)
  - mke2fs (for ext2-lite root filesystem image)
  - nasm (for arch/x86/boot.asm)
```

### 6.3 Build system

The build produces three artifacts:

1. **Kernel ELF** (`sls_kernel.elf`): the microkernel + initrd loaded by GRUB.
2. **Sidecar image archive** (`sidecars.cpio`): all sidecar ELF binaries + manifests.
3. **Root filesystem image** (`rootfs.ext2`): aerofs-lite/ext2 image for POSIX.

The Makefile targets:

```makefile
# Phase 5 self-hosted targets
.PHONY: selfhost-kernel selfhost-sidecar selfhost-rootfs selfhost-iso

selfhost-kernel:
    $(CC) $(KERNEL_CFLAGS) -c kernel/kernel.c -o kernel.o
    # ... (all kernel objects)
    $(LD) $(LDFLAGS) -o sls_kernel.elf $(KERNEL_OBJS)

selfhost-sidecar:
    cargo build --target x86_64-unknown-none --release -p init
    cargo build --target x86_64-unknown-none --release -p posix
    cargo build --target x86_64-unknown-none --release -p block_service
    cargo build --target x86_64-unknown-none --release -p filesystem
    cargo build --target x86_64-unknown-none --release -p network
    cargo build --target x86_64-unknown-none --release -p nvme_driver
    cargo build --target x86_64-unknown-none --release -p nic_driver
    cargo build --target x86_64-unknown-none --release -p watchdog
    # Generate manifests from .aeroidl files
    aeroidl-gen idl/*.aeroidl --out manifests/

selfhost-rootfs:
    # Build aerofs-lite image with BusyBox + /etc
    ./tools/genrootfs --busybox user/busybox/busybox \
                      --output rootfs.ext2 \
                      --block-size 512

selfhost-iso:
    # Package kernel + sidecar initrd + rootfs into a bootable ISO
    mkdir -p iso/boot/grub
    cp sls_kernel.elf iso/boot/
    find sidecars/ rootfs.ext2 | cpio -o -H newc > iso/boot/initrd.cpio
    cp grub.cfg iso/boot/grub/grub.cfg
    grub-mkrescue -o aerosls-selfhost.iso iso/
```

### 6.4 Minimal binary set to reach a shell

The absolute minimum to boot to a shell prompt:

1. **Kernel** (~200 KiB): capability layer, scheduler, channel transport,
   sidecar creation, PCI discovery, frame allocator.
2. **Init sidecar** (~16 KiB): reads device registry, spawns Device Manager,
   waits for ready signal, spawns POSIX sidecar.
3. **Device Manager sidecar** (~32 KiB): PCI enumeration, spawns driver
   sidecars, manages hotplug.
4. **NVMe driver sidecar** (~8 KiB): block I/O to NVMe device.
5. **Block Service sidecar** (~16 KiB): block cache, read-ahead.
6. **Filesystem sidecar** (~32 KiB): ext2/aerofs-lite mount, inode management.
7. **Console driver sidecar** (~4 KiB): VGA text-mode output.
8. **POSIX sidecar** (~128 KiB): shell, VFS, process model.
9. **BusyBox** (~300 KiB): shell, ls, cat, echo, mount, init.
10. **Root filesystem image** (~300 KiB): BusyBox + /etc + /dev.

Total: ~1 MiB. This fits easily in a 4 MiB initrd.

---

## 7. Demo scenario

### 7.1 Boot to POSIX shell

**Steps:**
1. Power on → U-Boot/BIOS → GRUB → load kernel + initrd.
2. Kernel boots, discovers PCI devices, creates init sidecar.
3. Init spawns Device Manager → NVMe driver → NIC driver → console →
   block service → filesystem → network stack → watchdog.
4. Init spawns POSIX sidecar with channels to filesystem, console, network.
5. POSIX sidecar mounts root filesystem, spawns `/bin/busybox init`.
6. BusyBox init runs `/etc/init.d/rcS`, mounts `/tmp`, `/dev`.
7. BusyBox spawns shell → `# ` prompt on serial console.

**Expected serial output:**
```
[AEROSLS BOOT LOGGER V1.0.0 RUNNING]
[BSP] Loading GDT and IDT...
[HW] CPU IntelGenuine family=6 model=142
[HW] Memory map:
[HW]   0000000000000000 +    640 KiB  RAM
[HW]   0000000000100000 +  65408 KiB  RAM
[HW] Usable RAM: 64 MiB
[SLS] Capability layer initialized (128 entries)
[SLS] Launching init sidecar...
[INIT] found 3 PCI device(s)
[INIT] NVMe at slot 0, e1000 at slot 1, VGA at slot 2
[INIT] spawning Device Manager...
[DM] enumerating devices: NVMe(0), e1000(1), VGA(2)
[DM] spawning NVMe driver...
[NVME] Admin queue ready. I/O queue ready. 64 GiB capacity.
[DM] spawning e1000 driver...
[E1000] NIC initialized. MAC: 52:54:00:12:34:56
[DM] spawning block service...
[DM] spawning filesystem...
[FS] mounted root (aerofs-lite, read-only, 512 sectors)
[DM] spawning network stack...
[NET] ARP: gratuitous ARP sent. Link up.
[DM] spawning POSIX sidecar...
[POSIX] /dev/console = console device
[POSIX] /dev/null = null device
[POSIX] mounting /tmp (ramfs)...
[POSIX] spawning /bin/busybox init...
[POSIX] shell started (pid 1)
#
```

### 7.2 Running ls, cat, and a network fetch

**`ls /etc/`:**
```
# ls /etc/
passwd    group     fstab
```
Sidecars involved: POSIX → Filesystem → Block Service → NVMe Driver.

**`cat /etc/passwd`:**
```
# cat /etc/passwd
root:x:0:0:root:/root:/bin/sh
nobody:x:65534:65534:nobody:/nonexistent:/bin/false
```
Same sidecar chain. Data flows: NVMe → Block Service cache → Filesystem
inode lookup → POSIX → shell stdout → Console driver → VGA/serial.

**Network fetch (`wget` / `curl` equivalent):**
```
# fetch http://10.0.2.2:8080/hello.txt
Hello from the host!
```
Sidecars involved: POSIX → Network Stack → NIC Driver → (external network)
→ NIC Driver → Network Stack → POSIX.

The `fetch` command (a BusyBox-like applet) performs:
1. `socket(AF_INET, SOCK_STREAM, 0)` → POSIX sends `SOCK_CREATE` to Network
   Stack → new CHAN cap returned → fd allocated.
2. `getaddrinfo("10.0.2.2", "8080")` → POSIX sends `DNS_RESOLVE` → Network
   Stack resolves (or uses hardcoded address) → `SocketAddr` returned.
3. `connect(fd, addr, len)` → POSIX sends `SOCK_CONNECT` → Network Stack
   performs TCP 3-way handshake via NIC Driver → connected.
4. `send(fd, "GET /hello.txt HTTP/1.0\r\n\r\n")` → POSIX sends `SOCK_SEND`
   with MEM cap → Network Stack transmits via NIC Driver.
5. `recv(fd, buf, 4096)` → POSIX sends `SOCK_RECV` → Network Stack reads
   from NIC Driver (zero-copy receive buffer) → returns via MEM cap →
   POSIX copies to user buffer → print.

### 7.3 WASM → Lisp cross-call with network

**The WASM module** (`examples/fetch_and_process.wasm`):
```rust
// Compiled to WASM. Calls Lisp for data processing, uses network for fetch.
extern "C" {
    fn lisp_eval(expr: *const u8, len: u32) -> i32;
}

#[no_mangle]
pub extern "C" fn _start() {
    // 1. Fetch data from network
    let data = fetch("http://10.0.2.2:8080/data.json");

    // 2. Call Lisp to process it
    let result = lisp_eval(b"(json-parse data)", 20);

    // 3. Print result
    print(result);
}
```

**The Lisp sidecar** (`svc.lisp.0`):
```lisp
;; Registered via AeroIDL
(defun json-parse (data)
  ;; Simple JSON parsing
  (parse-json data))
```

**Execution flow:**
1. POSIX spawns WASM runtime sidecar.
2. WASM runtime loads the module, sets up the shared arena (Phase 3).
3. WASM calls `fetch()` → Network Stack → NIC Driver → host → NIC → Network
   Stack → WASM (zero-copy via MEM cap in shared arena).
4. WASM calls `lisp_eval()` → channel to Lisp runtime → Lisp processes
   data → result via shared arena (zero-copy) → channel back to WASM.
5. WASM calls `print()` → channel to POSIX → stdout → Console driver.

Sidecars involved: POSIX → WASM Runtime → Lisp Runtime + Network Stack →
NIC Driver.

---

## 8. Reliability

### 8.1 Watchdog sidecar

The watchdog sidecar (`svc.watchdog.0`) monitors all other sidecars:

```
┌───────────────────────────────────────────────────────────┐
│  Watchdog Sidecar                                         │
│                                                           │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐     │
│  │ NVMe    │  │ NIC     │  │ FS      │  │ Network │     │
│  │ Driver  │  │ Driver  │  │ Service │  │ Stack   │     │
│  │ health  │  │ health  │  │ health  │  │ health  │     │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘     │
│       │            │            │            │            │
│       ▼            ▼            ▼            ▼            │
│  ┌─────────────────────────────────────────────────┐     │
│  │ Health Monitor                                   │     │
│  │ - periodic heartbeat (every 100ms)               │     │
│  │ - checks channel liveness via cap_info           │     │
│  │ - logs health state transitions                  │     │
│  └─────────────────────────────────────────────────┘     │
└───────────────────────────────────────────────────────────┘
```

The watchdog connects to each sidecar via a monitoring channel. Each sidecar
periodically sends a `HEARTBEAT` message. If no heartbeat arrives within the
deadline, the watchdog triggers a restart via the Device Manager.

### 8.2 Restart policies for critical services

| Service | Restart policy | Max retries | Backoff |
|---------|---------------|-------------|---------|
| NVMe Driver | Always restart | 8 | 100ms → 5s exp |
| NIC Driver | Always restart | 8 | 100ms → 5s exp |
| Block Service | Always restart | 4 | 200ms → 5s exp |
| Filesystem | Always restart | 4 | 200ms → 5s exp |
| Network Stack | Always restart | 4 | 200ms → 5s exp |
| POSIX | Never restart (fatal) | 0 | — |
| Watchdog | Always restart | 16 | 50ms → 2s exp |

Restart policies are defined in the Device Manager's configuration and
enforced via the `CAP_SPAWN` + `create_sidecar` path (Driver Respawn Spec).

### 8.3 USB device hotplug

USB hotplug is handled by the Device Manager receiving an interrupt when
a new PCI device appears on the bus:

1. Device Manager detects new PCI device via MMIO BAR configuration
   change (or ACPI notification).
2. Device Manager looks up the driver manifest from the device registry.
3. Device Manager spawns the appropriate driver sidecar.
4. Device Manager registers the new service in the capability registry.
5. Existing sidecars can discover the new device via `SVC_RESOLVE`.

For USB mass storage (future): the NVMe driver sidecar pattern generalizes
to an AHCI/USB-storage sidecar that implements the same `BlockDevice` IDL.

### 8.4 Network driver crash without losing connections

When the NIC driver crashes:

1. The kernel delivers `CLOSE_PEER_DEAD` to the Network Stack sidecar.
2. The Network Stack marks all active TCP connections as **stale** but
   does NOT close them — it holds the connection state (sequence numbers,
   window sizes, pending data) in its own memory.
3. The Device Manager respawns the NIC driver (fresh CAP_SPAWN + create_sidecar).
4. The Network Stack re-establishes the NIC driver channel and resumes
   packet I/O on the new driver.
5. For established TCP connections: the Network Stack can resume sending
   because TCP supports retransmission — packets lost during the downtime
   are retransmitted by the sender (the remote host). The TCP sequence
   numbers and window state are preserved in the Network Stack's memory.
6. For new connections: they are created normally after the NIC driver is
   back.

**What is NOT preserved:** packets that were in the NIC driver's TX/RX
ring buffers at the moment of crash. These are lost and must be
retransmitted by the TCP layer. This is acceptable because TCP already
handles packet loss.

**State recovery sequence:**
```
NIC driver crash
  → CLOSE_PEER_DEAD to Network Stack
  → Network Stack: mark connections "degraded" (no TX/RX possible)
  → Device Manager: respawn NIC driver
  → Network Stack: new channel to NIC driver
  → Network Stack: reconfigure MAC filter, multicast list
  → Network Stack: resume TX/RX on all connections
  → TCP retransmits any packets lost during downtime
  → Connections recover transparently (to the POSIX sidecar)
```

---

## 9. Security

### 9.1 Secure boot chain

The boot chain establishes trust from firmware to sidecars:

```
Stage 1: Firmware (UEFI/OpenSBI)
  - Signed firmware image
  - Verifies boot loader signature
  - Transfers control to boot loader

Stage 2: Boot loader (GRUB)
  - Loaded by firmware
  - Verifies kernel ELF signature (secure boot)
  - Loads kernel + initrd into memory

Stage 3: Kernel
  - Boots with capability layer
  - Loads init sidecar from initrd
  - Validates init sidecar manifest (CRC + signature)
  - Builds initial capability table
  - Grants ONLY the capabilities declared in the manifest

Stage 4: Init sidecar
  - Has CAP_SPAWN for Device Manager only
  - Cannot create arbitrary sidecars
  - Cannot modify kernel state

Stage 5: Device Manager
  - Has CAP_SPAWN for driver manifests only
  - Each driver manifest is validated by the kernel
  - Cannot grant capabilities beyond what its manifest declares
```

**TCB minimization:** the trusted computing base is:

| Component | Lines of code (est.) | Why trusted |
|-----------|---------------------|-------------|
| Kernel (capability layer) | ~5,000 | Core enforcement of capability invariants |
| Kernel (channel transport) | ~3,000 | Message delivery, cap minting |
| Kernel (MMU/page tables) | ~2,000 | Memory isolation between sidecars |
| Kernel (PCI/MMIO discovery) | ~1,000 | Hardware enumeration |
| Init sidecar | ~1,000 | Orchestrator, spawn authority |
| Device Manager | ~2,000 | Device lifecycle, driver manifest registry |
| **Total TCB** | **~14,000 lines** | |

Everything else — NVMe driver, NIC driver, TCP/IP stack, filesystem, POSIX
sidecar, WASM runtime, Lisp runtime — is NOT trusted. A bug or compromise
in any of these is contained by the capability system.

### 9.2 Capability partitioning at boot

Each sidecar's manifest declares EXACTLY the capabilities it needs:

| Sidecar | Capabilities |
|---------|-------------|
| Init | budget(rw), img.ro(rx), console(rw), device_registry(r), spawn(DeviceManager) |
| Device Manager | budget(rw), img.ro(rx), console(rw), registry_channel(rw), spawn(drivers) |
| NVMe Driver | budget(rw), mmio_bar(rw), console(rw), parent_channel(rw) |
| NIC Driver | budget(rw), mmio_bar(rw), ring_buffers(rw), console(rw), parent_channel(rw) |
| Block Service | budget(rw), console(rw), nvme_channel(rw) |
| Filesystem | budget(rw), console(rw), block_channel(rw) |
| POSIX | budget(rw), img.ro(rx), console(rw), fs_channel(rw), socket_channel(rw), spawn(posix) |
| Network Stack | budget(rw), console(rw), nic_channel(rw), parent_channel(rw) |

No sidecar holds capabilities for resources it does not use. The NVMe driver
cannot reach the NIC. The filesystem cannot reach the network. The POSIX
sidecar cannot access NVMe MMIO directly — it must go through the filesystem
or block service.

### 9.3 TCB minimization analysis

**What must be correct for the system to be secure:**
1. The kernel correctly enforces capability invariants (no amplification, no
   cross-sidecar access, correct mint/derive/revoke lifecycle).
2. The kernel correctly manages MMU page tables (no cross-sidecar memory
   access in isolated mode).
3. The init sidecar correctly spawns sidecars from validated manifests only.
4. The Device Manager correctly validates driver manifests against the kernel's
   registry.

**What can be buggy without compromising the system:**
- Any driver (NVMe, NIC, console) — bugs are contained by capability
  boundaries.
- The filesystem — cannot access raw block devices, only the block service.
- The POSIX sidecar — cannot mint capabilities, only use what it holds.
- The WASM/Lisp runtime — contained by the POSIX sidecar's process model.

**What CANNOT be fixed by capability enforcement:**
- A compromised POSIX sidecar can corrupt its own internal state (fd table,
  VFS mounts) because it is one trust domain (Phase 2 §7.4). This is the
  honest limitation of the shared-address-space model. Mitigated by Rust's
  memory safety within the core.

---

## 10. Performance targets and benchmark plan

### 10.1 Boot time targets

| Phase | Time target | Description |
|-------|-------------|-------------|
| Firmware → kernel entry | < 500 ms | GRUB → kernel_main() |
| Kernel init | < 200 ms | PCI scan, capability init, scheduler |
| Sidecar creation (all) | < 500 ms | init → Device Manager → all drivers → POSIX |
| POSIX → shell prompt | < 300 ms | VFS mount, BusyBox init, shell spawn |
| **Total: power → shell** | **< 1.5 s** | Comparable to embedded Linux on same HW |

Comparison: Linux on the same QEMU target boots in ~2–3 s (with full
systemd) or ~800 ms (with minimal init). AeroSLS should be competitive
with the minimal init case.

### 10.2 File I/O throughput targets

| Metric | Target | Linux comparison |
|--------|--------|-----------------|
| Sequential read (4 KiB blocks) | > 500 MiB/s | ~800 MiB/s (ext4, page cache) |
| Sequential write (4 KiB blocks) | > 200 MiB/s | ~400 MiB/s (ext4, write-back) |
| Random 4K read IOPS | > 50,000 | ~100,000 (ext4, page cache) |
| `cat` of 1 MiB file | < 10 ms | ~5 ms |

**Note:** AeroSLS overhead comes from: (a) channel round-trips per block
request (the cross-sidecar call cost), (b) capability validation at the
kernel level, (c) cap grant minting/revocation per request. With RD_MAP
(zero-copy for read-only mounts), most metadata reads are direct memory
references, so the per-request overhead applies only to data block reads
not in cache.

### 10.3 Network throughput targets

| Metric | Target | Linux comparison |
|--------|--------|-----------------|
| TCP throughput (single flow) | > 8 Gbps | ~9.4 Gbps (10G NIC, TCP offload) |
| TCP latency (loopback) | < 10 μs | ~5 μs |
| TCP latency (cross-sidecar) | < 20 μs | N/A (Linux has no sidecar concept) |
| HTTP request round-trip | < 100 μs | ~50 μs (nginx) |

**Bottleneck analysis:** The network path is:
```
POSIX send() → channel → Network Stack → channel → NIC Driver → DMA
```
Each channel round-trip is ~200 ns (Phase 3 benchmark). With 2 channel
round-trips per packet, the overhead is ~400 ns per packet. For a 1500-byte
MTU at 10 Gbps, that's ~830,000 packets/second — the overhead is ~330 μs/s
or 0.033%, negligible.

The real bottleneck is the NIC driver's DMA latency and the TCP/IP stack's
processing (checksum, window management). These are bounded by the existing
`net/tcp.c` implementation.

### 10.4 Cross-sidecar call latency targets

| Call pattern | Target | Measurement method |
|---|---|---|
| Channel send → recv (no payload) | < 200 ns | Phase 3 benchmark, counter-based |
| Channel send → recv (4 KiB payload) | < 500 ns | Phase 3 benchmark |
| Channel send → recv (MEM cap, zero-copy) | < 300 ns | Phase 3 shared arena benchmark |
| Block I/O (cache hit) | < 2 μs | POSIX → VFS → Block Service → POSIX |
| Block I/O (cache miss, RD_MAP) | < 10 μs | POSIX → VFS → NVMe → POSIX (no block service) |
| Socket send → NIC TX | < 5 μs | POSIX → Network Stack → NIC Driver → DMA |
| WASM → Lisp call | < 500 ns | Phase 3 AeroIDL benchmark |

### 10.5 Benchmark plan

**Phase 5a — Microbenchmarks (host-side):**
- Channel latency: measure send→recv round-trip across a channel pair
  using TSC counters (same approach as Phase 3's `bench-polyglot.jsonl`).
- Cap grant overhead: measure mint+revoke cycle time.
- Zero-copy vs. copy path: compare RD_READ (inline) vs. RD_MAP (mapped)
  for 4 KiB and 1 MiB files.

**Phase 5b — Integration benchmarks (QEMU):**
- Boot time: measure from `kernel_main()` entry to shell prompt via
  serial timestamp (kernel timer ticks).
- File I/O: `dd if=/etc/passwd of=/dev/null bs=4096 count=10000` through
  the POSIX sidecar, measure throughput.
- Network: `iperf3` equivalent (custom BusyBox applet) between AeroSLS
  guest and host, measure throughput and latency.
- Cross-sidecar: time 10,000 iterations of a trivial POSIX→Filesystem→Block
  round-trip.

**Phase 5c — Comparison with Linux (same QEMU config):**
- Boot time: Linux with `rdinit=/bin/sh` on identical QEMU flags.
- File I/O: `dd` with ext4 on the same NVMe device.
- Network: `iperf3` between Linux guest and host.
- Memory: measure sidecar memory usage vs. Linux process overhead.

### 10.6 Memory budget

| Sidecar | Budget | Notes |
|---------|--------|-------|
| Init | 16 MiB | Orchestration only |
| Device Manager | 16 MiB | PCI enumeration, driver manifests |
| NVMe Driver | 8 MiB | MMIO BAR, command queues |
| NIC Driver | 16 MiB | Ring buffers (8 MiB), driver state |
| Block Service | 32 MiB | Block cache (256 MiB max, configurable) |
| Filesystem | 16 MiB | Inode cache, dentry cache |
| Network Stack | 32 MiB | TCP connection table, packet buffers |
| POSIX | 32 MiB | Shell, process model, VFS |
| Watchdog | 4 MiB | Health monitoring state |
| WASM Runtime | 16 MiB | WASM linear memory |
| Lisp Runtime | 16 MiB | Lisp heap |
| **Total** | **~200 MiB** | Fits in 256 MiB QEMU guest |

---

## Appendix A: Implementation roadmap

### Phase 5.1 — Kernel sidecar infrastructure (4 weeks)
- Implement `create_sidecar()` in `kernel/cap.c`
- Implement channel transport (send/recv/wait) in `kernel/chan.c`
- Implement `CAP_SPAWN` and manifest registry
- Boot Info Block generation
- Init sidecar binary and manifest

### Phase 5.2 — Device Manager sidecar (2 weeks)
- PCI enumeration in sidecar context
- Driver manifest registry
- Driver lifecycle management (spawn, monitor, respawn)
- Hotplug support (PCI device hot-plug detection)

### Phase 5.3 — Storage stack (3 weeks)
- NVMe driver sidecar (port `drivers/nvme*.c`)
- Block Service sidecar (port `user/blockcache`)
- Filesystem sidecar (ext2-lite + aerofs-lite)
- Block I/O channel protocol implementation
- RD_MAP zero-copy path

### Phase 5.4 — Network stack (3 weeks)
- NIC driver sidecar (port `net/e1000.c`)
- TCP/IP stack sidecar (port `net/tcp.c`, `net/consensus.c`)
- Socket channel protocol
- DNS resolver
- Connection-as-capability model

### Phase 5.5 — POSIX integration (2 weeks)
- Extend POSIX sidecar to use sidecar channels instead of ramdisk
- Socket syscalls via network stack channel
- File I/O via filesystem channel
- Console via console driver sidecar

### Phase 5.6 — Polyglot demo (2 weeks)
- WASM runtime sidecar
- Lisp runtime sidecar
- WASM→Lisp cross-call via shared arena (Phase 3)
- Network fetch from WASM module

### Phase 5.7 — Reliability and polish (2 weeks)
- Watchdog sidecar
- Restart policies
- Crash recovery demonstration
- Performance benchmarking
- Boot time optimization

**Total: ~18 weeks**

---

## Appendix B: Open questions

1. **Should the init sidecar be replaced by a Device Manager that IS the init?**
   The two-layer (init → Device Manager) design adds latency and complexity.
   A single "init + device manager" sidecar would be simpler, but violates
   the single-responsibility principle and makes the init sidecar's authority
   too broad. Recommend keeping the two-layer design for security, but
   measure the boot-time cost.

2. **Should the POSIX sidecar own ALL fds, or should each sidecar own its own?**
   In the current design, the POSIX sidecar owns all file descriptors and
   channels to the filesystem and network stack. An alternative is for each
   sidecar to own its own channels. The current design is simpler and matches
   the Phase 2 model; the alternative is more scalable for many independent
   sidecars. Recommend the current design for Phase 5, revisit for Phase 6.

3. **RD_MAP vs. RD_READ for the root filesystem?**
   The aerofs-lite root image is read-only and typically small (< 1 MiB).
   RD_MAP (zero-copy) is the obvious choice — the filesystem sidecar maps
   the whole image and reads inodes directly from memory. But RD_MAP
   requires the NVMe driver to have the memory-mapped capability, which
   means the NVMe driver must hold a MEM cap covering the entire disk.
   For a ramdisk this is fine (the "disk" is already in RAM); for real NVMe
   with DMA, the MMIO BAR is identity-mapped below 4 GiB and RD_MAP works.
   But for NVMe devices with BARs above 4 GiB, RD_MAP is blocked by the
   current address-space design. **Recommendation:** use RD_MAP for ramdisk
   and NVMe with low BARs, fall back to RD_READ for NVMe with high BARs.
   Document the limitation and plan a fix for Phase 6 (address-space
   extension for high-BAR MMIO).

4. **How does the POSIX sidecar discover the network stack for socket calls?**
   Option A: the Device Manager wires the network channel into the POSIX
   sidecar's manifest at creation time (manifest wiring, transport spec
   path 2). Option B: the POSIX sidecar resolves the network stack via the
   capability registry at boot. Option A is simpler and more deterministic;
   Option B is more flexible. **Recommendation:** Option A for Phase 5
   (hardcoded manifest), with Option B as a Phase 6 extension.

5. **Shared arena sizing for WASM↔Lisp calls?**
   The shared arena (Phase 3) is a MEM cap visible to both sidecars. Its
   size must be declared in both manifests. A 4 MiB arena is sufficient for
   most workloads but may be tight for large data processing. The arena size
   should be configurable per-deployment. **Recommendation:** 4 MiB default,
   configurable via manifest.

6. **What happens when the watchdog itself crashes?**
   The watchdog is the only sidecar that monitors others. If it crashes, no
   sidecar is being monitored. The Device Manager should detect this (its
   control channel to the watchdog closes) and respawn the watchdog. The
   watchdog must not be marked "never restart" — it must be the most
   aggressively restarted sidecar (policy: restart always, max 16 retries,
   50ms initial backoff). If the watchdog exhausts its retries, the system
   continues without monitoring but logs a critical warning.
