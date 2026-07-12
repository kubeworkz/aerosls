Building a **Single-Level Store (SLS) operating system** removes the traditional barrier between volatile main memory (RAM) and non-volatile secondary storage (disk/SSD). In an SLS architecture, all data—including running processes, system configurations, and persistent user files—is treated as a **single, massive, flat address space**. Programs do not make standard file system calls (like `open`, `read`, or `write`). Instead, they assign data to memory pointers, and the operating system handles transparency and persistence implicitly under the hood via paging mechanics.

Historically implemented in legendary platforms like **Multics** and **IBM's OS/400 (IBM i)**, modern interest in SLS systems is surging due to ultra-low latency NVMe hardware and vast 64-bit address spaces.

Here is the architectural roadmap we took to build a mock SLS operating system.

---

## **1. Conceptual Architecture**

To construct an SLS, we must redesign core kernel resource allocation strategies:

- **Unified Address Space**: Everything is a segment or an object referenced by a global, unique virtual memory pointer. 
- **Implicit I/O**: Hard drives or SSDs are treated merely as the physical spillover mechanism for RAM. Disk operations are strictly triggered via **Page Faults**.
- **Persistent Execution State**: Because memory is persistent, saving a file is equivalent to leaving it in memory. If the system loses power, the kernel reloads the exact state of active application objects from the storage medium upon booting up.

## **2. Core Implementation Phases**

Building any OS from scratch requires structured phases. For an SLS system, we will modify standard ++**[OSDev Wiki procedures](https://wiki.osdev.org/Creating_an_Operating_System)**++ to implement transparent persistence.

### **Phase I: Bootstrapping and Environment**

### **Setup a Cross-Compiler**: Configure `gcc` or `clang` targeting our choice architecture (like x86_64 or ARM64) on a host system.

- **Write the Bootloader**: Code a minimal assembly routine to switch the processor from 16-bit real mode into long 64-bit protected mode.
- **Target Emulator**: Use tools like ++**[QEMU](https://www.qemu.org/)**++ to test our custom kernel without crashing physical machines.

### **Phase II: Memory Management (The SLS Engine)**

- **Global Descriptor Table (GDT)**: Establish segment definitions to dictate code and data bounds.
- **Page Table Mapping**: Set up 4-level or 5-level page tables mapping virtual addresses directly to physical memory.
- **Custom Page Fault Handler**: This is the heart of an SLS. When a program requests a memory address that isn't mapped to RAM, our handler must trap the `CR2` register fault, identify which storage block corresponds to that virtual address, fetch it from secondary storage, map it, and resume execution.

### **Phase III: Hardware & Object Layer**

- **Storage Drivers**: Implement a raw AHCI or NVMe storage driver capable of reading and writing fixed-size blocks (typically 4KB) based on an address mapping tree.
- **Object Directory Tracker**: Implement a thread-safe, resilient tree structure (like a B-Tree or an inverted page table) that permanently records which virtual address blocks map to physical sectors on our persistent disk.
- **Language Safety Integration**: Because classical hardware memory management can degrade performance, consider a modern approach like ++**[Microsoft's Midori research](https://wiki.c2.com/?SingleAddressSpaceOperatingSystem)**++, utilizing a type-safe language (such as Rust or specialized C#) to enforce object boundary security in software rather than pure hardware tables.

---

## **3. Key Technical Challenges to Solve**


| **Challenge**              | **Impact on SLS Design**                                         | **Remediation Strategy**                                                                    |
| -------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| **Pointer Corruption**     | If memory locations drift, references break across reboots.      | Use Object Capability mechanisms or immutable relative offsets within shared memory spaces. |
| **Power Loss Consistency** | Unwritten cache pages can cause data fragmentation upon crashes. | Implement a non-volatile journaling/shadowing mechanism to commit snapshots periodically.   |
| **Garbage Collection**     | Deleted pointers create "orphaned" memory spaces over time.      | Use global object tables with hardware-backed reference counting to sweep dead data blocks. |


### Next Steps to Launch

1. **Run:** git clone [git@github.com](mailto:git@github.com):kubeworkz/slsos.git slsos
2. **Open the folder:** Launch VS Code or CLion, and select **Open Folder** on the freshly populated root workspace directory.
3. **Run our cross-compilation loops:** You can execute our targets directly inside our integrated terminal window:
  - **For the x86_64 target:** Run `make x86-run`
  - **For the RISC-V target:** Run `make riscv-run`

The full structural suite is now perfectly formatted and readable.

---

## **4. Build & Run Guide**

This section covers everything needed to compile AeroSLS, boot it in QEMU, and connect the Navigator simulator to the live kernel's REST API.

### Prerequisites

Install the following tools on Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y \
    nasm \
    grub-pc-bin grub-common xorriso \
    qemu-system-x86 \
    gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
    build-essential
```

The cross-compiler is expected as `x86_64-elf-gcc`. If your distro provides it as `x86_64-linux-gnu-gcc` instead, create a symlink:

```bash
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld  /usr/local/bin/x86_64-elf-ld
```

For the RISC-V target, also install:

```bash
sudo apt install -y gcc-riscv64-unknown-elf
```

### Step 1 — Clone and build the ISO

```bash
git clone https://github.com/kubeworkz/slsos.git
cd slsos
make x86-iso
```

This produces `sls_operating_system.iso` (a bootable GRUB2 disc image containing the kernel binary).

### Step 2 — Create a persistent storage image (first run only)

```bash
qemu-img create -f raw sls_storage.img 2G
```

### Step 3 — Boot in QEMU

```bash
qemu-system-x86_64 \
  -cdrom sls_operating_system.iso \
  -drive id=disk,file=sls_storage.img,if=none,format=raw \
  -device nvme,drive=disk,serial=slsdev0 \
  -netdev user,id=net0,hostfwd=tcp::3001-:3000 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
  -m 1G -smp 2 -boot d \
  -serial file:sls_kernel_debug.log \
  -display none -daemonize
```

Or use the Makefile shortcut (interactive, with display):

```bash
make x86-run
```

The kernel boots in ~2 seconds. The serial log is written to `sls_kernel_debug.log`. A clean boot looks like:

```
[BSP] LAPIC and IRQ0 timer online.
[MK] Service ONLINE: VirtualMemoryMgr   PID=101
[MK] Service ONLINE: NativeDbStoreMgr   PID=103
[NET] e1000 RX/TX rings online.
[HTTP] Listening on port 3000.
```

### Step 4 — Verify the REST API

The kernel exposes a full REST API on guest port 3000, forwarded to host port 3001:

```bash
# System health
curl http://localhost:3001/api/health

# Object catalog + WAL stats
curl http://localhost:3001/api/scan

# Microkernel service status
curl http://localhost:3001/api/services

# Storage tiers (L1 cache / L2 DRAM / L3 SSD)
curl http://localhost:3001/api/tiers

# Authenticate (returns a lease token)
curl -X POST http://localhost:3001/auth/token \
     -H "Content-Type: application/json" \
     -d '{"email":"dave@gridworkz.com","password":"any"}'
```

### Step 5 — Connect the Navigator Simulator

The simulator is a React/TypeScript SPA in `/home/ubuntu/slsos-sim` (separate repo). Once the kernel is running, start the simulator and it will proxy all OS API calls to the live kernel:

```bash
cd slsos-sim
npm install
npm run dev -- --port 3000 --host
```

Open `http://localhost:3000` in a browser. The simulator's **Address Space Map**, **WAL Log**, **Service Monitor**, and **Query Console** tabs all read live data from the running kernel.

> The simulator proxy is configured in `server.ts` via `http-proxy-middleware`. Routes `/api/scan`, `/api/services`, `/api/tiers`, `/api/wal`, `/api/processes`, `/api/query`, `/api/valloc`, `/api/record`, `/api/tx`, `/auth/token`, and `/auth/verify` are forwarded to `http://localhost:3001`. All other routes (`/api/health`, `/api/v1/*`, Gemini AI) are handled by the simulator itself.

### Step 6 — Stop the kernel

```bash
pkill -f qemu-system-x86_64
```

---

## **5. API Reference**

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/health` | Kernel liveness probe |
| GET | `/api/scan` | Object catalog, WAL stats, service summary |
| GET | `/api/objects` | All SLS objects |
| GET | `/api/objects/<name>` | Single object by name |
| GET | `/api/services` | Microkernel service list + IPC stats |
| GET | `/api/wal` | Write-Ahead Log entries |
| GET | `/api/tiers` | Storage tier contents (L1/L2/L3) |
| GET | `/api/processes` | Ring-3 process table |
| GET | `/api/query?q=<sql>` | SQL-style query engine |
| POST | `/api/valloc` | Allocate a virtual memory object |
| POST | `/api/record` | Write a record to the object store |
| POST | `/api/tx/begin` | Begin a transaction |
| POST | `/api/tx/commit` | Commit a transaction |
| POST | `/api/tx/rollback` | Roll back a transaction |
| POST | `/auth/token` | Issue a lease token (email → role) |
| GET | `/auth/verify` | Validate a Bearer token |

Demo tokens (no real password check):

| Email | Role | Token |
|-------|------|-------|
| `dave@gridworkz.com` | `DB_ADMIN` | `deadbeef01234567cafebabe76543210` |
| `bob@vance.com` | `APP_USER` | `cafebabe7654321089abcdef01234567` |
| `carol@gridworkz.com` | `DB_ADMIN` | `feedf00dabcdef0112345678deadc0de` |
| `guest@sandbox.com` | `GUEST` | `deadc0de9988776655443322aabbccdd` |
